"""Is the host clock disciplined? A ledger marker, never a reason to delay recording.

After a boot the wall clock can be off by a few hundred milliseconds until ``systemd-timesyncd`` has
made its first exchange (about 36 s on the D0 host, and ``time-sync.target`` is not pulled in there, so
``After=time-sync.target`` waits for nothing). Waiting for the sync would cost the first minute of data on
every reboot; recording through it costs a timestamp offset that ``CLOCK_PROBE`` can measure. So the
recorder records through it and writes a ``CLOCK_STATE`` event whenever the state changes, which lets
``report`` say exactly which interval carries unsynchronised receive times.

The state comes from timesyncd's marker file, not from ``adjtimex``: the unit sets ``ProtectClock=yes``,
whose seccomp filter denies ``adjtimex`` outright (reads included). The marker lives in ``/run`` (tmpfs, gone
after a reboot), is created at the first successful exchange and touched at every later one.
"""

from __future__ import annotations

import asyncio
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

TIMESYNC_DIR = Path("/run/systemd/timesync")
SYNC_MARKER = TIMESYNC_DIR / "synchronized"
# timesyncd polls every 32 s to 34 min once locked; a marker untouched for this long means
# the NTP exchanges stopped
MAX_MARKER_AGE_S = 3 * 3600.0


@dataclass(frozen=True)
class SyncState:
    synced: (
        bool | None
    )  # None: cannot tell (no systemd-timesyncd runtime directory: chrony, a dev machine, ...)
    source: str  # "timesyncd" | "unknown"
    marker_age_s: float | None = None  # seconds since the last successful NTP exchange
    reason: str | None = None  # why synced is False: "no_marker" | "marker_stale"


def read_sync_state(marker: Path = SYNC_MARKER, *, now_s: float | None = None) -> SyncState:
    try:
        st = marker.stat()
    except FileNotFoundError:
        if marker.parent.is_dir():
            return SyncState(False, "timesyncd", None, "no_marker")
        return SyncState(None, "unknown")
    except OSError:
        return SyncState(None, "unknown")
    age = max(0.0, (time.time() if now_s is None else now_s) - st.st_mtime)
    if age > MAX_MARKER_AGE_S:
        return SyncState(False, "timesyncd", age, "marker_stale")
    return SyncState(True, "timesyncd", age)


class ClockStateWatcher:
    """Emits ``CLOCK_STATE`` at start and on every change of the ``synced`` flag.

    ``start()`` runs during boot, before any record exists, so the first event precedes all data of the run.
    A change to synced carries ``unsynced_s``: how long this watcher saw the clock unsynchronised.
    """

    def __init__(
        self,
        emit: Callable[..., None],
        *,
        read: Callable[[], SyncState] = read_sync_state,
        mono: Callable[[], float] = time.monotonic,
        unsynced_poll_s: float = 1.0,
        synced_poll_s: float = 30.0,
    ) -> None:
        self._emit = emit
        self._read = read
        self._mono = mono
        self._unsynced_poll_s = unsynced_poll_s
        self._synced_poll_s = synced_poll_s
        self._last: SyncState | None = None
        self._unsynced_since: float | None = None

    @staticmethod
    def _fields(st: SyncState) -> dict[str, Any]:
        return {
            "synced": st.synced,
            "source": st.source,
            "marker_age_s": None if st.marker_age_s is None else round(st.marker_age_s, 1),
            "reason": st.reason,
        }

    def start(self) -> None:
        st = self._read()
        self._last = st
        if st.synced is False:
            self._unsynced_since = self._mono()
        self._emit("CLOCK_STATE", initial=True, **self._fields(st))

    def poll_once(self) -> None:
        st = self._read()
        prev = self._last
        self._last = st
        if prev is not None and st.synced == prev.synced:
            return
        extra: dict[str, Any] = {}
        if st.synced is False and self._unsynced_since is None:
            self._unsynced_since = self._mono()
        elif st.synced is not False and self._unsynced_since is not None:
            if st.synced is True:
                extra["unsynced_s"] = round(self._mono() - self._unsynced_since, 1)
            self._unsynced_since = None
        self._emit("CLOCK_STATE", initial=False, **self._fields(st), **extra)

    async def run(self, stop: asyncio.Event) -> None:
        while not stop.is_set():
            unsynced = self._last is not None and self._last.synced is False
            try:
                await asyncio.wait_for(
                    stop.wait(), timeout=self._unsynced_poll_s if unsynced else self._synced_poll_s
                )
            except TimeoutError:
                pass
            if stop.is_set():
                return
            self.poll_once()
