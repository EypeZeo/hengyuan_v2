"""Unit tests for P2-BT-01 vectorized OHLCV backtest engine.

All outputs are backtesting estimates only.
NOT financial advice. NOT trading authorization.
"""

from __future__ import annotations

import math
from datetime import UTC, datetime, timedelta
from decimal import Decimal
from typing import Any

import pandas as pd

from py_core.backtests.models import BacktestConfig
from py_core.backtests.vectorized_engine import (
    run_vectorized_backtest,
    validate_inputs,
)
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)

_BASE_DT = datetime(2024, 1, 1, tzinfo=UTC)
_MARKET = ManualMarket.CRYPTO_SPOT
_SYMBOL = CanonicalMarketSymbol("BTC/USDT")
_TF = OhlcvTimeframe("1d")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _rec(
    day: int,
    open_p: float,
    *,
    close_p: float | None = None,
    high_p: float | None = None,
    low_p: float | None = None,
    volume: float = 1000.0,
) -> NormalizedOhlcvRecord:
    """Build a single NormalizedOhlcvRecord for testing."""
    _c = close_p if close_p is not None else open_p
    _h = high_p if high_p is not None else max(open_p, _c)
    _l = low_p if low_p is not None else min(open_p, _c)
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


def _make_signals(records: list[NormalizedOhlcvRecord], values: list[float]) -> pd.Series[Any]:
    """Build a signal Series with the same DatetimeIndex as the records."""
    index = pd.DatetimeIndex([r.event_time_utc for r in records])
    return pd.Series(values, index=index, dtype=float)


def _cfg(**kwargs: Any) -> BacktestConfig:
    defaults: dict[str, Any] = {
        "initial_capital": 100_000.0,
        "fee_bps": 0.0,
        "slippage_bps": 0.0,
    }
    defaults.update(kwargs)
    return BacktestConfig(**defaults)


# ---------------------------------------------------------------------------
# R5 — 7 required tests
# ---------------------------------------------------------------------------


def test_zero_cost_buy_and_hold() -> None:
    """持续多头仓位、零成本时 total_return ≈ open[-1] / open[1] − 1。

    引擎使用 next-bar open 执行，因此实际入场在 open[1]（第 1 根 bar 的开盘），
    退出在 open[-1]（最后一根 bar 的开盘），最后一根 bar 的 fwd_return=0。
    """
    # 3 bars: open=[100, 200, 400]
    records = [_rec(0, 100.0), _rec(1, 200.0), _rec(2, 400.0)]
    signals = _make_signals(records, [1.0, 1.0, 1.0])
    cfg = _cfg()

    result = run_vectorized_backtest(cfg, records, signals)

    # positions = [0, 1, 1]（shift+fillna）
    # bar 1: fwd_return = 400/200 - 1 = 1.0
    # bar 0, 2: position=0 or fwd=0 → no gross gain
    # expected equity = [100k, 100k, 200k]
    expected_return = 400.0 / 200.0 - 1.0  # = 1.0
    assert math.isclose(result.metrics.total_return, expected_return, rel_tol=1e-9)
    assert result.output_label == "backtesting estimates only"
    assert len(result.non_auth_assertion) > 0


def test_all_flat_signal() -> None:
    """全零信号时权益曲线保持不变（等于初始资本）。"""
    records = [_rec(0, 100.0), _rec(1, 150.0), _rec(2, 200.0)]
    signals = _make_signals(records, [0.0, 0.0, 0.0])
    cfg = _cfg()

    result = run_vectorized_backtest(cfg, records, signals)

    assert math.isclose(result.metrics.total_return, 0.0, abs_tol=1e-12)
    # 所有权益应等于初始资本
    for eq in result.equity_curve:
        assert math.isclose(eq, cfg.initial_capital, rel_tol=1e-9)


def test_fee_bps_cost_deducted() -> None:
    """有手续费时回报低于零成本基准。"""
    # 3 bars: open=[100, 100, 200] → 持有 bar 1 和 bar 2
    records = [_rec(0, 100.0), _rec(1, 100.0), _rec(2, 200.0)]
    signals = _make_signals(records, [1.0, 1.0, 0.0])

    result_no_fee = run_vectorized_backtest(_cfg(fee_bps=0.0), records, signals)
    result_fee = run_vectorized_backtest(_cfg(fee_bps=100.0), records, signals)

    # 有费用时回报必须更低
    assert result_fee.metrics.total_return < result_no_fee.metrics.total_return
    # 费用成本大于零
    assert result_fee.metrics.cost_impact_bps > 0.0
    assert result_fee.metrics.cost_impact_total > 0.0


def test_max_drawdown_calculation() -> None:
    """已知回撤场景验证 max_drawdown 计算正确（约 50%）。

    权益曲线: [100k, 200k, 100k, 100k] → 峰值 200k，谷值 100k
    max_drawdown = (200k - 100k) / 200k = 0.5
    """
    records = [_rec(0, 100.0), _rec(1, 200.0), _rec(2, 100.0), _rec(3, 100.0)]
    # signals=[1,1,0,0] → positions=[0,1,1,0] after shift
    signals = _make_signals(records, [1.0, 1.0, 0.0, 0.0])
    cfg = _cfg()

    result = run_vectorized_backtest(cfg, records, signals)

    # bar 1: pos=1, fwd_return = 100/200-1 = -0.5 → equity = 100k×(1+1.0)×(1-0.5) = 100k
    # peak at bar 1 equity = 200k
    assert math.isclose(result.metrics.max_drawdown, 0.5, rel_tol=1e-9)


def test_anti_lookahead_same_bar_close_not_used() -> None:
    """验证信号 T 使用的是 open[T+1]，而非 close[T]。

    设计：close 价格与下根 bar 的 open 差异巨大。
    若用了 open 价格，total_return ≈ 1.0；若用了 close 价格，则为 0.0。
    """
    # close=999 与下根 bar open 差异极大（open: 100→150→300）
    records = [
        _rec(0, open_p=100.0, close_p=999.0),
        _rec(1, open_p=150.0, close_p=999.0),
        _rec(2, open_p=300.0, close_p=999.0),
    ]
    signals = _make_signals(records, [1.0, 1.0, 0.0])
    cfg = _cfg()

    result = run_vectorized_backtest(cfg, records, signals)

    # positions=[0,1,1]
    # bar 1: fwd_open_return = open[2]/open[1]-1 = 300/150-1 = 1.0
    # bar 1: fwd_close_return = close[2]/close[1]-1 = 999/999-1 = 0.0 (lookahead 错误路径)
    # 正确时 total_return = 1.0；如果用了 close 则 total_return ≈ 0.0
    assert math.isclose(result.metrics.total_return, 1.0, rel_tol=1e-9), (
        f"expected 1.0 (open-based), got {result.metrics.total_return:.6f}. "
        "Possible lookahead: close prices were used instead of next-bar open."
    )


def test_validation_report_non_monotonic_timestamps() -> None:
    """非单调时间戳时 ValidationReport 报告错误。"""
    dates = [
        _BASE_DT + timedelta(days=1),
        _BASE_DT,  # 时间倒退
    ]
    df = pd.DataFrame(
        {
            "open": [100.0, 110.0],
            "high": [110.0, 120.0],
            "low": [90.0, 100.0],
            "close": [105.0, 115.0],
            "volume": [1000.0, 1000.0],
        },
        index=pd.DatetimeIndex(dates),
    )
    signals: pd.Series[Any] = pd.Series([0.0, 0.0], index=pd.DatetimeIndex(dates))

    vr = validate_inputs(df, signals)

    assert vr.timestamp_monotonic is False
    assert vr.is_valid is False
    assert len(vr.issues) > 0


def test_validation_report_nan_close() -> None:
    """close 列含 NaN 时 ValidationReport 报告错误。"""
    dates = [_BASE_DT, _BASE_DT + timedelta(days=1)]
    df = pd.DataFrame(
        {
            "open": [100.0, 110.0],
            "high": [110.0, 120.0],
            "low": [90.0, 100.0],
            "close": [float("nan"), 115.0],
            "volume": [1000.0, 1000.0],
        },
        index=pd.DatetimeIndex(dates),
    )
    signals: pd.Series[Any] = pd.Series([0.0, 0.0], index=pd.DatetimeIndex(dates))

    vr = validate_inputs(df, signals)

    assert vr.no_nan_close is False
    assert vr.is_valid is False
    assert len(vr.issues) > 0
