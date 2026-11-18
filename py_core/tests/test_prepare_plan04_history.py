"""Tests for normalization of public spot klines and daily quote marks."""

from __future__ import annotations

from datetime import UTC, date, datetime, timedelta
from decimal import Decimal

import pytest

from py_core.tools.prepare_plan04_history import _quote_marks, normalize_spot_kline

_EPOCH = datetime(1970, 1, 1, tzinfo=UTC)
_OPEN_2024 = datetime(2024, 1, 1, tzinfo=UTC)
_OPEN_2025 = datetime(2025, 1, 1, tzinfo=UTC)


def source_ticks(value: datetime, *, microseconds: bool) -> int:
    elapsed = value - _EPOCH
    total_microseconds = (elapsed.days * 86_400 + elapsed.seconds) * 1_000_000 + elapsed.microseconds
    if microseconds:
        return total_microseconds
    assert total_microseconds % 1_000 == 0
    return total_microseconds // 1_000


def kline_row(
    start: datetime,
    *,
    microseconds: bool,
    open_price: str = "10.0",
    high_price: str = "12.0",
    low_price: str = "9.0",
    close_price: str = "11.0",
) -> list[str]:
    tick_us = 1 if microseconds else 1_000
    period_units = 4 * 60 * 60 * 1_000_000 // tick_us
    open_tick = source_ticks(start, microseconds=microseconds)
    close_tick_inclusive = open_tick + period_units - 1
    return [
        str(open_tick),
        open_price,
        high_price,
        low_price,
        close_price,
        "100.0",
        str(close_tick_inclusive),
        "1100.0",
        "10",
        "50.0",
        "550.0",
        "0",
    ]


def midnight(day: date) -> datetime:
    return datetime.combine(day, datetime.min.time(), tzinfo=UTC)


def test_kline_normalization_uses_milliseconds_through_2024() -> None:
    raw = kline_row(_OPEN_2024, microseconds=False)

    open_time, close_time, open_price, close_price = normalize_spot_kline(raw, 2024)

    assert open_time == _OPEN_2024
    assert close_time == _OPEN_2024 + timedelta(hours=4)
    assert open_time.utcoffset() == timedelta(0)
    assert close_time.utcoffset() == timedelta(0)
    assert open_price == Decimal("10.0")
    assert close_price == Decimal("11.0")


def test_kline_normalization_uses_microseconds_from_2025_and_adds_inclusive_tick() -> None:
    raw = kline_row(_OPEN_2025, microseconds=True)

    open_time, close_time, open_price, close_price = normalize_spot_kline(raw, 2025)

    assert open_time == _OPEN_2025
    assert close_time == _OPEN_2025 + timedelta(hours=4)
    assert close_time - open_time == timedelta(hours=4)
    assert open_price == Decimal("10.0")
    assert close_price == Decimal("11.0")


@pytest.mark.parametrize(
    "mutation",
    [
        "open_timestamp",
        "close_timestamp",
        "off_grid_open",
        "wrong_close_duration",
        "short_high",
        "high_low_above_open",
        "nonnumeric_price",
    ],
)
def test_kline_normalization_rejects_invalid_timestamps_or_ohlc(mutation: str) -> None:
    raw = kline_row(_OPEN_2024, microseconds=False)
    if mutation == "open_timestamp":
        raw[0] = "not-a-timestamp"
    elif mutation == "close_timestamp":
        raw[6] = "not-a-timestamp"
    elif mutation == "off_grid_open":
        raw[0] = str(source_ticks(_OPEN_2024 + timedelta(minutes=1), microseconds=False))
    elif mutation == "wrong_close_duration":
        raw[6] = str(int(raw[6]) + 1)
    elif mutation == "short_high":
        raw[2] = "10.5"
    elif mutation == "high_low_above_open":
        raw[3] = "10.5"
    else:
        raw[4] = "not-a-price"

    with pytest.raises(ValueError):
        normalize_spot_kline(raw, 2024)


@pytest.mark.parametrize("field_count", [11, 13])
def test_kline_normalization_rejects_rows_with_wrong_field_count(field_count: int) -> None:
    raw = kline_row(_OPEN_2025, microseconds=True)
    raw = raw[:11] if field_count == 11 else [*raw, "extra"]

    with pytest.raises(ValueError):
        normalize_spot_kline(raw, 2025)


def test_quote_marks_use_strictly_prior_ecb_rates_and_asof_usdt_daily_closes() -> None:
    day_0 = date(2025, 1, 1)
    day_1 = date(2025, 1, 2)
    day_2 = date(2025, 1, 3)
    usd_cny = {
        date(2024, 12, 31): Decimal(7),
        day_0: Decimal(8),
    }
    usdt_usd = {
        midnight(day_0): Decimal("1.01"),
        midnight(day_1): Decimal("1.02"),
        midnight(day_2): Decimal("1.03"),
    }

    marks = _quote_marks(midnight(day_0), midnight(day_2 + timedelta(days=1)), usd_cny, usdt_usd)

    assert marks == [
        (midnight(date(2024, 12, 31)), midnight(day_0), Decimal("7.07")),
        (midnight(day_0), midnight(day_1), Decimal("8.16")),
        (midnight(day_0), midnight(day_2), Decimal("8.24")),
    ]


def test_quote_marks_accept_exact_available_midnight_and_reject_missing_sources() -> None:
    day = date(2025, 1, 1)
    day_midnight = midnight(day)
    usd_cny = {date(2024, 12, 31): Decimal(7)}

    exact_mark = _quote_marks(
        day_midnight,
        day_midnight + timedelta(days=1),
        usd_cny,
        {day_midnight: Decimal("1.01")},
    )
    assert exact_mark == [
        (midnight(date(2024, 12, 31)), day_midnight, Decimal("7.07")),
    ]

    with pytest.raises(ValueError):
        _quote_marks(
            day_midnight,
            day_midnight + timedelta(days=1),
            {day: Decimal(8)},
            {day_midnight: Decimal("1.01")},
        )
    with pytest.raises(ValueError):
        _quote_marks(
            day_midnight,
            day_midnight + timedelta(days=1),
            usd_cny,
            {day_midnight + timedelta(hours=1): Decimal("1.01")},
        )
