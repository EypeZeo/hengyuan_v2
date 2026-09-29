#!/usr/bin/env python3
"""Build the deployment bundle: application package + hash-verified wheels, one flat directory.

    python scripts/build_bundle.py --out dist                  # download the wheels named in the lock
    python scripts/build_bundle.py --out dist --wheel-dir DIR  # use wheels already on disk (hash-checked)

Output: ``dist/hy-recorder-<digest16>.tar.gz`` and ``dist/hy-recorder-<digest16>.tar.gz.sha256`` (the
latter in ``sha256sum -c`` format). The release id (``<digest16>``) is content-addressed, see
``hy_recorder/bundle.py``. The tarball itself is built reproducibly (sorted entries, zero mtime and ids).
"""

from __future__ import annotations

import argparse
import gzip
import io
import json
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from hy_recorder import __version__  # noqa: E402
from hy_recorder.bundle import (  # noqa: E402
    DIGEST_CHARS,
    FORMAT,
    MANIFEST,
    hash_tree,
    sha256_path,
    tree_digest,
)

PY_ABI = ("3.11", "cp", "cp311")
PLATFORMS = (
    "manylinux_2_28_x86_64",
    "manylinux_2_17_x86_64",
    "manylinux2014_x86_64",
    "manylinux_2_5_x86_64",
    "manylinux1_x86_64",
)


class BuildError(SystemExit):
    pass


def norm(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).lower()


def parse_lock(path: Path) -> dict[str, tuple[str, set[str]]]:
    text = re.sub(r"\\\r?\n", " ", path.read_text(encoding="utf-8"))
    out: dict[str, tuple[str, set[str]]] = {}
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.match(r"^([A-Za-z0-9_.\-]+)==(\S+)((?:\s+--hash=sha256:[0-9a-f]{64})+)$", line)
        if not m:
            raise BuildError("cannot parse lock line: %r" % raw)
        out[norm(m.group(1))] = (m.group(2), set(re.findall(r"--hash=sha256:([0-9a-f]{64})", m.group(3))))
    if not out:
        raise BuildError("the lock file lists no packages")
    return out


def download_wheels(lock: Path, dest: Path) -> None:
    cmd = [
        sys.executable,
        "-m",
        "pip",
        "download",
        "--quiet",
        "--only-binary=:all:",
        "--no-deps",
        "--require-hashes",
        "-r",
        str(lock),
        "-d",
        str(dest),
        "--python-version",
        PY_ABI[0],
        "--implementation",
        PY_ABI[1],
        "--abi",
        PY_ABI[2],
    ]
    for plat in PLATFORMS:
        cmd += ["--platform", plat]
    subprocess.run(cmd, check=True)


def check_wheels(wheel_dir: Path, lock: dict[str, tuple[str, set[str]]]) -> list[tuple[Path, str]]:
    found: dict[str, tuple[Path, str]] = {}
    for whl in sorted(wheel_dir.glob("*.whl")):
        parts = whl.name.split("-")
        name, version = norm(parts[0]), parts[1]
        if name not in lock:
            raise BuildError("wheel %s is not in the lock" % whl.name)
        want_version, hashes = lock[name]
        sha = sha256_path(whl)
        if version != want_version or sha not in hashes:
            raise BuildError(
                "wheel %s does not match the lock (version %s, sha256 %s)" % (whl.name, want_version, sha)
            )
        if name in found:
            raise BuildError("two wheels for %s in %s" % (name, wheel_dir))
        found[name] = (whl, sha)
    missing = sorted(set(lock) - set(found))
    if missing:
        raise BuildError("no wheel for: %s" % ", ".join(missing))
    return [found[k] for k in sorted(found)]


def extract_wheel(whl: Path, stage: Path) -> None:
    with zipfile.ZipFile(whl) as zf:
        for info in zf.infolist():
            target = (stage / info.filename).resolve()
            if stage.resolve() not in target.parents and target != stage.resolve():
                raise BuildError("wheel %s has an unsafe path %r" % (whl.name, info.filename))
            if info.is_dir() or "__pycache__" in info.filename or info.filename.endswith(".pyc"):
                continue
            if target.exists():
                raise BuildError("wheel %s overwrites %s" % (whl.name, info.filename))
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(zf.read(info))


def git_info() -> dict[str, object]:
    def run(*args: str) -> str:
        return subprocess.run(
            ["git", *args], cwd=ROOT, capture_output=True, text=True, check=True
        ).stdout.strip()

    try:
        return {"sha": run("rev-parse", "HEAD"), "dirty": bool(run("status", "--porcelain", "--", "."))}
    except (OSError, subprocess.CalledProcessError):
        return {"sha": "unknown", "dirty": None}


def write_tar_gz(stage: Path, release: str, out: Path) -> None:
    def info(name: str, *, is_dir: bool, size: int = 0) -> tarfile.TarInfo:
        ti = tarfile.TarInfo(name)
        ti.type = tarfile.DIRTYPE if is_dir else tarfile.REGTYPE
        ti.mode, ti.size, ti.mtime, ti.uid, ti.gid, ti.uname, ti.gname = (
            (0o755 if is_dir else 0o644),
            size,
            0,
            0,
            0,
            "",
            "",
        )
        return ti

    paths = sorted(stage.rglob("*"), key=lambda p: p.relative_to(stage).as_posix())
    with open(out, "wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=0) as gz:
            with tarfile.open(fileobj=gz, mode="w", format=tarfile.GNU_FORMAT) as tar:
                tar.addfile(info(release, is_dir=True))
                for path in paths:
                    name = "%s/%s" % (release, path.relative_to(stage).as_posix())
                    if path.is_dir():
                        tar.addfile(info(name, is_dir=True))
                    else:
                        data = path.read_bytes()
                        tar.addfile(info(name, is_dir=False, size=len(data)), io.BytesIO(data))


def build(out_dir: Path, lock_path: Path, wheel_dir: Path | None) -> tuple[str, Path]:
    lock = parse_lock(lock_path)
    out_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="hy-bundle-") as tmp:
        tmpdir = Path(tmp)
        if wheel_dir is None:
            wheel_dir = tmpdir / "wheels"
            wheel_dir.mkdir()
            download_wheels(lock_path, wheel_dir)
        wheels = check_wheels(wheel_dir, lock)
        stage = tmpdir / "stage"
        stage.mkdir()
        shutil.copytree(
            ROOT / "hy_recorder", stage / "hy_recorder", ignore=shutil.ignore_patterns("__pycache__", "*.pyc")
        )
        for whl, _sha in wheels:
            extract_wheel(whl, stage)
        files = hash_tree(stage)
        digest = tree_digest(files)
        manifest = {
            "format": FORMAT,
            "name": "hy-recorder",
            "version": __version__,
            "git": git_info(),
            "python": "cp311",
            "platform": "linux x86_64, manylinux (glibc >= 2.28)",
            "wheels": [{"file": whl.name, "sha256": sha} for whl, sha in wheels],
            "digest": digest,
            "files": files,
        }
        (stage / MANIFEST).write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n", encoding="utf-8")
        release = digest[:DIGEST_CHARS]
        tarball = out_dir / ("hy-recorder-%s.tar.gz" % release)
        write_tar_gz(stage, release, tarball)
    sha = sha256_path(tarball)
    (out_dir / (tarball.name + ".sha256")).write_text(
        "%s  %s\n" % (sha, tarball.name), encoding="utf-8", newline="\n"
    )
    return release, tarball


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=ROOT / "dist")
    ap.add_argument("--lock", type=Path, default=ROOT / "requirements.lock")
    ap.add_argument("--wheel-dir", type=Path, default=None)
    args = ap.parse_args(argv)
    release, tarball = build(args.out, args.lock, args.wheel_dir)
    print("release %s\n%s (%d bytes)" % (release, tarball, tarball.stat().st_size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
