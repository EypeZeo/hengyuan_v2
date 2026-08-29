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
    # AUDIT METRIC-POSLEN-052: len(positions) was never validated against n, unlike
    # equity_curve above -- a caller passing a mismatched positions list silently got a
    # wrong exposure/turnover (division by the wrong denominator) instead of an error.
    if len(positions) != n:
        raise ValueError(f"positions 长度 ({len(positions)}) 与 returns 长度 ({n}) 不一致")
    if initial_capital <= 0:
        raise ValueError(f"initial_capital 必须为正数，当前值: {initial_capital}")

    # --- total_return ---
    total_return = (equity_curve[-1] / initial_capital) - 1.0

    # --- annualized_return ---
    # AUDIT METRIC-COMPLEX-043: (1+total_return) is the compounding base. When equity goes
    # negative (total_return < -1), that base is negative, and raising a negative base to
    # the fractional power (annualization_factor / n) returns a COMPLEX number in Python --
    # it then flows silently into BacktestMetrics.annualized_return / calmar_ratio / JSON
    # with no error anywhere. A negative-equity state is itself already beyond the ordinary
    # meaning of "a loss": there is no more capital left to keep compounding, so the honest
    # annualized figure is a flat -100%, not whatever the complex-valued fractional power
    # happens to evaluate to.
    base = 1.0 + total_return
    if base < 0.0:
        annualized_return = -1.0
    else:
        annualized_return = base ** (annualization_factor / n) - 1.0

    # --- annualized_volatility ---
    mean_ret = sum(returns) / n
    variance = sum((r - mean_ret) ** 2 for r in returns) / max(n - 1, 1)
    std_ret = math.sqrt(variance)
    annualized_volatility = std_ret * math.sqrt(annualization_factor)

    # --- sharpe_ratio （无风险利率可配置）---
    bar_rf = risk_free_rate / annualization_factor
    excess_mean = mean_ret - bar_rf
    # AUDIT METRIC-ZEROVAR-SHARPE-053: std_ret <= 1e-12 used to hard-return 0.0 regardless
    # of excess_mean's sign -- a strategy with (near-)zero return variance and a genuinely
    # positive excess return (the closest thing to a risk-free arbitrage a backtest can
    # show) scored identically to a strategy that never trades at all. Since Sharpe is the
    # DEFAULT selection_metric for walk-forward/CPCV/PBO, that tie meant the best possible
    # strategy could never actually be selected over doing nothing. Mathematically, as
    # std_ret -> 0+, excess_mean/std_ret -> +-inf depending on the sign of excess_mean; an
    # exact-zero excess_mean over an exact-zero variance is the only case with no signal to
    # rank, so that one alone keeps returning 0.0.
    if std_ret > 1e-12:
        sharpe_ratio = excess_mean / std_ret * math.sqrt(annualization_factor)
    elif excess_mean > 1e-12:
        sharpe_ratio = float("inf")
    elif excess_mean < -1e-12:
        sharpe_ratio = float("-inf")
    else:
        sharpe_ratio = 0.0

    # --- max_drawdown ---
    # AUDIT METRIC-CALMAR-INF-049: peak used to seed from equity_curve[0] rather than
    # initial_capital. When equity_curve[0] is itself already below initial_capital (e.g.
    # the first bar's return is negative) and the curve never recovers, every subsequent
    # bar sits at-or-above that first (already-depressed) value, so max_dd measured 0.0 for
    # a strategy that had, relative to the capital actually put up, lost money the whole
    # time. Seeding from initial_capital makes drawdown measured against what was actually
    # risked, not against wherever the curve happened to start.
    peak = initial_capital
    max_dd = 0.0
    for eq in equity_curve:
        peak = max(peak, eq)
        dd = (peak - eq) / peak if peak > 1e-12 else 0.0
        # AUDIT METRIC-DD-UNBOUNDED-050: eq can go negative (this is a simplistic
        # mark-to-equity model with no margin-call cutoff), which made dd exceed 1.0 --
        # measured as high as 6.0 (600%) in practice. A drawdown ratio is conventionally a
        # fraction of peak equity in [0, 1]; anything past "everything, and then some" is
        # already total loss and clamping avoids an unbounded number leaking into
        # calmar_ratio and any downstream consumer that assumes a [0, 1] range.
        dd = min(dd, 1.0)
        max_dd = max(max_dd, dd)

    # --- calmar_ratio ---
    # AUDIT METRIC-CALMAR-INF-049: max_dd <= 1e-12 used to map to +inf unconditionally, with
    # no regard for the sign of annualized_return -- so a strategy that LOST money while
    # (due to the peak-baseline bug above) recording zero measured drawdown reported an
    # infinitely good Calmar ratio, guaranteed to win any selection that maximizes it. Now
    # the sign of annualized_return decides which infinity (or exactly 0.0) is honest.
    if max_dd > 1e-12:
        calmar_ratio = annualized_return / max_dd
    elif annualized_return > 1e-12:
        calmar_ratio = float("inf")
    elif annualized_return < -1e-12:
        calmar_ratio = float("-inf")
    else:
        calmar_ratio = 0.0

    # --- win_rate: 收益为正的 bar 比例 ---
    win_rate = sum(1 for r in returns if r > 0) / n

    # --- exposure: 持仓非零的 bar 比例 ---
    exposure = sum(1 for p in positions if abs(p) > 1e-12) / n

    # --- turnover: 每 bar 平均绝对仓位变化 ---
    # AUDIT METRIC-TURNOVER-051: pos_changes has n-1 elements (there is no "change" for bar
    # 0), but the old code divided by n -- systematically understating turnover by a factor
    # of (n-1)/n (20% for a 5-bar series). Dividing by len(pos_changes) (== n-1, since
    # positions is now validated to have exactly n elements above) makes this the correct
    # per-bar-with-a-defined-change average.
    pos_changes = [abs(positions[i] - positions[i - 1]) for i in range(1, len(positions))]
    turnover = sum(pos_changes) / len(pos_changes) if pos_changes else 0.0

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
