from __future__ import annotations

import gzip
import importlib.util
import io
import json
import tarfile
import zipfile
from pathlib import Path

import pytest

from hy_recorder.bundle import MANIFEST, hash_tree, tree_digest, verify_bundle

ROOT = Path(__file__).resolve().parents[1]


def load_builder():
    spec = importlib.util.spec_from_file_location("build_bundle", ROOT / "scripts" / "build_bundle.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


bb = load_builder()


def make_wheel(dirpath: Path, name: str, version: str, extra: dict[str, bytes] | None = None) -> Path:
    path = dirpath / ("%s-%s-cp311-cp311-manylinux2014_x86_64.whl" % (name, version))
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("%s/__init__.py" % name, "VERSION = %r\n" % version)
        zf.writestr("%s-%s.dist-info/METADATA" % (name, version), "Name: %s\nVersion: %s\n" % (name, version))
        for k, v in (extra or {}).items():
            zf.writestr(k, v)
    return path


def make_lock(path: Path, wheels: list[Path]) -> Path:
    lines = []
    for w in wheels:
        name, version = w.name.split("-")[:2]
        lines.append("%s==%s \\\n    --hash=sha256:%s\n" % (name, version, bb.sha256_path(w)))
    path.write_text("# test lock\n" + "".join(lines), encoding="utf-8")
    return path


@pytest.fixture
def wheels(tmp_path):
    wd = tmp_path / "wheels"
    wd.mkdir()
    ws = [make_wheel(wd, "alpha", "1.0"), make_wheel(wd, "beta_pkg", "2.5")]
    return wd, ws, make_lock(tmp_path / "requirements.lock", ws)


# -- identity -------------------------------------------------------------------------------------
def test_tree_digest_depends_on_content_and_names_only(tmp_path):
    a = tmp_path / "a"
    (a / "pkg").mkdir(parents=True)
    (a / "pkg" / "x.py").write_text("x = 1\n")
    (a / "y.txt").write_text("y")
    base = tree_digest(hash_tree(a))
    (a / "y.txt").touch()  # mtime only
    (a / "pkg" / "__pycache__").mkdir()
    (a / "pkg" / "__pycache__" / "x.cpython-311.pyc").write_bytes(b"junk")
    (a / MANIFEST).write_text("{}")  # the manifest is not part of its own digest
    assert tree_digest(hash_tree(a)) == base
    (a / "y.txt").write_text("Y")
    assert tree_digest(hash_tree(a)) != base
    (a / "y.txt").write_text("y")
    (a / "y.txt").rename(a / "z.txt")
    assert tree_digest(hash_tree(a)) != base


# -- build ----------------------------------------------------------------------------------------
def test_build_is_reproducible_and_self_verifying(tmp_path, wheels):
    wd, _ws, lock = wheels
    rel1, tar1 = bb.build(tmp_path / "out1", lock, wd)
    rel2, tar2 = bb.build(tmp_path / "out2", lock, wd)
    assert rel1 == rel2 and len(rel1) == 16
    assert tar1.read_bytes() == tar2.read_bytes(), "the tarball must be byte-for-byte reproducible"
    sha_line = (tmp_path / "out1" / (tar1.name + ".sha256")).read_text()
    assert sha_line == "%s  %s\n" % (bb.sha256_path(tar1), tar1.name)

    with tarfile.open(tar1, "r:gz") as tf:
        members = tf.getmembers()
        assert all(m.name == rel1 or m.name.startswith(rel1 + "/") for m in members)
        assert all(m.mtime == 0 and m.uid == 0 and m.gid == 0 and not m.name.startswith("/") for m in members)
        assert [m.name for m in members] == sorted(m.name for m in members) or members[0].name == rel1
        # the extraction filter argument exists from 3.11.4/3.12; the recording host has 3.11.2
        tf.extractall(tmp_path / "x", **({"filter": "data"} if hasattr(tarfile, "data_filter") else {}))
    tree = tmp_path / "x" / rel1
    assert verify_bundle(tree, check_name=True) == []
    assert (tree / "hy_recorder" / "__init__.py").exists() and (tree / "alpha" / "__init__.py").exists()
    assert not list(tree.rglob("__pycache__"))
    manifest = json.loads((tree / MANIFEST).read_bytes())
    assert manifest["digest"].startswith(rel1) and manifest["python"] == "cp311"
    assert {w["file"].split("-")[0] for w in manifest["wheels"]} == {"alpha", "beta_pkg"}
    assert "MANIFEST.json" not in manifest["files"]
    # gzip header carries no timestamp
    assert gzip.open(io.BytesIO(tar1.read_bytes())).read(1) is not None
    assert tar1.read_bytes()[4:8] == b"\0\0\0\0"


def test_the_release_id_changes_when_the_application_changes(tmp_path, wheels, monkeypatch):
    wd, _ws, lock = wheels
    rel1, _ = bb.build(tmp_path / "o1", lock, wd)
    fake_root = tmp_path / "srcroot"
    (fake_root / "hy_recorder").mkdir(parents=True)
    (fake_root / "hy_recorder" / "__init__.py").write_text("__version__ = 'x'\n")
    monkeypatch.setattr(bb, "ROOT", fake_root)
    rel2, _ = bb.build(tmp_path / "o2", lock, wd)
    assert rel1 != rel2


def test_build_refuses_wheels_that_do_not_match_the_lock(tmp_path, wheels):
    wd, ws, lock = wheels
    # 1. a modified wheel (same name, different bytes)
    tampered = tmp_path / "wheels_tampered"
    tampered.mkdir()
    make_wheel(tampered, "alpha", "1.0", {"alpha/evil.py": b"import os\n"})
    (tampered / ws[1].name).write_bytes(ws[1].read_bytes())
    with pytest.raises(bb.BuildError, match="does not match the lock"):
        bb.build(tmp_path / "o", lock, tampered)
    # 2. a wheel the lock never heard of
    extra = tmp_path / "wheels_extra"
    extra.mkdir()
    for w in ws:
        (extra / w.name).write_bytes(w.read_bytes())
    make_wheel(extra, "gamma", "0.1")
    with pytest.raises(bb.BuildError, match="not in the lock"):
        bb.build(tmp_path / "o", lock, extra)
    # 3. a locked package with no wheel
    short = tmp_path / "wheels_short"
    short.mkdir()
    (short / ws[0].name).write_bytes(ws[0].read_bytes())
    with pytest.raises(bb.BuildError, match="no wheel for"):
        bb.build(tmp_path / "o", lock, short)
    # 4. wrong version
    other = tmp_path / "wheels_ver"
    other.mkdir()
    make_wheel(other, "alpha", "1.1")
    (other / ws[1].name).write_bytes(ws[1].read_bytes())
    with pytest.raises(bb.BuildError, match="does not match the lock"):
        bb.build(tmp_path / "o", lock, other)


def test_a_wheel_with_a_path_escape_is_refused(tmp_path):
    wd = tmp_path / "w"
    wd.mkdir()
    bad = make_wheel(wd, "alpha", "1.0", {"../../evil.py": b"x"})
    lock = make_lock(tmp_path / "l.lock", [bad])
    with pytest.raises(bb.BuildError, match="unsafe path"):
        bb.build(tmp_path / "o", lock, wd)


def test_the_real_lock_parses_and_names_only_binary_wheels_for_the_host():
    lock = bb.parse_lock(ROOT / "requirements.lock")
    assert set(lock) == {"websockets", "zstandard"}
    for _name, (version, hashes) in lock.items():
        assert len(hashes) == 1 and version[0].isdigit()


def test_lock_versions_satisfy_the_declared_ranges():
    import tomllib

    py = tomllib.loads((ROOT / "pyproject.toml").read_text(encoding="utf-8"))
    lock = bb.parse_lock(ROOT / "requirements.lock")

    def as_tuple(v):
        return tuple(int(p) for p in v.split("."))

    for dep in py["project"]["dependencies"]:
        name = dep.split(">")[0].strip()
        lo = as_tuple(dep.split(">=")[1].split(",")[0])
        hi = as_tuple(dep.split("<")[1])
        assert lo <= as_tuple(lock[name][0]) < hi, dep


# -- verification ---------------------------------------------------------------------------------
def bundle_dir(tmp_path, name="0123456789abcdef"):
    d = tmp_path / name
    (d / "pkg").mkdir(parents=True)
    (d / "pkg" / "a.py").write_text("a = 1\n")
    (d / "b.py").write_text("b = 2\n")
    files = hash_tree(d)
    (d / MANIFEST).write_text(json.dumps({"format": 1, "digest": tree_digest(files), "files": files}))
    return d


def test_verify_bundle_accepts_an_intact_tree(tmp_path):
    assert verify_bundle(bundle_dir(tmp_path)) == []


def test_verify_bundle_detects_every_kind_of_damage(tmp_path):
    d = bundle_dir(tmp_path)
    (d / "b.py").write_text("b = 3\n")
    (d / "pkg" / "a.py").unlink()
    (d / "extra.py").write_text("sneaky = True\n")
    problems = verify_bundle(d)
    assert (
        "modified: b.py" in problems
        and "missing: pkg/a.py" in problems
        and "unexpected file: extra.py" in problems
    )


def test_verify_bundle_detects_a_manifest_that_was_edited_to_match(tmp_path):
    d = bundle_dir(tmp_path)
    m = json.loads((d / MANIFEST).read_bytes())
    (d / "b.py").write_text("b = 3\n")
    m["files"]["b.py"] = hash_tree(d)["b.py"]  # attacker fixes the list but cannot fix the digest
    (d / MANIFEST).write_text(json.dumps(m))
    assert any("digest" in p for p in verify_bundle(d))


def test_verify_bundle_checks_the_directory_name_on_request(tmp_path):
    d = bundle_dir(tmp_path, name="not-the-digest")
    assert verify_bundle(d) == []
    assert any("not the release digest" in p for p in verify_bundle(d, check_name=True))


def test_verify_bundle_without_a_manifest(tmp_path):
    (tmp_path / "empty").mkdir()
    assert verify_bundle(tmp_path / "empty") and "cannot read" in verify_bundle(tmp_path / "empty")[0]
