"""Append-only manifest of sealed segments (and their later pruning).

One JSON object per line in ``<root>/manifest/<YYYYMMDD>.jsonl`` (date of the event).
Events::

    {"ev":"seal","name":"raw/spot/depth/20260929/14-000012.jsonl.zst","bytes":..,"sha256":"..",
     "records":..,"first":{"r":..,"q":..,"t":..},"last":{...},"streams":{"spot:...":n},
     "venue":"spot","cls":"depth","level":9,"reason":"rotate","truncated":false,"recovered":false,
     "wall_us":..}
    {"ev":"prune","name":"...","sha256":"..","reason":"acked_expired|evicted_unacked|...",
     "acked":true,"first_t":..,"last_t":..,"bytes":..,"wall_us":..}

SHA-256 detects accidental corruption and transfer errors only; an adversary with write access
to both a segment and the manifest is outside the threat model.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

from .fsutil import fsync_dir, utc_date


class Manifest:
    def __init__(self, root: Path) -> None:
        self.root = Path(root)
        self.dir = self.root / "manifest"

    def append(self, event: dict[str, Any]) -> None:
        wall_us = int(event["wall_us"])
        self.dir.mkdir(parents=True, exist_ok=True)
        path = self.dir / (utc_date(wall_us) + ".jsonl")
        created = not path.exists()
        with open(path, "ab") as fh:
            fh.write(json.dumps(event, separators=(",", ":")).encode("utf-8") + b"\n")
            fh.flush()
            os.fsync(fh.fileno())
        if created:
            fsync_dir(self.dir)

    def read_all(self) -> list[dict[str, Any]]:
        events: list[dict[str, Any]] = []
        if not self.dir.exists():
            return events
        for fp in sorted(self.dir.glob("*.jsonl")):
            for line in fp.read_bytes().splitlines():
                line = line.strip()
                if not line:
                    continue
                try:
                    events.append(json.loads(line))
                except ValueError:
                    events.append({"ev": "corrupt_line", "file": fp.name})
        return events

    def live_segments(self) -> dict[str, dict[str, Any]]:
        """Sealed segments that have not been pruned, keyed by relative name."""
        live: dict[str, dict[str, Any]] = {}
        for ev in self.read_all():
            if ev.get("ev") == "seal":
                live[ev["name"]] = ev
            elif ev.get("ev") == "prune":
                live.pop(ev.get("name", ""), None)
        return live
