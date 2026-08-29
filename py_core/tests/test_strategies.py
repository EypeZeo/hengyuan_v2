"""Unit tests for py_core.strategies (P2-STRAT-01).

All outputs are backtesting estimates only.
NOT financial advice. NOT trading authorization.
"""

from __future__ import annotations

from datetime import UTC, datetime, timedelta
from decimal import Decimal
from typing import Any

import pandas as pd
import pytest

from py_core.backtests.models import BacktestConfig
from py_core.backtests.vectorized_engine import (
    records_to_dataframe,
    run_vectorized_backtest,
)
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)
from py_core.strategies.base import (
    Strategy,
    assert_no_lookahead,
    load_strategy,
    validate_signal_output,
)
from py_core.strategies.momentum import MomentumStrategy
from py_core.strategies.sma_crossover import SmaCrossoverStrategy

_BASE_DT = datetime(2024, 1, 1, tzinfo=UTC)
_MARKET = ManualMarket.CRYPTO_SPOT
_SYMBOL = CanonicalMarketSymbol("BTC/USDT")
_TF = OhlcvTimeframe("1d")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _rec(day: int, open_p: float, *, close_p: float | None = None, volume: float = 1000.0) -> NormalizedOhlcvRecord:
    """Build a single NormalizedOhlcvRecord for testing (mirrors
    test_vectorized_backtest_engine.py's own _rec() helper)."""
    _c = close_p if close_p is not None else open_p
    _h = max(open_p, _c)
    _l = min(open_p, _c)
    return NormalizedOhlcvRecord(
        market=_MARKET,
        symbol=_SYMBOL,
        timeframe=_TF,
        event_time_utc=_BASE_DT + timedelta(days=day),
        open_price=Decimal(str(open_p)),
        high_price=Decimal(str(_h)),
        low_price=Decimal(str(_l)),
        close_price=Decimal(str(_c)),
        volume=Decimal(str(volume)),
    )


def _cfg(**kwargs: Any) -> BacktestConfig:
    defaults: dict[str, Any] = {
        "initial_capital": 100_000.0,
        "fee_bps": 0.0,
        "slippage_bps": 0.0,
    }
    defaults.update(kwargs)
    return BacktestConfig(**defaults)


class _CheatingStrategy(Strategy):
    """Deliberately looks at the NEXT bar's close via shift(-1) -- exists only
    to prove assert_no_lookahead() actually catches this, not a real strategy."""

    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        future_close = df["close"].shift(-1)
        signal = (future_close > df["close"]).astype(float)
        return signal.mask(future_close.isna(), other=float("nan"))


# ---------------------------------------------------------------------------
# SmaCrossoverStrategy
# ---------------------------------------------------------------------------


def test_sma_crossover_signal_shape_and_warmup() -> None:
    closes = [100, 100, 100, 100, 200, 200, 200, 200]
    records = [_rec(i, c, close_p=c) for i, c in enumerate(closes)]
    df = records_to_dataframe(records)

    strategy = SmaCrossoverStrategy(fast_window=2, slow_window=4)
    signal = strategy.generate_signals(df)

    assert signal.index.equals(df.index)
    # warm-up: slow_window=4 需要至少 4 根 bar，前 3 根应为 NaN
    assert signal.iloc[:3].isna().all()
    assert not signal.iloc[3:].isna().any()
    assert signal.iloc[3] == 0.0  # fast==slow==100，未上穿
    assert signal.iloc[4] == 1.0  # fast(150) > slow(125)，上穿


def test_sma_crossover_rejects_fast_gte_slow() -> None:
    with pytest.raises(ValueError):
        SmaCrossoverStrategy(fast_window=10, slow_window=10)
    with pytest.raises(ValueError):
        SmaCrossoverStrategy(fast_window=20, slow_window=10)


def test_sma_crossover_rejects_non_positive_windows() -> None:
    with pytest.raises(ValueError):
        SmaCrossoverStrategy(fast_window=0, slow_window=10)
    with pytest.raises(ValueError):
        SmaCrossoverStrategy(fast_window=5, slow_window=-1)


# ---------------------------------------------------------------------------
# MomentumStrategy
# ---------------------------------------------------------------------------


def test_momentum_signal_direction() -> None:
    closes = [100 + i * 10 for i in range(10)]  # 单调上涨
    records = [_rec(i, c, close_p=c) for i, c in enumerate(closes)]
    df = records_to_dataframe(records)

    strategy = MomentumStrategy(lookback=3)
    signal = strategy.generate_signals(df)

    assert signal.iloc[:3].isna().all()
    assert (signal.iloc[3:] == 1.0).all()


def test_momentum_rejects_non_positive_lookback() -> None:
    with pytest.raises(ValueError):
        MomentumStrategy(lookback=0)
    with pytest.raises(ValueError):
        MomentumStrategy(lookback=-5)


# ---------------------------------------------------------------------------
# validate_signal_output()
# ---------------------------------------------------------------------------


def test_validate_signal_output_allows_nan_rejects_inf_and_misaligned_index() -> None:
    records = [_rec(i, 100.0, close_p=100.0) for i in range(5)]
    df = records_to_dataframe(records)

    ok_signal = pd.Series([float("nan"), 0.0, 1.0, 0.0, 1.0], index=df.index)
    validate_signal_output(ok_signal, df)  # 不抛错

    inf_signal = ok_signal.copy()
    inf_signal.iloc[0] = float("inf")
    with pytest.raises(ValueError):
        validate_signal_output(inf_signal, df)

    misaligned = pd.Series([0.0] * 5, index=df.index[::-1])
    with pytest.raises(ValueError):
        validate_signal_output(misaligned, df)

    with pytest.raises(TypeError):
        validate_signal_output([0.0, 1.0, 0.0, 1.0, 0.0], df)  # 不是 Series

    with pytest.raises(TypeError):
        validate_signal_output(None, df)


# ---------------------------------------------------------------------------
# assert_no_lookahead()
# ---------------------------------------------------------------------------


def test_assert_no_lookahead_catches_shift_negative_one() -> None:
    closes = [100, 105, 95, 110, 90, 120]
    records = [_rec(i, c, close_p=c) for i, c in enumerate(closes)]
    df = records_to_dataframe(records)

    cheater = _CheatingStrategy()
    with pytest.raises(ValueError):
        assert_no_lookahead(cheater, df, sample_points=[1, 2, 3])


def test_assert_no_lookahead_passes_for_honest_strategy() -> None:
    closes = [100, 100, 100, 100, 200, 200, 200, 200]
    records = [_rec(i, c, close_p=c) for i, c in enumerate(closes)]
    df = records_to_dataframe(records)

    strategy = SmaCrossoverStrategy(fast_window=2, slow_window=4)
    # 不抛错即通过 -- 证明这个核验工具本身不会对诚实的策略误报。
    assert_no_lookahead(strategy, df, sample_points=[3, 4, 5, 6, 7])


def test_assert_no_lookahead_rejects_out_of_range_sample_point() -> None:
    closes = [100, 105, 95]
    records = [_rec(i, c, close_p=c) for i, c in enumerate(closes)]
    df = records_to_dataframe(records)

    strategy = MomentumStrategy(lookback=1)
    with pytest.raises(ValueError):
        assert_no_lookahead(strategy, df, sample_points=[99])


# ---------------------------------------------------------------------------
# load_strategy()
# ---------------------------------------------------------------------------


def test_load_strategy_valid_spec_with_params() -> None:
    strategy = load_strategy(
        "py_core.strategies.sma_crossover:SmaCrossoverStrategy",
        fast_window=5,
        slow_window=10,
    )
    assert isinstance(strategy, SmaCrossoverStrategy)
    assert strategy.fast_window == 5
    assert strategy.slow_window == 10


def test_load_strategy_rejects_malformed_spec() -> None:
    with pytest.raises(ValueError):
        load_strategy("py_core.strategies.sma_crossover.SmaCrossoverStrategy")  # 缺 ":"
    with pytest.raises(ValueError):
        load_strategy(":SmaCrossoverStrategy")
    with pytest.raises(ValueError):
        load_strategy("py_core.strategies.sma_crossover:")


def test_load_strategy_rejects_non_strategy_class() -> None:
    with pytest.raises(TypeError):
        load_strategy("py_core.backtests.models:BacktestConfig", allow_external=True)


def test_load_strategy_rejects_external_namespace_by_default() -> None:
    with pytest.raises(PermissionError):
        load_strategy("py_core.backtests.models:BacktestConfig")
    # allow_external=True 放开命名空间限制之后，仍然会因为不是 Strategy 子类而 TypeError
    # -- 证明命名空间检查跟"是不是 Strategy 子类"检查是两道独立的关卡。
    with pytest.raises(TypeError):
        load_strategy("py_core.backtests.models:BacktestConfig", allow_external=True)


# ---------------------------------------------------------------------------
# End-to-end: 策略产出直接喂给既有引擎
# ---------------------------------------------------------------------------


def test_strategy_output_feeds_vectorized_backtest_end_to_end() -> None:
    closes = [100, 100, 100, 100, 200, 200, 200, 200]
    records = [_rec(i, c, close_p=c) for i, c in enumerate(closes)]

    strategy = SmaCrossoverStrategy(fast_window=2, slow_window=4)
    df = records_to_dataframe(records)
    signal = strategy.generate_signals(df)

    result = run_vectorized_backtest(_cfg(), records, signal)
    assert result.validation_report.is_valid
    assert result.validation_report.bar_count == len(records)
    assert len(result.equity_curve) == len(records)
