"""Historical OHLCV data warehouse — Parquet, monthly partitions — 批次 1 PR-1.

This is the research stack's data foundation (路书 batch 1): a durable, month-partitioned
Parquet store for historical OHLCV bars, with a single content validator shared by every
read and write path. It has no network I/O of its own — ``fetch_binance_ohlcv()`` (batch
1's PR-2) will be the only source of new records — and it never touches account state,
credentials, or order submission. Research/backtest use only.

**Layout**: ``<root>/<market>/<symbol>/<timeframe>/<year:04d>/<month:02d>.parquet``, one
file per calendar month (UTC). Month partitions bound a single file's size for every
supported timeframe (``1m`` tops out around 43,200 rows/month) without any adaptive sizing
logic.

**Design history**: this module went through seven rounds of external design review before
implementation began (recorded in the batch-1 plan). The two ideas worth knowing before
reading the code:

1. **``validate_partition_contents()`` is the only function in this module that reads a
   partition's data pages.** Every other function — ``write_records()``'s old-file read,
   ``read_range()``, ``coverage()``, ``scan_gaps()`` — goes through it. Footer/metadata
   correctness does not prove data-page correctness (a file can have a perfectly valid
   Parquet footer and still contain NaN, duplicate timestamps, or rows misfiled into the
   wrong month), so every reader must pay for full-content validation, not just a cheap
   footer check.
2. **No DuckDB.** The original batch-1 sketch called for a "Parquet + DuckDB" layer, but
   once content validation requires fully reading every touched partition's data anyway,
   SQL predicate pushdown has nothing left to save — it would only add an unverified
   external dependency. Everything here is ``pyarrow`` + ``numpy``, streamed one month at a
   time so a query's memory/I-O scales with the query range, not with total repository
   history.

**Durability is honestly platform-split, not glossed over**: publishing a partition replaces
its file via ``os.fsync`` + ``os.replace()`` on both platforms (this alone is
process-crash-safe: any concurrent reader sees a complete old or new file, never a partial
write). On POSIX, the parent directory is additionally ``fsync``'d after the replace — the
standard extra step required for a rename to survive a genuine power loss — and a failure of
that directory fsync is reported as a failure, not swallowed. **Windows has no verified
standard-library path to flush directory metadata**, so a successful write on Windows is
process-crash-safe only; it is not confirmed power-loss durable. Do not read a
power-loss guarantee into a successful call on Windows.

All outputs are research/backtest data. No trading authorization, no live-readiness claim.
"""

from __future__ import annotations

import math
import os
import re
from collections.abc import Iterator
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from decimal import Decimal
from pathlib import Path

import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

from py_core.backtests.annualization import timeframe_seconds
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    ManualOhlcvValidationError,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
    validate_ohlcv_batch,
    validate_ohlcv_record,
)
from py_core.market_data._partition_lock import PartitionLockBusyError, partition_lock

# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------


class WarehouseError(Exception):
    """Base class for every error this module raises."""


class WarehouseInvalidIdentityError(WarehouseError):
    """A market/symbol/timeframe identity, or a resolved path, is not safe/supported."""


class WarehouseInvalidTimestampError(WarehouseError):
    """A record's event_time_utc has a nonzero microsecond component or is off the bar grid."""


class WarehouseValueNotRepresentableError(WarehouseError):
    """A record's OHLCV field is finite as Decimal but not representable as float64.

    Distinct from ManualOhlcvValidationError: the record is valid per manual_ohlcv's own
    rules (finite, positive price, non-negative volume), it just does not fit in the
    float64 storage type this warehouse uses.
    """


class WarehouseBusyError(WarehouseError):
    """Another process/handle currently holds the lock for this partition."""


class WarehouseCorruptError(WarehouseError):
    """A partition file failed content validation (footer, metadata, or data-page check)."""


@dataclass(frozen=True, slots=True)
class PartitionPublishResult:
    """Identity/path/row-count of one partition successfully published in a write_records()
    call. Deliberately carries no record values."""

    market: ManualMarket
    symbol: str
    timeframe: str
    year: int
    month: int
    path: Path
    rows_written: int
    rows_skipped_duplicate: int


class WarehousePartialWriteError(WarehouseError):
    """A multi-partition write_records() call failed after publishing some partitions.

    Per-partition publish is atomic (see module docstring); the whole batch is not
    transactional. ``published_partitions`` lists every partition that was durably
    published before the failure -- callers can safely retry the entire original batch,
    because already-published partitions will hit the duplicate-only no-op fast path on
    retry (old-wins + timestamp-presence dedup is idempotent) while the partitions that
    never got published will actually write this time.
    """

    def __init__(
        self,
        published_partitions: tuple[PartitionPublishResult, ...],
        cause: Exception,
    ) -> None:
        self.published_partitions = published_partitions
        super().__init__(
            f"write_records() failed after publishing {len(published_partitions)} "
            f"partition(s); cause: {cause!r}. The full batch may be safely retried."
        )
        self.__cause__ = cause


# ---------------------------------------------------------------------------
# Identity / path safety
# ---------------------------------------------------------------------------

# Deliberately NOT imported from binance_public_rest.py -- that module's
# _SUPPORTED_INTERVALS_MS/_SYMBOL_PATTERN are its own private implementation details, and
# this module should not depend on another module's underscored names. test_warehouse.py
# asserts these stay equal to binance_public_rest.py's private constants so the two can
# never silently drift apart.
SUPPORTED_TIMEFRAMES = frozenset(
    {"1m", "3m", "5m", "15m", "30m", "1h", "2h", "4h", "6h", "8h", "12h", "1d", "3d", "1w"}
)
SYMBOL_PATTERN = re.compile(r"^[A-Z0-9]+$")

_PARTITION_FILENAME_PATTERN = re.compile(r"^(\d{2})\.parquet$")
_PARTITION_YEAR_DIR_PATTERN = re.compile(r"^(\d{4})$")

_WAREHOUSE_SCHEMA_VERSION = b"1"
_LOCK_SUFFIX = ".lock"


def _validate_identity(
    market: ManualMarket, symbol: CanonicalMarketSymbol, timeframe: OhlcvTimeframe
) -> None:
    if not SYMBOL_PATTERN.fullmatch(symbol.value):
        raise WarehouseInvalidIdentityError(
            f"symbol {symbol.value!r} does not match {SYMBOL_PATTERN.pattern} -- "
            "CanonicalMarketSymbol itself only rejects whitespace, so this warehouse "
            "re-validates before using the value in any path."
        )
    if timeframe.value not in SUPPORTED_TIMEFRAMES:
        raise WarehouseInvalidIdentityError(
            f"timeframe {timeframe.value!r} is not in SUPPORTED_TIMEFRAMES"
        )


def _identity_dir(
    root: Path, market: ManualMarket, symbol: CanonicalMarketSymbol, timeframe: OhlcvTimeframe
) -> Path:
    _validate_identity(market, symbol, timeframe)
    return root / market.value / symbol.value / timeframe.value


def _no_symlink_between(root: Path, target: Path) -> None:
    """Refuse if any path component between root (exclusive) and target (inclusive) is a
    symlink/reparse point. Accepted trust boundary, not a full TOCTOU defense: a real
    open()-time O_NOFOLLOW directory-fd chain (POSIX) or reparse-point HANDLE check
    (Windows) would be needed for that, and this single-machine research repo does not
    take on that complexity. This check catches the obvious/accidental cases."""
    root_resolved = root.resolve()
    try:
        relative = target.resolve().relative_to(root_resolved)
    except ValueError as exc:
        raise WarehouseInvalidIdentityError(
            f"path {target} escapes warehouse root {root}"
        ) from exc
    current = root_resolved
    for part in relative.parts:
        current = current / part
        if current.is_symlink():
            raise WarehouseInvalidIdentityError(
                f"refusing to follow symlink/reparse point at {current}"
            )


def partition_path(
    root: Path,
    market: ManualMarket,
    symbol: CanonicalMarketSymbol,
    timeframe: OhlcvTimeframe,
    year: int,
    month: int,
) -> Path:
    """``<root>/<market>/<symbol>/<timeframe>/<year:04d>/<month:02d>.parquet``.

    year/month must come from an already-validated ``event_time_utc`` -- callers never pass
    raw path fragments. The result is re-checked against ``root`` after resolution as a
    second, independent line of defense (belt-and-suspenders, not the only check).
    """
    if not (1 <= month <= 12):
        raise WarehouseInvalidIdentityError(f"month must be 1-12, got {month}")
    path = _identity_dir(root, market, symbol, timeframe) / f"{year:04d}" / f"{month:02d}.parquet"
    resolved_root = root.resolve()
    resolved_path = path.resolve()
    if not resolved_path.is_relative_to(resolved_root):
        raise WarehouseInvalidIdentityError(f"partition path {path} escapes root {root}")
    return path


def _lock_path_for(partition: Path) -> Path:
    """Lock path is derived from an already-validated partition path -- never an
    independent caller-supplied path, so the lock API cannot become a second path-injection
    point."""
    return partition.with_name(partition.name + _LOCK_SUFFIX)


# ---------------------------------------------------------------------------
# Exact-grid arithmetic -- the single source of truth shared by write-time
# precheck, content validation, and cross-partition continuity checks.
# ---------------------------------------------------------------------------

_EPOCH_UTC = datetime(1970, 1, 1, tzinfo=UTC)
# Binance weekly klines open Monday 00:00 UTC; the Unix epoch (1970-01-01) is a Thursday,
# 4 days earlier. Every other supported timeframe has the epoch itself as a valid grid
# point, so this offset is zero for all non-weekly units.
_WEEKLY_GRID_ANCHOR_US = 4 * 86_400 * 1_000_000


def interval_us_for(timeframe: OhlcvTimeframe) -> int:
    """Microseconds per bar for this timeframe."""
    return timeframe_seconds(timeframe) * 1_000_000


def grid_anchor_us_for(timeframe: OhlcvTimeframe) -> int:
    """Microsecond offset of this timeframe's grid from the Unix epoch."""
    return _WEEKLY_GRID_ANCHOR_US if timeframe.value.endswith("w") else 0


def epoch_us_exact(dt: datetime) -> int:
    """Exact integer microseconds since the Unix epoch.

    Uses timedelta-on-timedelta floor division (exact integer arithmetic on Python's
    internal days/seconds/microseconds representation) rather than ``dt.timestamp()``,
    which returns a float and risks rounding for large values.
    """
    return (dt - _EPOCH_UTC) // timedelta(microseconds=1)


def is_on_grid(dt: datetime, timeframe: OhlcvTimeframe) -> bool:
    """True iff dt has zero microseconds and falls exactly on this timeframe's bar grid."""
    if dt.microsecond != 0:
        return False
    interval_us = interval_us_for(timeframe)
    anchor_us = grid_anchor_us_for(timeframe)
    return (epoch_us_exact(dt) - anchor_us) % interval_us == 0


# ---------------------------------------------------------------------------
# Fixed Arrow schema + strict metadata
# ---------------------------------------------------------------------------

_ARROW_SCHEMA = pa.schema(
    [
        pa.field("event_time_utc", pa.timestamp("us", tz="UTC"), nullable=False),
        pa.field("open", pa.float64(), nullable=False),
        pa.field("high", pa.float64(), nullable=False),
        pa.field("low", pa.float64(), nullable=False),
        pa.field("close", pa.float64(), nullable=False),
        pa.field("volume", pa.float64(), nullable=False),
    ]
)

_METADATA_KEYS = (
    b"warehouse_schema_version",
    b"market",
    b"symbol",
    b"timeframe",
    b"written_at_utc",
    b"writer_tool_version",
)
_WRITER_TOOL_VERSION = b"hengyuan-v2-py_core-warehouse/1"


def _build_metadata(market: ManualMarket, symbol: str, timeframe: str) -> dict[bytes, bytes]:
    return {
        b"warehouse_schema_version": _WAREHOUSE_SCHEMA_VERSION,
        b"market": market.value.encode("utf-8"),
        b"symbol": symbol.encode("utf-8"),
        b"timeframe": timeframe.encode("utf-8"),
        b"written_at_utc": datetime.now(UTC).isoformat().encode("utf-8"),
        b"writer_tool_version": _WRITER_TOOL_VERSION,
    }


def _check_schema_and_metadata(
    schema: pa.Schema, *, market: ManualMarket, symbol: str, timeframe: str
) -> None:
    """Raise WarehouseCorruptError unless schema/metadata prove this file belongs to the
    identity implied by its path. written_at_utc is intentionally excluded from comparison.
    """
    fields_only = pa.schema([pa.field(f.name, f.type, nullable=f.nullable) for f in schema])
    expected_fields_only = pa.schema(
        [pa.field(f.name, f.type, nullable=f.nullable) for f in _ARROW_SCHEMA]
    )
    if fields_only != expected_fields_only:
        raise WarehouseCorruptError(
            f"field/type/nullability mismatch: got {fields_only}, expected {expected_fields_only}"
        )
    metadata = schema.metadata or {}
    for key in _METADATA_KEYS:
        if key not in metadata:
            raise WarehouseCorruptError(f"missing required metadata key {key!r}")
    if metadata[b"warehouse_schema_version"] != _WAREHOUSE_SCHEMA_VERSION:
        raise WarehouseCorruptError(
            f"unknown warehouse_schema_version {metadata[b'warehouse_schema_version']!r} "
            f"(this reader only supports {_WAREHOUSE_SCHEMA_VERSION!r}); fail-closed rather "
            "than assume forward compatibility"
        )
    expected_identity = {
        b"market": market.value.encode("utf-8"),
        b"symbol": symbol.encode("utf-8"),
        b"timeframe": timeframe.encode("utf-8"),
    }
    for key, expected_value in expected_identity.items():
        if metadata[key] != expected_value:
            raise WarehouseCorruptError(
                f"metadata {key!r}={metadata[key]!r} does not match path-implied identity "
                f"{expected_value!r}"
            )


# ---------------------------------------------------------------------------
# The single content validator -- the only function that reads data pages.
# ---------------------------------------------------------------------------


def validate_partition_contents(
    path: Path,
    *,
    market: ManualMarket,
    symbol: CanonicalMarketSymbol,
    timeframe: OhlcvTimeframe,
    year: int,
    month: int,
) -> pa.Table:
    """Fully validate one partition file's footer, metadata, and data pages.

    This is the only function in the module that reads a partition's data pages --
    write_records()'s old-file read and every public read API (read_range/coverage/
    scan_gaps) go through it. Returns the validated table (all 6 columns, on-disk physical
    row order) on success.

    Checks (any failure raises WarehouseCorruptError):
    1. Footer schema (6 non-nullable fields) + metadata (fixed keys present, exact version
       match, market/symbol/timeframe match the path-implied identity).
    2. Table is non-empty -- this module's own writer never produces an empty partition
       file (write_records([]) creates nothing; a duplicate-only merge never touches disk),
       so an empty-but-present file is evidence of external tampering, not "no data".
    3. All 6 columns have null_count == 0, checked before any numeric operation (a
       schema-valid file can still contain null data pages; nullable=False is a schema
       declaration, not a read-time safety check on its own).
    4. event_time_utc, cast to int64 microseconds, has strictly positive adjacent deltas
       throughout the table's on-disk physical order -- this single check proves both
       "already strictly ascending" (a duplicate or reorder gives a non-positive delta) and
       "no duplicates" in one pass. It does NOT check that deltas equal the timeframe's bar
       interval -- internal gaps are a legitimate, expected state for a warehouse that has
       not been fully backfilled yet, reported by scan_gaps()/coverage(), not treated as
       corruption here.
    5. Every timestamp is on the timeframe's bar grid (is_on_grid(), vectorized).
    6. Every row's (year, month) matches this partition file's own identity.
    7. OHLCV columns: finite, price fields > 0, volume >= 0, high >= max(open,close,low),
       low <= min(open,close,high).
    """
    with pq.ParquetFile(path) as pf:
        _check_schema_and_metadata(
            pf.schema_arrow, market=market, symbol=symbol.value, timeframe=timeframe.value
        )
        table = pf.read()

    if table.num_rows == 0:
        raise WarehouseCorruptError(f"{path}: partition file has zero rows (never legitimate)")

    for name in _ARROW_SCHEMA.names:
        if table.column(name).null_count != 0:
            raise WarehouseCorruptError(f"{path}: column {name!r} contains null value(s)")

    ts_int64 = pc.cast(table.column("event_time_utc"), pa.int64(), safe=True).to_numpy()
    if len(ts_int64) > 1:
        deltas = np.diff(ts_int64)
        if not np.all(deltas > 0):
            raise WarehouseCorruptError(
                f"{path}: event_time_utc is not strictly ascending / contains duplicates"
            )

    interval_us = interval_us_for(timeframe)
    anchor_us = grid_anchor_us_for(timeframe)
    off_grid = (ts_int64 - anchor_us) % interval_us != 0
    if np.any(off_grid):
        raise WarehouseCorruptError(
            f"{path}: {int(np.count_nonzero(off_grid))} timestamp(s) not on the bar grid"
        )

    years = pc.year(table.column("event_time_utc")).to_numpy()
    months = pc.month(table.column("event_time_utc")).to_numpy()
    if np.any(years != year) or np.any(months != month):
        raise WarehouseCorruptError(
            f"{path}: contains row(s) whose (year, month) != this partition's own ({year}, {month})"
        )

    _validate_ohlc_columns(table, path)

    return table


def _validate_ohlc_columns(table: pa.Table, path: Path) -> None:
    open_ = table.column("open")
    high = table.column("high")
    low = table.column("low")
    close = table.column("close")
    volume = table.column("volume")

    for name, col in (("open", open_), ("high", high), ("low", low), ("close", close), ("volume", volume)):
        if not pc.all(pc.is_finite(col)).as_py():
            raise WarehouseCorruptError(f"{path}: column {name!r} contains a non-finite value")

    for name, col in (("open", open_), ("high", high), ("low", low), ("close", close)):
        if not pc.all(pc.greater(col, 0.0)).as_py():
            raise WarehouseCorruptError(f"{path}: column {name!r} contains a non-positive price")

    if not pc.all(pc.greater_equal(volume, 0.0)).as_py():
        raise WarehouseCorruptError(f"{path}: column 'volume' contains a negative value")

    max_ohc = pc.max_element_wise(open_, high, close)
    min_olc = pc.min_element_wise(open_, low, close)
    if not pc.all(pc.greater_equal(high, max_ohc)).as_py():
        raise WarehouseCorruptError(f"{path}: high < max(open, close, low) for some row")
    if not pc.all(pc.less_equal(low, min_olc)).as_py():
        raise WarehouseCorruptError(f"{path}: low > min(open, close, high) for some row")


# ---------------------------------------------------------------------------
# write_records()
# ---------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class WriteReport:
    rows_written: int
    rows_skipped_duplicate: int
    partitions_touched: tuple[Path, ...]


def _decimal_to_finite_float(value: Decimal, field_name: str) -> float:
    try:
        as_float = float(value)
    except (OverflowError, ValueError) as exc:
        raise WarehouseValueNotRepresentableError(
            f"field {field_name!r} value {value} could not be converted to float64"
        ) from exc
    if not math.isfinite(as_float):
        # Python's float(Decimal) does NOT raise OverflowError for values like 1e400 --
        # it silently returns inf. This isfinite() check is the check that actually
        # catches that case; it is not redundant with the try/except above (verified: see
        # implementation-step-1 smoke test).
        raise WarehouseValueNotRepresentableError(
            f"field {field_name!r} value {value} is finite as Decimal but not representable "
            "as a finite float64"
        )
    return as_float


def _partition_key(
    record: NormalizedOhlcvRecord,
) -> tuple[ManualMarket, CanonicalMarketSymbol, OhlcvTimeframe, int, int]:
    return (
        record.market,
        record.symbol,
        record.timeframe,
        record.event_time_utc.year,
        record.event_time_utc.month,
    )


def _records_to_new_table(records: list[NormalizedOhlcvRecord]) -> pa.Table:
    """Build the fixed-schema Arrow table for a batch of records already known to belong
    to a single partition, in their original relative order (not re-sorted -- sorting
    happens later, in the merge step, using an explicit rank key)."""
    return pa.table(
        {
            "event_time_utc": pa.array(
                [r.event_time_utc for r in records], type=pa.timestamp("us", tz="UTC")
            ),
            "open": pa.array(
                [_decimal_to_finite_float(r.open_price, "open") for r in records], type=pa.float64()
            ),
            "high": pa.array(
                [_decimal_to_finite_float(r.high_price, "high") for r in records], type=pa.float64()
            ),
            "low": pa.array(
                [_decimal_to_finite_float(r.low_price, "low") for r in records], type=pa.float64()
            ),
            "close": pa.array(
                [_decimal_to_finite_float(r.close_price, "close") for r in records], type=pa.float64()
            ),
            "volume": pa.array(
                [_decimal_to_finite_float(r.volume, "volume") for r in records], type=pa.float64()
            ),
        },
        schema=_ARROW_SCHEMA,
    )


def _merge_old_wins(old_table: pa.Table, new_table: pa.Table) -> pa.Table:
    """Deterministic old-wins merge. See module docstring / plan for why sorting alone does
    not deduplicate -- this is the precise algorithm, not a description of intent.

    1. Tag old rows source_rank=0, new rows source_rank=1; source_row_ordinal is each
       table's own original row position.
    2. Concatenate and sort by (event_time_utc, source_rank, source_row_ordinal) -- a
       total order over every row (the ordinal breaks all remaining ties), so the result
       does not depend on Arrow's sort-stability guarantees.
    3. Compute a keep-mask via a single explicit numpy comparison on the now-sorted int64
       timestamp column: keep row i iff i == 0 or ts[i] != ts[i-1]. Because source_rank=0
       always sorts before source_rank=1 for equal timestamps, the kept row for any
       collision is deterministically the old one.
    4. Drop the two temporary columns; they never reach parquet.
    """
    n_old = old_table.num_rows
    n_new = new_table.num_rows
    ranked_old = old_table.append_column(
        "source_rank", pa.array([0] * n_old, type=pa.int8())
    ).append_column("source_row_ordinal", pa.array(range(n_old), type=pa.int64()))
    ranked_new = new_table.append_column(
        "source_rank", pa.array([1] * n_new, type=pa.int8())
    ).append_column("source_row_ordinal", pa.array(range(n_new), type=pa.int64()))

    combined = pa.concat_tables([ranked_old, ranked_new])
    sorted_combined = combined.sort_by(
        [
            ("event_time_utc", "ascending"),
            ("source_rank", "ascending"),
            ("source_row_ordinal", "ascending"),
        ]
    )
    ts_int64 = pc.cast(sorted_combined.column("event_time_utc"), pa.int64(), safe=True).to_numpy()
    keep_mask = np.empty(len(ts_int64), dtype=bool)
    keep_mask[0] = True
    keep_mask[1:] = ts_int64[1:] != ts_int64[:-1]

    deduped = sorted_combined.filter(pa.array(keep_mask))
    return deduped.drop_columns(["source_rank", "source_row_ordinal"])


def _new_timestamp_count(old_table: pa.Table | None, new_table: pa.Table) -> int:
    """How many of new_table's timestamps are NOT already present in old_table. Uses
    old_table's exact validated timestamp column (never raw/unvalidated data) for
    membership -- see plan P1-4."""
    if old_table is None:
        return new_table.num_rows
    old_ts = set(pc.cast(old_table.column("event_time_utc"), pa.int64(), safe=True).to_pylist())
    new_ts = pc.cast(new_table.column("event_time_utc"), pa.int64(), safe=True).to_pylist()
    return sum(1 for t in new_ts if t not in old_ts)


def _fsync_directory(dir_path: Path) -> None:
    """POSIX only: fsync the parent directory so a rename/replace inside it survives a
    genuine power loss. A failure here is reported, not swallowed -- silently claiming
    success while the directory entry may not be durable would defeat the point."""
    fd = os.open(str(dir_path), os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _publish_partition_file(
    final_path: Path, table: pa.Table, *, market: ManualMarket, symbol: str, timeframe: str
) -> None:
    """Write table to a temp file in final_path's own parent directory, fsync it, then
    os.replace() it onto final_path.

    Durability: process-crash-safe on both platforms (any concurrent reader sees a complete
    old or new file). On POSIX, additionally fsyncs the parent directory after the replace
    -- the extra step a rename needs to survive a real power loss; failure of that fsync
    raises. On Windows there is no verified standard-library equivalent, so a successful
    call there is process-crash-safe only, not confirmed power-loss durable.
    """
    final_path.parent.mkdir(parents=True, exist_ok=True)
    schema_with_metadata = _ARROW_SCHEMA.with_metadata(_build_metadata(market, symbol, timeframe))
    table_to_write = table.cast(schema_with_metadata)
    temp_path = final_path.parent / f".tmp-{final_path.name}-{os.urandom(8).hex()}"
    try:
        with pq.ParquetWriter(temp_path, schema_with_metadata) as writer:
            writer.write_table(table_to_write)
        fd = os.open(str(temp_path), os.O_RDWR)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
        os.replace(temp_path, final_path)
    except BaseException:
        if temp_path.exists():
            temp_path.unlink(missing_ok=True)
        raise
    if os.name == "posix":
        _fsync_directory(final_path.parent)


def write_records(root: Path, records: list[NormalizedOhlcvRecord]) -> WriteReport:
    """Validate and durably store a batch of records, grouped into their target monthly
    partitions. See module docstring for the durability contract and
    WarehousePartialWriteError for multi-partition failure semantics.

    Precheck (before any I/O): every record's identity fields must be path-safe, every
    timestamp must be on its timeframe's exact bar grid (microsecond == 0 and grid-aligned
    -- both checked via is_on_grid(), the same function used everywhere else in this
    module), every Decimal OHLCV field must be representable as a finite float64, and each
    (market,symbol,timeframe) group must pass validate_ohlcv_record()/
    validate_ohlcv_batch() as given (not re-sorted -- a batch with an internal duplicate or
    out-of-order timestamp is a genuine upstream bug and is rejected, not silently fixed).
    Any failure here means zero filesystem side effects.
    """
    if not records:
        return WriteReport(rows_written=0, rows_skipped_duplicate=0, partitions_touched=())

    for record in records:
        _validate_identity(record.market, record.symbol, record.timeframe)
        if not is_on_grid(record.event_time_utc, record.timeframe):
            raise WarehouseInvalidTimestampError(
                f"event_time_utc {record.event_time_utc.isoformat()} is not on the "
                f"{record.timeframe.value} bar grid (microsecond={record.event_time_utc.microsecond})"
            )
        for field_name, value in (
            ("open", record.open_price),
            ("high", record.high_price),
            ("low", record.low_price),
            ("close", record.close_price),
            ("volume", record.volume),
        ):
            _decimal_to_finite_float(value, field_name)

    groups: dict[tuple[ManualMarket, CanonicalMarketSymbol, OhlcvTimeframe], list[NormalizedOhlcvRecord]] = {}
    for record in records:
        key = (record.market, record.symbol, record.timeframe)
        groups.setdefault(key, []).append(record)

    for group_records in groups.values():
        issues = tuple(issue for r in group_records for issue in validate_ohlcv_record(r))
        issues += validate_ohlcv_batch(tuple(group_records))
        if issues:
            raise ManualOhlcvValidationError(issues)

    partitions: dict[
        tuple[ManualMarket, CanonicalMarketSymbol, OhlcvTimeframe, int, int],
        list[NormalizedOhlcvRecord],
    ] = {}
    for record in records:
        partitions.setdefault(_partition_key(record), []).append(record)

    sorted_keys = sorted(
        partitions.keys(),
        key=lambda k: str(partition_path(root, k[0], k[1], k[2], k[3], k[4])),
    )

    published: list[PartitionPublishResult] = []
    total_written = 0
    total_skipped = 0
    try:
        for key in sorted_keys:
            market, symbol, timeframe, year, month = key
            final_path = partition_path(root, market, symbol, timeframe, year, month)
            final_path.parent.mkdir(parents=True, exist_ok=True)
            # Re-check containment/symlink-safety after mkdir -- the directory could not
            # have existed as a symlink before this call created it, but this check is the
            # one that would catch a symlink introduced concurrently between the check and
            # the lock acquisition below (accepted trust boundary, not full TOCTOU defense;
            # see _no_symlink_between()'s docstring).
            _no_symlink_between(root, final_path.parent)
            lock_path = _lock_path_for(final_path)

            with partition_lock(lock_path):
                old_table: pa.Table | None = None
                if final_path.exists():
                    old_table = validate_partition_contents(
                        final_path, market=market, symbol=symbol, timeframe=timeframe, year=year, month=month
                    )
                new_table = _records_to_new_table(partitions[key])
                new_count = _new_timestamp_count(old_table, new_table)
                incoming_count = new_table.num_rows
                skipped = incoming_count - new_count

                if new_count == 0:
                    published.append(
                        PartitionPublishResult(
                            market=market,
                            symbol=symbol.value,
                            timeframe=timeframe.value,
                            year=year,
                            month=month,
                            path=final_path,
                            rows_written=0,
                            rows_skipped_duplicate=skipped,
                        )
                    )
                    total_skipped += skipped
                    continue

                merged = new_table if old_table is None else _merge_old_wins(old_table, new_table)
                _assert_merge_invariants(merged, year=year, month=month)

                _publish_partition_file(
                    final_path,
                    merged,
                    market=market,
                    symbol=symbol.value,
                    timeframe=timeframe.value,
                )
                published.append(
                    PartitionPublishResult(
                        market=market,
                        symbol=symbol.value,
                        timeframe=timeframe.value,
                        year=year,
                        month=month,
                        path=final_path,
                        rows_written=new_count,
                        rows_skipped_duplicate=skipped,
                    )
                )
                total_written += new_count
                total_skipped += skipped
    except PartitionLockBusyError as exc:
        if published:
            raise WarehousePartialWriteError(tuple(published), exc) from exc
        raise WarehouseBusyError(str(exc)) from exc
    except Exception as exc:
        if published:
            raise WarehousePartialWriteError(tuple(published), exc) from exc
        raise

    return WriteReport(
        rows_written=total_written,
        rows_skipped_duplicate=total_skipped,
        partitions_touched=tuple(p.path for p in published if p.rows_written > 0),
    )


def _assert_merge_invariants(table: pa.Table, *, year: int, month: int) -> None:
    """Defense-in-depth: the merge algorithm's construction already guarantees these, but
    verifying costs little relative to the write and catches a future bug in the merge
    logic before it reaches disk instead of only at the next read."""
    ts_int64 = pc.cast(table.column("event_time_utc"), pa.int64(), safe=True).to_numpy()
    if len(ts_int64) > 1 and not np.all(np.diff(ts_int64) > 0):
        raise WarehouseError("internal error: merge output is not strictly ascending/unique")
    years = pc.year(table.column("event_time_utc")).to_numpy()
    months = pc.month(table.column("event_time_utc")).to_numpy()
    if np.any(years != year) or np.any(months != month):
        raise WarehouseError("internal error: merge output contains rows outside target month")


# ---------------------------------------------------------------------------
# Streaming partition enumeration + read APIs
# ---------------------------------------------------------------------------


def _enumerate_partition_paths(
    root: Path, market: ManualMarket, symbol: CanonicalMarketSymbol, timeframe: OhlcvTimeframe
) -> list[tuple[Path, int, int]]:
    """Pure path discovery (no I/O beyond directory listing): returns (path, year, month)
    sorted ascending by (year, month)."""
    identity_dir = _identity_dir(root, market, symbol, timeframe)
    if not identity_dir.is_dir():
        return []
    results: list[tuple[Path, int, int]] = []
    for year_dir in identity_dir.iterdir():
        if not year_dir.is_dir():
            continue
        year_match = _PARTITION_YEAR_DIR_PATTERN.fullmatch(year_dir.name)
        if year_match is None:
            continue
        year = int(year_match.group(1))
        for month_file in year_dir.iterdir():
            month_match = _PARTITION_FILENAME_PATTERN.fullmatch(month_file.name)
            if month_match is None:
                continue
            month = int(month_match.group(1))
            results.append((month_file, year, month))
    results.sort(key=lambda item: (item[1], item[2]))
    return results


def _month_bounds(year: int, month: int) -> tuple[datetime, datetime]:
    start = datetime(year, month, 1, tzinfo=UTC)
    end = datetime(year + 1, 1, 1, tzinfo=UTC) if month == 12 else datetime(year, month + 1, 1, tzinfo=UTC)
    return start, end


def _iter_validated_partitions(
    root: Path,
    market: ManualMarket,
    symbol: CanonicalMarketSymbol,
    timeframe: OhlcvTimeframe,
    *,
    start_utc: datetime | None = None,
    end_utc: datetime | None = None,
) -> Iterator[tuple[Path, int, int, pa.Table]]:
    """Yield (path, year, month, validated_table) for partitions ascending in time,
    restricted to those whose month range intersects [start_utc, end_utc) when given. Stops
    enumerating as soon as a month's start is >= end_utc (paths are sorted ascending, so no
    later month can match). Each yielded table should be consumed and released by the
    caller before advancing -- this generator holds no reference to a table across yields."""
    for path, year, month in _enumerate_partition_paths(root, market, symbol, timeframe):
        month_start, month_end = _month_bounds(year, month)
        if end_utc is not None and month_start >= end_utc:
            break
        if start_utc is not None and month_end <= start_utc:
            continue
        yield path, year, month, validate_partition_contents(
            path, market=market, symbol=symbol, timeframe=timeframe, year=year, month=month
        )


@dataclass(frozen=True, slots=True)
class Coverage:
    record_count: int
    covered_start_utc: datetime
    covered_end_utc: datetime  # last bar's event_time_utc + one interval (exclusive bound)
    is_contiguous: bool


def coverage(
    root: Path, market: ManualMarket, symbol: CanonicalMarketSymbol, timeframe: OhlcvTimeframe
) -> Coverage | None:
    """None is the only representation of "no data" -- if this identity has zero verified
    partitions. If any Coverage is returned, record_count is always >= 1."""
    interval_td = timedelta(microseconds=interval_us_for(timeframe))
    interval_us = interval_us_for(timeframe)

    first_ts: datetime | None = None
    last_ts: datetime | None = None
    total_count = 0
    is_contig = True
    prev_month_last_ts: datetime | None = None

    for _path, _year, _month, table in _iter_validated_partitions(root, market, symbol, timeframe):
        n = table.num_rows
        month_first_ts = table.column("event_time_utc")[0].as_py()
        month_last_ts = table.column("event_time_utc")[-1].as_py()

        if first_ts is None:
            first_ts = month_first_ts
        elif prev_month_last_ts is not None and prev_month_last_ts + interval_td != month_first_ts:
            is_contig = False

        if n > 1:
            ts_int64 = pc.cast(table.column("event_time_utc"), pa.int64(), safe=True).to_numpy()
            if not np.all(np.diff(ts_int64) == interval_us):
                is_contig = False

        total_count += n
        last_ts = month_last_ts
        prev_month_last_ts = month_last_ts
        del table

    if total_count == 0 or first_ts is None or last_ts is None:
        return None

    return Coverage(
        record_count=total_count,
        covered_start_utc=first_ts,
        covered_end_utc=last_ts + interval_td,
        is_contiguous=is_contig,
    )


@dataclass(frozen=True, slots=True)
class GapIssue:
    at: datetime
    partition: Path


@dataclass(frozen=True, slots=True)
class GapReport:
    issues: tuple[GapIssue, ...]
    total_issue_count: int
    truncated: bool


def scan_gaps(
    root: Path,
    market: ManualMarket,
    symbol: CanonicalMarketSymbol,
    timeframe: OhlcvTimeframe,
    *,
    max_issues: int = 1000,
) -> GapReport:
    """Stream through every partition for this identity reporting missing bars. Continues
    scanning past max_issues to keep total_issue_count accurate, but stops constructing
    GapIssue objects once the cap is hit -- bounds memory even against a badly
    under-backfilled repository."""
    interval_td = timedelta(microseconds=interval_us_for(timeframe))
    interval_us = interval_us_for(timeframe)

    issues: list[GapIssue] = []
    total_issue_count = 0
    prev_month_last_ts: datetime | None = None
    prev_path: Path | None = None

    for path, _year, _month, table in _iter_validated_partitions(root, market, symbol, timeframe):
        n = table.num_rows
        ts_column = table.column("event_time_utc")
        month_first_ts = ts_column[0].as_py()

        if prev_month_last_ts is not None and prev_month_last_ts + interval_td != month_first_ts:
            total_issue_count += 1
            if len(issues) < max_issues:
                # The missing bar(s) start right after the previous month's last bar;
                # attribute the issue to the earlier (prev) partition, where the sequence
                # broke off.
                issues.append(GapIssue(at=prev_month_last_ts + interval_td, partition=prev_path))

        if n > 1:
            ts_int64 = pc.cast(ts_column, pa.int64(), safe=True).to_numpy()
            deltas = np.diff(ts_int64)
            gap_positions = np.nonzero(deltas != interval_us)[0]
            for pos in gap_positions:
                total_issue_count += 1
                if len(issues) < max_issues:
                    missing_at_us = int(ts_int64[pos]) + interval_us
                    missing_at = _EPOCH_UTC + timedelta(microseconds=missing_at_us)
                    issues.append(GapIssue(at=missing_at, partition=path))

        prev_month_last_ts = ts_column[-1].as_py()
        prev_path = path
        del table

    return GapReport(
        issues=tuple(issues),
        total_issue_count=total_issue_count,
        truncated=total_issue_count > len(issues),
    )


def read_range(
    root: Path,
    market: ManualMarket,
    symbol: CanonicalMarketSymbol,
    timeframe: OhlcvTimeframe,
    start_utc: datetime,
    end_utc: datetime,
) -> pd.DataFrame:
    """Return a records_to_dataframe()-shaped pandas DataFrame (UTC DatetimeIndex, columns
    open/high/low/close/volume, float64) for [start_utc, end_utc). Only partitions whose
    month range intersects the query are read/validated -- memory and I/O scale with the
    query range, not with total repository history. Empty result returns a same-shape
    empty DataFrame, never None or an exception."""
    if start_utc.tzinfo is None or end_utc.tzinfo is None:
        raise ValueError("start_utc/end_utc must be timezone-aware UTC")
    if start_utc >= end_utc:
        raise ValueError("start_utc must be before end_utc")

    tables = [
        table for _path, _year, _month, table in _iter_validated_partitions(
            root, market, symbol, timeframe, start_utc=start_utc, end_utc=end_utc
        )
    ]
    columns = ["open", "high", "low", "close", "volume"]
    if not tables:
        empty = pd.DataFrame(columns=columns, dtype="float64")
        empty.index = pd.DatetimeIndex([], tz="UTC", name="event_time_utc")
        return empty

    combined = pa.concat_tables(tables)
    start_scalar = pa.scalar(start_utc, type=pa.timestamp("us", tz="UTC"))
    end_scalar = pa.scalar(end_utc, type=pa.timestamp("us", tz="UTC"))
    mask = pc.and_(
        pc.greater_equal(combined.column("event_time_utc"), start_scalar),
        pc.less(combined.column("event_time_utc"), end_scalar),
    )
    filtered = combined.filter(mask)
    df = filtered.to_pandas()
    df = df.set_index("event_time_utc")
    df.index.name = "event_time_utc"
    return df[columns]
