"""Segment files: ``.jsonl.zst``, one zstd frame per segment, crash-consistent commit.

Layout: ``<root>/raw/<venue>/<cls>/<YYYYMMDD>/<HH>-<segseq>.jsonl.zst`` (``.part`` while open).
``segseq`` is a persistent counter, so names never collide even if the wall clock steps back.

Commit protocol (writer thread only)::

    open ``.part`` exclusively (O_EXCL)  ->  compress + block flush ~1 s  ->  fsync ~2 s
    seal: finish the frame (writes the frame checksum) -> fsync -> read back and hash
          -> atomic rename -> fsync directory -> append the manifest event (fsync)

A sealed segment is always exactly one zstd frame, so generic readers (``zstd -dc``, pandas,
Polars, DuckDB) see every record. python-zstandard's streaming reader stops after the first
frame by default; multi-frame files would be silently truncated there.

Recovery (:func:`recover`): a leftover ``.part`` is decoded up to its last complete record,
re-encoded into a fresh single-frame sealed segment (``truncated``/``recovered`` flags in the
manifest) and the original is kept under ``recovered/``. A sealed file missing from the
manifest is re-hashed and registered late. A manifest entry whose file is gone (and was never
pruned) is reported as ``MANIFEST_DANGLING`` and makes ``verify`` fail.
"""

from __future__ import annotations

import os
import re
from collections.abc import Callable
from pathlib import Path
from typing import Any

import zstandard as zstd

from .clock import Clock
from .fsutil import US_PER_HOUR, fsync_dir, hour_key, sha256_file, utc_parts
from .manifest import Manifest

SEGMENT_SUFFIX = ".jsonl.zst"
_HEAD_RE = re.compile(
    rb'^\{"v":1,"k":"(m|s)","r":(\d+),"q":(\d+),"g":(\d+),"t":(\d+),"m":(\d+),"s":"([^"]+)",'
)
_DECODE_CHUNK = 64 * 1024


class SegmentWriter:
    """The currently open segment of one ``(venue, cls)``. Owned by the writer thread."""

    def __init__(
        self,
        root: Path,
        venue: str,
        cls: str,
        *,
        level: int,
        next_segseq: Callable[[], int],
        manifest: Manifest,
        clock: Clock,
        max_bytes: int = 256 * 1024 * 1024,
    ) -> None:
        self.root = Path(root)
        self.venue = venue
        self.cls = cls
        self.level = level
        self._next_segseq = next_segseq
        self._manifest = manifest
        self._clock = clock
        self.max_bytes = max_bytes
        self._fh: Any = None
        self._cobj: Any = None
        self._hour_key = 0
        self._reset()

    # -- state -------------------------------------------------------------------
    def _reset(self) -> None:
        self._rel = ""
        self._part: Path | None = None
        self._final: Path | None = None
        self._records = 0
        self._bytes_out = 0
        self._first: dict[str, int] | None = None
        self._last: dict[str, int] | None = None
        self._streams: dict[str, int] = {}
        self._dirty_since_sync = False

    @property
    def is_open(self) -> bool:
        return self._fh is not None

    @property
    def bytes_out(self) -> int:
        return self._bytes_out

    @property
    def rel_name(self) -> str:
        return self._rel

    # -- writing -----------------------------------------------------------------
    def _open_new(self, hk: int) -> None:
        date, hh = utc_parts(hk * US_PER_HOUR)
        seq = self._next_segseq()
        rel = "raw/%s/%s/%s/%s-%06d%s" % (self.venue, self.cls, date, hh, seq, SEGMENT_SUFFIX)
        final = self.root / rel
        final.parent.mkdir(parents=True, exist_ok=True)
        part = final.with_name(final.name + ".part")
        self._fh = open(part, "xb")  # exclusive create: a name is never reused
        fsync_dir(final.parent)
        self._cobj = zstd.ZstdCompressor(level=self.level, write_checksum=True).compressobj()
        self._rel, self._part, self._final = rel, part, final
        self._hour_key = hk
        self._records = 0
        self._bytes_out = 0
        self._first = self._last = None
        self._streams = {}

    def write(self, line: bytes, *, run: int, seq: int, wall_us: int, stream: str) -> dict[str, Any] | None:
        """Append one record line. Returns the manifest event if this call sealed a segment first."""
        hk = hour_key(wall_us)
        sealed = None
        if self._fh is not None and (hk > self._hour_key or self._bytes_out >= self.max_bytes):
            sealed = self.seal("rotate" if hk > self._hour_key else "size")
        if self._fh is None:
            self._open_new(hk)
        chunk = self._cobj.compress(line)
        if chunk:
            self._fh.write(chunk)
            self._bytes_out += len(chunk)
        ref = {"r": run, "q": seq, "t": wall_us}
        if self._first is None:
            self._first = ref
        self._last = ref
        self._records += 1
        self._streams[stream] = self._streams.get(stream, 0) + 1
        self._dirty_since_sync = True
        return sealed

    def roll_if_due(self, now_wall_us: int, grace_us: int = 5_000_000) -> dict[str, Any] | None:
        """Seal the open segment once its hour is over (plus a grace period for records still in flight).

        Without this a class that is written rarely (hourly snapshot keyframes, daily reference data) would
        keep its data in an unsealed ``.part`` file until the next record arrives, and unsealed data can be
        neither pulled nor verified."""
        if self._fh is not None and now_wall_us >= (self._hour_key + 1) * US_PER_HOUR + grace_us:
            return self.seal("rotate")
        return None

    def flush_block(self) -> None:
        """Make everything written so far decodable (block boundary) and hand it to the OS."""
        if self._fh is None:
            return
        chunk = self._cobj.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK)
        if chunk:
            self._fh.write(chunk)
            self._bytes_out += len(chunk)
        self._fh.flush()

    def sync(self) -> None:
        if self._fh is not None and self._dirty_since_sync:
            self._fh.flush()
            os.fsync(self._fh.fileno())
            self._dirty_since_sync = False

    def seal(self, reason: str, *, recovered: bool = False, truncated: bool = False) -> dict[str, Any] | None:
        if self._fh is None:
            return None
        assert self._part is not None and self._final is not None
        tail = self._cobj.flush(zstd.COMPRESSOBJ_FLUSH_FINISH)
        self._fh.write(tail)
        self._fh.flush()
        os.fsync(self._fh.fileno())
        self._fh.close()
        size = self._part.stat().st_size
        digest = sha256_file(self._part)  # read back what actually reached the file
        os.replace(self._part, self._final)
        fsync_dir(self._final.parent)
        event = {
            "ev": "seal",
            "name": self._rel,
            "bytes": size,
            "sha256": digest,
            "records": self._records,
            "first": self._first,
            "last": self._last,
            "streams": self._streams,
            "venue": self.venue,
            "cls": self.cls,
            "level": self.level,
            "reason": reason,
            "truncated": truncated,
            "recovered": recovered,
            "wall_us": self._clock.wall_us(),
        }
        self._manifest.append(event)
        self._fh = None
        self._cobj = None
        self._reset()
        return event

    def abort_for_test(self) -> None:
        """Drop the open handle without sealing (simulates a crash)."""
        if self._fh is not None:
            self._fh.close()
        self._fh = None
        self._cobj = None


def _decode_prefix(data: bytes) -> tuple[bytes, bool]:
    """Decode as much of a (possibly truncated) zstd frame as possible.

    Returns ``(decoded_bytes, frame_complete)``.
    """
    dobj = zstd.ZstdDecompressor().decompressobj()
    out = bytearray()
    complete = False
    try:
        for i in range(0, len(data), _DECODE_CHUNK):
            out += dobj.decompress(data[i : i + _DECODE_CHUNK])
        complete = bool(getattr(dobj, "eof", False))
    except zstd.ZstdError:
        complete = False
    return bytes(out), complete


def recover(
    root: Path,
    *,
    manifest: Manifest,
    level: int,
    next_segseq: Callable[[], int],
    clock: Clock,
    emit: Callable[..., None],
) -> list[dict[str, Any]]:
    """Seal leftovers from a previous run. Returns the manifest events created."""
    root = Path(root)
    created: list[dict[str, Any]] = []
    raw = root / "raw"
    if not raw.exists():
        return created

    # 1. leftover .part files -> re-encode into a proper single-frame sealed segment
    for part in sorted(raw.rglob("*" + SEGMENT_SUFFIX + ".part")):
        rel_parts = part.relative_to(root).parts  # raw/<venue>/<cls>/<date>/<file>
        if len(rel_parts) != 5:
            continue
        venue, cls = rel_parts[1], rel_parts[2]
        decoded, complete = _decode_prefix(part.read_bytes())
        lines = decoded.split(b"\n")
        partial_tail = lines[-1] != b""
        lines = lines[:-1]  # the last element is an incomplete line or empty
        good: list[tuple[bytes, re.Match[bytes]]] = []
        for line in lines:
            m = _HEAD_RE.match(line[:256])
            if m is None or not line.endswith(b"}"):
                partial_tail = True
                break
            good.append((line, m))
        truncated = (not complete) or partial_tail
        writer = SegmentWriter(
            root, venue, cls, level=level, next_segseq=next_segseq, manifest=manifest, clock=clock
        )
        sealed_names: list[str] = []
        for line, m in good:
            ev = writer.write(
                line + b"\n",
                run=int(m.group(2)),
                seq=int(m.group(3)),
                wall_us=int(m.group(5)),
                stream=m.group(7).decode("ascii"),
            )
            if ev is not None:
                sealed_names.append(ev["name"])
        ev = writer.seal("recovered", recovered=True, truncated=truncated)
        if ev is not None:
            sealed_names.append(ev["name"])
            created.append(ev)
        rec_dir = root / "recovered"
        rec_dir.mkdir(parents=True, exist_ok=True)
        os.replace(part, rec_dir / (part.name))
        fsync_dir(rec_dir)
        emit(
            "SEGMENT_RECOVERED",
            original=part.relative_to(root).as_posix(),
            sealed=sealed_names,
            records=len(good),
            truncated=truncated,
        )

    # 2. sealed but unregistered -> hash and register late
    live = manifest.live_segments()
    pruned = {ev.get("name") for ev in manifest.read_all() if ev.get("ev") == "prune"}
    for fp in sorted(raw.rglob("*" + SEGMENT_SUFFIX)):
        rel = fp.relative_to(root).as_posix()
        if rel in live or rel in pruned:
            continue
        digest = sha256_file(fp)
        event = {
            "ev": "seal",
            "name": rel,
            "bytes": fp.stat().st_size,
            "sha256": digest,
            "records": None,
            "first": None,
            "last": None,
            "streams": {},
            "venue": fp.relative_to(root).parts[1],
            "cls": fp.relative_to(root).parts[2],
            "level": None,
            "reason": "registered_late",
            "truncated": False,
            "recovered": True,
            "wall_us": clock.wall_us(),
        }
        manifest.append(event)
        created.append(event)
        emit(
            "SEGMENT_RECOVERED",
            original=rel,
            sealed=[rel],
            records=None,
            truncated=False,
            registered_late=True,
        )

    # 3. manifest entries whose file vanished (and was not pruned)
    for name in manifest.live_segments():
        if not (root / name).exists():
            emit("MANIFEST_DANGLING", name=name)
    return created
