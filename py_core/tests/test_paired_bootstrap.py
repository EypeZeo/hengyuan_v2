"""Tests for paired stationary-bootstrap return comparisons."""

from __future__ import annotations

import json
import math

import numpy as np
import pytest

from py_core.research.paired_bootstrap import (
    compare_paired_mean,
    evaluate_fixed_family,
    stationary_bootstrap_indices,
)
from py_core.tools import calibrate_paired_bootstrap


class FixedIndexRng:
    """Supply fixed start indices and restart uniforms to the bootstrap sampler."""

    def __init__(self, starts: np.ndarray, uniforms: np.ndarray) -> None:
        self.starts = np.asarray(starts, dtype=np.intp)
        self.uniforms = np.asarray(uniforms, dtype=np.float64)

    def integers(self, high: int, *, size: tuple[int, int], dtype: type) -> np.ndarray:
        assert high == self.starts.shape[1]
        assert tuple(size) == self.starts.shape
        return self.starts.astype(dtype, copy=True)

    def random(self, size: tuple[int, int]) -> np.ndarray:
        assert tuple(size) == self.uniforms.shape
        return self.uniforms.copy()


def _all_restarted_rng(indices: np.ndarray) -> FixedIndexRng:
    return FixedIndexRng(indices, np.zeros(indices.shape, dtype=np.float64))


def test_stationary_indices_follow_geometric_restarts_and_wrap_circularly() -> None:
    starts = np.array([[4, 0, 3, 0, 0, 0], [1, 5, 0, 0, 2, 0]], dtype=np.intp)
    uniforms = np.array([[0.9, 0.9, 0.1, 0.9, 0.9, 0.9], [0.9, 0.1, 0.9, 0.9, 0.1, 0.9]])

    indices = stationary_bootstrap_indices(
        6,
        block_length=2,
        n_resamples=2,
        rng=FixedIndexRng(starts, uniforms),  # type: ignore[arg-type]
    )

    np.testing.assert_array_equal(indices, [[4, 5, 3, 4, 5, 0], [1, 5, 0, 1, 2, 3]])


def test_common_row_pairing_leaves_fixed_comparisons_unchanged() -> None:
    benchmark = np.array([0.25, 0.5, -0.25, -0.5])
    first_difference = np.array([0.125, -0.25, 0.5, -0.125])
    second_difference = np.array([-0.125, 0.25, 0.125, -0.25])
    returns = np.column_stack((benchmark, benchmark + first_difference, benchmark + second_difference))
    common_offset = np.array([2.0, -3.0, 4.0, 1.0])[:, None]
    shifted = returns + common_offset

    paired = compare_paired_mean(
        returns[:, :2], block_length=1, n_resamples=8, rng=np.random.default_rng(41)
    )
    shifted_paired = compare_paired_mean(
        shifted[:, :2], block_length=1, n_resamples=8, rng=np.random.default_rng(41)
    )
    family = evaluate_fixed_family(
        returns, block_length=1, n_resamples=8, rng=np.random.default_rng(41)
    )
    shifted_family = evaluate_fixed_family(
        shifted, block_length=1, n_resamples=8, rng=np.random.default_rng(41)
    )

    assert paired == shifted_paired
    assert family == shifted_family


def test_basic_interval_and_centered_family_max_match_direct_oracles() -> None:
    resample_indices = np.array([[0, 1, 2, 3], [0, 0, 0, 0], [3, 3, 3, 3], [0, 2, 0, 2]])
    returns = np.column_stack((np.zeros(4), [0.0, 1.0, 2.0, 3.0]))
    differences = returns[:, 1:] - returns[:, :1]
    observed = np.mean(differences, axis=0)
    centered = differences - observed
    errors = np.mean(centered[resample_indices], axis=1)[:, 0]
    low_error, high_error, one_sided_error = np.quantile(
        errors, (0.025, 0.975, 0.95), method="linear"
    )
    expected_mean = float(observed[0])

    interval = compare_paired_mean(
        returns,
        block_length=1,
        n_resamples=len(resample_indices),
        rng=_all_restarted_rng(resample_indices),  # type: ignore[arg-type]
    )

    assert interval.mean_difference == pytest.approx(expected_mean)
    assert interval.lower == pytest.approx(expected_mean - high_error)
    assert interval.upper == pytest.approx(expected_mean - low_error)
    assert interval.one_sided_lower == pytest.approx(expected_mean - one_sided_error)

    family_returns = np.column_stack((np.zeros(4), [0.0, 1.0, 2.0, 3.0], [0.0, 0.0, 0.0, 4.0]))
    family_differences = family_returns[:, 1:] - family_returns[:, :1]
    family_observed = family_differences.mean(axis=0)
    family_centered = family_differences - family_observed
    family_errors = np.mean(family_centered[resample_indices], axis=1)
    winner = int(np.argmax(family_observed))
    exceedances = int(np.count_nonzero(np.max(family_errors, axis=1) >= family_observed[winner]))
    expected_p = (1 + exceedances) / (1 + len(resample_indices))

    result = evaluate_fixed_family(
        family_returns,
        block_length=1,
        n_resamples=len(resample_indices),
        rng=_all_restarted_rng(resample_indices),  # type: ignore[arg-type]
    )

    assert result.winner == winner == 0
    assert result.mean_difference == pytest.approx(family_observed[winner])
    assert result.p_value == pytest.approx(expected_p)
    assert result.p_value == pytest.approx(0.4)


@pytest.mark.parametrize(
    "returns",
    [
        None,
        [1.0, 2.0],
        [[1.0, 2.0]],
        [[1.0], [2.0]],
        [[1.0, float("nan")], [2.0, 3.0]],
        np.ma.array([[1.0, 2.0], [3.0, 4.0]], mask=[[False, True], [False, False]]),
        np.array([[1.0 + 1.0j, 2.0], [2.0 + 2.0j, 3.0]], dtype=np.complex128),
    ],
    ids=["missing", "one-dimensional", "one-row", "no-candidate", "nan", "masked", "complex"],
)
def test_paired_comparison_rejects_invalid_or_missing_matrices(returns: object) -> None:
    with pytest.raises(ValueError):
        compare_paired_mean(
            returns, block_length=2, n_resamples=8, rng=np.random.default_rng(3)  # type: ignore[arg-type]
        )


def test_fixed_comparison_rejects_more_than_one_candidate() -> None:
    returns = np.zeros((4, 3))

    with pytest.raises(ValueError, match="exactly one candidate"):
        compare_paired_mean(returns, block_length=2, n_resamples=8, rng=np.random.default_rng(3))


@pytest.mark.parametrize(
    ("n_observations", "block_length", "n_resamples"),
    [(1, 2.0, 4), (8, 0.5, 4), (8, math.inf, 4), (8, None, 4), (8, 2.0, 1)],
)
def test_stationary_index_sampler_rejects_invalid_parameters(
    n_observations: int, block_length: float | None, n_resamples: int
) -> None:
    with pytest.raises(ValueError):
        stationary_bootstrap_indices(
            n_observations,
            block_length=block_length,  # type: ignore[arg-type]
            n_resamples=n_resamples,
            rng=np.random.default_rng(5),
        )


def test_paired_comparison_rejects_missing_alpha() -> None:
    with pytest.raises(ValueError, match="alpha"):
        compare_paired_mean(
            np.zeros((4, 2)),
            block_length=2,
            n_resamples=8,
            rng=np.random.default_rng(3),
            alpha=None,  # type: ignore[arg-type]
        )


def test_smoke_calibration_is_repeatable_finite_and_not_a_formal_pass() -> None:
    first = calibrate_paired_bootstrap.run_calibration(smoke=True)
    second = calibrate_paired_bootstrap.run_calibration(smoke=True)

    assert first == second
    assert first["profile"] == "smoke"
    assert first["status"] == "smoke"
    assert first["status"] != "within_tolerance"
    assert first["repetitions_per_scenario"] == 12
    assert first["resamples_per_repetition"] == 39
    assert len(first["results"]) == len(calibrate_paired_bootstrap.SCENARIOS) == 6

    for row in first["results"]:
        assert row["failures"] == {}
        metric = "positive_rejection" if row["scenario"]["candidates"] == 1 else "family_false_positive"
        assert row[metric]["trials"] == 12
        if row["scenario"]["candidates"] == 1:
            assert row["coverage"]["trials"] == 12

    json.dumps(first, allow_nan=False)
