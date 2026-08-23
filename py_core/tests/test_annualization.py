"""Unit tests for 批次 0 annualization-factor derivation.

Covers the real calculation bug this module fixes: ``BacktestConfig.annualization_factor``
used to default to ``252`` (equity trading days) while the only data source in this repo is
Binance spot, a 24/7 market. Before this round the entire test suite never asserted a single
engine-computed annualized value — the only occurrences of ``sharpe_ratio``/``annualized_*``
were hand-written literals in ``test_artifact_export.py`` — which is precisely why the wrong
constant survived. The end-to-end regression test at the bottom is the one that pins it.

All outputs are backtesting estimates only.
NOT financial advice. NOT trading authorization.
"""

from __future__ import annotations

import math
from datetime import UTC, datetime, timedelta
from decimal import Decimal
from typing import Any

import pandas as pd
import pytest

from py_core.backtests.annualization import (
    CALENDAR_SECONDS_PER_YEAR,
    EQUITY_TRADING_DAYS_PER_YEAR,
    annualization_factor_for,
    annualization_factor_for_records,
    timeframe_seconds,
)
from py_core.backtests.models import BacktestConfig
from py_core.backtests.vectorized_engine import run_vectorized_backtest
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)

_BASE_DT = datetime(2024, 1, 1, tzinfo=UTC)
_SYMBOL = CanonicalMarketSymbol("BTC/USDT")


# ---------------------------------------------------------------------------
# timeframe_seconds
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("timeframe", "expected"),
    [
        ("1m", 60),
        ("5m", 300),
        ("15m", 900),
        ("1h", 3_600),
        ("4h", 14_400),
        ("1d", 86_400),
        ("1w", 604_800),
        # Unusual multiples the OhlcvTimeframe regex allows. These are the cases a
        # hard-coded lookup table would silently miss, which is why the implementation
        # parses <count><unit> generically instead.
        ("45m", 2_700),
        ("7h", 25_200),
        ("3d", 259_200),
    ],
)
def test_timeframe_seconds_parses_count_and_unit(timeframe: str, expected: int) -> None:
    assert timeframe_seconds(OhlcvTimeframe(timeframe)) == expected


# ---------------------------------------------------------------------------
# annualization_factor_for — crypto (24/7)
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("timeframe", "expected"),
    [
        ("1d", 365.0),
        ("1h", 8_760.0),
        ("4h", 2_190.0),
        ("15m", 35_040.0),
        ("1m", 525_600.0),
    ],
)
def test_crypto_factor_uses_calendar_year_not_trading_days(
    timeframe: str, expected: float
) -> None:
    """The core fix: crypto spot is 24/7/365, never 252 trading days."""
    actual = annualization_factor_for(ManualMarket.CRYPTO_SPOT, OhlcvTimeframe(timeframe))
    assert actual == pytest.approx(expected)
    assert actual != pytest.approx(252.0)


def test_crypto_weekly_factor_is_not_rounded_to_int() -> None:
    """365/7 does not divide evenly; rounding would inject avoidable bias."""
    actual = annualization_factor_for(ManualMarket.CRYPTO_SPOT, OhlcvTimeframe("1w"))
    assert actual == pytest.approx(365.0 / 7.0)
    assert actual != int(actual)


def test_crypto_factor_matches_calendar_seconds_identity() -> None:
    tf = OhlcvTimeframe("6h")
    assert annualization_factor_for(ManualMarket.CRYPTO_SPOT, tf) == pytest.approx(
        CALENDAR_SECONDS_PER_YEAR / timeframe_seconds(tf)
    )


# ---------------------------------------------------------------------------
# annualization_factor_for — equities (non-continuous)
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "market",
    [ManualMarket.A_SHARE, ManualMarket.US_STOCK, ManualMarket.HK_STOCK],
)
def test_equity_daily_uses_trading_days(market: ManualMarket) -> None:
    assert annualization_factor_for(market, OhlcvTimeframe("1d")) == pytest.approx(
        float(EQUITY_TRADING_DAYS_PER_YEAR)
    )


def test_equity_weekly_scales_trading_days() -> None:
    assert annualization_factor_for(ManualMarket.US_STOCK, OhlcvTimeframe("1w")) == pytest.approx(
        EQUITY_TRADING_DAYS_PER_YEAR / 7.0
    )


@pytest.mark.parametrize("timeframe", ["1m", "15m", "1h", "4h"])
def test_equity_intraday_refuses_to_guess(timeframe: str) -> None:
    """Fail-closed: intraday equity bars need a trading calendar this module does not have.

    Guessing a number here would silently produce wrong Sharpe values — the exact failure
    mode this whole module exists to eliminate.
    """
    with pytest.raises(ValueError, match="不是 7×24 连续交易"):
        annualization_factor_for(ManualMarket.A_SHARE, OhlcvTimeframe(timeframe))


# ---------------------------------------------------------------------------
# annualization_factor_for_records
# ---------------------------------------------------------------------------


def _rec(day: int, price: float, *, timeframe: str = "1d") -> NormalizedOhlcvRecord:
    return NormalizedOhlcvRecord(
        market=ManualMarket.CRYPTO_SPOT,
        symbol=_SYMBOL,
        timeframe=OhlcvTimeframe(timeframe),
        event_time_utc=_BASE_DT + timedelta(days=day),
        open_price=Decimal(str(price)),
        high_price=Decimal(str(price)),
        low_price=Decimal(str(price)),
        close_price=Decimal(str(price)),
        volume=Decimal(1000),
    )


def test_factor_for_records_derives_from_first_record() -> None:
    records = [_rec(i, 100.0 + i) for i in range(5)]
    assert annualization_factor_for_records(records) == pytest.approx(365.0)


def test_factor_for_records_rejects_empty() -> None:
    with pytest.raises(ValueError, match="records 不能为空"):
        annualization_factor_for_records([])


# ---------------------------------------------------------------------------
# End-to-end regression — this is the test that actually pins the bug fix
# ---------------------------------------------------------------------------


def _signals(records: list[NormalizedOhlcvRecord], values: list[float]) -> pd.Series[Any]:
    index = pd.DatetimeIndex([r.event_time_utc for r in records])
    return pd.Series(values, index=index, dtype=float)


def _volatile_run(annualization_factor: float | None) -> Any:
    """Run the same backtest twice differing only in annualization factor.

    Prices alternate so returns have non-zero dispersion — a flat series would give
    std_ret == 0 and short-circuit sharpe_ratio to 0.0, making the comparison vacuous.
    """
    prices = [100.0, 104.0, 99.0, 107.0, 101.0, 109.0, 103.0, 111.0]
    records = [_rec(i, p) for i, p in enumerate(prices)]
    signals = _signals(records, [1.0] * len(records))
    config = BacktestConfig(
        initial_capital=100_000.0,
        fee_bps=0.0,
        slippage_bps=0.0,
        annualization_factor=annualization_factor,
    )
    return run_vectorized_backtest(config, records, signals)


def test_engine_auto_derives_crypto_factor_instead_of_252() -> None:
    """Sharpe scales with sqrt(factor); auto-derived 365 must differ from the old 252.

    This is the end-to-end proof that the derived factor actually reaches compute_metrics()
    rather than being computed and then dropped on the floor.
    """
    auto = _volatile_run(None)
    legacy = _volatile_run(252.0)

    assert auto.metrics.sharpe_ratio != pytest.approx(legacy.metrics.sharpe_ratio)
    assert auto.metrics.sharpe_ratio == pytest.approx(
        legacy.metrics.sharpe_ratio * math.sqrt(365.0 / 252.0)
    )
    # The old default understated Sharpe for crypto — confirm the direction of the error.
    assert abs(auto.metrics.sharpe_ratio) > abs(legacy.metrics.sharpe_ratio)


def test_explicit_annualization_factor_still_overrides_derivation() -> None:
    explicit = _volatile_run(252.0)
    assert explicit.config.annualization_factor == 252.0
    # Recompute what 252 implies, independent of the engine, to confirm the override took.
    auto = _volatile_run(None)
    assert explicit.metrics.sharpe_ratio == pytest.approx(
        auto.metrics.sharpe_ratio * math.sqrt(252.0 / 365.0)
    )


def test_annualized_return_also_reflects_derived_factor() -> None:
    auto = _volatile_run(None)
    legacy = _volatile_run(252.0)
    assert auto.metrics.annualized_return != pytest.approx(legacy.metrics.annualized_return)


def test_config_rejects_non_positive_explicit_factor() -> None:
    with pytest.raises(ValueError, match="annualization_factor 必须为正数"):
        BacktestConfig(initial_capital=1000.0, annualization_factor=0.0)


def test_config_accepts_none_meaning_auto_derive() -> None:
    assert BacktestConfig(initial_capital=1000.0).annualization_factor is None
