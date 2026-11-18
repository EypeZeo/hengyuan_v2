"""Tests for prewhitened paired intervals using outward rank quantiles."""

from __future__ import annotations

import json
import math

import numpy as np
import pytest

from py_core.research import paired_bootstrap
from py_core.research.paired_bootstrap import (
    _outward_quantiles,
    compare_paired_prewhitened_quantiles,
)
from py_core.tools import calibrate_paired_studentized


def test_outward_quantiles_use_predeclared_ranks_and_require_finite_tails() -> None:
    errors = np.arange(399, dtype=np.float64)[::-1]

    assert _outward_quantiles(errors, 0.05) == (9.0, 389.0, 379.0)
    assert _outward_quantiles(np.arange(39, dtype=np.float64), 0.05) == (0.0, 38.0, 37.0)
    with pytest.raises(ValueError, match="too few draws"):
        _outward_quantiles(np.arange(38, dtype=np.float64), 0.05)


def test_outward_rank_interval_includes_linear_interval_outward_at_alpha_point_05() -> None:
    errors = np.array([((index * 73) % 401) - 200 for index in range(399)], dtype=np.float64)
    low_rank, high_rank, one_sided_rank = _outward_quantiles(errors, 0.05)
    low_linear, high_linear, one_sided_linear = np.quantile(
        errors, (0.025, 0.975, 0.95), method="linear"
    )

    assert low_rank <= low_linear
    assert high_rank >= high_linear
    assert one_sided_rank >= one_sided_linear


def test_public_ranked_intervals_invert_known_pivots_with_one_shared_pivot_call(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    mean, scale = 0.75, 1.5
    pivots = np.arange(399, dtype=np.float64)
    calls: list[tuple[tuple[object, ...], dict[str, object]]] = []

    def fixed_pivots(*args: object, **kwargs: object) -> tuple[float, float, np.ndarray]:
        calls.append((args, kwargs))
        return mean, scale, pivots

    monkeypatch.setattr(paired_bootstrap, "_studentized_paired_pivots", fixed_pivots)
    linear, ranked = compare_paired_prewhitened_quantiles(
        np.zeros((8, 2)),
        block_length=3,
        variance_lag=2,
        n_resamples=pivots.size,
        rng=np.random.default_rng(27),
        alpha=0.05,
    )

    assert len(calls) == 1
    assert calls[0][1]["prewhiten"] is True
    assert calls[0][1]["n_resamples"] == pivots.size
    low_linear, high_linear, one_sided_linear = np.quantile(
        pivots, (0.025, 0.975, 0.95), method="linear"
    )
    assert linear.mean_difference == pytest.approx(mean)
    assert linear.lower == pytest.approx(mean - scale * high_linear)
    assert linear.upper == pytest.approx(mean - scale * low_linear)
    assert linear.one_sided_lower == pytest.approx(mean - scale * one_sided_linear)
    assert ranked.mean_difference == pytest.approx(mean)
    assert ranked.lower == pytest.approx(mean - scale * 389.0)
    assert ranked.upper == pytest.approx(mean - scale * 9.0)
    assert ranked.one_sided_lower == pytest.approx(mean - scale * 379.0)
    assert ranked.lower <= linear.lower
    assert ranked.upper >= linear.upper
    assert ranked.one_sided_lower <= linear.one_sided_lower


def test_ranked_runner_reuses_the_basic_method_row_indices(monkeypatch: pytest.MonkeyPatch) -> None:
    draws: list[np.ndarray] = []
    original = paired_bootstrap.stationary_bootstrap_indices

    def record_indices(*args: object, **kwargs: object) -> np.ndarray:
        indices = original(*args, **kwargs)
        draws.append(indices)
        return indices

    monkeypatch.setattr(paired_bootstrap, "stationary_bootstrap_indices", record_indices)
    scenario = calibrate_paired_studentized.PAIR_SCENARIOS[1]
    result = calibrate_paired_studentized.calibrate_scenario(
        scenario,
        repetitions=1,
        n_resamples=39,
        seed=np.random.SeedSequence(20261124),
        ranked=True,
    )

    assert result["failures"] == {}
    assert "linear_prewhitened" in result
    assert len(draws) == 2
    np.testing.assert_array_equal(draws[0], draws[1])


def test_ranked_smoke_runner_is_repeatable_and_reports_smoke_profile() -> None:
    first = calibrate_paired_studentized.run_calibration(smoke=True, ranked=True)
    second = calibrate_paired_studentized.run_calibration(smoke=True, ranked=True)

    assert first == second
    assert first["protocol"] == "plan-05-ranked-paired-v1"
    assert first["seed"] == 20261124
    assert first["seed"] == calibrate_paired_studentized.RANKED_SEED
    assert first["profile"] == "smoke"
    assert first["status"] == "smoke"
    assert first["status"] != "within_tolerance"
    assert len(first["results"]) == len(calibrate_paired_studentized.PAIR_SCENARIOS) == 4
    for row in first["results"]:
        assert "linear_prewhitened" in row
        for method in ("studentized", "linear_prewhitened"):
            summary = row[method]
            assert summary["coverage"]["trials"] > 0
            assert summary["positive_rejection"]["trials"] > 0
            assert math.isfinite(summary["mean_width"])
    json.dumps(first, allow_nan=False)
