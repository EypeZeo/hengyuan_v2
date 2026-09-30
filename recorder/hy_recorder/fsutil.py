"""Small filesystem helpers: durable directory sync, atomic JSON files, hashing, UTC path parts."""

from __future__ import annotations

import hashlib
import json
import os
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

US_PER_HOUR = 3_600_000_000


def fsync_dir(path: Path) -> None:
    """Flush directory metadata (needed for a rename or create to survive power loss).

    Windows has no standard-library path for this; there it is a no-op (crash-safe only).
    """
    if os.name == "nt":
        return
    fd = os.open(str(path), os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def atomic_write_bytes(path: Path, data: bytes) -> None:
    """Write ``data`` to ``path`` atomically: temp file, fsync, replace, fsync directory."""
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "wb") as fh:
        fh.write(data)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, path)
    fsync_dir(path.parent)


def atomic_write_json(path: Path, obj: Any) -> None:
    atomic_write_bytes(path, json.dumps(obj, separators=(",", ":"), sort_keys=True).encode("utf-8"))


def sha256_file(path: Path, chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            block = fh.read(chunk)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def hour_key(wall_us: int) -> int:
    """Whole UTC hours since the epoch."""
    return wall_us // US_PER_HOUR


def utc_parts(wall_us: int) -> tuple[str, str]:
    """``(YYYYMMDD, HH)`` in UTC for an epoch-microsecond timestamp."""
    dt = datetime.fromtimestamp(wall_us / 1e6, tz=UTC)
    return dt.strftime("%Y%m%d"), dt.strftime("%H")


def utc_date(wall_us: int) -> str:
    return utc_parts(wall_us)[0]
