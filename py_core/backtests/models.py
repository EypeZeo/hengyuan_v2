"""
回测数据模型 — P2-BT-01

所有输出均为回测估算值（backtesting estimates only）。
不代表财务建议、交易授权、干跑批准或实盘批准。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime

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
    annualization_factor: int = 252
    output_label: str = BACKTEST_ESTIMATES_LABEL

    def __post_init__(self) -> None:
        if self.initial_capital <= 0:
            raise ValueError(f"initial_capital 必须为正数，当前值: {self.initial_capital}")
        if self.fee_bps < 0 or self.fee_bps > 500:
            raise ValueError(f"fee_bps 必须在 [0, 500] 范围内，当前值: {self.fee_bps}")
        if self.slippage_bps < 0 or self.slippage_bps > 500:
            raise ValueError(f"slippage_bps 必须在 [0, 500] 范围内，当前值: {self.slippage_bps}")
        if self.annualization_factor <= 0:
            raise ValueError(
                f"annualization_factor 必须为正数，当前值: {self.annualization_factor}"
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

    @property
    def is_valid(self) -> bool:
        return self.timestamp_monotonic and self.no_nan_close and self.no_future_shift_detected


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
