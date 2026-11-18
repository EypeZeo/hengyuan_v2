"""Tests for ranked simultaneous confidence bands on PLAN-04 tail differences."""

from __future__ import annotations

import json
import math

import numpy as np
import pytest

from py_core.research import plan04_paired_tail
from py_core.research.plan04_paired_tail import (
    bootstrap_tail_band_variants,
    bootstrap_tail_bands,
)
from py_core.tools import calibrate_plan04_tail_ranked


def _oracle_bands(
    values: np.ndarray,
    indices: np.ndarray,
    *,
    quantile: float,
) -> tuple[float, np.ndarray, np.ndarray, np.ndarray]:
    tail_count = math.ceil(values.shape[0] * 0.05)
    observed_tails = np.sort(values, axis=0)[:tail_count].mean(axis=0)
    observed = observed_tails[1::2] - observed_tails[::2]
    errors = []
    for selected_rows in indices:
        draw_tails = np.sort(values[selected_rows], axis=0)[:tail_count].mean(axis=0)
        draw_differences = draw_tails[1::2] - draw_tails[::2]
        errors.append(np.max(np.abs(draw_differences - observed)))
    rank = math.ceil((len(indices) + 1) * quantile)
    half_width = float(np.sort(errors)[rank - 1])
    return half_width, observed_tails, observed - half_width, observed + half_width


def test_rank_variants_use_one_shared_sampler_and_match_naive_rank_oracle(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    n_dates = 40
    n_resamples = 39
    index_draws = np.tile(np.arange(1, n_dates, dtype=np.intp)[:, None], (1, n_dates))
    row = np.arange(n_dates, dtype=np.float64)
    difference_a = row**2 / 100
    difference_b = row**2 / 200 + row / 100
    values = np.column_stack((row / 10, row / 10 + difference_a, row / 5, row / 5 + difference_b))
    calls: list[tuple[int, float, int, np.random.Generator]] = []

    def fixed_indices(
        observations: int,
        *,
        block_length: float,
        n_resamples: int,
        rng: np.random.Generator,
    ) -> np.ndarray:
        calls.append((observations, block_length, n_resamples, rng))
        return index_draws.copy()

    monkeypatch.setattr(plan04_paired_tail, "stationary_bootstrap_indices", fixed_indices)
    rng = np.random.default_rng(20261129)

    baseline, adjusted = bootstrap_tail_band_variants(
        values,
        block_length=7,
        n_resamples=n_resamples,
        rng=rng,
    )

    expected_baseline = _oracle_bands(values, index_draws, quantile=0.95)
    expected_adjusted = _oracle_bands(values, index_draws, quantile=0.965)

    assert len(calls) == 1
    assert calls[0][:3] == (n_dates, 7, n_resamples)
    assert calls[0][3] is rng
    assert baseline.half_width == pytest.approx(expected_baseline[0])
    assert adjusted.half_width == pytest.approx(expected_adjusted[0])
    assert adjusted.half_width > baseline.half_width
    assert baseline.tail_observations == adjusted.tail_observations == 2
    for result, expected in ((baseline, expected_baseline), (adjusted, expected_adjusted)):
        for pair_index, band in enumerate(result.bands):
            assert band.reference_tail == pytest.approx(expected[1][2 * pair_index])
            assert band.trend_tail == pytest.approx(expected[1][2 * pair_index + 1])
            assert band.difference == pytest.approx(expected[1][2 * pair_index + 1] - expected[1][2 * pair_index])
            assert band.lower == pytest.approx(expected[2][pair_index])
            assert band.upper == pytest.approx(expected[3][pair_index])


def test_default_bootstrap_result_remains_the_baseline_variant(monkeypatch: pytest.MonkeyPatch) -> None:
    n_dates = 40
    n_resamples = 39
    index_draws = np.tile(np.arange(1, n_dates, dtype=np.intp)[:, None], (1, n_dates))
    row = np.arange(n_dates, dtype=np.float64)
    values = np.column_stack((row / 10, row / 10 + row**2 / 100))

    monkeypatch.setattr(
        plan04_paired_tail,
        "stationary_bootstrap_indices",
        lambda observations, *, block_length, n_resamples, rng: index_draws.copy(),
    )
    baseline, _adjusted = bootstrap_tail_band_variants(
        values,
        block_length=14,
        n_resamples=n_resamples,
        rng=np.random.default_rng(10),
    )
    original = bootstrap_tail_bands(
        values,
        block_length=14,
        n_resamples=n_resamples,
        rng=np.random.default_rng(10),
    )

    assert baseline == original


def _replication_metrics(primary_coverage: bool) -> calibrate_plan04_tail_ranked.ReplicationMetrics:
    base = calibrate_plan04_tail_ranked.ReplicationMetrics
    return base(
        coverage=(False, primary_coverage, False),
        false_positive=(False, False, False),
        stable_false_positive=False,
        any_detected=False,
        all_detected=False,
        half_widths=(0.1, 0.1, 0.1),
    )


def test_primary_coverage_gain_uses_paired_discordant_outcomes() -> None:
    paired = calibrate_plan04_tail_ranked.PairedReplication
    outcomes = tuple(
        paired(_replication_metrics(baseline), _replication_metrics(adjusted))
        for baseline, adjusted in (
            *((False, True),) * 15,
            *((True, False),) * 5,
            *((False, False),) * 20,
        )
    )

    result = calibrate_plan04_tail_ranked.compare_primary_coverage(outcomes)

    assert result["baseline_only"] == 5
    assert result["adjusted_only"] == 15
    assert result["net_gain"] == pytest.approx(0.25)
    assert result["discordant_adjusted_coverage"]["events"] == 15
    assert result["discordant_adjusted_coverage"]["trials"] == 20
    assert result["discordant_adjusted_coverage"]["lower"] > 0.5
    assert result["criterion"] == "within_tolerance"


def test_primary_coverage_gain_fails_when_point_gain_or_paired_lower_bound_is_too_small() -> None:
    paired = calibrate_plan04_tail_ranked.PairedReplication
    outcomes = tuple(
        paired(_replication_metrics(baseline), _replication_metrics(adjusted))
        for baseline, adjusted in (
            *((False, True),) * 10,
            *((True, False),) * 10,
            *((False, False),) * 20,
        )
    )

    result = calibrate_plan04_tail_ranked.compare_primary_coverage(outcomes)

    assert result["net_gain"] == pytest.approx(0)
    assert result["discordant_adjusted_coverage"]["fraction"] == pytest.approx(0.5)
    assert result["discordant_adjusted_coverage"]["lower"] < 0.5
    assert result["criterion"] != "within_tolerance"


def test_ranked_smoke_calibration_is_deterministic_and_reports_frozen_protocol() -> None:
    first = calibrate_plan04_tail_ranked.run_calibration(smoke=True, workers=1)
    second = calibrate_plan04_tail_ranked.run_calibration(smoke=True, workers=1)

    assert first == second
    assert first["protocol"] == "plan04-tail-rank-calibration-v1"
    assert first["status"] == "smoke"
    assert first["seed"] == 20261129
    assert first["repetitions_per_scenario"] == 4
    assert first["resamples_per_length"] == 39
    for result in first["results"]:
        assert result["baseline"]["valid_repetitions"] == 4
        assert result["adjusted"]["valid_repetitions"] == 4
        assert result["baseline"]["failures"] == {}
        assert result["adjusted"]["failures"] == {}
        assert result["paired_primary_coverage"] is not None
    json.dumps(first, allow_nan=False)
