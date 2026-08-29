"""
风险感知回测集成 — P2-RM-02

将 RiskCalculator（P2-RM-01）集成到向量化 OHLCV 回测路径（P2-BT-01）中。
每 bar 调用 RiskCalculator，得到 RiskDecisionArtifact，并用 capped_notional
驱动净值曲线算术。

执行语义与 P2-BT-01 保持一致：bar T 信号 → bar T+1 open 成交（next-bar open，无 lookahead）。

所有输出均为研究用途估算值，不具授权效力。
本模块不授权任何交易下单、paper trading、dry-run 或 live 执行。
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from decimal import Decimal
from typing import Any

import pandas as pd

from py_core.backtests.metrics import compute_metrics
from py_core.backtests.models import (
    NON_AUTH_ASSERTION,
    BacktestConfig,
    BacktestMetrics,
    BacktestResult,
)
from py_core.backtests.vectorized_engine import (
    _run_vectorized_backtest_on_df,
    records_to_dataframe,
    resolve_annualization_factor,
    validate_inputs,
)
from py_core.manual_ohlcv import NormalizedOhlcvRecord
from py_core.risk.risk_calculator import RiskCalculator
from py_core.risk.risk_config import RiskConfig
from py_core.risk.risk_decision_artifact import RiskDecisionArtifact, RiskDecisionStatus

_RISK_INTEGRATION_LABEL = "risk-aware backtest estimates only — NON-AUTHORIZING RESEARCH USE ONLY"


@dataclass
class RiskAwareBacktestResult:
    """风险感知回测完整结果。

    所有输出均为研究用途的回测估算值。
    non_authorizing 恒为 True，不代表财务建议、交易授权、
    策略批准、paper trading、dry-run 批准或实盘批准。
    """

    base_result: BacktestResult
    """原始 P2-BT-01 结果（不含风险调整，供对比用）。"""

    risk_equity_curve: list[float]
    """每 bar 末的风险调整净值（绝对值）。"""

    risk_returns: list[float]
    """每 bar 的风险调整净收益率（简单收益）。"""

    risk_positions_fraction: list[float]
    """每 bar 的实际暴露比例（capped_notional / equity，∈ [0, 1]）。"""

    risk_decisions: list[RiskDecisionArtifact]
    """每 bar 的 RiskDecisionArtifact（含 status、cap_reason、warnings）。"""

    risk_metrics: BacktestMetrics
    """基于风险调整净值曲线计算的绩效指标。"""

    timestamps: list[datetime]
    """与每 bar 对应的时间戳（UTC）。"""

    non_authorizing: bool = True
    output_label: str = _RISK_INTEGRATION_LABEL
    non_auth_assertion: str = NON_AUTH_ASSERTION

    def __post_init__(self) -> None:
        if not self.non_authorizing:
            raise ValueError(
                "non_authorizing 必须为 True：RiskAwareBacktestResult 是研究专用非授权制品"
            )


def _signal_value_to_target(position_value: float) -> str:
    """将数值仓位（shift 后）映射到 RiskCalculator 所需的 signal_target 字符串。

    - position_value >= 0.5 → ``"long"``
    - 其余 → ``"none"``

    V1 不支持 short；short 信号在 RiskCalculator 内部会 REJECT。

    Args:
        position_value: shift(1) 后的仓位值，通常为 0.0 或 1.0。

    Returns:
        ``"long"`` 或 ``"none"``。
    """
    if position_value >= 0.5:
        return "long"
    return "none"


def run_risk_aware_backtest(
    config: BacktestConfig,
    records: list[NormalizedOhlcvRecord],
    signals: pd.Series[Any],
    risk_config: RiskConfig,
    stop_distance_fraction: Decimal | None = None,
) -> RiskAwareBacktestResult:
    """运行风险感知回测，将 RiskCalculator 集成到逐 bar 仿真中。

    执行语义（与 P2-BT-01 一致）：
    - signal[T] 产生于 bar T 收盘后；
    - 在 bar T+1 的 open 价格执行（positions = signals.shift(1)）；
    - bar T 的 fwd_return = open[T+1] / open[T] - 1；
    - 最后一 bar 无后续 open，fwd_return = 0。

    风险调整逻辑：
    - 在 bar T，根据 positions[T]（即 signal[T-1]）调用::
          RiskCalculator.calculate(
              equity=equity_T,
              price=open_T,
              signal_target=signal[T-1],
              config=risk_config,
              stop_distance_fraction=stop_distance_fraction,
          )
    - exposure_fraction = capped_notional / equity_T（上限 1.0，不启用杠杆）；
    - gross_bar_return = exposure_fraction × fwd_open_return[T]；
    - cost_rate = |exposure_fraction - prev_exposure| × (fee + slippage) bps / 10000；
    - net_bar_return = gross_bar_return - cost_rate；
    - equity_{T+1} = equity_T × (1 + net_bar_return)。

    Args:
        config: 回测配置（初始资本、费率/滑点 bps）。
        records: NormalizedOhlcvRecord 列表（已按时间升序排列）。
        signals: 与 records 时间轴对齐的 pd.Series，值 ∈ {0.0, 1.0}（0=空仓，1=多头）。
        risk_config: 风险参数（risk_fraction、max_position_fraction、max_notional、
                     max_risk_per_trade 等）。
        stop_distance_fraction: 可选止损距离（Decimal），传入后激活 max_risk_per_trade 上限。
                       若为 None，该上限不生效。

    Returns:
        :class:`RiskAwareBacktestResult`，含风险调整净值曲线、逐 bar RiskDecisionArtifact
        列表及绩效指标。``non_authorizing`` 恒为 True。

    Raises:
        ValueError: 输入不合法（空 records、NaN close、非单调 timestamps 等）。
    """
    if not records:
        raise ValueError("records 不能为空")

    # ── 构建 OHLCV DataFrame ───────────────────────────────────────────────
    df = records_to_dataframe(records)

    # 确保 signals 拥有 DatetimeIndex
    signals_work = signals.copy()
    if not isinstance(signals_work.index, pd.DatetimeIndex):
        signals_work.index = pd.DatetimeIndex(signals_work.index)

    signals_aligned: pd.Series[Any] = signals_work.reindex(df.index).fillna(0.0)

    # ── 输入校验 ───────────────────────────────────────────────────────────
    validation_report = validate_inputs(df, signals_aligned)
    if not validation_report.is_valid:
        raise ValueError(f"输入校验失败：{validation_report.issues}")

    # ── 年化因子：显式配置优先，否则从数据的 market+timeframe 推导 ────────
    # 基础回测与风险调整回测必须用同一个因子，否则两份 metrics 不可比。
    annualization_factor = resolve_annualization_factor(config, records)

    # ── 运行基础回测（P2-BT-01，不做修改）────────────────────────────────
    # 复用上面已经构建好的 df，不再让 _run_vectorized_backtest_on_df 内部重新调用
    # records_to_dataframe(records) 构建第二份一样的 DataFrame。
    base_result = _run_vectorized_backtest_on_df(
        config, df, signals, annualization_factor=annualization_factor
    )

    # ── next-bar 仓位（与 P2-BT-01 shift 语义相同）───────────────────────
    positions: pd.Series[Any] = signals_aligned.shift(1).fillna(0.0)

    # ── 前向 open 收益率（与 P2-BT-01 相同）──────────────────────────────
    next_open: pd.Series[Any] = df["open"].shift(-1)
    fwd_open_return: pd.Series[Any] = ((next_open - df["open"]) / df["open"]).fillna(0.0)

    # ── 逐 bar 风险感知仿真 ───────────────────────────────────────────────
    calculator = RiskCalculator()
    total_cost_bps = config.fee_bps + config.slippage_bps

    current_equity: float = config.initial_capital
    prev_exposure_fraction: float = 0.0

    risk_equity_curve: list[float] = []
    risk_returns: list[float] = []
    risk_positions_fraction: list[float] = []
    risk_decisions: list[RiskDecisionArtifact] = []
    cost_impact_total: float = 0.0

    bar_count = len(df)

    for bar_idx in range(bar_count):
        shifted_signal = float(positions.iloc[bar_idx])
        signal_target = _signal_value_to_target(shifted_signal)

        bar_open = float(df["open"].iloc[bar_idx])
        bar_fwd_return = float(fwd_open_return.iloc[bar_idx])

        # RiskCalculator 使用 Decimal 接口
        decision = calculator.calculate(
            equity=Decimal(str(current_equity)),
            price=Decimal(str(bar_open)),
            signal_target=signal_target,
            config=risk_config,
            stop_distance_fraction=stop_distance_fraction,
        )
        risk_decisions.append(decision)

        # 暴露比例：ACCEPTED 或 CAPPED 时用 capped_notional / equity
        if decision.status in (RiskDecisionStatus.ACCEPTED, RiskDecisionStatus.CAPPED):
            raw_fraction = float(decision.capped_position_size) / current_equity
            # V1 不支持杠杆，上限 1.0
            exposure_fraction = min(raw_fraction, 1.0)
        else:
            exposure_fraction = 0.0

        # 成本率：仅在暴露变化时产生（与 P2-BT-01 成本触发逻辑一致）
        exposure_delta = abs(exposure_fraction - prev_exposure_fraction)
        cost_rate = exposure_delta * total_cost_bps / 10_000.0

        # 净收益率
        gross_return = exposure_fraction * bar_fwd_return
        net_return = gross_return - cost_rate

        # 成本金额（用当前净值近似）
        cost_impact_total += cost_rate * current_equity

        # 更新净值
        current_equity = current_equity * (1.0 + net_return)

        risk_returns.append(net_return)
        risk_equity_curve.append(current_equity)
        risk_positions_fraction.append(exposure_fraction)

        prev_exposure_fraction = exposure_fraction

    # ── 时间戳 ─────────────────────────────────────────────────────────────
    timestamps = [ts.to_pydatetime() for ts in df.index]

    # ── 风险调整绩效指标 ───────────────────────────────────────────────────
    risk_metrics = compute_metrics(
        equity_curve=risk_equity_curve,
        returns=risk_returns,
        positions=risk_positions_fraction,
        cost_impact_total=cost_impact_total,
        initial_capital=config.initial_capital,
        annualization_factor=annualization_factor,
        risk_free_rate=config.risk_free_rate,
    )

    return RiskAwareBacktestResult(
        base_result=base_result,
        risk_equity_curve=risk_equity_curve,
        risk_returns=risk_returns,
        risk_positions_fraction=risk_positions_fraction,
        risk_decisions=risk_decisions,
        risk_metrics=risk_metrics,
        timestamps=timestamps,
        non_authorizing=True,
        output_label=_RISK_INTEGRATION_LABEL,
    )
