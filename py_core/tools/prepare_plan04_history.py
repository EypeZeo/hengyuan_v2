"""Prepare local Binance, ECB, and Coinbase archives for PLAN-04."""

from __future__ import annotations

import argparse
import csv
import io
import json
from bisect import bisect_left
from collections.abc import Iterable, Sequence
from dataclasses import asdict
from datetime import UTC, date, datetime, time, timedelta
from decimal import Decimal, InvalidOperation
from pathlib import Path
from tempfile import TemporaryDirectory
from zipfile import BadZipFile, ZipFile

from py_core.research.plan04_comparison import ExecutionAssumptions
from py_core.research.plan04_history import load_historical_market

_EPOCH = datetime(1970, 1, 1, tzinfo=UTC)
_DAY = timedelta(days=1)
_FOUR_HOURS = timedelta(hours=4)
_SYMBOL_STEPS = {"BTCUSDT": Decimal("0.00001"), "ETHUSDT": Decimal("0.0001")}
SpotBar = tuple[datetime, datetime, Decimal, Decimal]
QuoteMark = tuple[datetime, datetime, Decimal]


def _price(value: str | int | Decimal) -> Decimal:
    try:
        result = Decimal(value)
    except InvalidOperation as error:
        raise ValueError(f"invalid decimal price {value!r}") from error
    if not result.is_finite() or result <= 0:
        raise ValueError(f"price must be finite and positive, got {value!r}")
    return result


def _midnight(day: date) -> datetime:
    return datetime.combine(day, time.min, tzinfo=UTC)


def normalize_spot_kline(row: Sequence[str], year: int) -> SpotBar:
    """Convert a Binance Spot bar to an exact half-open UTC interval."""
    if len(row) != 12:
        raise ValueError("Binance Spot kline must have twelve fields")
    tick_us = 1_000 if year < 2025 else 1
    try:
        opened = _EPOCH + timedelta(microseconds=int(row[0]) * tick_us)
        closed = _EPOCH + timedelta(microseconds=(int(row[6]) + 1) * tick_us)
    except (ValueError, OverflowError) as error:
        raise ValueError("invalid Binance Spot kline timestamp") from error
    if opened.year != year or (opened - _EPOCH) % _FOUR_HOURS or closed - opened != _FOUR_HOURS:
        raise ValueError(f"invalid 4-hour kline boundary at {opened.isoformat()}")
    opening, high, low, closing = map(_price, row[1:5])
    if not low <= min(opening, closing) <= max(opening, closing) <= high:
        raise ValueError(f"invalid kline price range at {opened.isoformat()}")
    return opened, closed, opening, closing


def _spot_bars(raw_dir: Path, symbol: str, year: int) -> list[SpotBar]:
    bars: list[SpotBar] = []
    for month in range(1, 13):
        stem = f"{symbol}-4h-{year}-{month:02d}"
        with (
            ZipFile(raw_dir / f"{stem}.zip") as archive,
            archive.open(f"{stem}.csv") as member,
            io.TextIOWrapper(member, encoding="utf-8") as handle,
        ):
            for row in csv.reader(handle):
                bar = normalize_spot_kline(row, year)
                if bar[0].month != month:
                    raise ValueError(f"{stem} contains a kline from a different month")
                bars.append(bar)
    return bars


def _usd_cny(path: Path) -> dict[date, Decimal]:
    observations: dict[tuple[date, str], Decimal] = {}
    with path.open(encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            key = date.fromisoformat(row["TIME_PERIOD"]), row["CURRENCY"]
            if key[1] not in {"CNY", "USD"} or key in observations:
                raise ValueError(f"unexpected or repeated ECB observation {key!r}")
            if row["OBS_VALUE"]:
                observations[key] = _price(row["OBS_VALUE"])
    result = {}
    for day in {day for day, currency in observations}:
        if (day, "CNY") not in observations or (day, "USD") not in observations:
            raise ValueError(f"unpaired ECB day {day}")
        result[day] = observations[day, "CNY"] / observations[day, "USD"]
    return result


def _usdt_usd(raw_dir: Path) -> dict[datetime, Decimal]:
    values: dict[datetime, Decimal] = {}
    for path in sorted(raw_dir.glob("coinbase-*.json")):
        for opened, low, high, opening, closing, _volume in json.loads(path.read_text(encoding="utf-8"), parse_float=Decimal):
            if type(opened) is not int or opened % 86_400:
                raise ValueError(f"invalid UTC USDT/USD daily candle in {path.name}")
            available = _EPOCH + timedelta(seconds=opened) + _DAY
            low, high, opening, closing = map(_price, (low, high, opening, closing))
            if not low <= min(opening, closing) <= max(opening, closing) <= high:
                raise ValueError(f"invalid USDT/USD price range at {available.isoformat()}")
            if available in values and values[available] != closing:
                raise ValueError(f"conflicting USDT/USD candle at {available.isoformat()}")
            values[available] = closing
    return values


def _quote_marks(
    start: datetime,
    end: datetime,
    usd_cny: dict[date, Decimal],
    usdt_usd: dict[datetime, Decimal],
) -> list[QuoteMark]:
    observation_days = sorted(usd_cny)
    result = []
    for index in range((end - start).days):
        day = start + index * _DAY
        previous = bisect_left(observation_days, day.date()) - 1
        if previous < 0:
            raise ValueError(f"no prior ECB observation for {day.date()}")
        if day not in usdt_usd:
            raise ValueError(f"missing USDT/USD close at {day.isoformat()}")
        observed = observation_days[previous]
        result.append((_midnight(observed), day, usd_cny[observed] * usdt_usd[day]))
    return result


def _write_csv(path: Path, header: tuple[str, ...], rows: Iterable[SpotBar | QuoteMark]) -> None:
    with path.open("x", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(header)
        writer.writerows(rows)


def prepare_snapshot(raw_dir: Path, output: Path, year: int) -> None:
    """Normalize and validate both markets before publishing the input directory."""
    start = _midnight(date(year, 1, 1))
    end = _midnight(date(year + 1, 1, 1))
    if output.exists():
        raise FileExistsError(f"output directory already exists: {output}")
    rates = _quote_marks(start, end, _usd_cny(raw_dir / "ecb.csv"), _usdt_usd(raw_dir))
    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix=".plan04-", dir=output.parent) as temporary:
        stage = Path(temporary)
        rates_file = "quote-rates.csv"
        _write_csv(stage / rates_file, ("observed_at_utc", "available_at_utc", "quote_cny_rate"), rates)
        for symbol, step in _SYMBOL_STEPS.items():
            bars_file = f"{symbol}-{year}-bars.csv"
            bars = _spot_bars(raw_dir, symbol, year)
            _write_csv(stage / bars_file, ("open_time_utc", "close_time_utc", "open_price", "close_price"), bars)
            execution = ExecutionAssumptions(
                price_tick=Decimal("0.01"),
                quantity_step=step,
                min_notional=Decimal(5),
                conversion_fee_rate=Decimal("0.002"),
                slippage_rate=Decimal("0.0005"),
                strategy_monthly_cost_cny=Decimal(30),
            )
            manifest = {
                "symbol": symbol,
                "data_version": f"binance-{symbol.lower()}-4h-{year}-ecb-cb-v1",
                "start_utc": start.isoformat(),
                "end_utc": end.isoformat(),
                "bars_csv": bars_file,
                "quote_rates_csv": rates_file,
                "max_rate_age_hours": 168,
                "execution": {key: str(value) for key, value in asdict(execution).items()},
            }
            manifest_path = stage / f"{symbol}.json"
            manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
            load_historical_market(manifest_path)
        stage.rename(output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--year", type=int, required=True)
    parser.add_argument("--raw-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        prepare_snapshot(args.raw_dir, args.output_dir, args.year)
    except (OSError, ValueError, KeyError, BadZipFile) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
