"""Unit tests for research.risk.RiskCalculator.

All expected values use Decimal to avoid float precision issues.
All artifacts must have non_authorizing=True.
"""

from __future__ import annotations

from decimal import Decimal

import pytest

from py_core.risk import (
    RiskCalculator,
    RiskConfig,
    RiskDecisionArtifact,
    RiskDecisionStatus,
)
from py_core.risk.risk_decision_artifact import (
    CAP_REASON_INVALID_INPUT,
    CAP_REASON_MAX_NOTIONAL,
    CAP_REASON_MAX_POSITION_FRACTION,
    CAP_REASON_MAX_RISK_PER_TRADE,
    CAP_REASON_NO_POSITION,
    CAP_REASON_UNSUPPORTED_SIGNAL,
)

# ── Helpers ──────────────────────────────────────────────────────────────────

D = Decimal


def _default_config(
    *,
    risk_fraction: str = "0.01",
    max_position_fraction: str = "0.10",
    max_notional: str = "5000",
    max_risk_per_trade: str = "200",
    volatility_estimate: str | None = None,
) -> RiskConfig:
    return RiskConfig(
        risk_fraction=D(risk_fraction),
        max_position_fraction=D(max_position_fraction),
        max_notional=D(max_notional),
        max_risk_per_trade=D(max_risk_per_trade),
        volatility_estimate=D(volatility_estimate) if volatility_estimate else None,
    )


def _calc() -> RiskCalculator:
    return RiskCalculator()


# ── Test 1: happy path — ACCEPTED ────────────────────────────────────────────


def test_happy_path_accepted() -> None:
    """Base notional well within all caps → ACCEPTED."""
    config = _default_config(
        risk_fraction="0.01",
        max_position_fraction="0.10",
        max_notional="5000",
        max_risk_per_trade="200",
    )
    result = _calc().calculate(
        equity=D("10000"),
        price=D("100"),
        signal_target="long",
        config=config,
    )
    assert result.status == RiskDecisionStatus.ACCEPTED
    assert result.cap_reason is None
    assert result.proposed_position_size == D("100")  # 10000 * 0.01
    assert result.capped_position_size == D("100")
    assert result.non_authorizing is True


def test_accepted_risk_used_without_stop_distance() -> None:
    """When no stop_distance_fraction provided, risk_used == capped_position_size."""
    config = _default_config()
    result = _calc().calculate(
        equity=D("10000"),
        price=D("100"),
        signal_target="long",
        config=config,
    )
    assert result.risk_used == result.capped_position_size


# ── Test 2: max_position_fraction triggers ───────────────────────────────────


def test_capped_by_max_position_fraction() -> None:
    """risk_fraction larger than max_position_fraction → CAPPED."""
    config = _default_config(
        risk_fraction="0.20",  # 20% — will exceed 10% max_position_fraction
        max_position_fraction="0.10",
        max_notional="99999",
    )
    result = _calc().calculate(
        equity=D("10000"),
        price=D("100"),
        signal_target="long",
        config=config,
    )
    assert result.status == RiskDecisionStatus.CAPPED
    assert result.cap_reason == CAP_REASON_MAX_POSITION_FRACTION
    assert result.capped_position_size == D("1000")  # 10000 * 0.10
    assert result.proposed_position_size == D("2000")  # 10000 * 0.20
    assert result.non_authorizing is True


# ── Test 3: max_notional triggers ────────────────────────────────────────────


def test_capped_by_max_notional() -> None:
    """Notional exceeds max_notional → CAPPED."""
    config = _default_config(
        risk_fraction="0.05",  # 5% of 10000 = 500; max_position_fraction=10% ok
        max_position_fraction="0.10",
        max_notional="200",  # below 500
    )
    result = _calc().calculate(
        equity=D("10000"),
        price=D("100"),
        signal_target="long",
        config=config,
    )
    assert result.status == RiskDecisionStatus.CAPPED
    assert result.cap_reason == CAP_REASON_MAX_NOTIONAL
    assert result.capped_position_size == D("200")
    assert result.non_authorizing is True


# ── Test 4: max_risk_per_trade triggers ──────────────────────────────────────


def test_capped_by_max_risk_per_trade_with_stop_distance() -> None:
    """With stop_distance_fraction, max_risk_per_trade cap engages."""
    config = _default_config(
        risk_fraction="0.01",
        max_position_fraction="0.10",
        max_notional="5000",
        max_risk_per_trade="50",
    )
    # base_notional = 10000 * 0.01 = 100
    # max_notional_by_risk = 50 / 0.10 = 500 — higher than 100; no cap here
    # Let's use a larger stop so the cap triggers: max_notional_by_risk = 50/1.0 = 50 < 100
    result = _calc().calculate(
        equity=D("10000"),
        price=D("100"),
        signal_target="long",
        config=config,
        stop_distance_fraction=D("1"),  # max_by_risk = 50 / 1 = 50 < 100
    )
    assert result.status == RiskDecisionStatus.CAPPED
    assert result.cap_reason == CAP_REASON_MAX_RISK_PER_TRADE
    assert result.capped_position_size == D("50")
    assert result.non_authorizing is True


def test_risk_used_with_stop_distance() -> None:
    """risk_used = capped_position_size * stop_distance_fraction when stop_distance_fraction > 0."""
    config = _default_config(
        risk_fraction="0.01",
        max_position_fraction="0.10",
        max_notional="5000",
        max_risk_per_trade="50",
    )
    stop = D("1")
    result = _calc().calculate(
        equity=D("10000"),
        price=D("100"),
        signal_target="long",
        config=config,
        stop_distance_fraction=stop,
    )
    assert result.risk_used == result.capped_position_size * stop


# ── Test 5: equity = 0 ───────────────────────────────────────────────────────


def test_rejected_equity_zero() -> None:
    result = _calc().calculate(
        equity=D("0"),
        price=D("100"),
        signal_target="long",
        config=_default_config(),
    )
    assert result.status == RiskDecisionStatus.REJECTED
    assert result.cap_reason == CAP_REASON_INVALID_INPUT
    assert result.capped_position_size == D("0")
    assert any("equity" in w for w in result.warnings)
    assert result.non_authorizing is True


# ── Test 6: equity < 0 ───────────────────────────────────────────────────────


def test_rejected_equity_negative() -> None:
    result = _calc().calculate(
        equity=D("-1"),
        price=D("100"),
        signal_target="long",
        config=_default_config(),
    )
    assert result.status == RiskDecisionStatus.REJECTED
    assert result.cap_reason == CAP_REASON_INVALID_INPUT
    assert any("equity" in w for w in result.warnings)
    assert result.non_authorizing is True


# ── Test 7: price = 0 ────────────────────────────────────────────────────────


def test_rejected_price_zero() -> None:
    result = _calc().calculate(
        equity=D("10000"),
        price=D("0"),
        signal_target="long",
        config=_default_config(),
    )
    assert result.status == RiskDecisionStatus.REJECTED
    assert result.cap_reason == CAP_REASON_INVALID_INPUT
    assert any("price" in w for w in result.warnings)
    assert result.non_authorizing is True


# ── Test 8: price < 0 ────────────────────────────────────────────────────────


def test_rejected_price_negative() -> None:
    result = _calc().calculate(
        equity=D("10000"),
        price=D("-50"),
        signal_target="long",
        config=_default_config(),
    )
    assert result.status == RiskDecisionStatus.REJECTED
    assert result.cap_reason == CAP_REASON_INVALID_INPUT
    assert any("price" in w for w in result.warnings)
    assert result.non_authorizing is True


# ── Test 9: signal_target = "none" ───────────────────────────────────────────


def test_rejected_signal_none() -> None:
    result = _calc().calculate(
        equity=D("10000"),
        price=D("100"),
        signal_target="none",
        config=_default_config(),
    )
    assert result.status == RiskDecisionStatus.REJECTED
    assert result.cap_reason == CAP_REASON_NO_POSITION
    assert result.capped_position_size == D("0")
    assert result.non_authorizing is True


# ── Test 10: signal_target = "short" ─────────────────────────────────────────


def test_rejected_signal_short_with_warning() -> None:
    result = _calc().calculate(
        equity=D("10000"),
        price=D("100"),
        signal_target="short",
        config=_default_config(),
    )
    assert result.status == RiskDecisionStatus.REJECTED
    assert result.cap_reason == CAP_REASON_UNSUPPORTED_SIGNAL
    assert any("short" in w for w in result.warnings)
    assert result.non_authorizing is True


# ── Test 11: unsupported signal ──────────────────────────────────────────────


@pytest.mark.parametrize("signal", ["BUY", "SELL", "buy", "LONG", "x", ""])
def test_rejected_unsupported_signal(signal: str) -> None:
    result = _calc().calculate(
        equity=D("10000"),
        price=D("100"),
        signal_target=signal,
        config=_default_config(),
    )
    assert result.status == RiskDecisionStatus.REJECTED
    assert result.cap_reason == CAP_REASON_UNSUPPORTED_SIGNAL
    assert result.non_authorizing is True


# ── Test 12: stop_distance_fraction <= 0 → warning, cap ignored ───────────────────────


def test_stop_distance_zero_adds_warning_no_cap() -> None:
    config = _default_config(max_risk_per_trade="1")  # very tight if applied
    result = _calc().calculate(
        equity=D("10000"),
        price=D("100"),
        signal_target="long",
        config=config,
        stop_distance_fraction=D("0"),
    )
    # stop_distance_fraction=0 → cap ignored; result should not be driven by max_risk_per_trade
    assert any("stop_distance_fraction" in w for w in result.warnings)
    assert result.cap_reason != CAP_REASON_MAX_RISK_PER_TRADE


def test_stop_distance_negative_adds_warning_no_cap() -> None:
    config = _default_config(max_risk_per_trade="1")
    result = _calc().calculate(
        equity=D("10000"),
        price=D("100"),
        signal_target="long",
        config=config,
        stop_distance_fraction=D("-0.5"),
    )
    assert any("stop_distance_fraction" in w for w in result.warnings)
    assert result.cap_reason != CAP_REASON_MAX_RISK_PER_TRADE


# ── Test 13: invalid RiskConfig → fail-fast ──────────────────────────────────


@pytest.mark.parametrize(
    "kwargs",
    [
        {"risk_fraction": D("-0.01")},
        {"max_position_fraction": D("-0.1")},
        {"max_notional": D("-1")},
        {"max_risk_per_trade": D("-100")},
    ],
)
def test_invalid_risk_config_raises(kwargs: dict) -> None:
    base = {
        "risk_fraction": D("0.01"),
        "max_position_fraction": D("0.10"),
        "max_notional": D("5000"),
        "max_risk_per_trade": D("200"),
    }
    base.update(kwargs)
    with pytest.raises(ValueError):
        RiskConfig(**base)


# ── Test 14: all artifacts have non_authorizing=True ─────────────────────────


@pytest.mark.parametrize(
    "kwargs",
    [
        {"signal_target": "long"},
        {"signal_target": "none"},
        {"signal_target": "short"},
        {"signal_target": "unknown"},
        {"equity": D("0"), "signal_target": "long"},
        {"price": D("0"), "signal_target": "long"},
    ],
)
def test_non_authorizing_always_true(kwargs: dict) -> None:
    base: dict = {
        "equity": D("10000"),
        "price": D("100"),
        "signal_target": "long",
        "config": _default_config(),
    }
    base.update(kwargs)
    result = _calc().calculate(**base)
    assert result.non_authorizing is True


# ── Test 15: inputs_snapshot has no secret-like keys ─────────────────────────


def test_inputs_snapshot_no_secrets() -> None:
    result = _calc().calculate(
        equity=D("10000"),
        price=D("100"),
        signal_target="long",
        config=_default_config(),
    )
    forbidden = {"secret", "api_key", "token", "password", "key"}
    snapshot_keys = {k.lower() for k in result.inputs_snapshot}
    assert snapshot_keys.isdisjoint(forbidden), (
        f"inputs_snapshot contains secret-like keys: " f"{snapshot_keys & forbidden}"
    )


# ── Test 16: chained caps — max_position_fraction then max_notional ──────────


def test_chained_caps_last_reason_wins() -> None:
    """max_position_fraction fires first, then max_notional tightens further."""
    config = _default_config(
        risk_fraction="0.50",  # 50% of 10000 = 5000
        max_position_fraction="0.20",  # cap to 2000
        max_notional="500",  # further cap to 500
        max_risk_per_trade="9999",
    )
    result = _calc().calculate(
        equity=D("10000"),
        price=D("100"),
        signal_target="long",
        config=config,
    )
    assert result.status == RiskDecisionStatus.CAPPED
    # Last binding cap should be MAX_NOTIONAL
    assert result.cap_reason == CAP_REASON_MAX_NOTIONAL
    assert result.capped_position_size == D("500")
    # Both cap warnings should appear
    assert any("MAX_POSITION_FRACTION" in w for w in result.warnings)
    assert any("MAX_NOTIONAL" in w for w in result.warnings)
    assert result.non_authorizing is True


# ── Test 17: RiskDecisionArtifact non_authorizing False raises ────────────────


def test_artifact_non_authorizing_false_raises() -> None:
    with pytest.raises(ValueError, match="non_authorizing"):
        RiskDecisionArtifact(
            proposed_position_size=D("100"),
            capped_position_size=D("100"),
            risk_used=D("100"),
            cap_reason=None,
            status=RiskDecisionStatus.ACCEPTED,
            warnings=[],
            non_authorizing=False,
        )


# ── Test 18: inputs_snapshot is populated on accepted path ───────────────────


def test_inputs_snapshot_populated() -> None:
    result = _calc().calculate(
        equity=D("10000"),
        price=D("100"),
        signal_target="long",
        config=_default_config(),
    )
    assert "equity" in result.inputs_snapshot
    assert "price" in result.inputs_snapshot
    assert "signal_target" in result.inputs_snapshot
    assert "risk_fraction" in result.inputs_snapshot
