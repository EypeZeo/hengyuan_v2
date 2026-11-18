"""Deterministic contract tests for the PLAN-05 calibration runner."""

from __future__ import annotations

import json
import math

import numpy as np
import pytest

from py_core.tools import calibrate_plan05


def test_wilson_rate_matches_known_interval_and_marks_zero_trials_insufficient() -> None:
    rate = calibrate_plan05.wilson_rate(5, 10)

    assert rate is not None
    assert rate.events == 5
    assert rate.trials == 10
    assert rate.fraction == pytest.approx(0.5)
    assert rate.lower == pytest.approx(0.2365930905, abs=1e-9)
    assert rate.upper == pytest.approx(0.7634069095, abs=1e-9)
    assert calibrate_plan05.wilson_rate(0, 0) is None
    assert calibrate_plan05.classify_rate(None, (0.92, 0.98)) == "insufficient_evidence"


def test_rate_classification_distinguishes_inside_overlap_and_outside() -> None:
    inside = calibrate_plan05.wilson_rate(50, 100)
    overlaps = calibrate_plan05.wilson_rate(5, 10)
    outside = calibrate_plan05.wilson_rate(0, 10)

    assert inside is not None and overlaps is not None and outside is not None
    assert calibrate_plan05.classify_rate(inside, (0.35, 0.65)) == "within_tolerance"
    assert calibrate_plan05.classify_rate(overlaps, (0.4, 0.6)) == "insufficient_evidence"
    assert calibrate_plan05.classify_rate(outside, (0.5, 0.8)) == "outside_tolerance"


def test_smoke_run_is_repeatable_finite_and_never_claims_formal_pass() -> None:
    first = calibrate_plan05.run_calibration(smoke=True)
    second = calibrate_plan05.run_calibration(smoke=True)

    assert first == second
    assert first["profile"] == "smoke"
    assert first["status"] == "smoke"
    assert first["repetitions_per_scenario"] == 12
    assert len(first["sharpe"]) == 7
    assert len(first["selection"]) == 2

    for row in first["sharpe"]:
        assert row["failures"] == {}
        assert row["coverage"] is not None
        assert row["coverage"]["trials"] == 12
        assert row["positive_rejection"]["trials"] == 12
        assert math.isfinite(row["mean_sharpe"])
        assert math.isfinite(row["empirical_sharpe_sd"])
        assert math.isfinite(row["mean_hac_se"])

    for row in first["selection"]:
        assert row["failures"] == {}
        assert row["uncorrected_hac_false_positive"]["trials"] == 12
        assert row["dsr_false_positive"]["trials"] == 12

    json.dumps(first, allow_nan=False)


def test_any_hac_failure_is_counted_and_blocks_formal_status(monkeypatch: pytest.MonkeyPatch) -> None:
    scenario = calibrate_plan05.SharpeScenario("controlled_failure", observations=4, lag=1)

    monkeypatch.setattr(calibrate_plan05, "SHARPE_SCENARIOS", (scenario,))
    monkeypatch.setattr(calibrate_plan05, "SELECTION_SCENARIOS", ())
    monkeypatch.setattr(calibrate_plan05, "REPLICATIONS", 2)
    monkeypatch.setattr(calibrate_plan05, "COVERAGE_TOLERANCE", (0.0, 1.0))
    monkeypatch.setattr(calibrate_plan05, "FALSE_POSITIVE_TOLERANCE", (0.0, 1.0))

    def controlled_returns(_scenario, repetitions: int, _rng: np.random.Generator):
        assert repetitions == 2
        return iter((np.full(4, 0.01), np.array([-1.0, 1.0, -1.0, 1.0])))

    monkeypatch.setattr(calibrate_plan05, "simulated_returns", controlled_returns)

    result = calibrate_plan05.run_calibration()

    assert result["profile"] == "formal"
    assert result["repetitions_per_scenario"] == 2
    row = result["sharpe"][0]
    assert row["coverage"]["trials"] == 1
    assert row["criteria"] == {"coverage": "within_tolerance", "false_positive": "within_tolerance"}
    assert row["failures"] == {"zero_variance": 1}
    assert result["status"] == "not_calibrated"
