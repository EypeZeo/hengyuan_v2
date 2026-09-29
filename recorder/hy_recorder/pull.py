"""Operator-side pull: mirror sealed segments from the recorder host, verify them, then acknowledge.

Runs on the operator's machine (never on the recorder host) through the operator's own ``ssh``
configuration, so the recorder holds no credentials and no way to reach the operator. Order matters:

1. fetch the manifest and ledger (append-only text, replaced atomically; a torn last line is dropped),
2. for each sealed, not-yet-mirrored segment: stream it into ``<name>.pulltmp``, compare size and SHA-256
   with the manifest entry, fsync, then atomically rename into place,
3. only after that, write ``acks/<sha256>`` markers on the host so the retention ladder may reclaim
   the space. A segment that failed verification is never acknowledged.

The local manifest describes what the local archive holds: seal events for segments that were not
mirrored are left out, and prune events are dropped for segments that ARE mirrored (the host's
retention removed its copy, the archive keeps it). Prune events for segments that were never mirrored
stay, because they explain the resulting sequence holes for ``verify``.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shlex
import subprocess
from collections.abc import Callable, Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Protocol

from .fsutil import fsync_dir

CHUNK = 1 << 20
ACK_BATCH = 100
_SEG_RE = re.compile(r"^raw/[a-z]+/[a-z]+/\d{8}/\d{2}-\d{6,}\.jsonl\.zst$")
_DATED_RE = re.compile(r"^(manifest|ledger)/\d{8}\.jsonl$")
_SHA_RE = re.compile(r"^[0-9a-f]{64}$")


class PullError(RuntimeError):
    pass


class Transport(Protocol):
    def read_file(self, rel: str) -> bytes: ...
    def list_dir(self, rel: str) -> list[str]: ...
    def stream_file(self, rel: str) -> Iterator[bytes]: ...
    def write_acks(self, shas: list[str]) -> None: ...


def _check_rel(rel: str) -> str:
    if not (
        _SEG_RE.match(rel) or _DATED_RE.match(rel) or rel in ("status.json", "acks", "manifest", "ledger")
    ):
        raise PullError("refusing to touch unexpected remote path %r" % rel)
    return rel


class LocalTransport:
    """Reads a recorder root on the local disk (tests, or a pull from an attached/mounted disk)."""

    def __init__(self, root: Path) -> None:
        self.root = Path(root)

    def read_file(self, rel: str) -> bytes:
        return (self.root / _check_rel(rel)).read_bytes()

    def list_dir(self, rel: str) -> list[str]:
        d = self.root / _check_rel(rel)
        return sorted(p.name for p in d.iterdir() if p.is_file()) if d.exists() else []

    def stream_file(self, rel: str) -> Iterator[bytes]:
        with open(self.root / _check_rel(rel), "rb") as fh:
            while chunk := fh.read(CHUNK):
                yield chunk

    def write_acks(self, shas: list[str]) -> None:
        acks = self.root / "acks"
        acks.mkdir(exist_ok=True)
        for sha in shas:
            if not _SHA_RE.match(sha):
                raise PullError("bad sha %r" % sha)
            (acks / sha).touch()


class SshTransport:
    """Runs a handful of fixed, quoted commands on the host through ``ssh``."""

    def __init__(
        self,
        host: str,
        remote_root: str,
        *,
        ssh: str = "ssh",
        options: tuple[str, ...] = ("-o", "BatchMode=yes", "-o", "ConnectTimeout=15"),
        run: Callable[..., subprocess.CompletedProcess[bytes]] = subprocess.run,
        popen: Callable[..., subprocess.Popen[bytes]] = subprocess.Popen,
    ) -> None:
        self.host, self.root = host, remote_root.rstrip("/")
        self._ssh, self._options, self._run, self._popen = ssh, options, run, popen

    def _argv(self, remote_cmd: str) -> list[str]:
        return [self._ssh, *self._options, self.host, remote_cmd]

    def _path(self, rel: str) -> str:
        return shlex.quote("%s/%s" % (self.root, _check_rel(rel)))

    def read_file(self, rel: str) -> bytes:
        proc = self._run(self._argv("cat -- " + self._path(rel)), capture_output=True, timeout=120)
        if proc.returncode != 0:
            raise PullError(
                "cat %s failed (%d): %s" % (rel, proc.returncode, proc.stderr.decode(errors="replace")[:200])
            )
        return proc.stdout

    def list_dir(self, rel: str) -> list[str]:
        proc = self._run(
            self._argv("ls -1 -- %s 2>/dev/null || true" % self._path(rel)), capture_output=True, timeout=60
        )
        if proc.returncode != 0:
            raise PullError("ls %s failed (%d)" % (rel, proc.returncode))
        return sorted(line for line in proc.stdout.decode().splitlines() if line and "/" not in line)

    def stream_file(self, rel: str) -> Iterator[bytes]:
        proc = self._popen(
            self._argv("cat -- " + self._path(rel)), stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        assert proc.stdout is not None
        try:
            while chunk := proc.stdout.read(CHUNK):
                yield chunk
        finally:
            proc.stdout.close()
            err = proc.stderr.read() if proc.stderr is not None else b""
            code = proc.wait()
        if code != 0:
            raise PullError("cat %s failed (%d): %s" % (rel, code, err.decode(errors="replace")[:200]))

    def write_acks(self, shas: list[str]) -> None:
        if not shas:
            return
        for sha in shas:
            if not _SHA_RE.match(sha):
                raise PullError("bad sha %r" % sha)
        acks = shlex.quote(self.root + "/acks")
        cmd = "mkdir -p %s && cd %s && touch -- %s" % (acks, acks, " ".join(shas))
        proc = self._run(self._argv(cmd), capture_output=True, timeout=60)
        if proc.returncode != 0:
            raise PullError(
                "writing acks failed (%d): %s" % (proc.returncode, proc.stderr.decode(errors="replace")[:200])
            )


@dataclass
class PullResult:
    fetched: list[str] = field(default_factory=list)
    already_had: list[str] = field(default_factory=list)
    archived_only: list[str] = field(
        default_factory=list
    )  # reclaimed by the host, still held (verified) locally
    acked: list[str] = field(default_factory=list)
    evicted_unfetched: list[str] = field(
        default_factory=list
    )  # the host reclaimed them before this pull got there
    errors: list[str] = field(default_factory=list)
    bytes_fetched: int = 0

    @property
    def ok(self) -> bool:
        return not self.errors and not self.evicted_unfetched


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".pulltmp")
    with open(tmp, "wb") as fh:
        fh.write(data)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, path)
    fsync_dir(path.parent)


def _sha256_file(path: Path) -> tuple[str, int]:
    h, n = hashlib.sha256(), 0
    with open(path, "rb") as fh:
        while chunk := fh.read(CHUNK):
            h.update(chunk)
            n += len(chunk)
    return h.hexdigest(), n


def _whole_lines(data: bytes) -> bytes:
    """Drop a torn last line (a fetch can race with an append)."""
    return data if not data or data.endswith(b"\n") else data[: data.rfind(b"\n") + 1]


def _parse(raw: bytes) -> list[tuple[bytes, dict]]:
    out: list[tuple[bytes, dict]] = []
    for line in raw.splitlines():
        if not line.strip():
            continue
        try:
            out.append((line, json.loads(line)))
        except ValueError:
            continue
    return out


def _cache_path(dest: Path) -> Path:
    return dest / ".pull-cache.json"


def _load_cache(dest: Path) -> dict[str, dict]:
    try:
        data = json.loads(_cache_path(dest).read_bytes())
        return data["files"] if data.get("v") == 1 and isinstance(data.get("files"), dict) else {}
    except (OSError, ValueError, KeyError, AttributeError):
        return {}


def _save_cache(dest: Path, cache: dict[str, dict]) -> None:
    _atomic_write(_cache_path(dest), json.dumps({"v": 1, "files": cache}, sort_keys=True).encode())


def _local_matches(dest: Path, name: str, want_sha: str, want_size: int, cache: dict[str, dict]) -> bool:
    """Does the local copy match the manifest? A file whose size and mtime are unchanged since it was last
    hashed is not hashed again (the cache is a convenience for daily pulls; ``verify`` always re-hashes)."""
    path = dest / name
    try:
        st = path.stat()
    except FileNotFoundError:
        return False
    if st.st_size != want_size:
        return False
    hit = cache.get(name)
    if hit and hit.get("size") == st.st_size and hit.get("mtime_ns") == st.st_mtime_ns:
        return hit.get("sha256") == want_sha
    sha, _ = _sha256_file(path)
    cache[name] = {"size": st.st_size, "mtime_ns": st.st_mtime_ns, "sha256": sha}
    return sha == want_sha


def pull(
    transport: Transport,
    dest: Path,
    *,
    ack: bool = True,
    dry_run: bool = False,
    log: Callable[[str], None] = lambda _m: None,
) -> PullResult:
    dest = Path(dest)
    res = PullResult()

    # 1. manifest (remote history), read first: it is the list of what exists and what it must hash to
    remote_manifest: dict[str, bytes] = {}
    for name in transport.list_dir("manifest"):
        rel = "manifest/" + name
        if _DATED_RE.match(rel):
            remote_manifest[name] = _whole_lines(transport.read_file(rel))
    events = [(line, ev, name) for name, raw in remote_manifest.items() for line, ev in _parse(raw)]
    seals: dict[str, dict] = {}
    pruned_names: set[str] = set()
    for _line, ev, _name in events:
        if ev.get("ev") == "seal":
            seals[ev["name"]] = ev
            pruned_names.discard(ev["name"])
        elif ev.get("ev") == "prune":
            pruned_names.add(ev["name"])
    live = {n: e for n, e in seals.items() if n not in pruned_names}
    cache = _load_cache(dest)

    # 2. segments
    held: dict[str, str] = {}  # name -> sha256 of every verified local copy (fetched now or before)
    for name, ev in sorted(live.items()):
        if not _SEG_RE.match(name):
            res.errors.append("unexpected segment name in manifest: %r" % name)
            continue
        want_sha, want_size = str(ev.get("sha256")), int(ev.get("bytes") or -1)
        local = dest / name
        if local.exists():
            if _local_matches(dest, name, want_sha, want_size, cache):
                held[name] = want_sha
                res.already_had.append(name)
                continue
            log("local copy of %s does not match the manifest, fetching again" % name)
        if dry_run:
            res.fetched.append(name)
            continue
        tmp = local.with_name(local.name + ".pulltmp")
        tmp.parent.mkdir(parents=True, exist_ok=True)
        h, n = hashlib.sha256(), 0
        try:
            with open(tmp, "wb") as fh:
                for chunk in transport.stream_file(name):
                    fh.write(chunk)
                    h.update(chunk)
                    n += len(chunk)
                fh.flush()
                os.fsync(fh.fileno())
        except (PullError, OSError, subprocess.SubprocessError) as exc:
            res.errors.append("%s: transfer failed: %s" % (name, exc))
            tmp.unlink(missing_ok=True)
            continue
        if h.hexdigest() != want_sha or n != want_size:
            res.errors.append(
                "%s: verification failed (sha %s.. size %d, manifest %s.. %d)"
                % (name, h.hexdigest()[:12], n, want_sha[:12], want_size)
            )
            tmp.unlink(missing_ok=True)
            continue
        os.replace(tmp, local)
        fsync_dir(local.parent)
        st = local.stat()
        cache[name] = {"size": st.st_size, "mtime_ns": st.st_mtime_ns, "sha256": want_sha}
        held[name] = want_sha
        res.fetched.append(name)
        res.bytes_fetched += n
        log("fetched %s (%d bytes)" % (name, n))

    # segments the host has already reclaimed: fine if the archive holds a verified copy, lost if not
    for name in sorted(pruned_names):
        seal = seals.get(name)
        if (
            seal is not None
            and _SEG_RE.match(name)
            and _local_matches(dest, name, str(seal.get("sha256")), int(seal.get("bytes") or -1), cache)
        ):
            held[name] = str(seal.get("sha256"))
            res.archived_only.append(name)
        else:
            res.evicted_unfetched.append(name)

    if dry_run:
        return res
    _save_cache(dest, cache)

    # 3. local manifest = what the local archive holds; ledger mirrored verbatim
    for name, raw in remote_manifest.items():
        kept: list[bytes] = []
        for line, ev in _parse(raw):
            kind, seg = ev.get("ev"), ev.get("name")
            if kind == "seal" and seg not in held:
                continue
            if kind == "prune" and seg in held:
                continue
            kept.append(line)
        _atomic_write(dest / "manifest" / name, b"".join(line + b"\n" for line in kept))
    for name in transport.list_dir("ledger"):
        rel = "ledger/" + name
        if _DATED_RE.match(rel):
            _atomic_write(dest / rel, _whole_lines(transport.read_file(rel)))
    try:
        _atomic_write(dest / "status.remote.json", transport.read_file("status.json"))
    except (PullError, OSError):
        pass  # informational only

    # 4. acknowledge only what is verified and held locally
    if ack and held:
        already = set(transport.list_dir("acks"))
        todo = sorted({sha for name, sha in held.items() if name in live and sha not in already})
        try:
            for i in range(0, len(todo), ACK_BATCH):
                transport.write_acks(todo[i : i + ACK_BATCH])
                res.acked.extend(todo[i : i + ACK_BATCH])
        except (PullError, OSError, subprocess.SubprocessError) as exc:
            res.errors.append("ack failed: %s" % exc)
    return res
