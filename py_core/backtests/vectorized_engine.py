"""
向量化 OHLCV 回测引擎 — P2-BT-01

执行模型：bar T 信号 → bar T+1 open 价格成交（next-bar open，无 lookahead）。
线性成本模型：fee_bps + slippage_bps，仅在仓位变化时触发。

所有输出均为回测估算值，不代表财务建议、交易授权或实盘批准。
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

import pandas as pd

from py_core.backtests.annualization import annualization_factor_for_records
from py_core.backtests.metrics import compute_metrics
from py_core.backtests.models import (
    BACKTEST_ESTIMATES_LABEL,
    BacktestConfig,
    BacktestResult,
    TradeRecord,
    ValidationReport,
)
from py_core.manual_ohlcv import NormalizedOhlcvRecord

if TYPE_CHECKING:
    from py_core.strategies.base import CausalEvidence


def records_to_dataframe(records: list[NormalizedOhlcvRecord]) -> pd.DataFrame:
    """将 NormalizedOhlcvRecord 列表转换为 OHLCV DataFrame。

    索引为 event_time_utc（DatetimeIndex），按时间升序排列。

    Args:
        records: NormalizedOhlcvRecord 列表，不得为空。

    Returns:
        columns = [open, high, low, close, volume]，索引为 DatetimeIndex。

    Raises:
        ValueError: records 为空。
    """
    if not records:
        raise ValueError("records 不能为空")

    rows = [
        {
            "event_time_utc": r.event_time_utc,
            "open": float(r.open_price),
            "high": float(r.high_price),
            "low": float(r.low_price),
            "close": float(r.close_price),
            "volume": float(r.volume),
        }
        for r in records
    ]
    df = pd.DataFrame(rows)
    df = df.set_index("event_time_utc")
    df.index = pd.DatetimeIndex(df.index)

    # 必须在 sort_index() 之前检查——排序会把乱序/重复时间戳悄悄"整理好"，导致
    # validate_inputs() 的"timestamp 单调递增"检查在实际入口（run_vectorized_backtest()/
    # run_risk_aware_backtest()，两者都先调用本函数再调用 validate_inputs()）里形同虚设：
    # 无论原始 records 是否乱序/重复，validate_inputs() 拿到的永远是已排序、永远"合法"的
    # df。is_monotonic_increasing 本身允许相等的相邻值（非严格递增），所以额外单独检查
    # has_duplicates 才能真正拒绝重复时间戳，不只是拒绝时间倒退。
    if not df.index.is_monotonic_increasing or df.index.has_duplicates:
        raise ValueError("records 的 event_time_utc 必须严格按时间升序排列且不含重复值（发现乱序或重复时间戳）")

    df = df.sort_index()
    return df


def validate_inputs(
    df: pd.DataFrame,
    signals: pd.Series[Any],
    *,
    causal_evidence: CausalEvidence | None = None,
) -> ValidationReport:
    """执行数据完整性与反 lookahead 校验。

    校验规则：
    1. timestamp 单调递增（无重复、无乱序）。
    2. close 列无 NaN。
    3. signal 的**原始**索引无重复，且每个时间戳都在 OHLCV index 内（整体偏移、时区不一致、
       引用 df 之外的时间戳都会因此被拒绝）。稀疏子集（只给了部分 bar 的信号）允许，其余 bar
       视为空仓。**必须在 reindex 之前调用**：SAFE-05 之前两个引擎都先 reindex 再调用本函数，
       比较的是已被对齐到 df.index 的索引，该项恒为真。

    本函数只做索引/时间戳层面的检查，所以报告里的 causal_check 为 "index_only"；只有调用方
    传入 causal_evidence（信号来自 checked_signals()）时才记为 "prefix_differential"。

    Args:
        df: OHLCV DataFrame（DatetimeIndex）。
        signals: signal Series，**未经 reindex 的原始索引**。
        causal_evidence: checked_signals() 返回的因果证据；None 表示信号没有经过因果门禁。

    Returns:
        ValidationReport，is_valid=False 时 issues 包含具体问题。
    """
    issues: list[str] = []

    # 1. timestamp 单调递增
    timestamp_monotonic = bool(df.index.is_monotonic_increasing)
    if not timestamp_monotonic:
        issues.append("timestamps 不是严格单调递增（可能存在重复或乱序）")

    # 2. close 列无 NaN
    no_nan_close = bool(not df["close"].isna().any())
    if not no_nan_close:
        issues.append("close 列存在 NaN 值")

    # 3. 反 lookahead：signal 的原始索引不得含重复，也不得含 df 之外的时间戳
    no_future_shift_detected = True
    if len(signals) > 0:
        if signals.index.has_duplicates:
            issues.append("signal index 含重复时间戳")
            no_future_shift_detected = False
        else:
            orphans = int((~signals.index.isin(df.index)).sum())
            if orphans:
                issues.append(
                    f"signal index 有 {orphans} 个时间戳不在 OHLCV index 中"
                    "（整体偏移、时区不一致或引用了 df 之外的时间戳，可能是 lookahead 或时间偏移）"
                )
                no_future_shift_detected = False

    if causal_evidence is not None:
        causal_check = "prefix_differential"
        causal_points = causal_evidence.points_compared
        causal_context = causal_evidence.to_context()
    else:
        causal_check = "index_only"
        causal_points = 0
        causal_context = {}

    return ValidationReport(
        timestamp_monotonic=timestamp_monotonic,
        no_nan_close=no_nan_close,
        no_future_shift_detected=no_future_shift_detected,
        bar_count=len(df),
        issues=issues,
        causal_check=causal_check,
        causal_points=causal_points,
        causal_context=causal_context,
    )


def run_vectorized_backtest(
    config: BacktestConfig,
    records: list[NormalizedOhlcvRecord],
    signals: pd.Series[Any],
    *,
    causal_evidence: CausalEvidence | None = None,
) -> BacktestResult:
    """运行单资产向量化回测。

    执行模型（next-bar open，无 lookahead）：
    - signal[T] 产生于 bar T 收盘后
    - 在 bar T+1 的 open 价格执行（positions = signals.shift(1)）
    - bar T 的 fwd_return = open[T+1] / open[T] - 1

    成本模型：
    - 仅在仓位变化时触发：|delta_pos| × (fee_bps + slippage_bps) / 10000
    - 成本从当 bar 净收益中扣除

    Args:
        config: 回测配置（资本、费率等）。
        records: NormalizedOhlcvRecord 列表（已排好时序）。
        signals: pandas Series，索引与 records 时间轴对齐，值 ∈ {0, 1} 或实数仓位。
        causal_evidence: 信号来自 checked_signals() 时传入它返回的证据，报告里的 causal_check
            才会是 "prefix_differential"；不传则只是 "index_only"（只做了索引检查）。

    Returns:
        BacktestResult，含 equity_curve、returns、positions、fills_approx、metrics。

    Raises:
        ValueError: 输入不合法（空 records、NaN close、非单调 timestamps 等）。
    """
    if not records:
        raise ValueError("records 不能为空")

    df = records_to_dataframe(records)
    return _run_vectorized_backtest_on_df(
        config,
        df,
        signals,
        annualization_factor=resolve_annualization_factor(config, records),
        causal_evidence=causal_evidence,
    )


def resolve_annualization_factor(
    config: BacktestConfig,
    records: list[NormalizedOhlcvRecord],
) -> float:
    """决定这次回测用哪个年化因子：显式配置优先，否则从数据的 market+timeframe 推导。

    单独成函数是为了让两个入口（run_vectorized_backtest / run_risk_aware_backtest）和 CLI
    共用同一套优先级规则，而不是各写一遍、各自漂移。
    """
    if config.annualization_factor is not None:
        return float(config.annualization_factor)
    return annualization_factor_for_records(records)


def _run_vectorized_backtest_on_df(
    config: BacktestConfig,
    df: pd.DataFrame,
    signals: pd.Series[Any],
    *,
    annualization_factor: float,
    causal_evidence: CausalEvidence | None = None,
) -> BacktestResult:
    """run_vectorized_backtest() 的内部实现，接收已经构建好的 OHLCV DataFrame。

    供 risk_integration.py/策略框架的 CLI 路径复用——两者都需要先自己算一遍
    records_to_dataframe(records)（喂给策略/风险计算），不应该再让这个函数内部重新构建
    一次同样的 DataFrame。

    Args:
        df: records_to_dataframe() 的输出形状（DatetimeIndex，按时间升序，
            columns=[open, high, low, close, volume]）。调用方负责保证这一点，本函数不
            重新校验 df 本身的形状（那是 records_to_dataframe() 的职责）。
        annualization_factor: 一年多少根 bar。**keyword-only 且必填**——这个函数只拿得到
            df，拿不到 records，没法自己推导；强制调用方传值可以确保
            resolve_annualization_factor() 在上游被真正调用过，而不是悄悄退回一个默认魔数。
        causal_evidence: 同 run_vectorized_backtest()。
    """
    # 确保 signals 拥有 DatetimeIndex
    signals_work = signals.copy()
    if not isinstance(signals_work.index, pd.DatetimeIndex):
        signals_work.index = pd.DatetimeIndex(signals_work.index)

    # --- 输入校验 ---
    # SAFE-05：必须在 reindex 之前，比较信号的**原始**索引。此前先 reindex 再校验，校验比较的
    # 是已被对齐到 df.index 的索引，no_future_shift_detected 因此恒为 True。
    validation_report = validate_inputs(df, signals_work, causal_evidence=causal_evidence)
    if not validation_report.is_valid:
        raise ValueError(f"输入校验失败：{validation_report.issues}")

    # 将 signals 对齐到 df index（重新索引，NaN 填 0）
    signals_aligned: pd.Series[Any] = signals_work.reindex(df.index).fillna(0.0)

    # --- 核心向量化模拟 ---
    #
    # next-bar open 执行：position[T] = signal[T-1]
    # 第 0 bar 无前序信号，持仓为 0（空仓）
    positions: pd.Series[Any] = signals_aligned.shift(1).fillna(0.0)

    # 前向 open 收益率：fwd_return[T] = (open[T+1] - open[T]) / open[T]
    # 最后一 bar 无后续 open，fwd_return = 0（无 P&L）
    next_open: pd.Series[Any] = df["open"].shift(-1)
    fwd_open_return: pd.Series[Any] = ((next_open - df["open"]) / df["open"]).fillna(0.0)

    # 仓位变化（用于成本触发）
    # 第 0 bar 的变化 = 第 0 bar 持仓本身（从 0 变为 position[0]）
    pos_change: pd.Series[Any] = positions.diff()
    pos_change.iloc[0] = positions.iloc[0]
    pos_change_abs: pd.Series[Any] = pos_change.abs()

    # 每 bar 成本率（占组合价值比例）
    total_cost_bps = config.fee_bps + config.slippage_bps
    cost_rate_per_bar: pd.Series[Any] = pos_change_abs * total_cost_bps / 10000.0

    # 每 bar 净收益率 = 持仓 × 前向 open 收益 - 成本率
    gross_bar_return: pd.Series[Any] = positions * fwd_open_return
    net_bar_return: pd.Series[Any] = gross_bar_return - cost_rate_per_bar

    # 累积净值曲线
    equity_curve_series: pd.Series[Any] = config.initial_capital * (1.0 + net_bar_return).cumprod()

    # --- 成本金额（绝对值，用前一 bar 净值近似）---
    prev_equity: pd.Series[Any] = equity_curve_series.shift(1).fillna(config.initial_capital)
    cost_abs_per_bar: pd.Series[Any] = cost_rate_per_bar * prev_equity
    cost_impact_total = float(cost_abs_per_bar.sum())

    # --- 收集模拟成交记录 ---
    fills_approx: list[TradeRecord] = []
    bar_count = len(df)
    pos_list = positions.tolist()

    for idx in range(bar_count):
        if float(pos_change_abs.iloc[idx]) > 1e-12:
            delta = float(pos_change.iloc[idx])
            direction = "buy" if delta > 0 else "sell"
            fills_approx.append(
                TradeRecord(
                    bar_index=idx,
                    timestamp_utc=df.index[idx].to_pydatetime(),
                    direction=direction,
                    execution_price=float(df["open"].iloc[idx]),
                    position_before=float(pos_list[idx - 1]) if idx > 0 else 0.0,
                    position_after=float(pos_list[idx]),
                    position_change=delta,
                    estimated_cost_rate=float(cost_rate_per_bar.iloc[idx]),
                )
            )

    # --- 转换为 Python 列表（供序列化）---
    returns_list = [float(r) for r in net_bar_return.tolist()]
    equity_list = [float(e) for e in equity_curve_series.tolist()]
    positions_list = [float(p) for p in positions.tolist()]
    timestamps_list = [ts.to_pydatetime() for ts in df.index]

    # --- 绩效指标 ---
    metrics = compute_metrics(
        equity_curve=equity_list,
        returns=returns_list,
        positions=positions_list,
        cost_impact_total=cost_impact_total,
        initial_capital=config.initial_capital,
        annualization_factor=annualization_factor,
        risk_free_rate=config.risk_free_rate,
    )

    return BacktestResult(
        config=config,
        metrics=metrics,
        validation_report=validation_report,
        equity_curve=equity_list,
        returns=returns_list,
        positions=positions_list,
        timestamps=timestamps_list,
        fills_approx=fills_approx,
        output_label=BACKTEST_ESTIMATES_LABEL,
    )
