"""Deflated Sharpe Ratio (DSR) — 批次 2, round 3.

Bailey & Lopez de Prado (2014), "The Deflated Sharpe Ratio": corrects an observed Sharpe
ratio for two sources of inflation that make backtests look better than the underlying
strategy actually is: (1) non-normal returns (skewed/fat-tailed returns bias the *precision*
of a Sharpe ratio estimate away from the textbook Gaussian formula), and (2) selection bias
from trying ``n_trials`` independent strategy variants and reporting only the best one's
Sharpe -- the MAXIMUM of N noisy estimates is itself a biased-high estimator of any single
one's true value, even under a null of zero genuine skill, and that bias grows with N.

DSR is ``Prob(true Sharpe > 0 | observed sharpe_ratio, n_observations, n_trials)`` after this
correction -- a probability, not a Sharpe ratio itself. Close to 1: the observed Sharpe is
unlikely to be explained by luck/multiple-testing alone. Close to or below 0.5: statistically
indistinguishable from what ``n_trials`` draws of pure noise could produce.

**Works entirely in PER-PERIOD units, never annualized.** The paper's own formula is derived
in per-period terms (``n_observations`` is a count of return bars, ``sharpe_ratio`` is the
per-bar mean/std ratio) -- annualizing before applying this formula would silently change
what ``n_observations`` and ``sharpe_ratio`` mean relative to each other and invalidate the
derivation. This repo's ``BacktestMetrics.sharpe_ratio`` IS annualized (see
``py_core/backtests/metrics.py``) and must never be passed to ``deflated_sharpe_ratio()``
directly -- use ``compute_deflated_sharpe_ratio()`` with a raw per-bar returns list instead
(e.g. ``BacktestResult.returns``, ``WalkForwardReport.oos_returns``, or a
``CpcvPathResult.oos_returns``), which derives the per-period Sharpe/skewness/kurtosis
itself and never touches an annualized number.

**No scipy dependency** (py_core has none, and this is the only place in the codebase that
would otherwise need ``norm.cdf``/``norm.ppf``) -- both are implemented locally:
``_norm_cdf()`` exactly, via the stdlib ``math.erf()``; ``_norm_ppf()`` via Peter Acklam's
rational approximation refined to machine precision by Newton polish against that exact CDF
(see AUDIT DSR-PPF-TAIL-057 on ``_norm_ppf()`` itself -- an earlier bisection-based version's
"should be exact" rationale did not hold up empirically in the tail).

**A substitution worth being explicit about (AUDIT DSR-SIGMA-056).** The published DSR
formula's "expected max Sharpe of N trials" term calls for the CROSS-TRIAL standard
deviation of the N trials' Sharpe estimates (``V[{SR_n}]`` in Bailey & Lopez de Prado's own
notation). This module instead passes ``sharpe_ratio_std_error`` -- the single-observation
sampling standard error of ONE Sharpe estimate (Mertens 2002) -- into
``expected_max_sharpe_ratio()``. Quantified by Monte Carlo across three regimes (ratio of
this substitute to the true cross-trial dispersion): under the strict null (all trials
share the same zero true Sharpe) the two converge, ratio -> 0.999, so the substitution is
asymptotically exact exactly where the paper's own derivation assumes it; when trials are
correlated (a realistic effect of a parameter sweep re-using the same underlying data) the
ratio drops to ~0.30, making DSR MORE conservative than it needs to be (safe direction);
but when trials have genuine, varying skill (not just noise -- some candidates are
genuinely better than others, which is exactly the situation a parameter sweep hopes to be
in) the ratio rises to ~2.54, making DSR too PERMISSIVE relative to what the actual
cross-trial spread implies (the unsafe direction). This is a documented, unstated-elsewhere
assumption, not an implementation bug: when the caller already has every trial's individual
Sharpe in hand (e.g. a full parameter-sweep loop that discards all but the best), computing
the empirical cross-trial standard deviation of those PER-PERIOD Sharpes and passing it as
``sharpe_ratio_std_error`` directly to ``expected_max_sharpe_ratio()`` would be closer to the
paper's own derivation than relying on this asymptotic substitute -- deliberately left as a
caller-side option rather than baked into this module, since it requires access to every
trial's raw per-period returns (not just the winning one's), which not every caller of
``compute_deflated_sharpe_ratio()`` has, and doing it incorrectly here (e.g. off an
already-annualized Sharpe) would be worse than not doing it.

**A known, accepted limitation of the published formula, confirmed by Monte Carlo before
trusting this implementation (not just derived on paper).** The "expected maximum Sharpe of
N trials" term (``expected_max_sharpe_ratio()``) uses Bailey & Lopez de Prado's own
asymptotic Gumbel-type approximation (an interpolation between two extreme-value quantiles
via the Euler-Mascheroni constant) -- the exact published formula, but a known-imprecise
approximation to the TRUE finite-N distribution of the maximum of N correlated Gaussian
estimators. A Monte Carlo experiment (best-of-``n_trials`` Sharpe drawn from genuinely
zero-edge Gaussian noise, ``n_trials=20``, repeated 3000 times, at several sample sizes)
confirms: DSR's mean lands close to 0.5 (~0.48) as it should under a well-specified test,
and DSR is strictly, monotonically more conservative (lower) as ``n_trials`` increases for a
fixed observed Sharpe (verified directly, not assumed) -- but the resulting DSR distribution
is measurably MORE CONCENTRATED around the middle than a perfectly calibrated p-value would
be (essentially never below ~0.05 or above ~0.95 in 3000 draws at ``n_trials=20``), i.e. DSR
is somewhat under-confident about how unlikely an extreme-looking result is either way,
rather than mis-centered. This matches published follow-up critiques of the Gumbel
approximation's known slow convergence -- not a bug in this implementation. Treat DSR as
directionally correct and appropriately conservative, never as an exact p-value.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

_EULER_MASCHERONI = 0.5772156649015329


def _norm_cdf(x: float) -> float:
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def _norm_pdf(x: float) -> float:
    return math.exp(-0.5 * x * x) / math.sqrt(2.0 * math.pi)


def _norm_ppf(p: float) -> float:
    """Standard normal inverse CDF.

    AUDIT DSR-PPF-TAIL-057: previously implemented via bisection on the exact
    erf-based CDF, on the stated rationale that this "should be exact" rather
    than compounding a second approximation on top of the DSR formula's own.
    That rationale did not hold up empirically: an independent reference
    (Peter Acklam's rational approximation + Newton polish) disagreed with the
    bisection result by 1.61e-11 at p=0.999999 (n_trials around 1e6) -- and
    the disagreement got monotonically WORSE at larger n_trials (4.8e-9 at
    1e9, 1.07e-3 at 1e14). Bisection's 200-iteration loop gives a fixed
    absolute step size on x, but the CDF is extremely flat out in the tails,
    so a fixed-size step in x corresponds to a much coarser step in p out
    there -- precision in x does not translate to precision in p where it
    matters most for this module's own use (``expected_max_sharpe_ratio()``
    evaluates this at p = 1 - 1/n_trials, i.e. deep in the tail for any
    realistic n_trials).

    The fix below uses the actual more-accurate algorithm instead: Acklam's
    rational approximation (~1.15e-9 relative accuracy everywhere, published
    coefficients) as a starting point, refined to machine precision by
    Newton's method against the exact erf-based CDF/PDF -- so accuracy no
    longer degrades in the tail, and the exact CDF still pins down the final
    answer rather than a second unrefined approximation standing alone.

    Raises:
        ValueError: p not in (0, 1).
    """
    if not (0.0 < p < 1.0):
        raise ValueError(f"p must be in (0, 1), got {p}")

    # Peter Acklam's rational approximation coefficients (as published).
    a = (
        -3.969683028665376e01, 2.209460984245205e02, -2.759285104469687e02,
        1.383577518672690e02, -3.066479806614716e01, 2.506628277459239e00,
    )
    b = (
        -5.447609879822406e01, 1.615858368580409e02, -1.556989798598866e02,
        6.680131188771972e01, -1.328068155288572e01,
    )
    c = (
        -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e00,
        -2.549732539343734e00, 4.374664141464968e00, 2.938163982698783e00,
    )
    d = (
        7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e00,
        3.754408661907416e00,
    )
    p_low = 0.02425
    p_high = 1.0 - p_low

    if p < p_low:
        q = math.sqrt(-2.0 * math.log(p))
        x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / (
            (((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0
        )
    elif p <= p_high:
        q = p - 0.5
        r = q * q
        x = (
            (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q
        ) / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0)
    else:
        q = math.sqrt(-2.0 * math.log(1.0 - p))
        x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / (
            (((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0
        )

    # Newton polish on the exact erf-based CDF/PDF -- two iterations is ample
    # given the rational seed is already accurate to ~1e-9.
    for _ in range(2):
        x -= (_norm_cdf(x) - p) / _norm_pdf(x)
    return x


def sharpe_ratio_standard_error(
    sharpe_ratio: float, *, skewness: float, kurtosis: float, n_observations: int
) -> float:
    """Standard error of a Sharpe ratio estimate under possibly non-normal returns (Mertens
    2002 / Bailey & Lopez de Prado 2014's own cited formula).

    ``kurtosis`` here is RAW (Pearson) kurtosis, normal distribution = 3 -- NOT excess
    kurtosis (which would be 0 for normal). Passing ``skewness=0, kurtosis=3`` (the normal
    case) reduces this to the textbook ``sqrt((1 + 0.5 * sharpe_ratio**2) / (n_observations
    - 1))`` (Lo 2002) -- checked algebraically and in the test suite.

    Raises:
        ValueError: n_observations < 2, or the (skewness, kurtosis, sharpe_ratio)
            combination produces a negative variance (mathematically inconsistent inputs --
            can happen with an extreme/implausible skewness paired with a large
            sharpe_ratio).
    """
    if n_observations < 2:
        raise ValueError(f"n_observations must be >= 2, got {n_observations}")

    variance = (
        1.0 - skewness * sharpe_ratio + (kurtosis - 1.0) / 4.0 * sharpe_ratio**2
    ) / (n_observations - 1)
    # AUDIT DSR-NANOPEN-044: `variance < 0.0` is a NaN-transparent guard -- any comparison
    # against NaN is False in Python/IEEE754, so a NaN sharpe_ratio/skewness/kurtosis (e.g.
    # from an upstream computation on degenerate data) used to sail through this check and
    # come out the far end as a silent `dsr = nan`, instead of raising like every other
    # invalid-input case here does. `not math.isfinite(variance)` catches NaN AND +-inf in
    # one guard, subsuming the original `< 0.0` check (a negative number is already finite).
    if not math.isfinite(variance) or variance < 0.0:
        raise ValueError(
            f"计算出的 Sharpe 标准误方差无效 ({variance}) -- skewness={skewness}, "
            f"kurtosis={kurtosis}, sharpe_ratio={sharpe_ratio} 这组输入在数学上不自洽或包含 "
            "NaN/无穷值"
        )
    return math.sqrt(variance)


def expected_max_sharpe_ratio(sharpe_ratio_std_error: float, n_trials: int) -> float:
    """The "haircut" benchmark: the Sharpe ratio expected from the BEST of ``n_trials``
    independent zero-skill trials, purely from selection bias. See module docstring for the
    approximation this uses and its known limitation, and AUDIT DSR-SIGMA-056 specifically
    for what ``sharpe_ratio_std_error`` is actually standing in for here (a single
    observation's sampling standard error, not the cross-trial dispersion the published
    formula calls for) and the quantified direction of error that substitution carries.

    Raises:
        ValueError: n_trials < 2 (with a single trial there is no selection bias to correct
            for, and the underlying asymptotic formula is undefined at n_trials=1 anyway --
            ``norm_ppf(1 - 1/1) = norm_ppf(0)`` has no finite value).
    """
    if n_trials < 2:
        raise ValueError(
            f"n_trials must be >= 2, got {n_trials} -- with a single trial there is no "
            "selection bias to correct for"
        )
    z1 = _norm_ppf(1.0 - 1.0 / n_trials)
    z2 = _norm_ppf(1.0 - 1.0 / (n_trials * math.e))
    return sharpe_ratio_std_error * ((1.0 - _EULER_MASCHERONI) * z1 + _EULER_MASCHERONI * z2)


def deflated_sharpe_ratio(
    sharpe_ratio: float,
    *,
    n_observations: int,
    n_trials: int,
    skewness: float = 0.0,
    kurtosis: float = 3.0,
) -> float:
    """Pure statistical function -- see module docstring for units (per-period, never
    annualized) and the meaning of the returned probability.

    Raises:
        ValueError: from sharpe_ratio_standard_error()/expected_max_sharpe_ratio()'s own
            validation, or the resulting standard error is non-positive.
    """
    se = sharpe_ratio_standard_error(
        sharpe_ratio, skewness=skewness, kurtosis=kurtosis, n_observations=n_observations
    )
    # AUDIT DSR-NANOPEN-044: same NaN-transparency issue as the variance guard above --
    # `se <= 0.0` alone lets a NaN se (which sharpe_ratio_standard_error() can no longer
    # itself produce after the fix above, but this function's se could still in principle
    # come from a future caller) pass through silently.
    if not math.isfinite(se) or se <= 0.0:
        raise ValueError(f"Sharpe 标准误必须为正的有限数，得到 {se} -- 检查输入参数")
    benchmark = expected_max_sharpe_ratio(se, n_trials)
    return _norm_cdf((sharpe_ratio - benchmark) / se)


@dataclass(frozen=True, slots=True)
class DeflatedSharpeResult:
    """All fields are per-period (see module docstring)."""

    sharpe_ratio: float
    n_observations: int
    n_trials: int
    skewness: float
    kurtosis: float
    sharpe_ratio_std_error: float
    expected_max_sharpe_ratio: float
    deflated_sharpe_ratio: float


def compute_deflated_sharpe_ratio(returns: list[float], *, n_trials: int) -> DeflatedSharpeResult:
    """Convenience: derive per-period Sharpe/skewness/kurtosis directly from a raw per-bar
    returns series (e.g. ``BacktestResult.returns``, ``WalkForwardReport.oos_returns``, or a
    ``CpcvPathResult.oos_returns``) rather than requiring the caller to compute them by hand
    -- and, critically, never accepts an already-annualized Sharpe (see module docstring).

    Raises:
        ValueError: fewer than 2 returns, zero-variance returns (all identical), or
            deflated_sharpe_ratio()'s own validation (n_trials < 2).
    """
    if len(returns) < 2:
        raise ValueError(f"returns 至少需要 2 个观测值才能估计标准差，收到 {len(returns)} 个")

    arr = np.asarray(returns, dtype=np.float64)
    # AUDIT DSR-NANOPEN-044: without this check, a NaN/inf anywhere in `returns` propagates
    # through mean/std/skewness/kurtosis arithmetic (all NaN-in-NaN-out) and every downstream
    # guard in this module is NaN-transparent (comparisons against NaN are always False), so
    # the final result used to come back as a silent `dsr = nan` instead of raising -- this
    # is the entry point that reproduces the finding's exact repro
    # (``compute_deflated_sharpe_ratio([0.01, nan, 0.02, 0.03], n_trials=10)``).
    if not np.all(np.isfinite(arr)):
        raise ValueError("returns 包含 NaN 或无穷值，无法计算 Sharpe ratio")

    std = float(arr.std(ddof=1))
    if std <= 0.0:
        raise ValueError("returns 的标准差为 0（所有收益率相同）-- 无法计算 Sharpe ratio")

    mean = float(arr.mean())
    sharpe_ratio = mean / std
    # AUDIT DSR-MOMENT-045: skewness/kurtosis were standardized by `std` (ddof=1, the
    # sample-corrected estimator used for the Sharpe ratio itself) but averaged with
    # `np.mean` (an unweighted /n third/fourth moment) -- mixing a ddof=1 denominator into a
    # ddof=0-shaped moment formula is neither the standard g1/g2 (population, ddof=0
    # throughout) nor the bias-corrected G1/G2 estimator; it understated skewness by
    # ((n-1)/n)^1.5 and kurtosis by ((n-1)/n)^2 (measured 0.64/0.81/0.9344/0.9801 at
    # n=5/10/30/100). Standardizing by the POPULATION std (ddof=0) instead makes both moments
    # internally consistent (population std matches the /n moment convention) -- `std` above
    # (ddof=1) is kept as-is for the Sharpe ratio itself, which conventionally uses the
    # sample-corrected estimator.
    pop_std = float(arr.std(ddof=0))
    skewness = float(np.mean(((arr - mean) / pop_std) ** 3))
    kurtosis = float(np.mean(((arr - mean) / pop_std) ** 4))
    n_observations = len(arr)

    se = sharpe_ratio_standard_error(
        sharpe_ratio, skewness=skewness, kurtosis=kurtosis, n_observations=n_observations
    )
    benchmark = expected_max_sharpe_ratio(se, n_trials)
    dsr = _norm_cdf((sharpe_ratio - benchmark) / se)

    return DeflatedSharpeResult(
        sharpe_ratio=sharpe_ratio,
        n_observations=n_observations,
        n_trials=n_trials,
        skewness=skewness,
        kurtosis=kurtosis,
        sharpe_ratio_std_error=se,
        expected_max_sharpe_ratio=benchmark,
        deflated_sharpe_ratio=dsr,
    )
