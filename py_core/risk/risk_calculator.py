"""Research-only RiskCalculator — position sizing via fixed fraction + caps.

This module is research-only and non-authorizing.
It does not authorize any trade, order placement, live execution,
dry-run execution, or strategy approval.

Position sizes are expressed in **notional amounts** (quote currency),
not units of the base asset.
"""

from __future__ import annotations

from decimal import Decimal

from py_core.risk.risk_config import RiskConfig
from py_core.risk.risk_decision_artifact import (
    CAP_REASON_INVALID_INPUT,
    CAP_REASON_MAX_NOTIONAL,
    CAP_REASON_MAX_POSITION_FRACTION,
    CAP_REASON_MAX_RISK_PER_TRADE,
    CAP_REASON_NO_POSITION,
    CAP_REASON_UNSUPPORTED_SIGNAL,
    RiskDecisionArtifact,
    RiskDecisionStatus,
)

_ZERO = Decimal("0")
_ONE = Decimal("1")
_SUPPORTED_SIGNALS = frozenset({"long", "none", "short"})


class RiskCalculator:
    """Research-only position sizing calculator.

    Implements fixed-fraction sizing with three hard caps:
    ``max_position_fraction``, ``max_notional``, and ``max_risk_per_trade``.

    Caps are applied in this order:
    1. ``max_position_fraction``
    2. ``max_notional``
    3. ``max_risk_per_trade`` (only when a valid ``stop_distance_fraction`` is given)

    If multiple caps trigger, ``cap_reason`` reflects the last (tightest)
    constraint applied; the full cap path is recorded in ``warnings``.

    All outputs are non-authorizing research artifacts.
    """

    def calculate(
        self,
        *,
        equity: Decimal,
        price: Decimal,
        signal_target: str,
        config: RiskConfig,
        stop_distance_fraction: Decimal | None = None,
    ) -> RiskDecisionArtifact:
        """Evaluate position sizing for the given inputs.

        Args:
            equity: Current total equity in quote currency.  Must be > 0.
            price:  Current asset price.  Must be > 0 (used for validation
                    and inputs_snapshot; sizing is notional-based).
            signal_target: Direction intent — ``"long"`` / ``"none"`` /
                           ``"short"`` (``"short"`` is rejected in V1).
            config: Immutable :class:`RiskConfig` parameters.
            stop_distance_fraction: Optional stop distance expressed as a
                           **fraction of price** — ``Decimal("0.02")`` means a
                           2% stop, NOT 2 quote-currency units.  Must be in
                           ``(0, 1]``.  When provided, activates the
                           ``max_risk_per_trade`` cap and is used to compute
                           ``risk_used``.

                           Audit PY-RISK-005: this parameter was named
                           ``stop_distance`` and documented as a "notional stop
                           distance" (an absolute quote amount), and the test
                           fixtures passed ``Decimal("500")`` accordingly — but
                           Rules 9 and 13 below are only dimensionally coherent
                           if it is a fraction, and ``risk_integration.py``
                           divides ``capped_position_size`` by equity, which
                           pins that output to quote currency.  Measured, the
                           two readings differed by 1000x in resulting position
                           size for the same inputs.  The name now carries the
                           unit so the ambiguity is unrepresentable.

        Returns:
            A :class:`RiskDecisionArtifact` with ``non_authorizing=True``.
        """
        warnings: list[str] = []

        # ── Build inputs snapshot (no secrets) ───────────────────────────
        inputs_snapshot: dict[str, str] = {
            "equity": str(equity),
            "price": str(price),
            "signal_target": signal_target,
            "risk_fraction": str(config.risk_fraction),
            "max_position_fraction": str(config.max_position_fraction),
            "max_notional": str(config.max_notional),
            "max_risk_per_trade": str(config.max_risk_per_trade),
            "volatility_estimate": str(config.volatility_estimate),
            "stop_distance_fraction": str(stop_distance_fraction),
        }

        # ── Rule 1: equity must be positive ──────────────────────────────
        if equity <= _ZERO:
            warnings.append("equity must be positive")
            return RiskDecisionArtifact(
                proposed_position_size=_ZERO,
                capped_position_size=_ZERO,
                risk_used=_ZERO,
                cap_reason=CAP_REASON_INVALID_INPUT,
                status=RiskDecisionStatus.REJECTED,
                warnings=warnings,
                non_authorizing=True,
                inputs_snapshot=inputs_snapshot,
            )

        # ── Rule 2: price must be positive ───────────────────────────────
        if price <= _ZERO:
            warnings.append("price must be positive")
            return RiskDecisionArtifact(
                proposed_position_size=_ZERO,
                capped_position_size=_ZERO,
                risk_used=_ZERO,
                cap_reason=CAP_REASON_INVALID_INPUT,
                status=RiskDecisionStatus.REJECTED,
                warnings=warnings,
                non_authorizing=True,
                inputs_snapshot=inputs_snapshot,
            )

        # ── Rule 3: no position requested ────────────────────────────────
        if signal_target == "none":
            warnings.append("no position requested")
            return RiskDecisionArtifact(
                proposed_position_size=_ZERO,
                capped_position_size=_ZERO,
                risk_used=_ZERO,
                cap_reason=CAP_REASON_NO_POSITION,
                status=RiskDecisionStatus.REJECTED,
                warnings=warnings,
                non_authorizing=True,
                inputs_snapshot=inputs_snapshot,
            )

        # ── Rule 4: short not supported in V1 ────────────────────────────
        if signal_target == "short":
            warnings.append("short signals not supported in V1")
            return RiskDecisionArtifact(
                proposed_position_size=_ZERO,
                capped_position_size=_ZERO,
                risk_used=_ZERO,
                cap_reason=CAP_REASON_UNSUPPORTED_SIGNAL,
                status=RiskDecisionStatus.REJECTED,
                warnings=warnings,
                non_authorizing=True,
                inputs_snapshot=inputs_snapshot,
            )

        # ── Rule 5: unsupported signal ────────────────────────────────────
        if signal_target not in _SUPPORTED_SIGNALS:
            warnings.append(f"unsupported signal target: {signal_target!r}")
            return RiskDecisionArtifact(
                proposed_position_size=_ZERO,
                capped_position_size=_ZERO,
                risk_used=_ZERO,
                cap_reason=CAP_REASON_UNSUPPORTED_SIGNAL,
                status=RiskDecisionStatus.REJECTED,
                warnings=warnings,
                non_authorizing=True,
                inputs_snapshot=inputs_snapshot,
            )

        # ── Rule 6: fixed-fraction base notional ─────────────────────────
        proposed_notional = equity * config.risk_fraction
        current_notional = proposed_notional
        cap_reason: str | None = None
        capped = False

        # ── Rule 7: max_position_fraction cap ────────────────────────────
        max_position_notional = equity * config.max_position_fraction
        if current_notional > max_position_notional:
            warnings.append(
                f"capped by MAX_POSITION_FRACTION: "
                f"{current_notional} -> {max_position_notional}"
            )
            current_notional = max_position_notional
            cap_reason = CAP_REASON_MAX_POSITION_FRACTION
            capped = True

        # ── Rule 8: max_notional cap ──────────────────────────────────────
        if current_notional > config.max_notional:
            warnings.append(
                f"capped by MAX_NOTIONAL: " f"{current_notional} -> {config.max_notional}"
            )
            current_notional = config.max_notional
            cap_reason = CAP_REASON_MAX_NOTIONAL
            capped = True

        # ── Rule 9: max_risk_per_trade cap (requires stop_distance_fraction) ──
        #
        # Dimensions (audit PY-RISK-005): current_notional is quote currency and
        # stop_distance_fraction is dimensionless, so
        #   risk_at_stop = notional x fraction        -> quote
        #   max notional = max_risk_per_trade / fraction -> quote
        # Both sides of the comparison below are quote amounts. Under the previous
        # "absolute quote distance" reading, max_notional_by_risk was dimensionless
        # and being compared against a quote amount.
        if stop_distance_fraction is not None:
            if stop_distance_fraction <= _ZERO or stop_distance_fraction > _ONE:
                warnings.append(
                    "stop_distance_fraction must be in (0, 1] — it is a fraction of "
                    "price, not an absolute quote amount; max_risk_per_trade cap ignored"
                )
            else:
                max_notional_by_risk = config.max_risk_per_trade / stop_distance_fraction
                if current_notional > max_notional_by_risk:
                    warnings.append(
                        f"capped by MAX_RISK_PER_TRADE: "
                        f"{current_notional} -> {max_notional_by_risk}"
                    )
                    current_notional = max_notional_by_risk
                    cap_reason = CAP_REASON_MAX_RISK_PER_TRADE
                    capped = True

        # ── Rule 10: final notional <= 0 → reject ────────────────────────
        if current_notional <= _ZERO:
            return RiskDecisionArtifact(
                proposed_position_size=proposed_notional,
                capped_position_size=_ZERO,
                risk_used=_ZERO,
                cap_reason=CAP_REASON_NO_POSITION,
                status=RiskDecisionStatus.REJECTED,
                warnings=warnings,
                non_authorizing=True,
                inputs_snapshot=inputs_snapshot,
            )

        # ── Rule 13: risk_used ────────────────────────────────────────────
        #
        # Both branches are quote currency (audit PY-RISK-005): with a stop, the
        # amount at risk is notional x fraction; without one, the whole position is
        # notionally at risk. Under the previous reading the first branch was
        # quote-squared, so the same field carried two different units depending on
        # whether an optional argument was supplied.
        if stop_distance_fraction is not None and _ZERO < stop_distance_fraction <= _ONE:
            risk_used = current_notional * stop_distance_fraction
        else:
            risk_used = current_notional

        # ── Rule 11/12: status ────────────────────────────────────────────
        status = RiskDecisionStatus.CAPPED if capped else RiskDecisionStatus.ACCEPTED

        return RiskDecisionArtifact(
            proposed_position_size=proposed_notional,
            capped_position_size=current_notional,
            risk_used=risk_used,
            cap_reason=cap_reason,
            status=status,
            warnings=warnings,
            non_authorizing=True,
            inputs_snapshot=inputs_snapshot,
        )
