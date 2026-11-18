"""Tests for PLAN-04 paired-tail simulation calibration and resampling."""

from __future__ import annotations

import json
import math

import numpy as np
import pytest

from py_core.research import plan04_paired_tail
from py_core.research.plan04_paired_tail import (
    TailBand,
    TailFamilyBands,
    bootstrap_tail_bands,
)
from py_core.tools import calibrate_plan04_tail


def _family_bands(
    truth: np.ndarray,
    *,
    overrides: dict[tuple[int, int], tuple[float, float]] | None = None,
) -> tuple[TailFamilyBands, ...]:
    results = []
    overrides = overrides or {}
    for length_index, half_width in enumerate((0.01, 0.02, 0.03)):
        bands = []
        for pair_index, true_value in enumerate(truth):
            lower, upper = overrides.get(
                (length_index, pair_index),
                (float(true_value) - 0.01, float(true_value) + 0.01),
            )
            bands.append(
                TailBand(
                    reference_tail=0.0,
                    trend_tail=float(true_value),
                    difference=float(true_value),
                    lower=lower,
                    upper=upper,
                )
            )
        results.append(TailFamilyBands(half_width, 19, tuple(bands)))
    return tuple(results)


def test_scenarios_have_known_mixed_truth_and_reproducible_alternating_panels() -> None:
    assert tuple(scenario.name for scenario in calibrate_plan04_tail.SCENARIOS) == (
        "gaussian_null",
        "ar_positive_null",
        "student_t5_null",
        "ar_mixed_alternative",
    )
    for scenario in calibrate_plan04_tail.SCENARIOS:
        truth = calibrate_plan04_tail.true_differences(scenario)
        assert truth.shape == (12,)
        if scenario.name == "ar_mixed_alternative":
            np.testing.assert_array_equal(truth[:6], np.full(6, -0.006))
            np.testing.assert_array_equal(truth[6:], np.zeros(6))
        else:
            np.testing.assert_array_equal(truth, np.zeros(12))

        first = calibrate_plan04_tail.simulated_returns(scenario, np.random.default_rng(20261128))
        second = calibrate_plan04_tail.simulated_returns(scenario, np.random.default_rng(20261128))
        assert first.shape == (365, 24)
        assert np.all(np.isfinite(first))
        np.testing.assert_array_equal(first, second)


def test_replication_coverage_is_familywise_for_each_length() -> None:
    scenario = calibrate_plan04_tail.SCENARIOS[-1]
    truth = calibrate_plan04_tail.true_differences(scenario)
    results = _family_bands(
        truth,
        overrides={(1, 2): (float(truth[2]) - 0.02, float(truth[2]) - 0.01)},
    )

    metrics = calibrate_plan04_tail.assess_replication(truth, results)

    assert metrics.coverage == (True, False, True)
    assert metrics.half_widths == (0.01, 0.02, 0.03)


def test_false_positives_count_only_true_nulls_and_stable_detection_uses_all_lengths() -> None:
    scenario = calibrate_plan04_tail.SCENARIOS[-1]
    truth = calibrate_plan04_tail.true_differences(scenario)
    results = _family_bands(
        truth,
        overrides={
            (0, 0): (-0.01, -0.002),
            (1, 0): (-0.01, -0.002),
            (2, 0): (-0.01, -0.002),
            (0, 6): (0.001, 0.02),
        },
    )

    metrics = calibrate_plan04_tail.assess_replication(truth, results)

    assert metrics.false_positive == (True, False, False)
    assert metrics.stable_false_positive is False
    assert metrics.any_detected is True
    assert metrics.all_detected is False


def test_stable_false_positive_requires_strict_direction_for_a_null_at_every_length() -> None:
    scenario = calibrate_plan04_tail.SCENARIOS[-1]
    truth = calibrate_plan04_tail.true_differences(scenario)
    results = _family_bands(
        truth,
        overrides={(length_index, 7): (0.001, 0.02) for length_index in range(3)},
    )

    metrics = calibrate_plan04_tail.assess_replication(truth, results)

    assert metrics.false_positive == (True, True, True)
    assert metrics.stable_false_positive is True


def test_all_mixed_alternatives_are_detected_only_when_every_length_excludes_zero() -> None:
    scenario = calibrate_plan04_tail.SCENARIOS[-1]
    truth = calibrate_plan04_tail.true_differences(scenario)
    overrides = {
        (length_index, pair_index): (float(truth[pair_index]) - 0.002, float(truth[pair_index]) + 0.002)
        for length_index in range(3)
        for pair_index in range(6)
    }
    results = _family_bands(truth, overrides=overrides)

    metrics = calibrate_plan04_tail.assess_replication(truth, results)

    assert metrics.any_detected is True
    assert metrics.all_detected is True


def test_optimized_resampled_tail_means_match_naive_sort_oracle_with_prefix_skips(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    n_dates = 41
    n_resamples = 19
    tail_count = math.ceil(n_dates * 0.05)
    values = np.arange(n_dates, dtype=np.float64) / 100
    returns = np.column_stack((values, values * 2, values * 3, values * 4))
    indices = np.full((n_resamples, n_dates), 30, dtype=np.intp)
    indices[0] = 40
    indices[1] = np.concatenate((np.full(1, 10), np.full(2, 11), np.full(n_dates - 3, 12)))
    indices[2] = 0
    indices[3] = 39
    calls: list[tuple[int, float, int]] = []

    def fixed_indices(
        observations: int,
        *,
        block_length: float,
        n_resamples: int,
        rng: np.random.Generator,
    ) -> np.ndarray:
        calls.append((observations, block_length, n_resamples))
        return indices.copy()

    monkeypatch.setattr(plan04_paired_tail, "stationary_bootstrap_indices", fixed_indices)
    result = bootstrap_tail_bands(
        returns,
        block_length=7,
        n_resamples=n_resamples,
        rng=np.random.default_rng(8),
    )

    observed_tails = np.sort(returns, axis=0)[:tail_count].mean(axis=0)
    observed_differences = observed_tails[1::2] - observed_tails[::2]
    family_errors = []
    for draw_indices in indices:
        draw_tails = np.sort(returns[draw_indices], axis=0)[:tail_count].mean(axis=0)
        draw_differences = draw_tails[1::2] - draw_tails[::2]
        family_errors.append(np.max(np.abs(draw_differences - observed_differences)))
    rank = math.ceil((n_resamples + 1) * 0.95)
    expected_half_width = np.sort(family_errors)[rank - 1]

    assert calls == [(n_dates, 7, n_resamples)]
    assert result.tail_observations == tail_count == 3
    assert result.half_width == pytest.approx(expected_half_width)
    for pair_index, band in enumerate(result.bands):
        difference = observed_differences[pair_index]
        assert band.reference_tail == pytest.approx(observed_tails[2 * pair_index])
        assert band.trend_tail == pytest.approx(observed_tails[2 * pair_index + 1])
        assert band.difference == pytest.approx(difference)
        assert band.lower == pytest.approx(difference - expected_half_width)
        assert band.upper == pytest.approx(difference + expected_half_width)


def test_smoke_calibration_is_deterministic_and_keeps_smoke_status() -> None:
    first = calibrate_plan04_tail.run_calibration(smoke=True, workers=1)
    second = calibrate_plan04_tail.run_calibration(smoke=True, workers=1)

    assert first == second
    assert first["protocol"] == "plan04-tail-coverage-v1"
    assert first["profile"] == "smoke"
    assert first["status"] == "smoke"
    assert first["repetitions_per_scenario"] == 4
    assert first["resamples_per_length"] == 39
    assert len(first["results"]) == 4
    for result in first["results"]:
        assert result["valid_repetitions"] == 4
        assert result["failures"] == {}
        assert len(result["blocks"]) == 3
    json.dumps(first, allow_nan=False)
