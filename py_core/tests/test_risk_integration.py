"""
tests/unit/research/backtests/test_risk_integration.py

P2-RM-02 风险感知回测集成单元测试。

所有测试均为研究用途验证，不代表交易授权、实盘结果或干跑批准。
"""

from __future__ import annotations

from datetime import UTC, datetime
from decimal import Decimal
from typing import Any

import pandas as pd
import pytest
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)
from py_core.backtests.models import BacktestConfig
from py_core.backtests.risk_integration import (
    RiskAwareBacktestResult,
    _signal_value_to_target,
    run_risk_aware_backtest,
)
from py_core.risk.risk_config import RiskConfig
from py_core.risk.risk_decision_artifact import RiskDecisionStatus

# ─────────────────────────────────────────────── 测试数据工厂 ──────────────────


def _rec(
    day: int,
    open_p: float,
    *,
    close_p: float,
    high_p: float | None = None,
    low_p: float | None = None,
    volume: float = 1000.0,
) -> NormalizedOhlcvRecord:
    """构造一条 NormalizedOhlcvRecord，便于测试使用。"""
    ts = datetime(2024, 1, day, tzinfo=UTC)
    return NormalizedOhlcvRecord(
        market=ManualMarket.CRYPTO_SPOT,
        symbol=CanonicalMarketSymbol("BTC/USDT"),
        timeframe=OhlcvTimeframe("1d"),
        event_time_utc=ts,
        open_price=Decimal(str(open_p)),
        high_price=Decimal(str(high_p if high_p is not None else open_p)),
        low_price=Decimal(str(low_p if low_p is not None else open_p)),
        close_price=Decimal(str(close_p)),
        volume=Decimal(str(volume)),
    )


def _make_signals(records: list[NormalizedOhlcvRecord], values: list[float]) -> pd.Series:
    """构造与 records 时间轴对齐的 signal pd.Series。"""
    idx = pd.DatetimeIndex([r.event_time_utc for r in records])
    return pd.Series(values, index=idx, dtype=float)


def _cfg(**kwargs: Any) -> BacktestConfig:
    """BacktestConfig 快捷构造，默认 initial_capital=10000。"""
    defaults: dict = {"initial_capital": 10_000.0, "fee_bps": 0.0, "slippage_bps": 0.0}
    defaults.update(kwargs)
    return BacktestConfig(**defaults)


def _risk_cfg(**kwargs: Any) -> RiskConfig:
    """RiskConfig 快捷构造，默认宽松风险参数。"""
    defaults: dict = {
        "risk_fraction": Decimal("0.5"),
        "max_position_fraction": Decimal("1.0"),
        "max_notional": Decimal("1000000"),
        "max_risk_per_trade": Decimal("1000000"),
    }
    defaults.update(kwargs)
    return RiskConfig(**defaults)


# ──────────────────────────────────── 辅助函数测试 ─────────────────────────────


class TestSignalValueToTarget:
    """_signal_value_to_target 辅助函数测试。"""

    def test_one_maps_to_long(self) -> None:
        assert _signal_value_to_target(1.0) == "long"

    def test_zero_maps_to_none(self) -> None:
        assert _signal_value_to_target(0.0) == "none"

    def test_half_maps_to_long(self) -> None:
        # 0.5 边界：>= 0.5 → "long"
        assert _signal_value_to_target(0.5) == "long"

    def test_just_below_half_maps_to_none(self) -> None:
        assert _signal_value_to_target(0.49) == "none"

    def test_negative_signal_maps_to_none_not_long(self) -> None:
        # V1 不支持空头：负向 / short-intent 信号必须映射为 "none"（flat），
        # 绝不能被静默当作 "long" 处理。
        assert _signal_value_to_target(-1.0) == "none"
        assert _signal_value_to_target(-0.5) == "none"


# ─────────────────────────────── RiskAwareBacktestResult 不变量测试 ────────────


class TestRiskAwareBacktestResultNonAuthorizing:
    """non_authorizing 不变量测试。"""

    def test_non_authorizing_true_by_default(self) -> None:
        """non_authorizing 默认为 True，不允许 False。"""
        records = [_rec(1, 100.0, close_p=105.0), _rec(2, 105.0, close_p=110.0)]
        signals = _make_signals(records, [1.0, 0.0])
        result = run_risk_aware_backtest(_cfg(), records, signals, _risk_cfg())
        assert result.non_authorizing is True

    def test_non_authorizing_false_raises(self) -> None:
        """直接构造 non_authorizing=False 必须抛出 ValueError。"""
        records = [_rec(1, 100.0, close_p=105.0), _rec(2, 105.0, close_p=110.0)]
        signals = _make_signals(records, [1.0, 0.0])
        base = run_risk_aware_backtest(_cfg(), records, signals, _risk_cfg())
        with pytest.raises(ValueError, match="non_authorizing"):
            RiskAwareBacktestResult(
                base_result=base.base_result,
                risk_equity_curve=base.risk_equity_curve,
                risk_returns=base.risk_returns,
                risk_positions_fraction=base.risk_positions_fraction,
                risk_decisions=base.risk_decisions,
                risk_metrics=base.risk_metrics,
                timestamps=base.timestamps,
                non_authorizing=False,  # ← 违反不变量
            )


# ─────────────────────────────── 全空仓信号（all-zero）─────────────────────────


class TestAllZeroSignals:
    """信号全为 0（全空仓）时的行为。"""

    def test_all_decisions_rejected(self) -> None:
        records = [
            _rec(1, 100.0, close_p=105.0),
            _rec(2, 105.0, close_p=110.0),
            _rec(3, 110.0, close_p=115.0),
        ]
        signals = _make_signals(records, [0.0, 0.0, 0.0])
        result = run_risk_aware_backtest(_cfg(), records, signals, _risk_cfg())

        for dec in result.risk_decisions:
            assert dec.status == RiskDecisionStatus.REJECTED

    def test_all_exposure_fractions_zero(self) -> None:
        records = [
            _rec(1, 100.0, close_p=105.0),
            _rec(2, 105.0, close_p=110.0),
            _rec(3, 110.0, close_p=115.0),
        ]
        signals = _make_signals(records, [0.0, 0.0, 0.0])
        result = run_risk_aware_backtest(_cfg(), records, signals, _risk_cfg())

        for frac in result.risk_positions_fraction:
            assert frac == 0.0

    def test_equity_stays_flat_with_no_cost(self) -> None:
        records = [
            _rec(1, 100.0, close_p=105.0),
            _rec(2, 105.0, close_p=110.0),
            _rec(3, 110.0, close_p=115.0),
        ]
        signals = _make_signals(records, [0.0, 0.0, 0.0])
        result = run_risk_aware_backtest(
            _cfg(initial_capital=10_000.0), records, signals, _risk_cfg()
        )

        # 无持仓 → 净值不变
        assert result.risk_equity_curve[-1] == pytest.approx(10_000.0)


# ───────────────────────────────── 全多头信号 ──────────────────────────────────


class TestAllLongSignals:
    """信号全为 1（全多头）时，risk 感知净值应随价格正向波动。"""

    def test_all_accepted_or_capped(self) -> None:
        records = [
            _rec(1, 100.0, close_p=101.0),
            _rec(2, 102.0, close_p=103.0),
            _rec(3, 104.0, close_p=105.0),
        ]
        signals = _make_signals(records, [1.0, 1.0, 1.0])
        result = run_risk_aware_backtest(_cfg(), records, signals, _risk_cfg())

        # 首 bar 因 shift 仍为空仓（no-lookahead），从第 2 bar 起为多头
        # 第 1 bar: positions[0] = signals[-1] = 0 → REJECTED
        assert result.risk_decisions[0].status == RiskDecisionStatus.REJECTED
        # 第 2/3 bar: positions[1/2] = signals[0/1] = 1.0 → ACCEPTED or CAPPED
        for dec in result.risk_decisions[1:]:
            assert dec.status in (RiskDecisionStatus.ACCEPTED, RiskDecisionStatus.CAPPED)

    def test_equity_grows_in_uptrend(self) -> None:
        """单调上涨价格序列下，风险调整净值应高于初始资本。"""
        records = [
            _rec(1, 100.0, close_p=100.0),
            _rec(2, 110.0, close_p=110.0),
            _rec(3, 121.0, close_p=121.0),
            _rec(4, 133.0, close_p=133.0),
        ]
        signals = _make_signals(records, [1.0, 1.0, 1.0, 1.0])
        result = run_risk_aware_backtest(
            _cfg(initial_capital=10_000.0, fee_bps=0.0), records, signals, _risk_cfg()
        )
        assert result.risk_equity_curve[-1] > 10_000.0


# ─────────────────────────────── max_notional 上限测试 ─────────────────────────


class TestMaxNotionalCap:
    """max_notional 过小时应触发 CAPPED 且持仓比例受限。"""

    def test_capped_by_max_notional(self) -> None:
        records = [
            _rec(1, 100.0, close_p=100.0),
            _rec(2, 100.0, close_p=100.0),
            _rec(3, 100.0, close_p=100.0),
        ]
        signals = _make_signals(records, [1.0, 1.0, 1.0])
        # risk_fraction=50% → proposed = 5000，但 max_notional=100 → CAPPED
        tight_risk = _risk_cfg(
            risk_fraction=Decimal("0.5"),
            max_notional=Decimal("100"),
        )
        result = run_risk_aware_backtest(
            _cfg(initial_capital=10_000.0), records, signals, tight_risk
        )

        for dec in result.risk_decisions[1:]:
            assert dec.status == RiskDecisionStatus.CAPPED

        # exposure fraction 应远小于 0.5
        for frac in result.risk_positions_fraction[1:]:
            assert frac < 0.5


# ─────────────────────────────── max_position_fraction 上限测试 ────────────────


class TestMaxPositionFractionCap:
    """max_position_fraction 设为 0.1 时，exposure_fraction 不得超过 0.1。"""

    def test_capped_by_max_position_fraction(self) -> None:
        records = [
            _rec(1, 100.0, close_p=100.0),
            _rec(2, 100.0, close_p=100.0),
            _rec(3, 100.0, close_p=100.0),
        ]
        signals = _make_signals(records, [1.0, 1.0, 1.0])
        tight_risk = _risk_cfg(
            risk_fraction=Decimal("0.9"),
            max_position_fraction=Decimal("0.1"),  # 强制上限 10%
            max_notional=Decimal("9999999"),
        )
        result = run_risk_aware_backtest(
            _cfg(initial_capital=10_000.0), records, signals, tight_risk
        )

        for frac in result.risk_positions_fraction[1:]:
            assert frac <= 0.11  # 允许 1% 浮点误差空间


# ────────────────────────────────── stop_distance 测试 ────────────────────────


class TestStopDistance:
    """stop_distance 激活 max_risk_per_trade 上限。"""

    def test_stop_distance_limits_position(self) -> None:
        records = [
            _rec(1, 100.0, close_p=100.0),
            _rec(2, 200.0, close_p=200.0),
            _rec(3, 200.0, close_p=200.0),
        ]
        signals = _make_signals(records, [1.0, 1.0, 1.0])
        # max_risk_per_trade=50, stop_distance=50 → max_notional_by_risk=1
        risk_cfg = _risk_cfg(
            risk_fraction=Decimal("0.5"),
            max_risk_per_trade=Decimal("50"),
            max_notional=Decimal("9999999"),
        )
        result = run_risk_aware_backtest(
            _cfg(initial_capital=10_000.0),
            records,
            signals,
            risk_cfg,
            stop_distance=Decimal("50"),
        )
        # max_notional_by_risk = 50/50 = 1；capped_notional=1，exposure=1/10000
        for frac in result.risk_positions_fraction[1:]:
            assert frac < 0.01


# ──────────────────────────────── 无 lookahead 回归测试 ───────────────────────


class TestNoLookahead:
    """验证 bar 0 永远为空仓（shift(1) 导致首 bar 的 positions=0）。"""

    def test_first_bar_always_flat(self) -> None:
        """无论信号如何，第 0 bar 的 positions[0] 始终为 0（空仓）。"""
        records = [
            _rec(1, 100.0, close_p=100.0),
            _rec(2, 110.0, close_p=110.0),
            _rec(3, 120.0, close_p=120.0),
        ]
        signals = _make_signals(records, [1.0, 1.0, 1.0])
        result = run_risk_aware_backtest(_cfg(), records, signals, _risk_cfg())

        # bar 0 的仓位应为 0（shift(1) 结果）
        assert result.risk_positions_fraction[0] == 0.0
        assert result.risk_decisions[0].status == RiskDecisionStatus.REJECTED

    def test_signal_t_executes_at_t_plus_1(self) -> None:
        """验证 bar T 信号在 bar T+1 执行：仅第 1 bar 持仓（由 signals[0]=1 驱动）。"""
        records = [
            _rec(1, 100.0, close_p=100.0),
            _rec(2, 110.0, close_p=110.0),
            _rec(3, 110.0, close_p=110.0),
        ]
        # 只有 signals[0]=1，后续为 0
        signals = _make_signals(records, [1.0, 0.0, 0.0])
        result = run_risk_aware_backtest(_cfg(), records, signals, _risk_cfg())

        # bar 0: positions[0]=shift → 0 → 空仓
        assert result.risk_positions_fraction[0] == 0.0
        # bar 1: positions[1]=signals[0]=1 → 多头
        assert result.risk_positions_fraction[1] > 0.0
        # bar 2: positions[2]=signals[1]=0 → 空仓
        assert result.risk_positions_fraction[2] == 0.0


# ────────────────────────────────── 指标一致性测试 ────────────────────────────


class TestMetricsConsistency:
    """风险调整指标的内部一致性检验。"""

    def test_metrics_len_matches_equity_curve(self) -> None:
        records = [
            _rec(1, 100.0, close_p=100.0),
            _rec(2, 105.0, close_p=105.0),
            _rec(3, 110.0, close_p=110.0),
        ]
        signals = _make_signals(records, [1.0, 1.0, 0.0])
        result = run_risk_aware_backtest(_cfg(), records, signals, _risk_cfg())

        assert len(result.risk_equity_curve) == len(records)
        assert len(result.risk_returns) == len(records)
        assert len(result.risk_positions_fraction) == len(records)
        assert len(result.risk_decisions) == len(records)
        assert len(result.timestamps) == len(records)

    def test_base_result_present_and_unchanged(self) -> None:
        """base_result 应与单独运行 P2-BT-01 得到的结果一致。"""
        from py_core.backtests.vectorized_engine import run_vectorized_backtest

        records = [
            _rec(1, 100.0, close_p=100.0),
            _rec(2, 110.0, close_p=110.0),
            _rec(3, 110.0, close_p=110.0),
        ]
        signals = _make_signals(records, [1.0, 1.0, 0.0])
        cfg = _cfg(fee_bps=5.0)

        result = run_risk_aware_backtest(cfg, records, signals, _risk_cfg())
        standalone = run_vectorized_backtest(cfg, records, signals)

        assert result.base_result.metrics.total_return == pytest.approx(
            standalone.metrics.total_return, abs=1e-9
        )

    def test_total_return_flat_market_no_cost(self) -> None:
        """价格不变、无费用、全空仓 → total_return ≈ 0。"""
        records = [_rec(i, 100.0, close_p=100.0) for i in range(1, 6)]
        signals = _make_signals(records, [0.0] * 5)
        result = run_risk_aware_backtest(
            _cfg(initial_capital=10_000.0, fee_bps=0.0), records, signals, _risk_cfg()
        )
        assert result.risk_metrics.total_return == pytest.approx(0.0, abs=1e-9)

    def test_output_label_non_authorizing(self) -> None:
        records = [_rec(1, 100.0, close_p=100.0), _rec(2, 100.0, close_p=100.0)]
        signals = _make_signals(records, [0.0, 0.0])
        result = run_risk_aware_backtest(_cfg(), records, signals, _risk_cfg())

        assert "NON-AUTHORIZING" in result.output_label.upper()
        assert result.non_authorizing is True


# ─────────────────────────────────────── 空 records 错误 ──────────────────────


class TestEmptyRecordsError:
    def test_raises_on_empty_records(self) -> None:
        with pytest.raises(ValueError, match="records"):
            run_risk_aware_backtest(
                _cfg(),
                [],
                pd.Series([], dtype=float),
                _risk_cfg(),
            )


# ─────────────────────────────────── 混合信号测试 ─────────────────────────────


class TestMixedSignals:
    """混合信号（0/1 交替）时，每 bar 决策独立且无状态污染。"""

    def test_alternating_signals_produce_correct_pattern(self) -> None:
        records = [
            _rec(1, 100.0, close_p=100.0),
            _rec(2, 100.0, close_p=100.0),
            _rec(3, 100.0, close_p=100.0),
            _rec(4, 100.0, close_p=100.0),
            _rec(5, 100.0, close_p=100.0),
        ]
        # 信号：0, 1, 0, 1, 0
        signals = _make_signals(records, [0.0, 1.0, 0.0, 1.0, 0.0])
        result = run_risk_aware_backtest(_cfg(), records, signals, _risk_cfg())

        # bar 0: shift → 0 → flat
        assert result.risk_positions_fraction[0] == 0.0
        # bar 1: signals[0]=0 → flat
        assert result.risk_positions_fraction[1] == 0.0
        # bar 2: signals[1]=1 → long
        assert result.risk_positions_fraction[2] > 0.0
        # bar 3: signals[2]=0 → flat
        assert result.risk_positions_fraction[3] == 0.0
        # bar 4: signals[3]=1 → long
        assert result.risk_positions_fraction[4] > 0.0
