"""Injectable time sources (tests replace them; production uses the system clocks)."""

from __future__ import annotations

import time


class Clock:
    """Wall clock (epoch microseconds) and monotonic clock (microseconds)."""

    def wall_us(self) -> int:
        return time.time_ns() // 1000

    def mono_us(self) -> int:
        return time.monotonic_ns() // 1000

    def mono_s(self) -> float:
        return time.monotonic()


SYSTEM_CLOCK = Clock()


class FakeClock(Clock):
    """Deterministic clock for tests; time only moves when ``advance`` is called."""

    def __init__(self, wall_us: int = 1_790_000_000_000_000, mono_us: int = 1_000_000) -> None:
        self._wall_us = wall_us
        self._mono_us = mono_us

    def wall_us(self) -> int:
        return self._wall_us

    def mono_us(self) -> int:
        return self._mono_us

    def mono_s(self) -> float:
        return self._mono_us / 1e6

    def advance(self, seconds: float) -> None:
        delta = int(seconds * 1e6)
        self._wall_us += delta
        self._mono_us += delta

    def jump_wall(self, seconds: float) -> None:
        """Step only the wall clock (NTP step / manual change); monotonic is unaffected."""
        self._wall_us += int(seconds * 1e6)
