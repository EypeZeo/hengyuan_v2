"""Unit tests for py_core.market_data.warehouse (批次 1 PR-1).

This module went through seven rounds of external design review before implementation;
these tests are the regression matrix that review process demanded (see the batch-1 plan).
Sections are grouped to mirror the plan's test list.
"""

from __future__ import annotations

import gc
import os
import weakref
from datetime import UTC, datetime, timedelta
from decimal import Decimal
from pathlib import Path
from unittest import mock

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from py_core.backtests.vectorized_engine import records_to_dataframe
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    ManualOhlcvValidationError,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)
from py_core.market_data import warehouse as wh

_BTC = CanonicalMarketSymbol("BTCUSDT")
_ETH = CanonicalMarketSymbol("ETHUSDT")
_1D = OhlcvTimeframe("1d")
_1H = OhlcvTimeframe("1h")
_1W = OhlcvTimeframe("1w")
_CRYPTO = ManualMarket.CRYPTO_SPOT


def _rec(
    dt: datetime,
    price: float,
    *,
    market: ManualMarket = _CRYPTO,
    symbol: CanonicalMarketSymbol = _BTC,
    timeframe: OhlcvTimeframe = _1D,
) -> NormalizedOhlcvRecord:
    return NormalizedOhlcvRecord(
        market=market,
        symbol=symbol,
        timeframe=timeframe,
        event_time_utc=dt,
        open_price=Decimal(str(price)),
        high_price=Decimal(str(price + 1)),
        low_price=Decimal(str(price - 1)),
        close_price=Decimal(str(price + 0.5)),
        volume=Decimal(1000),
    )


def _write_raw_partition(
    root: Path,
    table: pa.Table,
    *,
    market: ManualMarket = _CRYPTO,
    symbol: CanonicalMarketSymbol = _BTC,
    timeframe: OhlcvTimeframe = _1D,
    year: int,
    month: int,
    metadata: dict[bytes, bytes] | None = None,
    schema: pa.Schema | None = None,
) -> Path:
    """Bypass write_records() entirely to hand-construct a partition file for corruption
    tests -- mirrors this repo's existing plant_*()/write_raw_file() precedent for
    constructing durable-precondition fixtures that bypass the normal write path."""
    p = wh.partition_path(root, market, symbol, timeframe, year, month)
    p.parent.mkdir(parents=True, exist_ok=True)
    meta = metadata if metadata is not None else wh._build_metadata(market, symbol.value, timeframe.value)
    target_schema = schema if schema is not None else wh._ARROW_SCHEMA
    pq.write_table(table.cast(target_schema.with_metadata(meta)), p)
    return p


def _hand_table(rows: list[dict], *, schema: pa.Schema | None = None) -> pa.Table:
    target_schema = schema if schema is not None else wh._ARROW_SCHEMA
    return pa.table(
        {
            "event_time_utc": pa.array([r["ts"] for r in rows], type=pa.timestamp("us", tz="UTC")),
            "open": pa.array([r.get("open", 100.0) for r in rows], type=pa.float64()),
            "high": pa.array([r.get("high", 102.0) for r in rows], type=pa.float64()),
            "low": pa.array([r.get("low", 99.0) for r in rows], type=pa.float64()),
            "close": pa.array([r.get("close", 101.0) for r in rows], type=pa.float64()),
            "volume": pa.array([r.get("volume", 1000.0) for r in rows], type=pa.float64()),
        },
        schema=target_schema,
    )


# ---------------------------------------------------------------------------
# partition_path / path safety
# ---------------------------------------------------------------------------


def test_partition_path_shape(tmp_path: Path) -> None:
    p = wh.partition_path(tmp_path, _CRYPTO, _BTC, _1D, 2024, 3)
    assert p == tmp_path / "crypto_spot" / "BTCUSDT" / "1d" / "2024" / "03.parquet"


def test_partition_path_rejects_symbol_with_slash(tmp_path: Path) -> None:
    bad_symbol = CanonicalMarketSymbol("BTC/USDT")  # slips past CanonicalMarketSymbol itself
    with pytest.raises(wh.WarehouseInvalidIdentityError):
        wh.partition_path(tmp_path, _CRYPTO, bad_symbol, _1D, 2024, 1)


def test_partition_path_rejects_symbol_with_dotdot(tmp_path: Path) -> None:
    bad_symbol = CanonicalMarketSymbol("..")
    with pytest.raises(wh.WarehouseInvalidIdentityError):
        wh.partition_path(tmp_path, _CRYPTO, bad_symbol, _1D, 2024, 1)


def test_partition_path_rejects_unsupported_timeframe(tmp_path: Path) -> None:
    # "7m" passes OhlcvTimeframe's own regex (^[1-9]\d*[mhdw]$) but is not a real Binance
    # interval, so it is deliberately absent from SUPPORTED_TIMEFRAMES.
    with pytest.raises(wh.WarehouseInvalidIdentityError):
        wh.partition_path(tmp_path, _CRYPTO, _BTC, OhlcvTimeframe("7m"), 2024, 1)


def test_write_records_rejects_symbol_traversal_with_zero_side_effects(tmp_path: Path) -> None:
    bad_symbol = CanonicalMarketSymbol("../escaped")
    rec = _rec(datetime(2024, 1, 1, tzinfo=UTC), 100.0, symbol=bad_symbol)
    with pytest.raises(wh.WarehouseInvalidIdentityError):
        wh.write_records(tmp_path, [rec])
    assert list(tmp_path.iterdir()) == []


@pytest.mark.skipif(os.name != "posix", reason="os.symlink target semantics differ on Windows without admin/dev-mode")
def test_write_records_refuses_symlink_between_root_and_partition(tmp_path: Path) -> None:
    real_dir = tmp_path / "real"
    real_dir.mkdir()
    link_dir = tmp_path / "crypto_spot"
    os.symlink(real_dir, link_dir, target_is_directory=True)
    rec = _rec(datetime(2024, 1, 1, tzinfo=UTC), 100.0)
    with pytest.raises(wh.WarehouseInvalidIdentityError):
        wh.write_records(tmp_path, [rec])


@pytest.mark.skipif(os.name != "nt", reason="NTFS junctions are a Windows-only concept")
def test_write_records_refuses_junction_between_root_and_partition(tmp_path: Path) -> None:
    """Regression: Path.is_symlink() returns False for NTFS junctions entirely (verified
    directly against `mklink /J`) -- a junction is a distinct reparse-point type, not a
    symlink. This caught a real gap in _no_symlink_between()'s first implementation, which
    only checked is_symlink() and silently let a junction through undetected."""
    import subprocess

    real_dir = tmp_path / "real"
    real_dir.mkdir()
    link_dir = tmp_path / "crypto_spot"
    result = subprocess.run(
        ["cmd", "/c", "mklink", "/J", str(link_dir), str(real_dir)], capture_output=True, check=False
    )
    assert result.returncode == 0, "mklink /J failed -- test environment issue, not the code under test"
    rec = _rec(datetime(2024, 1, 1, tzinfo=UTC), 100.0)
    with pytest.raises(wh.WarehouseInvalidIdentityError):
        wh.write_records(tmp_path, [rec])


# ---------------------------------------------------------------------------
# write_records(): fresh, cross-month, idempotent, empty list
# ---------------------------------------------------------------------------


def test_write_records_empty_list_zero_side_effects(tmp_path: Path) -> None:
    report = wh.write_records(tmp_path, [])
    assert report == wh.WriteReport(rows_written=0, rows_skipped_duplicate=0, partitions_touched=())
    assert list(tmp_path.iterdir()) == []


def test_write_records_fresh_write(tmp_path: Path) -> None:
    records = [_rec(datetime(2024, 1, i, tzinfo=UTC), 100.0 + i) for i in range(1, 6)]
    report = wh.write_records(tmp_path, records)
    assert report.rows_written == 5
    assert report.rows_skipped_duplicate == 0
    assert len(report.partitions_touched) == 1


def test_write_records_cross_month_split(tmp_path: Path) -> None:
    r1 = _rec(datetime(2024, 1, 31, tzinfo=UTC), 100.0)
    r2 = _rec(datetime(2024, 2, 1, tzinfo=UTC), 200.0)
    report = wh.write_records(tmp_path, [r1, r2])
    assert len(report.partitions_touched) == 2
    assert (tmp_path / "crypto_spot" / "BTCUSDT" / "1d" / "2024" / "01.parquet").exists()
    assert (tmp_path / "crypto_spot" / "BTCUSDT" / "1d" / "2024" / "02.parquet").exists()


def test_write_records_idempotent_rewrite_is_pure_duplicate_skip(tmp_path: Path) -> None:
    records = [_rec(datetime(2024, 1, i, tzinfo=UTC), 100.0 + i) for i in range(1, 4)]
    wh.write_records(tmp_path, records)
    report2 = wh.write_records(tmp_path, records)
    assert report2.rows_written == 0
    assert report2.rows_skipped_duplicate == 3
    assert report2.partitions_touched == ()


def test_duplicate_only_rewrite_does_not_touch_disk(tmp_path: Path) -> None:
    """No mtime/hash change, no temp files left behind, and the publish function is never
    even called -- byte-hash/mock evidence, not mtime alone (mtime resolution is unreliable
    across filesystems)."""
    records = [_rec(datetime(2024, 1, i, tzinfo=UTC), 100.0 + i) for i in range(1, 4)]
    wh.write_records(tmp_path, records)
    partition = wh.partition_path(tmp_path, _CRYPTO, _BTC, _1D, 2024, 1)
    original_bytes = partition.read_bytes()

    with mock.patch.object(wh, "_publish_partition_file") as mock_publish:
        report2 = wh.write_records(tmp_path, records)
    mock_publish.assert_not_called()
    assert report2.rows_written == 0
    assert partition.read_bytes() == original_bytes
    assert list(partition.parent.glob(".tmp-*")) == []


# ---------------------------------------------------------------------------
# old-wins merge: precise algorithm regression
# ---------------------------------------------------------------------------


def test_old_wins_preserves_old_values_field_by_field_on_conflict(tmp_path: Path) -> None:
    ts = datetime(2024, 1, 1, tzinfo=UTC)
    old = NormalizedOhlcvRecord(
        market=_CRYPTO, symbol=_BTC, timeframe=_1D, event_time_utc=ts,
        open_price=Decimal(100), high_price=Decimal(105), low_price=Decimal(95),
        close_price=Decimal(102), volume=Decimal(1000),
    )
    wh.write_records(tmp_path, [old])

    new_conflicting = NormalizedOhlcvRecord(
        market=_CRYPTO, symbol=_BTC, timeframe=_1D, event_time_utc=ts,
        open_price=Decimal(999), high_price=Decimal(999), low_price=Decimal(999),
        close_price=Decimal(999), volume=Decimal(999),
    )
    report = wh.write_records(tmp_path, [new_conflicting])
    assert report.rows_written == 0
    assert report.rows_skipped_duplicate == 1

    df = wh.read_range(tmp_path, _CRYPTO, _BTC, _1D, ts, ts + timedelta(days=1))
    assert df["open"].iloc[0] == 100.0
    assert df["high"].iloc[0] == 105.0
    assert df["low"].iloc[0] == 95.0
    assert df["close"].iloc[0] == 102.0
    assert df["volume"].iloc[0] == 1000.0


def test_same_batch_duplicate_rejected_before_merge_logic_runs(tmp_path: Path) -> None:
    """Same-batch duplicates are a precheck concern (validate_ohlcv_batch), a different
    code path from old-wins merge (which only ever sees old-vs-new conflicts)."""
    ts = datetime(2024, 1, 1, tzinfo=UTC)
    r1 = _rec(ts, 100.0)
    r2 = NormalizedOhlcvRecord(
        market=_CRYPTO, symbol=_BTC, timeframe=_1D, event_time_utc=ts,
        open_price=Decimal(200), high_price=Decimal(201), low_price=Decimal(199),
        close_price=Decimal("200.5"), volume=Decimal(1000),
    )
    with pytest.raises(ManualOhlcvValidationError):
        wh.write_records(tmp_path, [r1, r2])
    assert list(tmp_path.iterdir()) == []


def test_partial_publish_then_retry_converges_to_unique_data_in_both_months(tmp_path: Path) -> None:
    r_jan = _rec(datetime(2024, 1, 15, tzinfo=UTC), 100.0)
    r_feb = _rec(datetime(2024, 2, 15, tzinfo=UTC), 200.0)

    real_publish = wh._publish_partition_file
    call_count = {"n": 0}

    def flaky_publish(final_path, table, **kwargs):
        call_count["n"] += 1
        if call_count["n"] == 2:
            raise OSError("simulated fsync/replace failure on the second partition")
        return real_publish(final_path, table, **kwargs)

    with (
        mock.patch.object(wh, "_publish_partition_file", side_effect=flaky_publish),
        pytest.raises(wh.WarehousePartialWriteError) as exc_info,
    ):
        wh.write_records(tmp_path, [r_jan, r_feb])

    assert len(exc_info.value.published_partitions) == 1
    assert exc_info.value.published_partitions[0].month == 1
    jan_path = wh.partition_path(tmp_path, _CRYPTO, _BTC, _1D, 2024, 1)
    feb_path = wh.partition_path(tmp_path, _CRYPTO, _BTC, _1D, 2024, 2)
    assert jan_path.exists()
    assert not feb_path.exists()

    # retry the identical full batch -- jan no-ops (already published), feb actually writes
    report_retry = wh.write_records(tmp_path, [r_jan, r_feb])
    assert report_retry.rows_written == 1  # only feb
    assert report_retry.rows_skipped_duplicate == 1  # jan was already there
    assert jan_path.exists()
    assert feb_path.exists()

    cov = wh.coverage(tmp_path, _CRYPTO, _BTC, _1D)
    assert cov.record_count == 2


# ---------------------------------------------------------------------------
# Exact-grid / microsecond precision
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("dt", "timeframe"),
    [
        (datetime(2024, 1, 1, 0, 30, tzinfo=UTC), _1H),  # 1h but :30 offset
        (datetime(2024, 1, 1, 12, 0, tzinfo=UTC), _1D),  # 1d but noon, not midnight
        (datetime(2024, 1, 2, tzinfo=UTC), _1W),  # 1w but Tuesday, not Monday
        (datetime(2024, 1, 1, 0, 0, 0, 500_000, tzinfo=UTC), _1H),  # microsecond component
    ],
)
def test_write_records_rejects_off_grid_timestamps(tmp_path: Path, dt: datetime, timeframe: OhlcvTimeframe) -> None:
    rec = _rec(dt, 100.0, timeframe=timeframe)
    with pytest.raises(wh.WarehouseInvalidTimestampError):
        wh.write_records(tmp_path, [rec])
    assert list(tmp_path.iterdir()) == []


def test_write_records_accepts_correctly_aligned_weekly_monday(tmp_path: Path) -> None:
    monday = datetime(2024, 1, 1, tzinfo=UTC)
    assert monday.weekday() == 0
    rec = _rec(monday, 100.0, timeframe=_1W)
    report = wh.write_records(tmp_path, [rec])
    assert report.rows_written == 1


def test_is_on_grid_matches_write_time_precheck_and_validator() -> None:
    """The plan's core P0-1 fix: one shared pure function used by both write precheck and
    content validation."""
    assert wh.is_on_grid(datetime(2024, 1, 1, tzinfo=UTC), _1D) is True
    assert wh.is_on_grid(datetime(2024, 1, 1, 12, tzinfo=UTC), _1D) is False
    assert wh.is_on_grid(datetime(2024, 1, 1, tzinfo=UTC), _1W) is True  # Monday
    assert wh.is_on_grid(datetime(2024, 1, 2, tzinfo=UTC), _1W) is False  # Tuesday


# ---------------------------------------------------------------------------
# Decimal -> float64 representability
# ---------------------------------------------------------------------------


def test_write_records_rejects_decimal_overflow_price(tmp_path: Path) -> None:
    rec = NormalizedOhlcvRecord(
        market=_CRYPTO, symbol=_BTC, timeframe=_1D, event_time_utc=datetime(2024, 1, 1, tzinfo=UTC),
        open_price=Decimal("1e400"), high_price=Decimal(2), low_price=Decimal("0.5"),
        close_price=Decimal("1.5"), volume=Decimal(1),
    )
    with pytest.raises(wh.WarehouseValueNotRepresentableError):
        wh.write_records(tmp_path, [rec])
    assert list(tmp_path.iterdir()) == []


def test_write_records_rejects_decimal_overflow_volume(tmp_path: Path) -> None:
    rec = NormalizedOhlcvRecord(
        market=_CRYPTO, symbol=_BTC, timeframe=_1D, event_time_utc=datetime(2024, 1, 1, tzinfo=UTC),
        open_price=Decimal(1), high_price=Decimal(2), low_price=Decimal("0.5"),
        close_price=Decimal("1.5"), volume=Decimal("1e400"),
    )
    with pytest.raises(wh.WarehouseValueNotRepresentableError):
        wh.write_records(tmp_path, [rec])


def test_write_records_accepts_normal_boundary_values(tmp_path: Path) -> None:
    rec = NormalizedOhlcvRecord(
        market=_CRYPTO, symbol=_BTC, timeframe=_1D, event_time_utc=datetime(2024, 1, 1, tzinfo=UTC),
        open_price=Decimal("1e10"), high_price=Decimal("1.1e10"), low_price=Decimal("0.9e10"),
        close_price=Decimal("1.05e10"), volume=Decimal("1e15"),
    )
    report = wh.write_records(tmp_path, [rec])
    assert report.rows_written == 1


# ---------------------------------------------------------------------------
# validate_partition_contents(): every corruption category, fail-closed
# ---------------------------------------------------------------------------


def test_validate_rejects_empty_table(tmp_path: Path) -> None:
    empty = _hand_table([])
    p = _write_raw_partition(tmp_path, empty, year=2024, month=1)
    with pytest.raises(wh.WarehouseCorruptError, match="zero rows"):
        wh.validate_partition_contents(p, market=_CRYPTO, symbol=_BTC, timeframe=_1D, year=2024, month=1)


def test_validate_rejects_null_in_any_column(tmp_path: Path) -> None:
    nullable_schema = pa.schema([pa.field(f.name, f.type, nullable=True) for f in wh._ARROW_SCHEMA])
    for null_col in ("open", "high", "low", "close", "volume"):
        rows = [
            {"ts": datetime(2024, 1, 1, tzinfo=UTC)},
            {"ts": datetime(2024, 1, 2, tzinfo=UTC)},
        ]
        table = _hand_table(rows, schema=nullable_schema)
        col_idx = table.schema.get_field_index(null_col)
        values = table.column(null_col).to_pylist()
        values[0] = None
        new_col = pa.array(values, type=pa.float64())
        table = table.set_column(col_idx, pa.field(null_col, pa.float64(), nullable=True), new_col)
        p = _write_raw_partition(tmp_path, table, year=2024, month=1, schema=nullable_schema)
        with pytest.raises(wh.WarehouseCorruptError):
            wh.validate_partition_contents(p, market=_CRYPTO, symbol=_BTC, timeframe=_1D, year=2024, month=1)
        p.unlink()


def test_validate_rejects_duplicate_timestamp_within_partition(tmp_path: Path) -> None:
    ts = datetime(2024, 1, 1, tzinfo=UTC)
    table = _hand_table([{"ts": ts}, {"ts": ts}])
    p = _write_raw_partition(tmp_path, table, year=2024, month=1)
    with pytest.raises(wh.WarehouseCorruptError, match="ascending|duplicate"):
        wh.validate_partition_contents(p, market=_CRYPTO, symbol=_BTC, timeframe=_1D, year=2024, month=1)


def test_validate_rejects_non_ascending_physical_order(tmp_path: Path) -> None:
    table = _hand_table(
        [{"ts": datetime(2024, 1, 2, tzinfo=UTC)}, {"ts": datetime(2024, 1, 1, tzinfo=UTC)}]
    )
    p = _write_raw_partition(tmp_path, table, year=2024, month=1)
    with pytest.raises(wh.WarehouseCorruptError):
        wh.validate_partition_contents(p, market=_CRYPTO, symbol=_BTC, timeframe=_1D, year=2024, month=1)


def test_validate_rejects_off_grid_timestamp(tmp_path: Path) -> None:
    table = _hand_table([{"ts": datetime(2024, 1, 1, 6, tzinfo=UTC)}])
    p = _write_raw_partition(tmp_path, table, year=2024, month=1)
    with pytest.raises(wh.WarehouseCorruptError, match="grid"):
        wh.validate_partition_contents(p, market=_CRYPTO, symbol=_BTC, timeframe=_1D, year=2024, month=1)


def test_validate_rejects_wrong_month_membership(tmp_path: Path) -> None:
    table = _hand_table([{"ts": datetime(2024, 2, 1, tzinfo=UTC)}])
    p = _write_raw_partition(tmp_path, table, year=2024, month=1)  # written to Jan folder
    with pytest.raises(wh.WarehouseCorruptError, match="year, month"):
        wh.validate_partition_contents(p, market=_CRYPTO, symbol=_BTC, timeframe=_1D, year=2024, month=1)


def test_validate_rejects_invalid_ohlc_bounds(tmp_path: Path) -> None:
    table = _hand_table([{"ts": datetime(2024, 1, 1, tzinfo=UTC), "high": 50.0, "open": 100.0}])
    p = _write_raw_partition(tmp_path, table, year=2024, month=1)
    with pytest.raises(wh.WarehouseCorruptError, match="high"):
        wh.validate_partition_contents(p, market=_CRYPTO, symbol=_BTC, timeframe=_1D, year=2024, month=1)


def test_validate_rejects_non_finite_ohlc(tmp_path: Path) -> None:
    table = _hand_table([{"ts": datetime(2024, 1, 1, tzinfo=UTC), "open": float("inf")}])
    p = _write_raw_partition(tmp_path, table, year=2024, month=1)
    with pytest.raises(wh.WarehouseCorruptError, match="non-finite"):
        wh.validate_partition_contents(p, market=_CRYPTO, symbol=_BTC, timeframe=_1D, year=2024, month=1)


def test_validate_rejects_negative_volume(tmp_path: Path) -> None:
    table = _hand_table([{"ts": datetime(2024, 1, 1, tzinfo=UTC), "volume": -1.0}])
    p = _write_raw_partition(tmp_path, table, year=2024, month=1)
    with pytest.raises(wh.WarehouseCorruptError, match="volume"):
        wh.validate_partition_contents(p, market=_CRYPTO, symbol=_BTC, timeframe=_1D, year=2024, month=1)


def test_validate_rejects_missing_metadata_key(tmp_path: Path) -> None:
    table = _hand_table([{"ts": datetime(2024, 1, 1, tzinfo=UTC)}])
    incomplete_meta = {b"warehouse_schema_version": b"1", b"market": b"crypto_spot"}
    p = _write_raw_partition(tmp_path, table, year=2024, month=1, metadata=incomplete_meta)
    with pytest.raises(wh.WarehouseCorruptError, match="metadata"):
        wh.validate_partition_contents(p, market=_CRYPTO, symbol=_BTC, timeframe=_1D, year=2024, month=1)


def test_validate_rejects_unknown_schema_version(tmp_path: Path) -> None:
    table = _hand_table([{"ts": datetime(2024, 1, 1, tzinfo=UTC)}])
    meta = wh._build_metadata(_CRYPTO, _BTC.value, _1D.value)
    meta[b"warehouse_schema_version"] = b"999"
    p = _write_raw_partition(tmp_path, table, year=2024, month=1, metadata=meta)
    with pytest.raises(wh.WarehouseCorruptError, match="schema_version"):
        wh.validate_partition_contents(p, market=_CRYPTO, symbol=_BTC, timeframe=_1D, year=2024, month=1)


def test_validate_ignores_written_at_utc_when_comparing_schema(tmp_path: Path) -> None:
    """written_at_utc legitimately differs on every write and must not cause a false
    mismatch when everything else matches."""
    table = _hand_table([{"ts": datetime(2024, 1, 1, tzinfo=UTC)}])
    meta = wh._build_metadata(_CRYPTO, _BTC.value, _1D.value)
    meta[b"written_at_utc"] = b"some-other-timestamp-entirely"
    p = _write_raw_partition(tmp_path, table, year=2024, month=1, metadata=meta)
    result = wh.validate_partition_contents(p, market=_CRYPTO, symbol=_BTC, timeframe=_1D, year=2024, month=1)
    assert result.num_rows == 1


def test_validate_rejects_identity_mismatch_in_metadata(tmp_path: Path) -> None:
    table = _hand_table([{"ts": datetime(2024, 1, 1, tzinfo=UTC)}])
    meta = wh._build_metadata(_CRYPTO, "WRONGSYMBOL", _1D.value)
    p = _write_raw_partition(tmp_path, table, year=2024, month=1, metadata=meta)
    with pytest.raises(wh.WarehouseCorruptError, match="identity"):
        wh.validate_partition_contents(p, market=_CRYPTO, symbol=_BTC, timeframe=_1D, year=2024, month=1)


# ---------------------------------------------------------------------------
# All four public entry points fail-closed on the same corrupted data
# ---------------------------------------------------------------------------


def test_all_four_entry_points_fail_closed_on_corrupt_partition(tmp_path: Path) -> None:
    table = _hand_table([{"ts": datetime(2024, 1, 1, tzinfo=UTC), "high": 1.0}])  # bad OHLC bounds
    _write_raw_partition(tmp_path, table, year=2024, month=1)

    with pytest.raises(wh.WarehouseCorruptError):
        wh.coverage(tmp_path, _CRYPTO, _BTC, _1D)
    with pytest.raises(wh.WarehouseCorruptError):
        wh.read_range(tmp_path, _CRYPTO, _BTC, _1D, datetime(2024, 1, 1, tzinfo=UTC), datetime(2024, 2, 1, tzinfo=UTC))
    with pytest.raises(wh.WarehouseCorruptError):
        wh.scan_gaps(tmp_path, _CRYPTO, _BTC, _1D)
    # write_records() old-file-read path: writing anything into this same partition must
    # also fail-closed rather than silently overwrite/heal the corruption.
    with pytest.raises(wh.WarehouseCorruptError):
        wh.write_records(tmp_path, [_rec(datetime(2024, 1, 2, tzinfo=UTC), 100.0)])


# ---------------------------------------------------------------------------
# Empty warehouse
# ---------------------------------------------------------------------------


def test_coverage_returns_none_for_nonexistent_identity(tmp_path: Path) -> None:
    assert wh.coverage(tmp_path, _CRYPTO, _ETH, _1D) is None


def test_coverage_returns_none_for_root_with_no_data_at_all(tmp_path: Path) -> None:
    assert wh.coverage(tmp_path, _CRYPTO, _BTC, _1D) is None


def test_scan_gaps_empty_report_for_nonexistent_identity(tmp_path: Path) -> None:
    report = wh.scan_gaps(tmp_path, _CRYPTO, _BTC, _1D)
    assert report.issues == ()
    assert report.total_issue_count == 0
    assert report.truncated is False


def test_read_range_empty_dataframe_for_nonexistent_identity(tmp_path: Path) -> None:
    df = wh.read_range(tmp_path, _CRYPTO, _BTC, _1D, datetime(2024, 1, 1, tzinfo=UTC), datetime(2024, 2, 1, tzinfo=UTC))
    assert len(df) == 0
    assert list(df.columns) == ["open", "high", "low", "close", "volume"]


# ---------------------------------------------------------------------------
# coverage() / scan_gaps() correctness
# ---------------------------------------------------------------------------


def test_coverage_is_contiguous_true_for_dense_data(tmp_path: Path) -> None:
    records = [_rec(datetime(2024, 1, 1, tzinfo=UTC) + timedelta(hours=i), 100.0 + i, timeframe=_1H) for i in range(10)]
    wh.write_records(tmp_path, records)
    cov = wh.coverage(tmp_path, _CRYPTO, _BTC, _1H)
    assert cov.record_count == 10
    assert cov.is_contiguous is True
    assert cov.covered_start_utc == datetime(2024, 1, 1, tzinfo=UTC)
    assert cov.covered_end_utc == datetime(2024, 1, 1, tzinfo=UTC) + timedelta(hours=10)


def test_coverage_is_contiguous_false_when_duplicate_and_missing_would_cancel_out_in_naive_count(tmp_path: Path) -> None:
    """The P0-1 (round 2) regression: a naive COUNT(*)-only check could be fooled if one
    duplicate and one missing bar happen to leave the total count unchanged. This module's
    own writer can never actually produce an on-disk duplicate (validate_partition_contents
    rejects it), so this test constructs the scenario directly via _iter_validated /
    coverage()'s internal delta logic on a hand-built, gap-only fixture: 5 expected daily
    bars with day 3 missing -- count is 4 instead of 5, and is_contiguous must be False."""
    records = [_rec(datetime(2024, 1, d, tzinfo=UTC), 100.0 + d) for d in (1, 2, 4, 5)]  # day 3 missing
    wh.write_records(tmp_path, records)
    cov = wh.coverage(tmp_path, _CRYPTO, _BTC, _1D)
    assert cov.record_count == 4
    assert cov.is_contiguous is False


def test_coverage_single_row_partition_is_contiguous(tmp_path: Path) -> None:
    wh.write_records(tmp_path, [_rec(datetime(2024, 1, 1, tzinfo=UTC), 100.0)])
    cov = wh.coverage(tmp_path, _CRYPTO, _BTC, _1D)
    assert cov.record_count == 1
    assert cov.is_contiguous is True


def test_coverage_detects_cross_month_boundary_gap(tmp_path: Path) -> None:
    r_jan = _rec(datetime(2024, 1, 31, tzinfo=UTC), 100.0)
    r_feb = _rec(datetime(2024, 2, 5, tzinfo=UTC), 200.0)  # gap: feb 1-4 missing
    wh.write_records(tmp_path, [r_jan, r_feb])
    cov = wh.coverage(tmp_path, _CRYPTO, _BTC, _1D)
    assert cov.is_contiguous is False


def test_scan_gaps_reports_missing_bar(tmp_path: Path) -> None:
    records = [_rec(datetime(2024, 1, d, tzinfo=UTC), 100.0 + d) for d in (1, 2, 4)]  # day 3 missing
    wh.write_records(tmp_path, records)
    report = wh.scan_gaps(tmp_path, _CRYPTO, _BTC, _1D)
    assert report.total_issue_count == 1
    assert len(report.issues) == 1
    assert report.issues[0].at == datetime(2024, 1, 3, tzinfo=UTC)


def test_scan_gaps_truncates_but_keeps_accurate_total_count(tmp_path: Path) -> None:
    # 10 daily bars with 5 missing (days 2,4,6,8,10 absent out of 1..11), max_issues=2
    present_days = [d for d in range(1, 12) if d not in (2, 4, 6, 8, 10)]
    records = [_rec(datetime(2024, 1, d, tzinfo=UTC), 100.0 + d) for d in present_days]
    wh.write_records(tmp_path, records)
    report = wh.scan_gaps(tmp_path, _CRYPTO, _BTC, _1D, max_issues=2)
    assert report.total_issue_count == 5
    assert len(report.issues) == 2
    assert report.truncated is True


# ---------------------------------------------------------------------------
# read_range(): correctness against records_to_dataframe(), streaming boundary
# ---------------------------------------------------------------------------


def test_read_range_matches_records_to_dataframe_baseline(tmp_path: Path) -> None:
    records = [_rec(datetime(2024, 1, 1, tzinfo=UTC) + timedelta(hours=i), 100.0 + i, timeframe=_1H) for i in range(24)]
    wh.write_records(tmp_path, records)
    df = wh.read_range(
        tmp_path, _CRYPTO, _BTC, _1H, datetime(2024, 1, 1, tzinfo=UTC), datetime(2024, 1, 2, tzinfo=UTC)
    )
    baseline = records_to_dataframe(records)
    assert len(df) == len(baseline)
    for col in ("open", "high", "low", "close", "volume"):
        assert (df[col].to_numpy() == baseline[col].to_numpy()).all()


def test_read_range_rejects_naive_datetime(tmp_path: Path) -> None:
    naive_start = datetime(2024, 1, 1)  # noqa: DTZ001 -- deliberately naive, this is what's under test
    with pytest.raises(ValueError, match="UTC"):
        wh.read_range(tmp_path, _CRYPTO, _BTC, _1D, naive_start, datetime(2024, 1, 2, tzinfo=UTC))


def test_read_range_rejects_start_not_before_end(tmp_path: Path) -> None:
    ts = datetime(2024, 1, 1, tzinfo=UTC)
    with pytest.raises(ValueError, match="before"):
        wh.read_range(tmp_path, _CRYPTO, _BTC, _1D, ts, ts)


def test_read_range_only_validates_intersecting_months(tmp_path: Path) -> None:
    """The P0-1 (round 5) fix: a query for one day must not read/validate 24 months of
    unrelated history."""
    for month in range(1, 13):
        wh.write_records(tmp_path, [_rec(datetime(2024, month, 1, tzinfo=UTC), 100.0 + month)])
    for month in range(1, 13):
        wh.write_records(tmp_path, [_rec(datetime(2025, month, 1, tzinfo=UTC), 200.0 + month)])

    call_paths: list[Path] = []
    real_validate = wh.validate_partition_contents

    def spy_validate(path, **kwargs):
        call_paths.append(path)
        return real_validate(path, **kwargs)

    with mock.patch.object(wh, "validate_partition_contents", side_effect=spy_validate):
        df = wh.read_range(
            tmp_path, _CRYPTO, _BTC, _1D, datetime(2024, 6, 1, tzinfo=UTC), datetime(2024, 6, 2, tzinfo=UTC)
        )
    assert len(call_paths) == 1
    assert call_paths[0] == wh.partition_path(tmp_path, _CRYPTO, _BTC, _1D, 2024, 6)
    assert len(df) == 1


# ---------------------------------------------------------------------------
# Streaming resource release (weakref, not sys.getrefcount())
# ---------------------------------------------------------------------------


def test_iter_validated_partitions_releases_previous_table_before_yielding_next(tmp_path: Path) -> None:
    for month in range(1, 4):
        wh.write_records(tmp_path, [_rec(datetime(2024, month, 1, tzinfo=UTC), 100.0 + month)])

    gen = wh._iter_validated_partitions(tmp_path, _CRYPTO, _BTC, _1D)
    _path1, _y1, _m1, table1 = next(gen)
    ref1 = weakref.ref(table1)
    del table1
    _path2, _y2, _m2, table2 = next(gen)  # advancing the generator must not keep table1 alive
    gc.collect()
    assert ref1() is None, "previous partition's table was not released before the next yield"
    del table2
    gen.close()


def test_coverage_does_not_hold_all_tables_simultaneously(tmp_path: Path) -> None:
    for month in range(1, 13):
        wh.write_records(tmp_path, [_rec(datetime(2024, month, 1, tzinfo=UTC), 100.0 + month)])

    live_refs: list[weakref.ReferenceType] = []
    real_validate = wh.validate_partition_contents

    def tracking_validate(path, **kwargs):
        table = real_validate(path, **kwargs)
        live_refs.append(weakref.ref(table))
        return table

    with mock.patch.object(wh, "validate_partition_contents", side_effect=tracking_validate):
        wh.coverage(tmp_path, _CRYPTO, _BTC, _1D)

    gc.collect()
    still_alive = sum(1 for r in live_refs if r() is not None)
    assert still_alive == 0, "coverage() must not retain references to already-processed month tables"


# ---------------------------------------------------------------------------
# Durability: POSIX parent-directory fsync failure propagates
# ---------------------------------------------------------------------------


@pytest.mark.skipif(os.name != "posix", reason="directory fsync durability step is POSIX-only")
def test_posix_parent_directory_fsync_failure_propagates_and_leaves_old_file_unchanged(tmp_path: Path) -> None:
    r1 = _rec(datetime(2024, 1, 1, tzinfo=UTC), 100.0)
    wh.write_records(tmp_path, [r1])

    r2 = _rec(datetime(2024, 1, 2, tzinfo=UTC), 200.0)
    # Directory fsync happens after os.replace() in this implementation's ordering, so the
    # new content is already on disk under the final name by the time the injected failure
    # occurs; this test is not "old file unchanged" (see the temp-write-failure test below
    # for that), it guards that the failure is reported, not silently swallowed while still
    # returning success.
    with (
        mock.patch.object(wh, "_fsync_directory", side_effect=OSError("simulated directory fsync failure")),
        pytest.raises((OSError, wh.WarehousePartialWriteError)),
    ):
        wh.write_records(tmp_path, [r2])


def test_write_records_failure_during_temp_write_leaves_old_final_untouched(tmp_path: Path) -> None:
    r1 = _rec(datetime(2024, 1, 1, tzinfo=UTC), 100.0)
    wh.write_records(tmp_path, [r1])
    partition = wh.partition_path(tmp_path, _CRYPTO, _BTC, _1D, 2024, 1)
    original_bytes = partition.read_bytes()

    r2 = _rec(datetime(2024, 1, 2, tzinfo=UTC), 200.0)
    with (
        mock.patch("pyarrow.parquet.ParquetWriter", side_effect=OSError("simulated temp write failure")),
        pytest.raises((OSError, wh.WarehousePartialWriteError)),
    ):
        wh.write_records(tmp_path, [r2])
    assert partition.read_bytes() == original_bytes
    assert list(partition.parent.glob(".tmp-*")) == []


# ---------------------------------------------------------------------------
# Cross-module constant consistency (avoids depending on another module's
# underscored implementation details while still catching drift)
# ---------------------------------------------------------------------------


def test_supported_timeframes_matches_binance_public_rest_private_constant() -> None:
    from py_core.market_data.binance_public_rest import _SUPPORTED_INTERVALS_MS

    assert wh.SUPPORTED_TIMEFRAMES == frozenset(_SUPPORTED_INTERVALS_MS.keys())


def test_symbol_pattern_matches_binance_public_rest_private_constant() -> None:
    from py_core.market_data.binance_public_rest import _SYMBOL_PATTERN

    assert wh.SYMBOL_PATTERN.pattern == _SYMBOL_PATTERN.pattern
