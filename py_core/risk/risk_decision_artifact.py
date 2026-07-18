"""Research-only RiskDecisionArtifact — position sizing output.

This module is research-only and non-authorizing.
It does not authorize any trade, order placement, live execution,
dry-run execution, or strategy approval.

``non_authorizing`` is always ``True`` and cannot be overridden.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from decimal import Decimal
from enum import StrEnum


class RiskDecisionStatus(StrEnum):
    """Outcome of the position-sizing evaluation."""

    ACCEPTED = "ACCEPTED"
    """Proposed size is within all limits; no cap applied."""

    CAPPED = "CAPPED"
    """Proposed size exceeded at least one limit and was reduced."""

    REJECTED = "REJECTED"
    """Signal or inputs are invalid; size is zero."""


# ── cap-reason string constants ─────────────────────────────────────────────
CAP_REASON_MAX_POSITION_FRACTION: str = "MAX_POSITION_FRACTION"
CAP_REASON_MAX_NOTIONAL: str = "MAX_NOTIONAL"
CAP_REASON_MAX_RISK_PER_TRADE: str = "MAX_RISK_PER_TRADE"
CAP_REASON_INVALID_INPUT: str = "INVALID_INPUT"
CAP_REASON_UNSUPPORTED_SIGNAL: str = "UNSUPPORTED_SIGNAL"
CAP_REASON_NO_POSITION: str = "NO_POSITION"


@dataclass(frozen=True)
class RiskDecisionArtifact:
    """Immutable output of the research-only position-sizing evaluation.

    ``non_authorizing`` is always ``True`` and will raise if set to ``False``.
    ``inputs_snapshot`` carries a string-encoded copy of the key inputs for
    reviewer traceability; it must not contain secrets.

    Raises:
        ValueError: if ``non_authorizing`` is not ``True``.
    """

    proposed_position_size: Decimal
    capped_position_size: Decimal
    risk_used: Decimal
    cap_reason: str | None
    status: RiskDecisionStatus
    warnings: list[str]
    non_authorizing: bool = True
    inputs_snapshot: dict[str, str] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if not self.non_authorizing:
            raise ValueError(
                "non_authorizing must be True; " "RiskDecisionArtifact is a research-only output."
            )
