"""Tests for the PLAN-04 daily NAV risk summary."""

from __future__ import annotations

from datetime import UTC, datetime, timedelta, timezone
from decimal import Decimal

import pytest

from py_core.research.plan04_risk import DailyNav, summarize_daily_risk

_START = datetime(2024, 1, 1, tzinfo=UTC)
_DAY = timedelta(days=1)
_D = Decimal


def daily_points(values: list[Decimal]) -> tuple[DailyNav, ...]:
    return tuple(DailyNav(_START + index * _DAY, value) for index, value in enumerate(values))


def test_daily_tail_and_recovered_drawdown_use_daily_return_observations() -> None:
    points = daily_points([_D("100"), _D("90"), _D("95"), _D("100")])

    risk = summarize_daily_risk(points)
    episode = risk.deepest_drawdown

    assert risk.observations == 3
    assert risk.tail_observations == 1
    assert risk.mean_worst_five_percent_return == _D("-0.1")
    assert risk.daily_max_drawdown == _D("0.1")
    assert episode is not None
    assert risk.longest_drawdown == episode
    assert risk.open_drawdown is None
    assert episode.peak_at_utc == points[0].as_of_utc
    assert episode.trough_at_utc == points[1].as_of_utc
    assert episode.ended_at_utc == points[3].as_of_utc
    assert episode.recovered_at_utc == points[3].as_of_utc
    assert episode.depth == _D("0.1")
    assert episode.duration_days == 3
    assert episode.right_censored is False


def test_five_percent_tail_uses_ceiling_for_nonintegral_observation_count() -> None:
    values = [_D("100"), _D("90"), _D("72")]
    values.extend([_D("72")] * 19)

    risk = summarize_daily_risk(daily_points(values))

    assert risk.observations == 21
    assert risk.tail_observations == 2
    assert risk.mean_worst_five_percent_return == _D("-0.15")


def test_drawdown_ties_choose_earliest_episode_and_trough() -> None:
    points = daily_points(
        [
            _D("100"),
            _D("100"),
            _D("90"),
            _D("90"),
            _D("100"),
            _D("110"),
            _D("99"),
            _D("99"),
            _D("110"),
        ]
    )

    risk = summarize_daily_risk(points)

    assert risk.deepest_drawdown is not None
    assert risk.longest_drawdown is not None
    assert risk.deepest_drawdown.peak_at_utc == points[1].as_of_utc
    assert risk.deepest_drawdown.trough_at_utc == points[2].as_of_utc
    assert risk.deepest_drawdown.recovered_at_utc == points[4].as_of_utc
    assert risk.deepest_drawdown.duration_days == 3
    assert risk.longest_drawdown.peak_at_utc == points[1].as_of_utc
    assert risk.deepest_drawdown.depth == _D("0.1")
    assert risk.longest_drawdown.depth == _D("0.1")


def test_unrecovered_drawdown_ends_at_last_point_and_is_right_censored() -> None:
    points = daily_points([_D("100"), _D("120"), _D("90"), _D("90"), _D("95")])

    risk = summarize_daily_risk(points)
    episode = risk.open_drawdown

    assert episode is not None
    assert risk.deepest_drawdown == episode
    assert risk.longest_drawdown == episode
    assert episode.peak_at_utc == points[1].as_of_utc
    assert episode.trough_at_utc == points[2].as_of_utc
    assert episode.ended_at_utc == points[-1].as_of_utc
    assert episode.recovered_at_utc is None
    assert episode.depth == _D("0.25")
    assert episode.duration_days == 3
    assert episode.right_censored is True


def test_one_nav_point_has_no_daily_risk_statistics() -> None:
    risk = summarize_daily_risk(daily_points([_D("100")]))

    assert risk.observations == 0
    assert risk.tail_observations == 0
    assert risk.mean_worst_five_percent_return is None
    assert risk.daily_max_drawdown is None
    assert risk.deepest_drawdown is None
    assert risk.longest_drawdown is None
    assert risk.open_drawdown is None


@pytest.mark.parametrize(
    "points",
    [
        (DailyNav(_START.replace(tzinfo=None), _D("100")),),
        (DailyNav(_START.astimezone(timezone(timedelta(hours=1))), _D("100")),),
        (DailyNav(_START.replace(hour=1), _D("100")),),
        (DailyNav(_START, _D("100")), DailyNav(_START + 2 * _DAY, _D("101"))),
        (DailyNav(_START, _D("100")), DailyNav(_START, _D("101"))),
        (DailyNav(_START, _D("100")), DailyNav(_START + 2 * _DAY, _D("101")), DailyNav(_START + _DAY, _D("99"))),
        (DailyNav(_START, _D("100")), DailyNav(_START + _DAY, _D("0"))),
        (DailyNav(_START, _D("100")), DailyNav(_START + _DAY, _D("NaN"))),
    ],
)
def test_invalid_nav_sequences_are_rejected(points: tuple[DailyNav, ...]) -> None:
    with pytest.raises(ValueError):
        summarize_daily_risk(points)
