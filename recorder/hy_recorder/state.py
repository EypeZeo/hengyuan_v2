"""Small persistent state: run number, segment counter, rate-limit freeze deadlines.

``run_no`` increases on every process start and never decreases, whatever the wall clock does,
so ``(run_no, seq)`` is a total order over everything the recorder ever wrote.
"""

from __future__ import annotations

import json
import re
import threading
from pathlib import Path
from typing import Any

from .fsutil import atomic_write_json

SCHEMA = 1
_SEG_RE = re.compile(r"-(\d{6,})\.jsonl\.zst(?:\.part)?$")


class StateFile:
    """Thread-safe wrapper around ``<root>/state.json`` (atomic replace on every change)."""

    def __init__(self, root: Path) -> None:
        self.root = Path(root)
        self.path = self.root / "state.json"
        self._lock = threading.Lock()
        self._data: dict[str, Any] = {}
        self.recovered = False  # True if the file was missing/corrupt and values were re-derived

    # -- loading -----------------------------------------------------------------
    def load(self) -> None:
        with self._lock:
            self.root.mkdir(parents=True, exist_ok=True)
            try:
                data = json.loads(self.path.read_bytes())
                if not isinstance(data, dict) or data.get("schema") != SCHEMA:
                    raise ValueError("bad schema")
                self._data = data
                self.recovered = False
            except FileNotFoundError:
                self._data = {}
                self.recovered = self._has_prior_data()
                if self.recovered:
                    self._rederive()
            except (ValueError, OSError):
                self._data = {}
                self.recovered = True
                self._rederive()

    def _has_prior_data(self) -> bool:
        return (self.root / "ledger").exists() or (self.root / "raw").exists()

    def _rederive(self) -> None:
        """Rebuild counters from what is on disk so numbering never goes backwards."""
        max_run = 0
        ledger = self.root / "ledger"
        if ledger.exists():
            for fp in sorted(ledger.glob("*.jsonl")):
                try:
                    for line in fp.read_bytes().splitlines():
                        try:
                            max_run = max(max_run, int(json.loads(line).get("run", 0)))
                        except (ValueError, AttributeError, TypeError):
                            continue
                except OSError:
                    continue
        max_seg = 0
        raw = self.root / "raw"
        if raw.exists():
            for fp in raw.rglob("*"):
                m = _SEG_RE.search(fp.name)
                if m:
                    max_seg = max(max_seg, int(m.group(1)))
        self._data = {"schema": SCHEMA, "run_no": max_run, "segseq": max_seg + 1000, "clean": False}

    # -- accessors ---------------------------------------------------------------
    def get(self, key: str, default: Any = None) -> Any:
        with self._lock:
            return self._data.get(key, default)

    def update(self, **kv: Any) -> None:
        with self._lock:
            self._data.update(kv)
            self._data["schema"] = SCHEMA
            atomic_write_json(self.path, self._data)

    # -- lifecycle ---------------------------------------------------------------
    def begin_run(self) -> tuple[int, bool]:
        """Start a new run. Returns ``(run_no, previous_shutdown_was_clean)``."""
        with self._lock:
            first = "run_no" not in self._data
            prev_clean = True if first and not self.recovered else bool(self._data.get("clean", False))
            run_no = int(self._data.get("run_no", 0)) + 1
            self._data.update({"schema": SCHEMA, "run_no": run_no, "clean": False})
            self._data.setdefault("segseq", 0)
            atomic_write_json(self.path, self._data)
            return run_no, prev_clean

    def mark_clean(self) -> None:
        self.update(clean=True)

    def next_segseq(self) -> int:
        with self._lock:
            seq = int(self._data.get("segseq", 0)) + 1
            self._data["segseq"] = seq
            self._data["schema"] = SCHEMA
            atomic_write_json(self.path, self._data)
            return seq
