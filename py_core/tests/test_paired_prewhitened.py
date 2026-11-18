"""Tests for AR(1)-prewhitened studentized paired intervals."""

from __future__ import annotations

import json

import numpy as np
import pytest

from py_core.research import paired_bootstrap
from py_core.research.paired_bootstrap import (
    _prewhitened_mean_se,
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


def _direct_prewhitened_mean_se(samples: np.ndarray, lag: int) -> np.ndarray:
    """Fit centered AR(1) and compute residual Bartlett LRV with denominator m=n-1."""
    values = np.asarray(samples, dtype=np.float64)
    n = values.shape[-1]
    centered = values - np.mean(values, axis=-1, keepdims=True)
    previous, current = centered[..., :-1], centered[..., 1:]
    phi = np.sum(previous * current, axis=-1) / np.sum(previous**2, axis=-1)
    residuals = current - phi[..., None] * previous
    residuals -= np.mean(residuals, axis=-1, keepdims=True)

    m = n - 1
    residual_lrv = np.zeros(values.shape[:-1], dtype=np.float64)
    for k in range(lag + 1):
        gamma = np.sum(residuals[..., k:] * residuals[..., : m - k], axis=-1) / m
        weight = 1.0 if k == 0 else 2.0 * (1.0 - k / (lag + 1.0))
        residual_lrv += weight * gamma
    return np.sqrt(residual_lrv / n) / np.abs(1.0 - phi)


@pytest.mark.parametrize("lag", [0, 2])
@pytest.mark.parametrize("batched", [False, True])
def test_prewhitened_mean_se_matches_direct_ar1_and_residual_bartlett(
    lag: int, batched: bool
) -> None:
    rows = np.array(
        [
            [2.0, 3.0, 0.0, 2.0, 4.0, 1.0, 3.0, 1.0, 4.0],
            [1.0, 4.0, 2.0, 0.0, 3.0, 5.0, 0.0, 4.0, 2.0],
            [2.0, 4.0, 3.0, 5.0, 0.0, 1.0, 6.0, 2.0, 3.0],
        ]
    )
    samples = rows if batched else rows[0]

    actual = _prewhitened_mean_se(samples, lag)
    expected = _direct_prewhitened_mean_se(samples, lag)

    np.testing.assert_allclose(actual, expected, rtol=1e-12, atol=1e-15)


def test_public_prewhitened_interval_matches_reference_pivots_with_same_seed() -> None:
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
    scale = float(_direct_prewhitened_mean_se(difference, variance_lag))
    centered_draws = (difference - mean)[indices]
    bootstrap_se = _direct_prewhitened_mean_se(centered_draws, variance_lag)
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
        prewhiten=True,
    )

    assert actual.mean_difference == pytest.approx(mean)
    assert actual.lower == pytest.approx(mean - scale * high_pivot)
    assert actual.upper == pytest.approx(mean - scale * low_pivot)
    assert actual.one_sided_lower == pytest.approx(mean - scale * one_sided_pivot)


def test_prewhitening_rejects_a_degenerate_original_fit() -> None:
    returns = np.column_stack((np.zeros(8), np.ones(8)))

    with pytest.raises(ValueError, match="identifiable stationary AR"):
        compare_paired_mean_studentized(
            returns,
            block_length=1,
            variance_lag=0,
            n_resamples=4,
            rng=np.random.default_rng(9),
            prewhiten=True,
        )


def test_prewhitening_rejects_a_nonstationary_unit_magnitude_fit() -> None:
    difference = np.array([1.0, -1.0, 1.0, -1.0, 1.0, -1.0])
    returns = np.column_stack((np.zeros(difference.size), difference))

    with pytest.raises(ValueError, match="identifiable stationary AR"):
        compare_paired_mean_studentized(
            returns,
            block_length=1,
            variance_lag=0,
            n_resamples=4,
            rng=np.random.default_rng(13),
            prewhiten=True,
        )


def test_prewhitening_rejects_a_degenerate_bootstrap_fit() -> None:
    difference = np.arange(6, dtype=np.float64)
    returns = np.column_stack((np.zeros(difference.size), difference))
    rng = FixedIndexRng(np.zeros((4, difference.size), dtype=np.intp))

    with pytest.raises(ValueError, match="identifiable stationary AR"):
        compare_paired_mean_studentized(
            returns,
            block_length=1,
            variance_lag=0,
            n_resamples=4,
            rng=rng,  # type: ignore[arg-type]
            prewhiten=True,
        )


def test_prewhitened_bandwidth_is_bounded_by_residual_length() -> None:
    difference = np.array([0.0, 1.0, -1.0, 2.0, 0.5, -0.5])
    returns = np.column_stack((np.zeros(difference.size), difference))

    with pytest.raises(ValueError, match="variance lag"):
        compare_paired_mean_studentized(
            returns,
            block_length=1,
            variance_lag=difference.size - 1,
            n_resamples=4,
            rng=np.random.default_rng(19),
            prewhiten=True,
        )


def test_prewhitened_runner_uses_identical_row_indices_for_both_intervals(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    draws: list[np.ndarray] = []
    original = paired_bootstrap.stationary_bootstrap_indices

    def record_indices(*args, **kwargs) -> np.ndarray:
        indices = original(*args, **kwargs)
        draws.append(indices)
        return indices

    monkeypatch.setattr(paired_bootstrap, "stationary_bootstrap_indices", record_indices)
    scenario = calibrate_paired_studentized.PAIR_SCENARIOS[1]

    result = calibrate_paired_studentized.calibrate_scenario(
        scenario,
        repetitions=1,
        n_resamples=16,
        seed=np.random.SeedSequence(23),
        prewhiten=True,
    )

    assert result["failures"] == {}
    assert len(draws) == 2
    np.testing.assert_array_equal(draws[0], draws[1])


def test_prewhitened_smoke_runner_records_fixed_protocol_and_seed() -> None:
    first = calibrate_paired_studentized.run_calibration(smoke=True, prewhiten=True)
    second = calibrate_paired_studentized.run_calibration(smoke=True, prewhiten=True)

    assert first == second
    assert first["protocol"] == "plan-05-prewhitened-paired-v1"
    assert first["seed"] == calibrate_paired_studentized.PREWHITENED_SEED
    assert first["profile"] == "smoke"
    assert first["status"] == "smoke"
    assert first["repetitions_per_scenario"] == 12
    assert first["resamples_per_repetition"] == 39
    assert len(first["results"]) == len(calibrate_paired_studentized.PAIR_SCENARIOS) == 4
    json.dumps(first, allow_nan=False)
