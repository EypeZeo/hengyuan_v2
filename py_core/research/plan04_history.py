"""Load a local historical PLAN-04 market and join rates as of each bar open."""

from __future__ import annotations

import csv
import json
from bisect import bisect_right
from collections.abc import Iterator
from dataclasses import dataclass, fields
from datetime import UTC, datetime, timedelta
from decimal import Decimal, InvalidOperation
from pathlib import Path

from py_core.research.plan04_comparison import (
    ComparisonBar,
    ExecutionAssumptions,
    validate_execution_assumptions,
)

_BAR_INTERVAL = timedelta(hours=4)
_MANIFEST_KEYS = {
    "symbol", "data_version", "start_utc", "end_utc",
    "bars_csv", "quote_rates_csv", "max_rate_age_hours", "execution",
}
_BAR_COLUMNS = ("open_time_utc", "close_time_utc", "open_price", "close_price")
_RATE_COLUMNS = ("observed_at_utc", "available_at_utc", "quote_cny_rate")


@dataclass(frozen=True, slots=True)
class HistoricalMarket:
    symbol: str
    data_version: str
    bars: tuple[ComparisonBar, ...]
    execution: ExecutionAssumptions
    max_rate_age_hours: int


@dataclass(frozen=True, slots=True)
class _QuoteRate:
    observed: datetime
    available: datetime
    rate: Decimal


def _text(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{field} must be a nonempty string")
    return value


def _timestamp(value: object, field: str) -> datetime:
    stamp = datetime.fromisoformat(_text(value, field))
    if stamp.utcoffset() != timedelta(0):
        raise ValueError(f"{field} must be an ISO 8601 UTC timestamp")
    return stamp


def _decimal(value: object, field: str) -> Decimal:
    try:
        result = Decimal(_text(value, field))
    except InvalidOperation as error:
        raise ValueError(f"{field} must be a decimal string") from error
    if not result.is_finite():
        raise ValueError(f"{field} must be finite")
    return result


def _positive(value: object, field: str) -> Decimal:
    result = _decimal(value, field)
    if result <= 0:
        raise ValueError(f"{field} must be positive")
    return result


def _rows(path: Path, columns: tuple[str, ...]) -> Iterator[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle, strict=True)
        try:
            header = reader.fieldnames or []
            if len(header) != len(columns) or set(header) != set(columns):
                raise ValueError(f"{path.name} must contain the columns {', '.join(columns)}")
            for row in reader:
                if None in row or any(value is None for value in row.values()):
                    raise ValueError(f"{path.name} has an incomplete or extra field on line {reader.line_num}")
                yield row
        except csv.Error as error:
            raise ValueError(f"{path.name} has malformed CSV near line {reader.line_num}: {error}") from error


def _read_rates(path: Path) -> list[_QuoteRate]:
    rates: list[_QuoteRate] = []
    for row in _rows(path, _RATE_COLUMNS):
        observed = _timestamp(row["observed_at_utc"], "observed_at_utc")
        available = _timestamp(row["available_at_utc"], "available_at_utc")
        if observed > available:
            raise ValueError("a quote rate cannot be available before its observation")
        if rates and available <= rates[-1].available:
            raise ValueError("quote-rate availability times must be strictly increasing")
        rates.append(_QuoteRate(observed, available, _positive(row["quote_cny_rate"], "quote_cny_rate")))
    return rates


def _read_bars(
    path: Path,
    rates: list[_QuoteRate],
    start: datetime,
    end: datetime,
    max_age: timedelta,
) -> tuple[ComparisonBar, ...]:
    available_times = [rate.available for rate in rates]
    bars = []
    expected_open = start
    for row in _rows(path, _BAR_COLUMNS):
        opened = _timestamp(row["open_time_utc"], "open_time_utc")
        closed = _timestamp(row["close_time_utc"], "close_time_utc")
        if opened != expected_open or opened >= end or closed != opened + _BAR_INTERVAL:
            raise ValueError(f"expected a contiguous 4-hour bar at {expected_open.isoformat()}, got {opened.isoformat()}")
        rate_index = bisect_right(available_times, opened) - 1
        if rate_index < 0:
            raise ValueError(f"no available quote rate at {opened.isoformat()}")
        rate = rates[rate_index]
        if opened - rate.observed > max_age:
            raise ValueError(f"stale quote rate at {opened.isoformat()}")
        bars.append(ComparisonBar(
            opened,
            _positive(row["open_price"], "open_price"),
            _positive(row["close_price"], "close_price"),
            rate.rate,
        ))
        expected_open = closed
    if expected_open != end:
        raise ValueError(f"bar coverage ends at {expected_open.isoformat()}, expected {end.isoformat()}")
    return tuple(bars)


def _execution(values: object) -> ExecutionAssumptions:
    names = {field.name for field in fields(ExecutionAssumptions)}
    if not isinstance(values, dict) or values.keys() != names:
        raise ValueError(f"execution must specify {', '.join(sorted(names))}")
    assumptions = ExecutionAssumptions(**{name: _decimal(value, name) for name, value in values.items()})
    validate_execution_assumptions(assumptions)
    return assumptions


def load_historical_market(manifest_path: Path) -> HistoricalMarket:
    """Require complete fixed-window coverage and use only rates available at the open."""
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    if not isinstance(manifest, dict) or manifest.keys() != _MANIFEST_KEYS:
        raise ValueError(f"manifest must specify {', '.join(sorted(_MANIFEST_KEYS))}")
    start = _timestamp(manifest["start_utc"], "start_utc")
    end = _timestamp(manifest["end_utc"], "end_utc")
    if not start < end <= datetime.now(UTC) or (end - start) % _BAR_INTERVAL:
        raise ValueError("historical window must contain completed 4-hour bars")
    age_hours = manifest["max_rate_age_hours"]
    if type(age_hours) is not int or age_hours <= 0:
        raise ValueError("max_rate_age_hours must be a positive integer")
    assumptions = _execution(manifest["execution"])
    directory = manifest_path.parent
    rates = _read_rates(directory / _text(manifest["quote_rates_csv"], "quote_rates_csv"))
    bars = _read_bars(
        directory / _text(manifest["bars_csv"], "bars_csv"), rates, start, end, timedelta(hours=age_hours)
    )
    return HistoricalMarket(
        _text(manifest["symbol"], "symbol"),
        _text(manifest["data_version"], "data_version"),
        bars,
        assumptions,
        age_hours,
    )
