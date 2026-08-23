"""
回测绩效指标计算 — P2-BT-01

所有计算均为回测估算。不代表财务建议或交易授权。
"""

from __future__ import annotations

import math

from py_core.backtests.models import BACKTEST_ESTIMATES_LABEL, BacktestMetrics


def compute_metrics(
    equity_curve: list[float],
    returns: list[float],
    positions: list[float],
    cost_impact_total: float,
    initial_capital: float,
    annualization_factor: float,
    risk_free_rate: float = 0.0,
) -> BacktestMetrics:
    """从 equity_curve / returns / positions 计算完整绩效指标集合。

    Args:
        equity_curve: 每 bar 的净值（绝对值，如 100000.0）。
        returns: 每 bar 的简单净收益率。
        positions: 每 bar 的持仓状态（0.0 或 1.0）。
        cost_impact_total: 总成本金额（绝对值）。
        initial_capital: 初始资本。
        annualization_factor: 一年包含多少根 bar。**必填，刻意不给默认值**——之前这里默认
            252（股票交易日数），对 7×24 的加密货币是错的且会静默生效；现在强制调用方明确
            解析出这个值（通常经由 py_core.backtests.annualization 从数据的 market+timeframe
            推导）。
        risk_free_rate: 年化无风险利率（默认 0.0）。

    Returns:
        BacktestMetrics，包含所有 R4 要求的 11 个指标。

    Raises:
        ValueError: equity_curve 或 returns 为空。
    """
    n = len(returns)
    if n == 0:
        raise ValueError("returns 不能为空")
    if len(equity_curve) != n:
        raise ValueError(f"equity_curve 长度 ({len(equity_curve)}) 与 returns 长度 ({n}) 不一致")
    if initial_capital <= 0:
        raise ValueError(f"initial_capital 必须为正数，当前值: {initial_capital}")

    # --- total_return ---
    total_return = (equity_curve[-1] / initial_capital) - 1.0

    # --- annualized_return ---
    annualized_return = (1.0 + total_return) ** (annualization_factor / n) - 1.0

    # --- annualized_volatility ---
    mean_ret = sum(returns) / n
    variance = sum((r - mean_ret) ** 2 for r in returns) / max(n - 1, 1)
    std_ret = math.sqrt(variance)
    annualized_volatility = std_ret * math.sqrt(annualization_factor)

    # --- sharpe_ratio （无风险利率可配置）---
    bar_rf = risk_free_rate / annualization_factor
    excess_mean = mean_ret - bar_rf
    sharpe_ratio = (
        (excess_mean / std_ret * math.sqrt(annualization_factor)) if std_ret > 1e-12 else 0.0
    )

    # --- max_drawdown ---
    peak = equity_curve[0]
    max_dd = 0.0
    for eq in equity_curve:
        if eq > peak:
            peak = eq
        dd = (peak - eq) / peak if peak > 1e-12 else 0.0
        if dd > max_dd:
            max_dd = dd

    # --- calmar_ratio ---
    calmar_ratio = annualized_return / max_dd if max_dd > 1e-12 else float("inf")

    # --- win_rate: 收益为正的 bar 比例 ---
    win_rate = sum(1 for r in returns if r > 0) / n

    # --- exposure: 持仓非零的 bar 比例 ---
    exposure = sum(1 for p in positions if abs(p) > 1e-12) / n

    # --- turnover: 每 bar 平均绝对仓位变化 ---
    pos_changes = [abs(positions[i] - positions[i - 1]) for i in range(1, len(positions))]
    turnover = sum(pos_changes) / n if pos_changes else 0.0

    # --- cost_impact_bps / cost_impact_total ---
    cost_impact_bps = (cost_impact_total / initial_capital) * 10000.0

    return BacktestMetrics(
        total_return=total_return,
        annualized_return=annualized_return,
        annualized_volatility=annualized_volatility,
        sharpe_ratio=sharpe_ratio,
        max_drawdown=max_dd,
        calmar_ratio=calmar_ratio,
        win_rate=win_rate,
        exposure=exposure,
        turnover=turnover,
        cost_impact_bps=cost_impact_bps,
        cost_impact_total=cost_impact_total,
        output_label=BACKTEST_ESTIMATES_LABEL,
    )
