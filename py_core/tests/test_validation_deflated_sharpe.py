"""Tests for py_core.validation.deflated_sharpe (批次 2, round 3).

The Monte Carlo calibration test at the bottom is the same experiment used to validate the
formula before trusting it (see deflated_sharpe.py's module docstring): the DSR of the BEST
of n_trials genuinely zero-edge (pure Gaussian noise) return series should average close to
0.5 across many repetitions, and become more conservative (lower) as n_trials grows for a
fixed observed Sharpe -- both checked directly, not assumed.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

from py_core.validation.deflated_sharpe import (
    _norm_cdf,
    _norm_ppf,
    compute_deflated_sharpe_ratio,
    deflated_sharpe_ratio,
    expected_max_sharpe_ratio,
    sharpe_ratio_standard_error,
)

# ---------------------------------------------------------------------------
# _norm_cdf / _norm_ppf: hand-verified against well-known standard normal quantiles
# ---------------------------------------------------------------------------


def test_norm_ppf_matches_well_known_quantiles() -> None:
    assert _norm_ppf(0.5) == pytest.approx(0.0, abs=1e-9)
    assert _norm_ppf(0.975) == pytest.approx(1.959963985, abs=1e-8)
    assert _norm_ppf(0.025) == pytest.approx(-1.959963985, abs=1e-8)
    assert _norm_ppf(0.8413447) == pytest.approx(1.0, abs=1e-6)


def test_norm_cdf_matches_well_known_values() -> None:
    assert _norm_cdf(0.0) == pytest.approx(0.5, abs=1e-9)
    assert _norm_cdf(1.959963985) == pytest.approx(0.975, abs=1e-8)
    assert _norm_cdf(-1.959963985) == pytest.approx(0.025, abs=1e-8)


@pytest.mark.parametrize("p", [0.001, 0.01, 0.1, 0.3, 0.5, 0.7, 0.9, 0.99, 0.999])
def test_norm_cdf_and_norm_ppf_round_trip(p: float) -> None:
    assert _norm_cdf(_norm_ppf(p)) == pytest.approx(p, abs=1e-6)


def test_norm_ppf_rejects_out_of_range_probability() -> None:
    with pytest.raises(ValueError, match="p must be"):
        _norm_ppf(0.0)
    with pytest.raises(ValueError, match="p must be"):
        _norm_ppf(1.0)


# ---------------------------------------------------------------------------
# sharpe_ratio_standard_error(): reduces to the textbook normal-returns formula
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("sharpe_ratio", [0.0, 0.05, -0.1, 0.3])
@pytest.mark.parametrize("n_observations", [10, 252, 1000])
def test_sharpe_ratio_standard_error_reduces_to_textbook_formula_under_normality(
    sharpe_ratio: float, n_observations: int
) -> None:
    # skewness=0, kurtosis=3 (normal distribution) -> sqrt((1 + 0.5*SR^2) / (T-1)) (Lo 2002).
    got = sharpe_ratio_standard_error(
        sharpe_ratio, skewness=0.0, kurtosis=3.0, n_observations=n_observations
    )
    expected = math.sqrt((1.0 + 0.5 * sharpe_ratio**2) / (n_observations - 1))
    assert got == pytest.approx(expected, rel=1e-12)


def test_sharpe_ratio_standard_error_rejects_too_few_observations() -> None:
    with pytest.raises(ValueError, match="n_observations"):
        sharpe_ratio_standard_error(0.1, skewness=0.0, kurtosis=3.0, n_observations=1)


def test_sharpe_ratio_standard_error_rejects_mathematically_inconsistent_inputs() -> None:
    # An extreme negative kurtosis paired with a large sharpe_ratio can drive the variance
    # formula negative -- this is a genuine mathematical inconsistency in the inputs, not a
    # scenario the function should silently paper over.
    with pytest.raises(ValueError, match="不自洽"):
        sharpe_ratio_standard_error(10.0, skewness=0.0, kurtosis=-100.0, n_observations=100)


# ---------------------------------------------------------------------------
# expected_max_sharpe_ratio(): monotonically increasing in n_trials
# ---------------------------------------------------------------------------


def test_expected_max_sharpe_ratio_increases_monotonically_with_n_trials() -> None:
    se = 0.1
    values = [expected_max_sharpe_ratio(se, n) for n in (2, 5, 10, 50, 200, 1000)]
    assert values == sorted(values)
    assert values[0] < values[-1]


def test_expected_max_sharpe_ratio_rejects_too_few_trials() -> None:
    with pytest.raises(ValueError, match="n_trials"):
        expected_max_sharpe_ratio(0.1, 1)


# ---------------------------------------------------------------------------
# deflated_sharpe_ratio(): monotonicity, extreme cases, validation
# ---------------------------------------------------------------------------


def test_deflated_sharpe_ratio_decreases_monotonically_with_n_trials_for_fixed_sharpe() -> None:
    """More trials tried -> more skepticism about a fixed observed Sharpe -- directly
    verified (see round-2 lesson about verifying engine/formula behavior empirically rather
    than only deriving it on paper)."""
    sharpe_ratio = 0.05
    values = [
        deflated_sharpe_ratio(sharpe_ratio, n_observations=252, n_trials=n)
        for n in (2, 5, 20, 100, 1000)
    ]
    assert values == sorted(values, reverse=True)
    assert values[0] > values[-1]


def test_deflated_sharpe_ratio_zero_sharpe_is_below_half() -> None:
    # A SHarpe of exactly 0 already sits below ANY n_trials>=2 selection-bias benchmark
    # (which is strictly positive), so DSR(0) < 0.5 always.
    dsr = deflated_sharpe_ratio(0.0, n_observations=252, n_trials=10)
    assert dsr < 0.5


def test_deflated_sharpe_ratio_large_genuine_sharpe_survives_many_trials() -> None:
    # A Sharpe far beyond what selection bias alone could plausibly produce should still
    # show a high DSR even after a large multiple-testing correction.
    dsr = deflated_sharpe_ratio(0.15, n_observations=252, n_trials=1000)
    assert dsr > 0.15  # deflated hard by n_trials=1000, but not driven to ~0 either
    dsr_few_trials = deflated_sharpe_ratio(0.15, n_observations=252, n_trials=2)
    assert dsr_few_trials > dsr, "fewer trials should deflate less than more trials, for the same observed Sharpe"


def test_deflated_sharpe_ratio_rejects_non_positive_standard_error_inputs() -> None:
    with pytest.raises(ValueError, match="n_observations"):
        deflated_sharpe_ratio(0.1, n_observations=1, n_trials=10)
    with pytest.raises(ValueError, match="n_trials"):
        deflated_sharpe_ratio(0.1, n_observations=252, n_trials=1)


# ---------------------------------------------------------------------------
# compute_deflated_sharpe_ratio(): returns-based convenience wrapper
# ---------------------------------------------------------------------------


def test_compute_deflated_sharpe_ratio_hand_verified_small_series() -> None:
    returns = [0.01, -0.005, 0.02, 0.0, -0.01, 0.015]
    arr = np.array(returns)
    expected_sharpe = arr.mean() / arr.std(ddof=1)

    result = compute_deflated_sharpe_ratio(returns, n_trials=5)
    assert result.sharpe_ratio == pytest.approx(expected_sharpe, rel=1e-12)
    assert result.n_observations == 6
    assert result.n_trials == 5
    assert 0.0 <= result.deflated_sharpe_ratio <= 1.0


def test_compute_deflated_sharpe_ratio_rejects_too_few_returns() -> None:
    with pytest.raises(ValueError, match="至少需要"):
        compute_deflated_sharpe_ratio([0.01], n_trials=5)


def test_compute_deflated_sharpe_ratio_rejects_zero_variance_returns() -> None:
    with pytest.raises(ValueError, match="标准差为 0"):
        compute_deflated_sharpe_ratio([0.01, 0.01, 0.01, 0.01], n_trials=5)


def test_compute_deflated_sharpe_ratio_genuine_edge_with_few_trials_shows_high_dsr() -> None:
    rng = np.random.default_rng(1)
    returns = (rng.normal(0.002, 0.01, size=500)).tolist()  # clear positive per-bar drift
    result = compute_deflated_sharpe_ratio(returns, n_trials=2)
    assert result.deflated_sharpe_ratio > 0.9


# ---------------------------------------------------------------------------
# Monte Carlo calibration: DSR of the best of n_trials pure-noise draws should
# average close to 0.5 across many repetitions -- the formula-validation
# experiment from the design phase, made a permanent regression test.
#
# A first version of this file also had a single-seed variant of this exact experiment
# ("best of 50 pure-noise draws should show DSR < 0.5") -- it failed under a different seed
# (DSR landed at 0.519, barely above 0.5). That is not a bug: DSR is *designed* to be
# approximately a coin flip around 0.5 under the null for any ONE draw -- asserting a single
# random draw lands on a specific side of 0.5 is exactly the same flaky-single-seed mistake
# round 1's walk-forward controls already ran into (see test_validation_walk_forward.py).
# The fix here is the same as there: only ever assert this kind of null-calibration property
# after averaging across many independent repetitions, which is what the test below does.
# ---------------------------------------------------------------------------


def test_dsr_monte_carlo_calibration_under_null_averages_near_half() -> None:
    rng = np.random.default_rng(42)
    n_trials = 20
    n_observations = 252
    n_experiments = 400  # smaller than the design-time 3000-rep validation, kept fast for CI

    dsr_values = []
    for _ in range(n_experiments):
        trial_returns = rng.normal(0.0, 0.01, size=(n_trials, n_observations))
        sharpes = trial_returns.mean(axis=1) / trial_returns.std(axis=1, ddof=1)
        best_idx = int(np.argmax(sharpes))
        winner = trial_returns[best_idx]
        result = compute_deflated_sharpe_ratio(winner.tolist(), n_trials=n_trials)
        dsr_values.append(result.deflated_sharpe_ratio)

    mean_dsr = sum(dsr_values) / len(dsr_values)
    # See module docstring: not a perfectly calibrated p-value (known Gumbel-approximation
    # limitation, confirmed compressed-but-centered at design time), so a generous band
    # around 0.5 rather than a tight one.
    assert 0.35 < mean_dsr < 0.65, (
        f"DSR of the best of {n_trials} pure-noise trials should average close to 0.5 across "
        f"many repetitions -- got {mean_dsr:.3f}, suggesting the deflation is systematically "
        "over- or under-correcting"
    )


# ---------------------------------------------------------------------------
# 批次 4 修复回归 -- DSR-NANOPEN-044 / DSR-MOMENT-045 / DSR-PPF-TAIL-057
# ---------------------------------------------------------------------------


def test_compute_deflated_sharpe_ratio_rejects_nan_in_returns() -> None:
    """AUDIT DSR-NANOPEN-044: previously returned dsr=nan silently instead of raising --
    every guard downstream (std<=0, variance<0, se<=0) is NaN-transparent."""
    with pytest.raises(ValueError, match="NaN"):
        compute_deflated_sharpe_ratio([0.01, float("nan"), 0.02, 0.03], n_trials=10)


def test_compute_deflated_sharpe_ratio_rejects_inf_in_returns() -> None:
    with pytest.raises(ValueError, match="NaN"):
        compute_deflated_sharpe_ratio([0.01, float("inf"), 0.02, 0.03], n_trials=10)


def test_sharpe_ratio_standard_error_rejects_nan_inputs() -> None:
    """The lower-level function must also reject NaN even when called directly (not just
    through compute_deflated_sharpe_ratio's entry-point guard)."""
    with pytest.raises(ValueError, match="无效"):
        sharpe_ratio_standard_error(
            float("nan"), skewness=0.0, kurtosis=3.0, n_observations=100
        )
    with pytest.raises(ValueError, match="无效"):
        sharpe_ratio_standard_error(
            1.0, skewness=float("nan"), kurtosis=3.0, n_observations=100
        )


def test_skewness_kurtosis_use_population_std_not_sample_std() -> None:
    """AUDIT DSR-MOMENT-045: skewness/kurtosis were standardized by the ddof=1 sample std
    while averaged with an unweighted (/n, ddof=0-shaped) np.mean -- an internally
    inconsistent mix that understated skewness by ((n-1)/n)**1.5 and kurtosis by
    ((n-1)/n)**2. With the fix (population std throughout), an independently computed
    reference using population std must match exactly."""
    rng = np.random.default_rng(7)
    returns = rng.normal(0.0005, 0.01, size=30).tolist()
    result = compute_deflated_sharpe_ratio(returns, n_trials=5)

    arr = np.asarray(returns, dtype=np.float64)
    mean = float(arr.mean())
    pop_std = float(arr.std(ddof=0))
    expected_skew = float(np.mean(((arr - mean) / pop_std) ** 3))
    expected_kurt = float(np.mean(((arr - mean) / pop_std) ** 4))

    assert result.skewness == pytest.approx(expected_skew, rel=1e-12)
    assert result.kurtosis == pytest.approx(expected_kurt, rel=1e-12)

    # The old (buggy) ddof=1-standardized moments would differ from the fixed ones by
    # exactly ((n-1)/n)**1.5 / ((n-1)/n)**2 -- confirm the fix actually changed the value,
    # not just that it matches a reference computed the same (right) way.
    sample_std = float(arr.std(ddof=1))
    old_buggy_skew = float(np.mean(((arr - mean) / sample_std) ** 3))
    assert result.skewness != pytest.approx(old_buggy_skew, rel=1e-6)


def test_norm_ppf_matches_independent_acklam_reference_in_the_deep_tail() -> None:
    """AUDIT DSR-PPF-TAIL-057: the replaced bisection implementation disagreed with an
    independent Acklam+Newton reference by 1.61e-11 at p=0.999999, growing worse at more
    extreme p. The module now uses that same higher-accuracy method directly, so it must
    round-trip through _norm_cdf to machine precision even deep in the tail."""
    for p in (0.999999, 1.0 - 1e-9, 1e-9, 1.0 - 1e-12):
        x = _norm_ppf(p)
        assert _norm_cdf(x) == pytest.approx(p, rel=1e-9, abs=1e-15)
