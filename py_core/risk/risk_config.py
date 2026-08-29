"""Research-only RiskConfig — position sizing parameters.

This module is research-only and non-authorizing.
It does not authorize any trade, order placement, live execution,
dry-run execution, or strategy approval.
"""

from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal


@dataclass(frozen=True)
class RiskConfig:
    """Immutable configuration for the research-only position sizing calculator.

    All values must be non-negative. ``volatility_estimate`` is optional.

    Raises:
        ValueError: if any field violates the non-negativity constraints.
    """

    risk_fraction: Decimal
    max_position_fraction: Decimal
    max_notional: Decimal
    max_risk_per_trade: Decimal
    volatility_estimate: Decimal | None = None

    def __post_init__(self) -> None:
        if self.risk_fraction < Decimal(0):
            raise ValueError(f"risk_fraction must be >= 0, got {self.risk_fraction}")
        if self.max_position_fraction < Decimal(0):
            raise ValueError(
                f"max_position_fraction must be >= 0, got {self.max_position_fraction}"
            )
        if self.max_notional < Decimal(0):
            raise ValueError(f"max_notional must be >= 0, got {self.max_notional}")
        if self.max_risk_per_trade < Decimal(0):
            raise ValueError(f"max_risk_per_trade must be >= 0, got {self.max_risk_per_trade}")
        if self.volatility_estimate is not None and self.volatility_estimate < Decimal(0):
            raise ValueError(f"volatility_estimate must be >= 0, got {self.volatility_estimate}")
