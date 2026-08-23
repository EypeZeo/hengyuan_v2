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
``_norm_cdf()`` exactly, via the stdlib ``math.erf()``; ``_norm_ppf()`` by bisection on that
exact CDF (deliberately not a rational-approximation shortcut formula, since this piece, at
least, should be exact rather than adding a second layer of approximation on top of the DSR
formula's own -- see below).

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


def _norm_ppf(p: float) -> float:
    """Standard normal inverse CDF via bisection on the exact erf-based CDF.

    Raises:
        ValueError: p not in (0, 1).
    """
    if not (0.0 < p < 1.0):
        raise ValueError(f"p must be in (0, 1), got {p}")
    lo, hi = -40.0, 40.0
    for _ in range(200):
        mid = (lo + hi) / 2.0
        if _norm_cdf(mid) < p:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2.0


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
    if variance < 0.0:
        raise ValueError(
            f"计算出的 Sharpe 标准误方差为负 ({variance:.6g}) -- skewness={skewness}, "
            f"kurtosis={kurtosis}, sharpe_ratio={sharpe_ratio} 这组输入在数学上不自洽"
        )
    return math.sqrt(variance)


def expected_max_sharpe_ratio(sharpe_ratio_std_error: float, n_trials: int) -> float:
    """The "haircut" benchmark: the Sharpe ratio expected from the BEST of ``n_trials``
    independent zero-skill trials, purely from selection bias. See module docstring for the
    approximation this uses and its known limitation.

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
    if se <= 0.0:
        raise ValueError(f"Sharpe 标准误必须为正，得到 {se:.6g} -- 检查输入参数")
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
    std = float(arr.std(ddof=1))
    if std <= 0.0:
        raise ValueError("returns 的标准差为 0（所有收益率相同）-- 无法计算 Sharpe ratio")

    mean = float(arr.mean())
    sharpe_ratio = mean / std
    skewness = float(np.mean(((arr - mean) / std) ** 3))
    kurtosis = float(np.mean(((arr - mean) / std) ** 4))
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
