"""Append-only event ledger (control-plane log) with a total order shared with the records.

Every event carries ``run`` (process run number), ``q`` (sequence number drawn from the same
counter as the records, so records and events interleave in one total order), ``t`` (wall
microseconds), ``m`` (monotonic microseconds) and ``k`` (kind). One JSON object per line in
``<root>/ledger/<YYYYMMDD>.jsonl``. Only the writer thread touches the files.

Kinds used by the recorder (see DATA_DICTIONARY.md for fields):
PROC_START PROC_STOP HOST_PROFILE WS_OPEN WS_CLOSE SUBSCRIBED_NO_DATA STREAM_STALL GAP_DETECTED
BRIDGE_OK SNAPSHOT SNAPSHOT_STALE SNAPSHOT_FAIL OVERRUN BAD_FRAME RATE_LIMIT REST_FROZEN BAN
DISK_WARN DISK_STOP EVICTED_UNACKED PRUNED_ACKED RETENTION_STEPDOWN RESERVE_RELEASED
CLOCK_PROBE CLOCK_STATE LOOP_LAG DNS_SLOW SEGMENT_RECOVERED MANIFEST_DANGLING STATE_RECOVERED
"""

from __future__ import annotations

import json
import os
import time
from pathlib import Path
from typing import Any

from .fsutil import fsync_dir, utc_date

RESERVED_KEYS = frozenset({"run", "q", "t", "m", "k"})


def make_event(run: int, seq: int, wall_us: int, mono_us: int, kind: str, **fields: Any) -> dict[str, Any]:
    """Build a ledger event. ``run/q/t/m/k`` are the envelope and can never be overwritten by a field:
    a caller that passes one (a programming error, caught by the structural test in ``test_ledger_schema``)
    gets it stored as ``x_<name>`` plus ``"schema_clash": true``, because silently replacing the event's own
    sequence number would leave a hole in the total order (the exact failure the end-to-end test found)."""
    ev: dict[str, Any] = {"run": run, "q": seq, "t": wall_us, "m": mono_us, "k": kind}
    clash = sorted(RESERVED_KEYS.intersection(fields))
    for key, value in fields.items():
        ev["x_" + key if key in RESERVED_KEYS else key] = value
    if clash:
        ev["schema_clash"] = clash
    return ev


class Ledger:
    def __init__(self, root: Path, fsync_interval_s: float = 1.0) -> None:
        self.dir = Path(root) / "ledger"
        self._fh: Any = None
        self._date: str | None = None
        self._dirty = False
        self._last_sync = time.monotonic()
        self._interval = fsync_interval_s

    def append(self, event: dict[str, Any], *, force_sync: bool = False) -> None:
        date = utc_date(int(event["t"]))
        if self._fh is None or date != self._date:
            self._rotate(date)
        self._fh.write(json.dumps(event, separators=(",", ":"), ensure_ascii=False).encode("utf-8") + b"\n")
        self._fh.flush()
        self._dirty = True
        if force_sync or time.monotonic() - self._last_sync >= self._interval:
            self.sync()

    def _rotate(self, date: str) -> None:
        self.close()
        self.dir.mkdir(parents=True, exist_ok=True)
        path = self.dir / (date + ".jsonl")
        created = not path.exists()
        self._fh = open(path, "ab")
        self._date = date
        if created:
            fsync_dir(self.dir)

    def sync(self) -> None:
        if self._fh is not None and self._dirty:
            self._fh.flush()
            os.fsync(self._fh.fileno())
            self._dirty = False
        self._last_sync = time.monotonic()

    def close(self) -> None:
        if self._fh is not None:
            self.sync()
            self._fh.close()
            self._fh = None
            self._date = None

    @staticmethod
    def read_all(root: Path) -> list[dict[str, Any]]:
        events: list[dict[str, Any]] = []
        d = Path(root) / "ledger"
        if not d.exists():
            return events
        for fp in sorted(d.glob("*.jsonl")):
            for line in fp.read_bytes().splitlines():
                line = line.strip()
                if not line:
                    continue
                try:
                    events.append(json.loads(line))
                except ValueError:
                    events.append({"k": "CORRUPT_LINE", "file": fp.name, "run": -1, "q": -1})
        events.sort(key=lambda e: (e.get("run", -1), e.get("q", -1)))
        return events
