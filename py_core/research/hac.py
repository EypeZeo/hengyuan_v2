"""HAC (Newey--West Bartlett) standard error of a per-period Sharpe ratio.

PLAN-05a, implementing exactly the estimator specified in
``docs/RESEARCH_EXECUTION_PLAN.md`` §6.3 for a complete, equally spaced net-return
series (including flat periods):

    mu = mean(x), sigma2 = mean((x - mu)**2), s = mu / sqrt(sigma2)
    psi_t = (x_t - mu) / sigma - (s / 2) * ((x_t - mu)**2 / sigma2 - 1)
    gamma_k = sum_{t=k+1..n} (psi_t - mean(psi)) * (psi_{t-k} - mean(psi)) / n
    Omega = gamma_0 + 2 * sum_{k=1..L} (1 - k / (L + 1)) * gamma_k
    SE_HAC(s) = sqrt(Omega / n)

All quantities are **per-period**, never annualized. The Bartlett bandwidth ``L`` is
chosen and frozen by the caller during development, not optimized against the result.
The returned report also evaluates the adjacent non-negative bandwidths (``L-1`` and
``L+1`` where representable) so sensitivity is visible without another call.

This slice deliberately does **not** feed the HAC standard error into DSR, alter any
admission threshold, resample returns, or claim that calibration is complete. HAC
handles time dependence of the sampling uncertainty; it does not correct selection
bias from trying many strategies. Structural-break detection is also out of scope:
the stationarity, moment and weak-dependence assumptions remain the caller's
responsibility and must be checked before interpreting the estimate.

Data/estimation failures are values, not NaNs: inspect ``HacEstimate.status``. A
failed primary estimate has no standard error and no sensitivity estimates. A failed
adjacent estimate is retained in the sensitivity tuple with its own status/reason so
a bandwidth problem cannot disappear silently.
"""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass, field
from enum import StrEnum

import numpy as np

__all__ = [
    "HacEstimate",
    "HacReport",
    "HacStatus",
    "estimate_sharpe_hac",
]


class HacStatus(StrEnum):
    """Outcome of one HAC estimate; only ``OK`` carries a usable standard error."""

    OK = "ok"
    INVALID_INPUT = "invalid_input"
    INSUFFICIENT_DATA = "insufficient_data"
    INVALID_BANDWIDTH = "invalid_bandwidth"
    NON_FINITE_INPUT = "non_finite_input"
    ZERO_VARIANCE = "zero_variance"
    DEGENERATE_INFLUENCE = "degenerate_influence"
    INVALID_LONG_RUN_VARIANCE = "invalid_long_run_variance"
    NUMERIC_FAILURE = "numeric_failure"


@dataclass(frozen=True, slots=True)
class HacEstimate:
    """One bandwidth's estimate and its diagnostics."""

    status: HacStatus
    lag: int
    n_observations: int
    sharpe_ratio: float | None = None
    long_run_variance: float | None = None
    standard_error: float | None = None
    influence_mean: float | None = None
    autocovariances: tuple[float, ...] = ()
    influence_function: np.ndarray | None = field(default=None, compare=False, repr=False)
    reason: str = ""


@dataclass(frozen=True, slots=True)
class HacReport:
    """The requested bandwidth followed by its adjacent-bandwidth sensitivity."""

    primary: HacEstimate
    sensitivity: tuple[HacEstimate, ...] = ()


def _failure(status: HacStatus, lag: int, n_observations: int, reason: str) -> HacEstimate:
    return HacEstimate(status=status, lag=lag, n_observations=n_observations, reason=reason)


def _autocovariances(centered: np.ndarray, lag: int) -> tuple[float, ...]:
    """Population autocovariances gamma_0..gamma_L, evaluated independently per lag."""
    n = centered.size
    values: list[float] = []
    for k in range(lag + 1):
        # math.fsum makes the manual reference and the implementation insensitive to
        # numpy's reduction order; the formula itself is the spec's /n convention.
        products = (float(centered[t]) * float(centered[t - k]) for t in range(k, n))
        try:
            values.append(math.fsum(products) / n)
        except (OverflowError, ValueError):
            values.append(math.nan)
    return tuple(values)


def _estimate_one(x: np.ndarray, lag: int) -> HacEstimate:
    n = int(x.size)
    if np.all(x == x[0]):
        return _failure(HacStatus.ZERO_VARIANCE, lag, n, "the return series has zero population variance")
    # Power-of-two scaling preserves the Sharpe ratio and influence function and
    # keeps squared deviations representable. Center the differences from the
    # first observation so the mean's precision is relative to the spread.
    _, exponent = math.frexp(float(np.max(np.abs(x))))
    with np.errstate(over="ignore", invalid="ignore", divide="ignore", under="ignore"):
        scaled = np.ldexp(x, -exponent)
        shifted = scaled - scaled[0]
        deviations = shifted - np.mean(shifted)
        squares = deviations**2
        sigma2 = float(np.mean(squares))
        if not math.isfinite(sigma2) or sigma2 <= 0.0:
            return _failure(HacStatus.NUMERIC_FAILURE, lag, n, "the population variance is not positive and finite")
        sigma = math.sqrt(sigma2)
        mean = math.fsum(scaled) / n
        sharpe = mean / sigma
        influence = deviations / sigma - (sharpe / 2.0) * ((squares - sigma2) / sigma2)
    if not math.isfinite(sharpe) or not np.all(np.isfinite(influence)):
        return _failure(HacStatus.NUMERIC_FAILURE, lag, n, "the Sharpe ratio or its influence function is non-finite")

    with np.errstate(over="ignore", invalid="ignore", divide="ignore", under="ignore"):
        influence_mean = float(np.mean(influence))
        if not math.isfinite(influence_mean):
            return _failure(HacStatus.NUMERIC_FAILURE, lag, n, "the influence-function mean is non-finite")
        centered = influence - influence_mean
        autocovariances = _autocovariances(centered, lag)
    if not all(math.isfinite(value) for value in autocovariances):
        return _failure(HacStatus.NUMERIC_FAILURE, lag, n, "an autocovariance is non-finite")
    if autocovariances[0] == 0.0 and all(value == 0.0 for value in autocovariances):
        return _failure(
            HacStatus.DEGENERATE_INFLUENCE,
            lag,
            n,
            "the centered influence function has no variance, so no HAC standard error is identified",
            )

    weights = tuple(1.0 - k / (lag + 1.0) for k in range(1, lag + 1))
    try:
        weighted_covariances = math.fsum(
            weight * gamma for weight, gamma in zip(weights, autocovariances[1:], strict=True)
        )
    except (OverflowError, ValueError):
        return _failure(HacStatus.NUMERIC_FAILURE, lag, n, "the Bartlett covariance sum is non-finite")
    omega = autocovariances[0] + 2.0 * weighted_covariances
    if not math.isfinite(omega):
        return _failure(HacStatus.NUMERIC_FAILURE, lag, n, "the long-run variance is non-finite")
    if omega <= 0.0:
        return _failure(
            HacStatus.INVALID_LONG_RUN_VARIANCE,
            lag,
            n,
            f"the Bartlett long-run variance is not positive ({omega}) at bandwidth {lag}",
        )

    standard_error = math.sqrt(omega / n)
    if not math.isfinite(standard_error) or standard_error <= 0.0:
        return _failure(
            HacStatus.NUMERIC_FAILURE,
            lag,
            n,
            f"the HAC standard error is not positive and finite ({standard_error})",
        )

    return HacEstimate(
        status=HacStatus.OK,
        lag=lag,
        n_observations=n,
        sharpe_ratio=sharpe,
        long_run_variance=omega,
        standard_error=standard_error,
        influence_mean=influence_mean,
        autocovariances=autocovariances,
        influence_function=influence,
    )


def estimate_sharpe_hac(
    returns: Sequence[float] | np.ndarray,
    *,
    lag: int = 0,
    include_adjacent: bool = True,
) -> HacReport:
    """Estimate the HAC standard error and adjacent-bandwidth sensitivity.

    Args:
        returns: one-dimensional, complete and equally spaced per-period net returns.
        lag: frozen Bartlett bandwidth, an integer in ``[0, n-1]``. It is not selected
            from the resulting standard errors. ``lag=0`` is the no-autocovariance case.
        include_adjacent: also estimate ``lag-1`` if ``lag > 0`` and ``lag+1`` if
            ``lag < n-1``. Both estimates have independent statuses.
    """
    try:
        if np.ma.is_masked(returns):
            raise ValueError("returns must not contain masked observations")
        values = np.asarray(returns)
        if values.dtype.kind in "cmM" or (
            values.dtype.kind == "O"
            and any(
                isinstance(value, (complex, np.complexfloating, np.datetime64, np.timedelta64)) for value in values.flat
            )
        ):
            raise ValueError("returns must contain real numbers")
        with np.errstate(over="ignore", invalid="ignore"):
            x = np.asarray(values, dtype=np.float64)
    except (TypeError, ValueError, OverflowError) as exc:
        return HacReport(primary=_failure(HacStatus.INVALID_INPUT, lag if isinstance(lag, int) else -1, 0, str(exc)))
    n = int(x.size) if x.ndim == 1 else 0
    if x.ndim != 1:
        return HacReport(primary=_failure(HacStatus.INVALID_INPUT, lag if isinstance(lag, int) else -1, n, "returns must be one-dimensional"))
    if isinstance(lag, bool) or not isinstance(lag, (int, np.integer)):
        return HacReport(primary=_failure(HacStatus.INVALID_BANDWIDTH, -1, n, "lag must be an integer"))
    lag = int(lag)
    if n < 2:
        return HacReport(primary=_failure(HacStatus.INSUFFICIENT_DATA, lag, n, "at least two return observations are required"))
    if lag < 0 or lag >= n:
        return HacReport(primary=_failure(HacStatus.INVALID_BANDWIDTH, lag, n, f"lag must be in [0, {n - 1}]"))
    if not np.all(np.isfinite(x)):
        return HacReport(primary=_failure(HacStatus.NON_FINITE_INPUT, lag, n, "returns contain NaN or infinity"))

    primary = _estimate_one(x, lag)
    if primary.status is not HacStatus.OK or not include_adjacent:
        return HacReport(primary=primary)

    adjacent_lags = tuple(candidate for candidate in (lag - 1, lag + 1) if 0 <= candidate < n)
    sensitivity = tuple(_estimate_one(x, candidate) for candidate in adjacent_lags)
    return HacReport(primary=primary, sensitivity=sensitivity)
