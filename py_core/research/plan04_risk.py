"""Daily NAV risk measures for PLAN-04 historical comparisons."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from datetime import datetime, time, timedelta
from decimal import Decimal
from itertools import pairwise
from statistics import mean

_DAY = timedelta(days=1)
_ONE = Decimal(1)


@dataclass(frozen=True, slots=True)
class DailyNav:
    as_of_utc: datetime
    nav: Decimal


@dataclass(frozen=True, slots=True)
class DrawdownEpisode:
    peak_at_utc: datetime
    trough_at_utc: datetime
    ended_at_utc: datetime
    recovered_at_utc: datetime | None
    depth: Decimal
    duration_days: int
    right_censored: bool


@dataclass(frozen=True, slots=True)
class DailyRisk:
    observations: int
    tail_observations: int
    mean_worst_five_percent_return: Decimal | None
    daily_max_drawdown: Decimal | None
    deepest_drawdown: DrawdownEpisode | None
    longest_drawdown: DrawdownEpisode | None
    open_drawdown: DrawdownEpisode | None


def _episode(peak: DailyNav, trough: DailyNav, end: DailyNav, *, recovered: bool) -> DrawdownEpisode:
    return DrawdownEpisode(
        peak_at_utc=peak.as_of_utc,
        trough_at_utc=trough.as_of_utc,
        ended_at_utc=end.as_of_utc,
        recovered_at_utc=end.as_of_utc if recovered else None,
        depth=_ONE - trough.nav / peak.nav,
        duration_days=(end.as_of_utc - peak.as_of_utc).days,
        right_censored=not recovered,
    )


def worst_five_percent_count(observations: int) -> int:
    """Count the observations in the empirical five-percent tail, rounding up."""
    return (observations + 19) // 20


def daily_simple_returns(points: Sequence[DailyNav]) -> tuple[Decimal, ...]:
    """Validate the clock and NAV values before deriving contiguous daily returns."""
    for point in points:
        stamp = point.as_of_utc
        if stamp.utcoffset() != timedelta(0) or stamp.time() != time.min:
            raise ValueError("daily NAV must be observed at UTC midnight")
        if not isinstance(point.nav, Decimal) or not point.nav.is_finite() or point.nav <= 0:
            raise ValueError("daily NAV must be a finite positive Decimal")
    returns = []
    for previous, current in pairwise(points):
        if current.as_of_utc - previous.as_of_utc != _DAY:
            raise ValueError("daily NAV observations must be consecutive")
        returns.append(current.nav / previous.nav - _ONE)
    return tuple(returns)


def summarize_daily_risk(points: Sequence[DailyNav]) -> DailyRisk:
    """Measure daily return tails and drawdown episodes, including an open final episode."""
    returns = daily_simple_returns(points)
    if not returns:
        return DailyRisk(0, 0, None, None, None, None, None)

    peak = points[0]
    trough: DailyNav | None = None
    episodes: list[DrawdownEpisode] = []
    for current in points[1:]:
        if current.nav >= peak.nav:
            if trough is not None:
                episodes.append(_episode(peak, trough, current, recovered=True))
            peak, trough = current, None
        elif trough is None or current.nav < trough.nav:
            trough = current

    open_drawdown = None
    if trough is not None:
        open_drawdown = _episode(peak, trough, points[-1], recovered=False)
        episodes.append(open_drawdown)

    deepest = max(episodes, key=lambda episode: episode.depth, default=None)
    longest = max(episodes, key=lambda episode: episode.duration_days, default=None)
    tail_count = worst_five_percent_count(len(returns))
    return DailyRisk(
        observations=len(returns),
        tail_observations=tail_count,
        mean_worst_five_percent_return=mean(sorted(returns)[:tail_count]),
        daily_max_drawdown=deepest.depth if deepest is not None else Decimal(0),
        deepest_drawdown=deepest,
        longest_drawdown=longest,
        open_drawdown=open_drawdown,
    )
