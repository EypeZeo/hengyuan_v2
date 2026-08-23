"""Unit tests for py_core.market_data.warehouse_backfill (批次 1 PR-2).

fetch_binance_ohlcv() is mocked throughout -- these tests exercise backfill()'s own resume-
point/prefix-conflict/already-current logic, not the network fetch logic itself (covered
separately in test_binance_public_rest.py).
"""

from __future__ import annotations

from datetime import UTC, datetime, timedelta
from decimal import Decimal
from pathlib import Path
from unittest import mock

import pytest

from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)
from py_core.market_data import warehouse as wh
from py_core.market_data import warehouse_backfill as bf
from py_core.market_data.binance_public_rest import FetchMeta

_BTC = CanonicalMarketSymbol("BTCUSDT")
_1D = OhlcvTimeframe("1d")
_CRYPTO = ManualMarket.CRYPTO_SPOT
_BASE = datetime(2024, 1, 1, tzinfo=UTC)


def _rec(day: int, price: float = 100.0) -> NormalizedOhlcvRecord:
    return NormalizedOhlcvRecord(
        market=_CRYPTO,
        symbol=_BTC,
        timeframe=_1D,
        event_time_utc=_BASE + timedelta(days=day),
        open_price=Decimal(str(price)),
        high_price=Decimal(str(price + 1)),
        low_price=Decimal(str(price - 1)),
        close_price=Decimal(str(price + 0.5)),
        volume=Decimal(1000),
    )


def _meta(coverage_end: datetime, **overrides: object) -> FetchMeta:
    defaults: dict[str, object] = {
        "request_symbol": "BTCUSDT",
        "request_interval": "1d",
        "request_start_utc": _BASE,
        "request_end_utc": coverage_end,
        "coverage_end_utc": coverage_end,
        "retrieval_cutoff_utc": coverage_end + timedelta(days=1),
        "dropped_unclosed_bar_count": 0,
        "host": "data-api.binance.vision",
        "page_count": 1,
        "csv_sha256": "",
        "schema_version": 1,
        "response_weight_headers": {},
    }
    defaults.update(overrides)
    return FetchMeta(**defaults)


def test_fresh_identity_uses_start_utc_and_calls_fetch(tmp_path: Path) -> None:
    records = [_rec(i) for i in range(5)]
    with mock.patch.object(
        bf, "fetch_binance_ohlcv", return_value=(records, _meta(_BASE + timedelta(days=5)))
    ) as fake_fetch:
        report = bf.backfill(tmp_path, _CRYPTO, _BTC, _1D, _BASE, _BASE + timedelta(days=10))
    assert report.already_current is False
    assert report.fetched_records == 5
    assert report.resumed_from == _BASE
    fake_fetch.assert_called_once_with(
        "BTCUSDT", "1d", _BASE, _BASE + timedelta(days=10), market=_CRYPTO, max_requests=50
    )


def test_fresh_identity_rejects_off_grid_start_without_calling_fetch(tmp_path: Path) -> None:
    off_grid_start = _BASE + timedelta(hours=6)
    with mock.patch.object(bf, "fetch_binance_ohlcv") as fake_fetch, pytest.raises(wh.WarehouseInvalidTimestampError):
        bf.backfill(tmp_path, _CRYPTO, _BTC, _1D, off_grid_start, _BASE + timedelta(days=10))
    fake_fetch.assert_not_called()
    assert list(tmp_path.iterdir()) == []


def test_existing_coverage_resumes_from_tail_not_start_utc(tmp_path: Path) -> None:
    wh.write_records(tmp_path, [_rec(i) for i in range(5)])  # covered_end_utc = day 5

    records = [_rec(i) for i in range(5, 8)]
    with mock.patch.object(
        bf, "fetch_binance_ohlcv", return_value=(records, _meta(_BASE + timedelta(days=8)))
    ) as fake_fetch:
        report = bf.backfill(tmp_path, _CRYPTO, _BTC, _1D, _BASE, _BASE + timedelta(days=10))
    assert report.resumed_from == _BASE + timedelta(days=5)
    fake_fetch.assert_called_once_with(
        "BTCUSDT", "1d", _BASE + timedelta(days=5), _BASE + timedelta(days=10), market=_CRYPTO, max_requests=50
    )


def test_already_current_returns_zero_network_calls(tmp_path: Path) -> None:
    wh.write_records(tmp_path, [_rec(i) for i in range(5)])  # covered_end_utc = day 5

    with mock.patch.object(bf, "fetch_binance_ohlcv") as fake_fetch:
        report = bf.backfill(tmp_path, _CRYPTO, _BTC, _1D, _BASE, _BASE + timedelta(days=5))
    assert report.already_current is True
    assert report.fetched_records == 0
    assert report.fetch_coverage_end_utc is None
    assert report.write_report == wh.WriteReport(rows_written=0, rows_skipped_duplicate=0, partitions_touched=())
    fake_fetch.assert_not_called()


def test_prefix_conflict_rejected_without_calling_fetch_or_touching_disk(tmp_path: Path) -> None:
    wh.write_records(tmp_path, [_rec(i) for i in range(10, 15)])  # covered_start = day 10
    partition = wh.partition_path(tmp_path, _CRYPTO, _BTC, _1D, 2024, 1)
    original_bytes = partition.read_bytes()

    with mock.patch.object(bf, "fetch_binance_ohlcv") as fake_fetch, pytest.raises(bf.BackfillPrefixNotSupportedError):
        bf.backfill(tmp_path, _CRYPTO, _BTC, _1D, _BASE, _BASE + timedelta(days=20))
    fake_fetch.assert_not_called()
    assert partition.read_bytes() == original_bytes


def test_coverage_shortfall_from_unclosed_bars_surfaced_not_hidden(tmp_path: Path) -> None:
    records = [_rec(i) for i in range(3)]
    short_coverage_end = _BASE + timedelta(days=3)  # requested end is day 10, but fetch only got to day 3
    with mock.patch.object(bf, "fetch_binance_ohlcv", return_value=(records, _meta(short_coverage_end))):
        report = bf.backfill(tmp_path, _CRYPTO, _BTC, _1D, _BASE, _BASE + timedelta(days=10))
    assert report.already_current is False
    assert report.fetch_coverage_end_utc == short_coverage_end
    assert report.fetch_coverage_end_utc < _BASE + timedelta(days=10)


def test_fetch_exception_propagates_unchanged(tmp_path: Path) -> None:
    from py_core.market_data.binance_public_rest import BinanceRateLimitedError

    with (
        mock.patch.object(
            bf, "fetch_binance_ohlcv", side_effect=BinanceRateLimitedError("rate limited", status_code=429)
        ),
        pytest.raises(BinanceRateLimitedError),
    ):
        bf.backfill(tmp_path, _CRYPTO, _BTC, _1D, _BASE, _BASE + timedelta(days=10))


def test_write_records_exception_propagates_unchanged(tmp_path: Path) -> None:
    records = [_rec(i) for i in range(3)]
    with (
        mock.patch.object(bf, "fetch_binance_ohlcv", return_value=(records, _meta(_BASE + timedelta(days=3)))),
        mock.patch.object(wh, "write_records", side_effect=wh.WarehouseBusyError("simulated")),
        pytest.raises(wh.WarehouseBusyError),
    ):
        bf.backfill(tmp_path, _CRYPTO, _BTC, _1D, _BASE, _BASE + timedelta(days=10))
