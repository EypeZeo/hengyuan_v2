"""
向量化 OHLCV 回测引擎 — P2-BT-01

执行模型：bar T 信号 → bar T+1 open 价格成交（next-bar open，无 lookahead）。
线性成本模型：fee_bps + slippage_bps，仅在仓位变化时触发。

所有输出均为回测估算值，不代表财务建议、交易授权或实盘批准。
"""

from __future__ import annotations

from typing import Any

import pandas as pd
from py_core.manual_ohlcv import NormalizedOhlcvRecord

from py_core.backtests.metrics import compute_metrics
from py_core.backtests.models import (
    BACKTEST_ESTIMATES_LABEL,
    BacktestConfig,
    BacktestResult,
    TradeRecord,
    ValidationReport,
)


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
    df = df.sort_index()
    return df


def validate_inputs(
    df: pd.DataFrame,
    signals: pd.Series[Any],
) -> ValidationReport:
    """执行数据完整性与反 lookahead 校验。

    校验规则：
    1. timestamp 单调递增（无重复、无乱序）。
    2. close 列无 NaN。
    3. signal index 与 OHLCV index 对齐（若偏移则疑似 lookahead）。

    Args:
        df: OHLCV DataFrame（DatetimeIndex）。
        signals: 与 df 对齐的 signal Series。

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

    # 3. 反 lookahead：signal index 是否与 df index 对齐
    no_future_shift_detected = True
    if len(signals) > 0 and not signals.index.equals(df.index):
        issues.append("signal index 与 OHLCV index 不对齐，可能存在 shift(-1) lookahead 或时间偏移")
        no_future_shift_detected = False

    return ValidationReport(
        timestamp_monotonic=timestamp_monotonic,
        no_nan_close=no_nan_close,
        no_future_shift_detected=no_future_shift_detected,
        bar_count=len(df),
        issues=issues,
    )


def run_vectorized_backtest(
    config: BacktestConfig,
    records: list[NormalizedOhlcvRecord],
    signals: pd.Series[Any],
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

    Returns:
        BacktestResult，含 equity_curve、returns、positions、fills_approx、metrics。

    Raises:
        ValueError: 输入不合法（空 records、NaN close、非单调 timestamps 等）。
    """
    if not records:
        raise ValueError("records 不能为空")

    # --- 构建 OHLCV DataFrame ---
    df = records_to_dataframe(records)

    # 确保 signals 拥有 DatetimeIndex
    signals_work = signals.copy()
    if not isinstance(signals_work.index, pd.DatetimeIndex):
        signals_work.index = pd.DatetimeIndex(signals_work.index)

    # 将 signals 对齐到 df index（重新索引，NaN 填 0）
    signals_aligned: pd.Series[Any] = signals_work.reindex(df.index).fillna(0.0)

    # --- 输入校验 ---
    validation_report = validate_inputs(df, signals_aligned)
    if not validation_report.is_valid:
        raise ValueError(f"输入校验失败：{validation_report.issues}")

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
        annualization_factor=config.annualization_factor,
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
