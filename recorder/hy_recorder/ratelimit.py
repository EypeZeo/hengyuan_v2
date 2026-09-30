"""REST rate governance for a shared egress address.

The recorder's own budget is per process, not per address: other processes on the same host share
the address limit. So the governor (a) keeps this process to a small fraction of the limit, (b) reads
the address-wide used-weight header and backs off when it is high, and (c) reacts to limit responses
immediately and durably:

* HTTP 429 (also 403 WAF and 451): **deep sleep** of all REST for at least 5 minutes (never shorter than
  ``Retry-After``); a second such response within an hour doubles the sleep, capped at 2 hours.
  The deadline is persisted, so a restart cannot end it early. Already-open WebSocket connections keep
  recording; reconnects are spaced at least 30 s apart while sleeping.
* HTTP 418: the address is banned. All Binance-bound traffic (REST and WebSocket reconnects) is frozen
  until the ban ends (``Retry-After``, at least 2 minutes), persisted as well.

Deadlines are honoured while EITHER clock says they are still running: within a process the monotonic
clock cannot be shortened by a wall-clock step, and across a restart the persisted wall deadline holds.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from .clock import Clock
from .guard import REST_WEIGHT_LIMIT_PER_MIN
from .state import StateFile

SLEEP_BASE_S = 300.0
SLEEP_CAP_S = 7200.0
LEVEL_WINDOW_S = 3600.0
BAN_MIN_S = 120.0
WS_SPACING_WHILE_SLEEPING_S = 30.0
_WINDOW_S = 60.0


@dataclass(frozen=True)
class Permit:
    ok: bool
    reason: str = ""
    retry_after_s: float = 0.0


def _retry_after(headers: dict[str, str]) -> float:
    raw = headers.get("retry-after", "")
    try:
        return max(0.0, float(raw))
    except ValueError:
        return 0.0


class RestGovernor:
    def __init__(
        self,
        clock: Clock,
        state: StateFile,
        emit: Callable[..., None],
        *,
        budget_fraction: float = 0.05,
        pressure_fraction: float = 0.5,
        limits: dict[str, int] | None = None,
    ) -> None:
        self._clock = clock
        self._state = state
        self._emit = emit
        self._budget = budget_fraction
        self._pressure = pressure_fraction
        self._limits = dict(limits or REST_WEIGHT_LIMIT_PER_MIN)
        self._spend: dict[str, deque[tuple[float, int]]] = {v: deque() for v in self._limits}
        self._used: dict[str, tuple[float, int]] = {}
        self._sleep_until_wall_us = int(state.get("rest_sleep_until_wall_us", 0))
        self._ban_until_wall_us = int(state.get("ban_until_wall_us", 0))
        self._sleep_until_mono_s = 0.0
        self._ban_until_mono_s = 0.0
        self.counters = {"429": 0, "418": 0, "403": 0, "451": 0}

    # -- deadlines ---------------------------------------------------------------
    def _remaining(self, until_wall_us: int, until_mono_s: float) -> float:
        by_wall = (until_wall_us - self._clock.wall_us()) / 1e6
        by_mono = until_mono_s - self._clock.mono_s()
        return max(by_wall, by_mono, 0.0)

    def sleep_remaining_s(self) -> float:
        return self._remaining(self._sleep_until_wall_us, self._sleep_until_mono_s)

    def ban_remaining_s(self) -> float:
        return self._remaining(self._ban_until_wall_us, self._ban_until_mono_s)

    def ws_blocked_s(self) -> float:
        """How long WebSocket (re)connects must wait: the remaining ban."""
        return self.ban_remaining_s()

    def ws_min_spacing_s(self) -> float:
        """Minimum spacing between WebSocket connection attempts (longer while REST is sleeping)."""
        return WS_SPACING_WHILE_SLEEPING_S if self.sleep_remaining_s() > 0 else 0.0

    # -- permits -----------------------------------------------------------------
    def permit(self, venue: str, weight: int) -> Permit:
        ban = self.ban_remaining_s()
        if ban > 0:
            return Permit(False, "ban", ban)
        sleeping = self.sleep_remaining_s()
        if sleeping > 0:
            return Permit(False, "deep_sleep", sleeping)
        now = self._clock.mono_s()
        limit = self._limits[venue]
        seen = self._used.get(venue)
        if seen is not None and now - seen[0] < _WINDOW_S and seen[1] >= self._pressure * limit:
            return Permit(False, "shared_pressure", max(1.0, _WINDOW_S - (now - seen[0])))
        window = self._spend[venue]
        while window and now - window[0][0] >= _WINDOW_S:
            window.popleft()
        spent = sum(w for _, w in window)
        if spent + weight > self._budget * limit:
            retry = _WINDOW_S - (now - window[0][0]) if window else 1.0
            return Permit(False, "budget", max(1.0, retry))
        return Permit(True)

    def spent(self, venue: str, weight: int) -> None:
        self._spend[venue].append((self._clock.mono_s(), weight))

    # -- responses ---------------------------------------------------------------
    def on_response(self, venue: str, status: int, headers: dict[str, str]) -> str:
        """Record a response. Returns ``"ok"``, ``"sleep"`` or ``"ban"``."""
        used = headers.get("x-mbx-used-weight-1m", "")
        if used.isdigit():
            self._used[venue] = (self._clock.mono_s(), int(used))
        if status == 418:
            self._start_ban(venue, status, _retry_after(headers))
            return "ban"
        if status in (429, 403, 451):
            if status == 451:
                self._emit("GEO_BLOCK_SUSPECT", venue=venue, status=status)
            self._start_sleep(venue, status, _retry_after(headers))
            return "sleep"
        return "ok"

    def _start_sleep(self, venue: str, status: int, retry_after_s: float) -> None:
        self.counters[str(status)] = self.counters.get(str(status), 0) + 1
        used = (self._used.get(venue) or (0, None))[1]
        if self.sleep_remaining_s() > 0:
            # A late response from a request that was already in flight when the sleep began: it is not a
            # new violation, so it neither escalates nor shortens the sleep.
            self._emit(
                "RATE_LIMIT",
                venue=venue,
                status=status,
                retry_after_s=retry_after_s,
                sleep_s=0.0,
                level=int(self._state.get("sleep_level", 0)),
                used_weight=used,
                during_sleep=True,
            )
            return
        now_wall = self._clock.wall_us()
        prev_end = int(self._state.get("last_limit_end_wall_us", 0))
        level = int(self._state.get("sleep_level", 0))
        # Escalate when the limit is hit again within an hour of the previous sleep ENDING (a request can
        # only be sent once the sleep is over, so measuring from the start would never escalate long sleeps).
        level = level + 1 if prev_end and (now_wall - prev_end) / 1e6 <= LEVEL_WINDOW_S else 0
        duration = min(SLEEP_CAP_S, max(SLEEP_BASE_S * (2**level), retry_after_s))
        self._sleep_until_wall_us = now_wall + int(duration * 1e6)
        self._sleep_until_mono_s = self._clock.mono_s() + duration
        self._state.update(
            rest_sleep_until_wall_us=self._sleep_until_wall_us,
            sleep_level=level,
            last_limit_end_wall_us=self._sleep_until_wall_us,
        )
        self._emit(
            "RATE_LIMIT",
            venue=venue,
            status=status,
            retry_after_s=retry_after_s,
            sleep_s=duration,
            level=level,
            used_weight=used,
        )

    def _start_ban(self, venue: str, status: int, retry_after_s: float) -> None:
        self.counters["418"] += 1
        duration = max(BAN_MIN_S, retry_after_s)
        self._ban_until_wall_us = self._clock.wall_us() + int(duration * 1e6)
        self._ban_until_mono_s = self._clock.mono_s() + duration
        self._state.update(ban_until_wall_us=self._ban_until_wall_us)
        self._emit("BAN", venue=venue, status=status, retry_after_s=retry_after_s, ban_s=duration)

    # -- observability -----------------------------------------------------------
    def snapshot(self) -> dict[str, Any]:
        return {
            "sleep_remaining_s": round(self.sleep_remaining_s(), 1),
            "ban_remaining_s": round(self.ban_remaining_s(), 1),
            "used_weight_1m": {v: u[1] for v, u in self._used.items()},
            "counters": dict(self.counters),
        }
