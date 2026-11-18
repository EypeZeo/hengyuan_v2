"""Tests for loading UTC historical bars and availability-timed quote rates."""

from __future__ import annotations

import json
from datetime import UTC, datetime, timedelta
from decimal import Decimal
from pathlib import Path

import pytest

from py_core.research.plan04_history import load_historical_market

_START = datetime(2024, 1, 1, tzinfo=UTC)


def stamp(value: datetime) -> str:
    return value.isoformat().replace("+00:00", "Z")


def bars_csv_rows(
    count: int,
    *,
    start: datetime = _START,
    missing_bar: int | None = None,
    cadence_hours: int = 4,
    close_offset_hours: int = 4,
) -> list[tuple[str, str, str, str]]:
    rows = []
    for index in range(count):
        if index == missing_bar:
            continue
        open_time = start + timedelta(hours=cadence_hours * index)
        close_time = open_time + timedelta(hours=close_offset_hours)
        price = str(Decimal(10) + index)
        rows.append((stamp(open_time), stamp(close_time), price, price))
    return rows


def _csv_text(header: tuple[str, ...], rows: list[tuple[str, ...]]) -> str:
    return "\n".join((",".join(header), *(",".join(row) for row in rows))) + "\n"


def write_manifest(
    root: Path,
    bar_rows: list[tuple[str, str, str, str]],
    rate_rows: list[tuple[str, str, str]],
    *,
    start: datetime = _START,
    end: datetime | None = None,
    max_rate_age_hours: int = 72,
    bars_name: str = "bars.csv",
    rates_name: str = "rates.csv",
    write_bars: bool = True,
    write_rates: bool = True,
    execution: dict[str, str] | None = None,
) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    if end is None:
        end = start + timedelta(hours=4 * len(bar_rows))
    if write_bars:
        (root / bars_name).write_text(
            _csv_text(
                ("open_time_utc", "close_time_utc", "open_price", "close_price"),
                bar_rows,
            ),
            encoding="utf-8",
        )
    if write_rates:
        (root / rates_name).write_text(
            _csv_text(
                ("observed_at_utc", "available_at_utc", "quote_cny_rate"),
                rate_rows,
            ),
            encoding="utf-8",
        )
    manifest = {
        "symbol": "BTC/USDT",
        "data_version": "fixture-bars-v1",
        "start_utc": stamp(start),
        "end_utc": stamp(end),
        "bars_csv": bars_name,
        "quote_rates_csv": rates_name,
        "max_rate_age_hours": max_rate_age_hours,
        "execution": execution
        or {
            "price_tick": "0.01",
            "quantity_step": "0.0001",
            "min_notional": "5",
            "trade_fee_rate": "0.001",
            "conversion_fee_rate": "0.002",
            "slippage_rate": "0.0005",
            "strategy_allocation": "0.5",
            "strategy_monthly_cost_cny": "30",
        },
    }
    path = root / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def rates_csv_rows(rows: list[tuple[datetime, datetime, str]]) -> list[tuple[str, str, str]]:
    return [(stamp(observed), stamp(available), rate) for observed, available, rate in rows]


def test_fx_join_uses_latest_available_rate_without_looking_ahead(tmp_path: Path) -> None:
    bars = bars_csv_rows(3)
    rates = rates_csv_rows(
        [
            (_START - timedelta(hours=24), _START, "7"),
            (_START + timedelta(hours=4), _START + timedelta(hours=5), "8"),
            (_START + timedelta(hours=8), _START + timedelta(hours=8), "9"),
        ]
    )
    manifest = write_manifest(tmp_path, bars, rates)

    loaded = load_historical_market(manifest)

    assert loaded.symbol == "BTC/USDT"
    assert loaded.data_version == "fixture-bars-v1"
    assert len(loaded.bars) == 3
    assert [bar.quote_cny_rate for bar in loaded.bars] == [Decimal(7), Decimal(7), Decimal(9)]
    assert loaded.bars[0].open_time_utc == _START
    assert loaded.execution.price_tick == Decimal("0.01")
    assert loaded.execution.quantity_step == Decimal("0.0001")
    assert loaded.execution.min_notional == Decimal(5)
    assert loaded.execution.strategy_allocation == Decimal("0.5")


def test_rate_available_at_bar_open_and_exact_age_limit_are_accepted(tmp_path: Path) -> None:
    bars = bars_csv_rows(1)
    rates = rates_csv_rows([(_START - timedelta(hours=24), _START, "7")])
    manifest = write_manifest(
        tmp_path,
        bars,
        rates,
        end=_START + timedelta(hours=4),
        max_rate_age_hours=24,
    )

    loaded = load_historical_market(manifest)

    assert loaded.bars[0].quote_cny_rate == Decimal(7)


def test_stale_rate_and_missing_pre_bar_rate_are_rejected(tmp_path: Path) -> None:
    bars = bars_csv_rows(2)
    stale_rates = rates_csv_rows([(_START - timedelta(hours=24), _START, "7")])
    stale_manifest = write_manifest(
        tmp_path / "stale",
        bars,
        stale_rates,
        end=_START + timedelta(hours=8),
        max_rate_age_hours=24,
    )
    with pytest.raises(ValueError):
        load_historical_market(stale_manifest)

    future_rates = rates_csv_rows([(_START + timedelta(hours=1), _START + timedelta(hours=1), "8")])
    future_manifest = write_manifest(
        tmp_path / "future",
        bars,
        future_rates,
        end=_START + timedelta(hours=8),
    )
    with pytest.raises(ValueError):
        load_historical_market(future_manifest)


@pytest.mark.parametrize(
    ("bar_rows", "start", "end"),
    [
        (bars_csv_rows(3, missing_bar=1), _START, _START + timedelta(hours=12)),
        (
            [(stamp(_START), stamp(_START + timedelta(hours=3)), "10", "10")],
            _START,
            _START + timedelta(hours=4),
        ),
        (
            [("2024-01-01T01:00:00+01:00", "2024-01-01T05:00:00+01:00", "10", "10")],
            _START,
            _START + timedelta(hours=4),
        ),
    ],
)
def test_bar_holes_and_noncanonical_close_times_are_rejected(
    tmp_path: Path,
    bar_rows: list[tuple[str, str, str, str]],
    start: datetime,
    end: datetime,
) -> None:
    rates = rates_csv_rows([(_START, _START, "7")])
    manifest = write_manifest(
        tmp_path,
        bar_rows,
        rates,
        start=start,
        end=end,
        max_rate_age_hours=100,
    )

    with pytest.raises(ValueError):
        load_historical_market(manifest)


@pytest.mark.parametrize(
    ("rate_rows", "start_text"),
    [
        (
            rates_csv_rows([(_START + timedelta(hours=1), _START, "7")]),
            stamp(_START),
        ),
        (
            [("2024-01-01T00:00:00+01:00", "2024-01-01T00:00:00+01:00", "7")],
            stamp(_START),
        ),
        (
            rates_csv_rows(
                [
                    (_START, _START + timedelta(hours=4), "7"),
                    (_START + timedelta(hours=4), _START + timedelta(hours=4), "8"),
                ]
            ),
            stamp(_START),
        ),
    ],
)
def test_invalid_fx_order_or_non_utc_timestamps_are_rejected(
    tmp_path: Path,
    rate_rows: list[tuple[str, str, str]],
    start_text: str,
) -> None:
    bars = bars_csv_rows(2)
    manifest = write_manifest(
        tmp_path,
        bars,
        rate_rows,
        start=datetime.fromisoformat(start_text),
        end=_START + timedelta(hours=8),
        max_rate_age_hours=100,
    )

    with pytest.raises(ValueError):
        load_historical_market(manifest)


@pytest.mark.parametrize(
    ("bars_name", "rates_name", "write_bars", "write_rates"),
    [
        ("missing-bars.csv", "rates.csv", False, True),
        ("bars.csv", "missing-rates.csv", True, False),
    ],
)
def test_missing_market_or_fx_file_is_rejected(
    tmp_path: Path,
    bars_name: str,
    rates_name: str,
    write_bars: bool,
    write_rates: bool,
) -> None:
    manifest = write_manifest(
        tmp_path,
        bars_csv_rows(1),
        rates_csv_rows([(_START, _START, "7")]),
        end=_START + timedelta(hours=4),
        bars_name=bars_name,
        rates_name=rates_name,
        write_bars=write_bars,
        write_rates=write_rates,
    )

    with pytest.raises(OSError):
        load_historical_market(manifest)


def test_invalid_execution_decimal_and_missing_rate_are_not_silently_defaulted(tmp_path: Path) -> None:
    manifest = write_manifest(
        tmp_path,
        bars_csv_rows(1),
        rates_csv_rows([(_START, _START, "7")]),
        end=_START + timedelta(hours=4),
        execution={
            "price_tick": "bad",
            "quantity_step": "0.0001",
            "min_notional": "5",
            "trade_fee_rate": "0.001",
            "conversion_fee_rate": "0.002",
            "slippage_rate": "0.0005",
            "strategy_allocation": "0.5",
            "strategy_monthly_cost_cny": "30",
        },
    )

    with pytest.raises(ValueError):
        load_historical_market(manifest)
