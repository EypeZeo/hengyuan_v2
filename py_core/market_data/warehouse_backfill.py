"""Network-integrated backfill for the historical OHLCV warehouse — 批次 1 PR-2.

``warehouse.py`` deliberately has no network I/O of its own; this module is the one place
that connects it to a real data source (``fetch_binance_ohlcv()``). Kept separate so
``warehouse.py``'s "no network I/O" invariant stays true by construction, not just by
convention.

**Scope, deliberately narrow**: ``backfill()`` only extends an identity's coverage forward
from its existing tail (or from a caller-supplied start, for a brand-new identity) up to a
requested end. It does **not** backfill an earlier prefix (a request for data older than
what is already stored is rejected, not silently ignored or silently satisfied by only
extending the tail) and does **not** auto-repair internal gaps (``scan_gaps()`` is the tool
for finding those; this module does not duplicate or paper over that job). Both exclusions
mirror the same scope discipline ``warehouse.py`` itself already applies to gap-repair.

All outputs are research/backtest data. No trading authorization, no live-readiness claim.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from py_core.manual_ohlcv import CanonicalMarketSymbol, ManualMarket, OhlcvTimeframe
from py_core.market_data import warehouse as wh
from py_core.market_data.binance_public_rest import fetch_binance_ohlcv


class BackfillError(Exception):
    """Base class for this module's own errors."""


class BackfillPrefixNotSupportedError(BackfillError):
    """The requested start is earlier than the identity's existing coverage start.

    backfill() only ever extends forward from the existing tail; it has no way to prepend
    an earlier prefix in the same call. Raised instead of silently ignoring the earlier
    start and reporting success for only the tail extension -- that would let a caller
    believe a wider range was backfilled than actually was.
    """


@dataclass(frozen=True, slots=True)
class BackfillReport:
    already_current: bool
    fetched_records: int
    fetch_coverage_end_utc: datetime | None
    write_report: wh.WriteReport
    resumed_from: datetime


def backfill(
    root: Path,
    market: ManualMarket,
    symbol: CanonicalMarketSymbol,
    timeframe: OhlcvTimeframe,
    start_utc: datetime,
    end_utc: datetime,
    *,
    max_requests: int = 50,
) -> BackfillReport:
    """Extend this identity's stored coverage from its existing tail up to end_utc.

    Resume point: if coverage() already exists for this identity, resume from its
    covered_end_utc (already the exact half-open point one interval past the last stored
    bar -- no manual adjustment needed). If no coverage exists yet, resume from start_utc,
    which is validated to be exactly on this timeframe's bar grid first (fail fast with a
    specific error, rather than letting a misaligned start surface later as a confusing
    ManualOhlcvValidationError deep inside write_records()).

    If coverage exists and start_utc is earlier than its covered_start_utc, raises
    BackfillPrefixNotSupportedError -- see module docstring.

    If the resume point is already >= end_utc, returns an already_current=True report with
    zero network calls and zero writes; fetch_binance_ohlcv() is never invoked (calling it
    with resume >= end_utc would hit its own start<end ValueError -- being already caught up
    is a legitimate idempotent outcome, not a failure).

    Otherwise calls fetch_binance_ohlcv() for [resume, end_utc) and writes the result via
    write_records(). Exceptions from either (BinancePublicRestError,
    ManualOhlcvValidationError, any Warehouse*Error) propagate unchanged -- this function
    does not wrap or swallow them.
    """
    existing = wh.coverage(root, market, symbol, timeframe)

    if existing is None:
        if not wh.is_on_grid(start_utc, timeframe):
            raise wh.WarehouseInvalidTimestampError(
                f"start_utc {start_utc.isoformat()} is not on the {timeframe.value} bar grid "
                f"(microsecond={start_utc.microsecond})"
            )
        resume_from = start_utc
    else:
        if start_utc < existing.covered_start_utc:
            raise BackfillPrefixNotSupportedError(
                f"requested start {start_utc.isoformat()} is earlier than existing coverage "
                f"start {existing.covered_start_utc.isoformat()} -- backfill() only extends "
                "forward from the existing tail, it cannot prepend an earlier prefix"
            )
        resume_from = existing.covered_end_utc

    if resume_from >= end_utc:
        return BackfillReport(
            already_current=True,
            fetched_records=0,
            fetch_coverage_end_utc=None,
            write_report=wh.WriteReport(rows_written=0, rows_skipped_duplicate=0, partitions_touched=()),
            resumed_from=resume_from,
        )

    records, meta = fetch_binance_ohlcv(
        symbol.value,
        timeframe.value,
        resume_from,
        end_utc,
        market=market,
        max_requests=max_requests,
    )
    write_report = wh.write_records(root, records)

    return BackfillReport(
        already_current=False,
        fetched_records=len(records),
        fetch_coverage_end_utc=meta.coverage_end_utc,
        write_report=write_report,
        resumed_from=resume_from,
    )
