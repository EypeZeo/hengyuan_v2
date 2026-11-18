"""Paired stationary bootstrap for aligned per-period net returns."""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass
from numbers import Real

import numpy as np


@dataclass(frozen=True, slots=True)
class PairedMeanInterval:
    mean_difference: float
    lower: float
    upper: float
    one_sided_lower: float


@dataclass(frozen=True, slots=True)
class FixedFamilyTest:
    winner: int
    mean_difference: float
    p_value: float


def stationary_bootstrap_indices(
    n_observations: int,
    *,
    block_length: float,
    n_resamples: int,
    rng: np.random.Generator,
) -> np.ndarray:
    """Draw circular row indices with geometrically distributed block lengths."""
    if isinstance(n_observations, bool) or not isinstance(n_observations, (int, np.integer)) or n_observations < 2:
        raise ValueError("at least two observations are required")
    if isinstance(n_resamples, bool) or not isinstance(n_resamples, (int, np.integer)) or n_resamples < 2:
        raise ValueError("at least two resamples are required")
    if isinstance(block_length, bool) or not isinstance(block_length, Real) or block_length < 1:
        raise ValueError("expected block length must be finite and at least one")
    try:
        restart_probability = 1 / float(block_length)
    except OverflowError as error:
        raise ValueError("expected block length must be finite and at least one") from error
    if not 0 < restart_probability <= 1:
        raise ValueError("expected block length must be finite and at least one")

    size = (n_resamples, n_observations)
    starts = rng.integers(n_observations, size=size, dtype=np.intp)
    restarts = rng.random(size) < restart_probability
    restarts[:, 0] = True
    positions = np.arange(n_observations, dtype=np.intp)
    last_start = np.maximum.accumulate(np.where(restarts, positions, 0), axis=1)
    return (np.take_along_axis(starts, last_start, axis=1) + positions - last_start) % n_observations


def _paired_differences(returns: Sequence[Sequence[float]] | np.ndarray) -> np.ndarray:
    try:
        if np.ma.is_masked(returns):
            raise ValueError("masked observations are unsupported")
        values = np.asarray(returns)
        if values.dtype.kind in "cmM" or (
            values.dtype.kind == "O"
            and any(isinstance(v, (complex, np.complexfloating, np.datetime64, np.timedelta64)) for v in values.flat)
        ):
            raise ValueError("returns must be real numbers")
        with np.errstate(over="ignore", invalid="ignore"):
            values = np.asarray(values, dtype=np.float64)
    except (TypeError, ValueError, OverflowError) as error:
        raise ValueError("returns must be a complete real-valued matrix") from error
    if values.ndim != 2 or values.shape[0] < 2 or values.shape[1] < 2:
        raise ValueError("returns must contain at least two rows and a benchmark and candidate column")
    if not np.all(np.isfinite(values)):
        raise ValueError("returns contain NaN or infinity")
    with np.errstate(over="ignore", invalid="ignore"):
        differences = values[:, 1:] - values[:, :1]
    if not np.all(np.isfinite(differences)):
        raise ValueError("paired differences are not finite")
    return differences


def _resample_mean_errors(
    differences: np.ndarray,
    block_length: float,
    n_resamples: int,
    rng: np.random.Generator,
) -> tuple[np.ndarray, np.ndarray]:
    indices = stationary_bootstrap_indices(
        differences.shape[0], block_length=block_length, n_resamples=n_resamples, rng=rng
    )
    with np.errstate(over="ignore", invalid="ignore"):
        observed = np.mean(differences, axis=0)
        centered = differences - observed
        # The same row indices are applied to every candidate and its benchmark difference.
        errors = np.mean(centered[indices], axis=1)
    if not np.all(np.isfinite(observed)) or not np.all(np.isfinite(errors)):
        raise ValueError("bootstrap means are not finite")
    return observed, errors


def _single_pair(returns: Sequence[Sequence[float]] | np.ndarray, alpha: float) -> np.ndarray:
    if not isinstance(alpha, Real) or not 0 < alpha < 1:
        raise ValueError("alpha must be between zero and one")
    differences = _paired_differences(returns)
    if differences.shape[1] != 1:
        raise ValueError("paired comparison requires exactly one candidate column")
    return differences


def _mean_interval(mean: float, errors: np.ndarray, alpha: float, scale: float = 1.0) -> PairedMeanInterval:
    with np.errstate(over="ignore", invalid="ignore"):
        low_error, high_error, one_sided_error = np.quantile(
            errors, (alpha / 2, 1 - alpha / 2, 1 - alpha), method="linear"
        )
    result = PairedMeanInterval(
        mean, mean - scale * float(high_error), mean - scale * float(low_error), mean - scale * float(one_sided_error)
    )
    if not all(math.isfinite(value) for value in (result.lower, result.upper, result.one_sided_lower)):
        raise ValueError("bootstrap interval is not finite")
    return result


def compare_paired_mean(
    returns: Sequence[Sequence[float]] | np.ndarray,
    *,
    block_length: float,
    n_resamples: int,
    rng: np.random.Generator,
    alpha: float = 0.05,
) -> PairedMeanInterval:
    """Estimate a fixed candidate's excess mean from benchmark and candidate columns.

    Rows must already be aligned and equally spaced, including flat periods.
    The basic bootstrap interval and the one-sided lower bound use the same draws.
    """
    differences = _single_pair(returns, alpha)
    observed, errors = _resample_mean_errors(differences, block_length, n_resamples, rng)
    return _mean_interval(float(observed[0]), errors[:, 0], alpha)


def _bartlett_mean_se(samples: np.ndarray, lag: int) -> np.ndarray:
    """Compute Bartlett standard errors for rows, using zero-padded moving sums."""
    n = samples.shape[-1]
    width = lag + 1
    with np.errstate(over="ignore", invalid="ignore", under="ignore"):
        # Center from a common observation to preserve small spreads near a nonzero mean.
        centered = samples - samples[..., :1]
        centered -= np.mean(centered, axis=-1, keepdims=True)
        prefix = np.empty(samples.shape[:-1] + (n + 2 * width - 1,), dtype=np.float64)
        prefix[..., :width] = 0.0
        np.cumsum(centered, axis=-1, out=prefix[..., width : width + n])
        prefix[..., width + n :] = prefix[..., width + n - 1 : width + n]
        window_sums = prefix[..., width:] - prefix[..., :-width]
        # Squaring the sums weights a lag-k product by (width-k)/width.
        np.square(window_sums, out=window_sums)
        se = np.sqrt(np.sum(window_sums, axis=-1) / (width * n * n))
    if not np.all(np.isfinite(se) & (se > 0.0)):
        raise ValueError("Bartlett standard errors must be positive and finite")
    return se


def compare_paired_mean_studentized(
    returns: Sequence[Sequence[float]] | np.ndarray,
    *,
    block_length: float,
    variance_lag: int,
    n_resamples: int,
    rng: np.random.Generator,
    alpha: float = 0.05,
) -> PairedMeanInterval:
    """Studentized paired interval with a Bartlett standard error for each draw.

    The input is an aligned benchmark/candidate pair. The Bartlett bandwidth is
    fixed in advance and applied to the original series and every resample.
    """
    difference = _single_pair(returns, alpha)[:, 0]
    n = difference.size
    if (
        isinstance(variance_lag, bool)
        or not isinstance(variance_lag, (int, np.integer))
        or not 0 <= variance_lag < n
    ):
        raise ValueError("variance lag must be an integer in [0, n-1]")
    indices = stationary_bootstrap_indices(n, block_length=block_length, n_resamples=n_resamples, rng=rng)
    scale = float(_bartlett_mean_se(difference, int(variance_lag)))
    with np.errstate(over="ignore", invalid="ignore", divide="ignore", under="ignore"):
        mean = float(np.mean(difference))
        centered_draws = (difference - mean)[indices]
        bootstrap_se = _bartlett_mean_se(centered_draws, int(variance_lag))
        pivots = np.mean(centered_draws, axis=1) / bootstrap_se
    if not math.isfinite(mean) or not np.all(np.isfinite(pivots)):
        raise ValueError("studentized bootstrap pivots are not finite")
    return _mean_interval(mean, pivots, alpha, scale)


def evaluate_fixed_family(
    returns: Sequence[Sequence[float]] | np.ndarray,
    *,
    block_length: float,
    n_resamples: int,
    rng: np.random.Generator,
) -> FixedFamilyTest:
    """Test the joint zero-excess-mean null for a fixed family using the maximum mean.

    Column zero is the benchmark. The winner is zero-indexed among candidate
    columns, with ties resolved in input order. The bootstrap p-value is approximate.
    """
    differences = _paired_differences(returns)
    observed, errors = _resample_mean_errors(differences, block_length, n_resamples, rng)
    winner = int(np.argmax(observed))
    best_mean = float(observed[winner])
    exceedances = int(np.count_nonzero(np.max(errors, axis=1) >= best_mean))
    return FixedFamilyTest(winner, best_mean, (1 + exceedances) / (1 + n_resamples))
