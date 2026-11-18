"""Tests for the studentized paired stationary-bootstrap interval."""

from __future__ import annotations

import json
import math

import numpy as np
import pytest

from py_core.research import paired_bootstrap
from py_core.research.paired_bootstrap import (
    _bartlett_mean_se,
    compare_paired_mean_studentized,
    stationary_bootstrap_indices,
)
from py_core.tools import calibrate_paired_studentized


class FixedIndexRng:
    def __init__(self, starts: np.ndarray) -> None:
        self.starts = np.asarray(starts, dtype=np.intp)

    def integers(self, high: int, *, size: tuple[int, int], dtype: type) -> np.ndarray:
        assert high == self.starts.shape[1]
        assert tuple(size) == self.starts.shape
        return self.starts.astype(dtype, copy=True)

    def random(self, size: tuple[int, int]) -> np.ndarray:
        assert tuple(size) == self.starts.shape
        return np.zeros(self.starts.shape, dtype=np.float64)


def _direct_bartlett_mean_se(samples: np.ndarray, lag: int) -> np.ndarray:
    """Direct /n autocovariance definition, independent of the production moving sums."""
    values = np.asarray(samples, dtype=np.float64)
    centered = values - np.mean(values, axis=-1, keepdims=True)
    n = values.shape[-1]
    long_run_variance = np.zeros(values.shape[:-1], dtype=np.float64)
    for k in range(lag + 1):
        gamma = np.sum(centered[..., k:] * centered[..., : n - k], axis=-1) / n
        weight = 1.0 if k == 0 else 2.0 * (1.0 - k / (lag + 1.0))
        long_run_variance += weight * gamma
    return np.sqrt(long_run_variance / n)


@pytest.mark.parametrize("lag", [0, 2])
@pytest.mark.parametrize("batched", [False, True])
def test_bartlett_mean_se_matches_direct_centered_population_autocovariances(
    lag: int, batched: bool
) -> None:
    rows = np.array(
        [
            [0.01, -0.02, 0.03, 0.005, -0.01, 0.02, 0.015],
            [0.03, 0.02, -0.01, 0.01, -0.02, -0.005, 0.025],
            [0.00, 0.01, 0.02, 0.01, 0.00, -0.01, 0.03],
        ]
    )
    samples = rows if batched else rows[0]

    actual = _bartlett_mean_se(samples, lag)
    expected = _direct_bartlett_mean_se(samples, lag)

    np.testing.assert_allclose(actual, expected, rtol=1e-12, atol=1e-15)


def test_studentized_interval_matches_direct_pivot_inversion_with_same_indices() -> None:
    difference = np.array(
        [0.002, -0.001, 0.003, 0.004, -0.002, 0.001, 0.000, 0.005, -0.003, 0.002, -0.001, 0.004]
    )
    benchmark = np.array([0.01, 0.015, 0.012, 0.02, 0.018, 0.011, 0.013, 0.019, 0.017, 0.014, 0.016, 0.01])
    returns = np.column_stack((benchmark, benchmark + difference))
    block_length, variance_lag, n_resamples, alpha, seed = 3, 2, 64, 0.05, 731

    indices = stationary_bootstrap_indices(
        len(difference),
        block_length=block_length,
        n_resamples=n_resamples,
        rng=np.random.default_rng(seed),
    )
    mean = float(np.mean(difference))
    scale = float(_direct_bartlett_mean_se(difference, variance_lag))
    centered_draws = (difference - mean)[indices]
    bootstrap_se = _direct_bartlett_mean_se(centered_draws, variance_lag)
    pivots = np.mean(centered_draws, axis=1) / bootstrap_se
    low_pivot, high_pivot, one_sided_pivot = np.quantile(
        pivots, (alpha / 2, 1 - alpha / 2, 1 - alpha), method="linear"
    )

    actual = compare_paired_mean_studentized(
        returns,
        block_length=block_length,
        variance_lag=variance_lag,
        n_resamples=n_resamples,
        rng=np.random.default_rng(seed),
        alpha=alpha,
    )

    assert actual.mean_difference == pytest.approx(mean)
    assert actual.lower == pytest.approx(mean - scale * high_pivot)
    assert actual.upper == pytest.approx(mean - scale * low_pivot)
    assert actual.one_sided_lower == pytest.approx(mean - scale * one_sided_pivot)


def test_studentized_interval_rejects_zero_scale_in_original_series() -> None:
    returns = np.column_stack((np.zeros(6), np.ones(6)))

    with pytest.raises(ValueError, match="Bartlett standard errors"):
        compare_paired_mean_studentized(
            returns, block_length=1, variance_lag=0, n_resamples=4, rng=np.random.default_rng(9)
        )


def test_studentized_interval_rejects_zero_scale_in_any_bootstrap_draw() -> None:
    indices = np.zeros((4, 6), dtype=np.intp)
    rng = FixedIndexRng(indices)
    returns = np.column_stack((np.zeros(6), np.arange(6, dtype=np.float64)))

    with pytest.raises(ValueError, match="Bartlett standard errors"):
        compare_paired_mean_studentized(
            returns, block_length=1, variance_lag=0, n_resamples=4, rng=rng  # type: ignore[arg-type]
        )


@pytest.mark.parametrize("variance_lag", [-1, True, 0.5, 6])
def test_studentized_interval_rejects_invalid_variance_lags(variance_lag: object) -> None:
    returns = np.column_stack((np.zeros(6), np.arange(6, dtype=np.float64)))

    with pytest.raises(ValueError, match="variance lag"):
        compare_paired_mean_studentized(
            returns,
            block_length=2,
            variance_lag=variance_lag,  # type: ignore[arg-type]
            n_resamples=8,
            rng=np.random.default_rng(11),
        )


def test_basic_and_studentized_runner_use_identical_row_indices(monkeypatch: pytest.MonkeyPatch) -> None:
    draws: list[np.ndarray] = []
    original = paired_bootstrap.stationary_bootstrap_indices

    def record_indices(*args, **kwargs) -> np.ndarray:
        indices = original(*args, **kwargs)
        draws.append(indices)
        return indices

    monkeypatch.setattr(paired_bootstrap, "stationary_bootstrap_indices", record_indices)
    scenario = calibrate_paired_studentized.PAIR_SCENARIOS[0]

    result = calibrate_paired_studentized.calibrate_scenario(
        scenario, repetitions=1, n_resamples=16, seed=np.random.SeedSequence(17)
    )

    assert result["failures"] == {}
    assert len(draws) == 2
    np.testing.assert_array_equal(draws[0], draws[1])


def test_smoke_calibration_is_repeatable_finite_and_never_claims_formal_pass() -> None:
    first = calibrate_paired_studentized.run_calibration(smoke=True)
    second = calibrate_paired_studentized.run_calibration(smoke=True)

    assert first == second
    assert first["profile"] == "smoke"
    assert first["status"] == "smoke"
    assert first["status"] != "within_tolerance"
    assert first["repetitions_per_scenario"] == 12
    assert first["resamples_per_repetition"] == 39
    assert len(first["results"]) == len(calibrate_paired_studentized.PAIR_SCENARIOS) == 4
    for row in first["results"]:
        assert row["failures"] == {}
        assert row["studentized"]["coverage"]["trials"] == 12
        assert row["studentized"]["positive_rejection"]["trials"] == 12
        assert math.isfinite(row["studentized"]["mean_width"])
    json.dumps(first, allow_nan=False)
