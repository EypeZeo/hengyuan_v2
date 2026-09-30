"""In-process stall monitor (a plain thread, not part of the event loop).

It detects two failures that a supervisor cannot see from outside without a notify protocol:

* the event loop stopped advancing (its heartbeat task updates ``loop_hb`` every second), and
* the writer thread stopped consuming (``Writer.hb_mono`` stops advancing).

On a stall it records the fact on stderr and exits the process with a distinct code, so that
``Restart=always`` restarts it and the next start recovers any ``.part`` files. It cannot help if the
whole interpreter freezes (a C extension holding the GIL), which is a documented residual risk.
A market with no messages is NOT a stall: liveness here is "the loop and writer are running", not
"data is arriving" (silent streams are handled by the per-connection stall rules).
"""

from __future__ import annotations

import os
import sys
import threading
import time
from collections.abc import Callable

EXIT_LOOP_STALL = 70
EXIT_WRITER_STALL = 71


class Monitor(threading.Thread):
    def __init__(
        self,
        get_loop_hb: Callable[[], float],
        get_writer_hb: Callable[[], float],
        *,
        loop_limit_s: float = 30.0,
        writer_limit_s: float = 60.0,
        interval_s: float = 2.0,
        exit_fn: Callable[[int], None] = os._exit,
        log: Callable[[str], None] | None = None,
        now: Callable[[], float] = time.monotonic,
    ) -> None:
        super().__init__(name="hy-monitor", daemon=True)
        self._loop_hb, self._writer_hb = get_loop_hb, get_writer_hb
        self._loop_limit, self._writer_limit = loop_limit_s, writer_limit_s
        self._interval = interval_s
        self._exit = exit_fn
        self._log = log or (lambda msg: print(msg, file=sys.stderr, flush=True))
        self._now = now
        self._stop_evt = threading.Event()
        self.fired: int | None = None

    def check_once(self) -> int | None:
        """One evaluation; returns the exit code it fired with, if any."""
        now = self._now()
        loop_age = now - self._loop_hb()
        if loop_age > self._loop_limit:
            self._log(
                "hy-monitor: event loop stalled for %.1fs (limit %.0fs), exiting"
                % (loop_age, self._loop_limit)
            )
            self.fired = EXIT_LOOP_STALL
            self._exit(EXIT_LOOP_STALL)
            return EXIT_LOOP_STALL
        writer_age = now - self._writer_hb()
        if writer_age > self._writer_limit:
            self._log(
                "hy-monitor: writer stalled for %.1fs (limit %.0fs), exiting"
                % (writer_age, self._writer_limit)
            )
            self.fired = EXIT_WRITER_STALL
            self._exit(EXIT_WRITER_STALL)
            return EXIT_WRITER_STALL
        return None

    def run(self) -> None:
        while not self._stop_evt.wait(self._interval):
            if self.check_once() is not None:
                return

    def stop(self) -> None:
        self._stop_evt.set()
