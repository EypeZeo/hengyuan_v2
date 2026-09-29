"""Per-run sequence counter shared by records and ledger events (one total order)."""

from __future__ import annotations

import threading


class SeqCounter:
    """Thread-safe strictly increasing counter starting at 1. ``(run_no, seq)`` orders everything."""

    def __init__(self, start: int = 0) -> None:
        self._value = start
        self._lock = threading.Lock()

    def next(self) -> int:
        with self._lock:
            self._value += 1
            return self._value

    @property
    def last(self) -> int:
        with self._lock:
            return self._value
