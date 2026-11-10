"""
回测数据模型 — P2-BT-01

所有输出均为回测估算值（backtesting estimates only）。
不代表财务建议、交易授权、干跑批准或实盘批准。
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any

BACKTEST_ESTIMATES_LABEL = "backtesting estimates only"
NON_AUTH_ASSERTION = (
    "NOT financial advice. NOT trading authorization. "
    "NOT dry-run readiness. NOT live readiness. "
    "Research and simulation use only."
)


@dataclass(frozen=True)
class BacktestConfig:
    """回测输入配置。"""

    initial_capital: float
    fee_bps: float = 0.0
    slippage_bps: float = 0.0
    risk_free_rate: float = 0.0
    # None = 从数据自身的 market + timeframe 自动推导（见 py_core/backtests/annualization.py）。
    # 之前这里硬编码 252（股票交易日数），对 7×24 的加密货币是错的，会系统性低估年化收益与
    # Sharpe，周期越细错得越多。显式传值仍然可以覆盖自动推导。
    annualization_factor: float | None = None
    output_label: str = BACKTEST_ESTIMATES_LABEL

    def __post_init__(self) -> None:
        # 每个数值字段先判有限，再判范围：NaN 与所有比较都为 False，``nan <= 0`` / ``nan < 0 or nan > 500``
        # 都不会触发下面的范围检查，会让 NaN 配置原样通过并污染整条净值曲线（审计 F-A-005）；``inf`` 则能通过
        # ``<= 0`` 的下限检查。
        if not math.isfinite(self.initial_capital) or self.initial_capital <= 0:
            raise ValueError(f"initial_capital 必须为正数（且为有限值），当前值: {self.initial_capital}")
        if not math.isfinite(self.fee_bps) or self.fee_bps < 0 or self.fee_bps > 500:
            raise ValueError(f"fee_bps 必须在 [0, 500] 范围内（且为有限值），当前值: {self.fee_bps}")
        if not math.isfinite(self.slippage_bps) or self.slippage_bps < 0 or self.slippage_bps > 500:
            raise ValueError(f"slippage_bps 必须在 [0, 500] 范围内（且为有限值），当前值: {self.slippage_bps}")
        if not math.isfinite(self.risk_free_rate):
            raise ValueError(f"risk_free_rate 必须为有限值，当前值: {self.risk_free_rate}")
        if self.annualization_factor is not None and (
            not math.isfinite(self.annualization_factor) or self.annualization_factor <= 0
        ):
            raise ValueError(
                f"annualization_factor 必须为正数（或 None 表示自动推导），"
                f"当前值: {self.annualization_factor}"
            )


@dataclass(frozen=True)
class TradeRecord:
    """单笔模拟成交记录（仅为估算）。"""

    bar_index: int
    timestamp_utc: datetime
    direction: str  # "buy" 或 "sell"
    execution_price: float
    position_before: float
    position_after: float
    position_change: float
    estimated_cost_rate: float  # 占当时组合价值的比例
    note: str = "fills_approx: backtesting estimate only"


@dataclass(frozen=True)
class BacktestMetrics:
    """回测绩效指标（所有值均为回测估算）。"""

    total_return: float
    annualized_return: float
    annualized_volatility: float
    sharpe_ratio: float
    max_drawdown: float
    calmar_ratio: float
    win_rate: float
    exposure: float
    turnover: float
    cost_impact_bps: float
    cost_impact_total: float
    output_label: str = BACKTEST_ESTIMATES_LABEL


@dataclass
class ValidationReport:
    """数据完整性与反 lookahead 校验报告。"""

    timestamp_monotonic: bool
    no_nan_close: bool
    no_future_shift_detected: bool
    bar_count: int
    issues: list[str] = field(default_factory=list)
    # SAFE-05：因果核验的实际强度，只陈述事实。"not_run"＝调用方没经过引擎的校验入口；
    # "index_only"＝只做了索引/时间戳检查（预计算信号文件、直接传入的 Series）；
    # "prefix_differential"＝信号来自 checked_signals()，前缀截断差分在 causal_points 个 bar 上通过。
    causal_check: str = "not_run"
    causal_points: int = 0
    causal_context: dict[str, Any] = field(default_factory=dict)
    # 审计 F-A-005：数值域检查。默认 True 让既有的直接构造保持可用；validate_inputs() 会如实填写。
    # prices_finite_positive：执行价格列（open，回测按它成交）与 high/low/close 全为有限正数。
    # signals_finite：信号里没有 ±inf（NaN 是文档化的“无信号 = 空仓”，仍按 0 处理，不算问题）。
    prices_finite_positive: bool = True
    signals_finite: bool = True

    @property
    def is_valid(self) -> bool:
        return (
            self.timestamp_monotonic
            and self.no_nan_close
            and self.no_future_shift_detected
            and self.prices_finite_positive
            and self.signals_finite
        )


@dataclass
class BacktestResult:
    """向量化回测完整结果。

    所有输出均为回测估算值。
    不代表财务建议、交易授权、策略批准、干跑批准或实盘批准。
    """

    config: BacktestConfig
    metrics: BacktestMetrics
    validation_report: ValidationReport
    equity_curve: list[float]
    returns: list[float]
    positions: list[float]
    timestamps: list[datetime]
    fills_approx: list[TradeRecord]
    output_label: str = BACKTEST_ESTIMATES_LABEL
    returns_type: str = "simple_net_returns"
    non_auth_assertion: str = NON_AUTH_ASSERTION
