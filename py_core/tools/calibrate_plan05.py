"""Run PLAN-05 Monte Carlo calibration against the production estimators."""

from __future__ import annotations

import argparse
import json
import math
from collections import Counter
from dataclasses import asdict, dataclass
from statistics import NormalDist

import numpy as np

from py_core.research.hac import HacStatus, estimate_sharpe_hac
from py_core.validation.deflated_sharpe import compute_deflated_sharpe_ratio

SEED = 20261118
REPLICATIONS = 2400
RETURN_SD = 0.01
ALPHA = 0.05
DSR_THRESHOLD = 0.95
COVERAGE_TOLERANCE = (0.92, 0.98)
FALSE_POSITIVE_TOLERANCE = (0.0, 0.075)
WALD_Z = NormalDist().inv_cdf(1 - ALPHA / 2)
POSITIVE_Z = NormalDist().inv_cdf(1 - ALPHA)


@dataclass(frozen=True, slots=True)
class SharpeScenario:
    name: str
    observations: int = 1024
    lag: int = 32
    phi: float = 0.0
    student_df: int | None = None
    true_sharpe: float = 0.0


SHARPE_SCENARIOS = (
    SharpeScenario("gaussian_null"),
    SharpeScenario("ar_positive_null", phi=0.5),
    SharpeScenario("ar_negative_null", phi=-0.5),
    SharpeScenario("student_t5_null", student_df=5),
    SharpeScenario("ar_positive_alternative", phi=0.5, true_sharpe=0.05),
    SharpeScenario("student_t5_alternative", student_df=5, true_sharpe=0.05),
    SharpeScenario("ar_positive_short_null", observations=256, lag=8, phi=0.5),
)


@dataclass(frozen=True, slots=True)
class SelectionScenario:
    name: str
    correlation: float
    candidates: int = 12
    observations: int = 252
    lag: int = 8


SELECTION_SCENARIOS = (
    SelectionScenario("independent_candidate_null", correlation=0.0),
    SelectionScenario("correlated_candidate_null", correlation=0.6),
)


@dataclass(frozen=True, slots=True)
class Rate:
    events: int
    trials: int
    fraction: float
    lower: float
    upper: float


def wilson_rate(events: int, trials: int) -> Rate | None:
    """Return the pointwise 95% Wilson interval for a Monte Carlo event rate."""
    if trials == 0:
        return None
    fraction = events / trials
    adjustment = WALD_Z**2 / trials
    center = (fraction + adjustment / 2) / (1 + adjustment)
    half_width = WALD_Z * math.sqrt((fraction * (1 - fraction) + adjustment / 4) / trials) / (1 + adjustment)
    return Rate(events, trials, fraction, max(0.0, center - half_width), min(1.0, center + half_width))


def classify_rate(rate: Rate | None, tolerance: tuple[float, float]) -> str:
    if rate is None:
        return "insufficient_evidence"
    lower, upper = tolerance
    if rate.upper < lower or rate.lower > upper:
        return "outside_tolerance"
    if rate.lower < lower or rate.upper > upper:
        return "insufficient_evidence"
    return "within_tolerance"


def simulated_returns(scenario: SharpeScenario, repetitions: int, rng: np.random.Generator) -> np.ndarray:
    size = (repetitions, scenario.observations)
    if scenario.student_df is None:
        values = rng.standard_normal(size)
        if scenario.phi:
            innovation_scale = math.sqrt(1 - scenario.phi**2)
            for t in range(1, scenario.observations):
                values[:, t] = scenario.phi * values[:, t - 1] + innovation_scale * values[:, t]
    else:
        values = rng.standard_t(scenario.student_df, size)
        values *= math.sqrt((scenario.student_df - 2) / scenario.student_df)
    return RETURN_SD * (scenario.true_sharpe + values)


def _rate_json(rate: Rate | None) -> dict | None:
    return asdict(rate) if rate else None


def calibrate_sharpe(scenario: SharpeScenario, repetitions: int, rng: np.random.Generator) -> dict:
    estimates, errors = [], []
    failures: Counter[str] = Counter()
    for values in simulated_returns(scenario, repetitions, rng):
        estimate = estimate_sharpe_hac(values, lag=scenario.lag, include_adjacent=False).primary
        if estimate.status is not HacStatus.OK:
            failures[estimate.status.value] += 1
            continue
        assert estimate.sharpe_ratio is not None and estimate.standard_error is not None
        estimates.append(estimate.sharpe_ratio)
        errors.append(estimate.standard_error)

    sharpe, se = np.asarray(estimates), np.asarray(errors)
    covered = int(np.count_nonzero(np.abs(sharpe - scenario.true_sharpe) <= WALD_Z * se))
    positive = int(np.count_nonzero(sharpe > POSITIVE_Z * se))
    coverage = wilson_rate(covered, sharpe.size)
    positive_rate = wilson_rate(positive, sharpe.size)
    criteria = {"coverage": classify_rate(coverage, COVERAGE_TOLERANCE)}
    if scenario.true_sharpe == 0.0:
        criteria["false_positive"] = classify_rate(positive_rate, FALSE_POSITIVE_TOLERANCE)
    return {
        "scenario": asdict(scenario),
        "failures": dict(failures),
        "coverage": _rate_json(coverage),
        "positive_rejection": _rate_json(positive_rate),
        "mean_sharpe": float(np.mean(sharpe)) if sharpe.size else None,
        "empirical_sharpe_sd": float(np.std(sharpe, ddof=1)) if sharpe.size > 1 else None,
        "mean_hac_se": float(np.mean(se)) if se.size else None,
        "criteria": criteria,
    }


def calibrate_selection(scenario: SelectionScenario, repetitions: int, rng: np.random.Generator) -> dict:
    """Repeat selection from a fixed family of candidate return series."""
    failures: Counter[str] = Counter()
    hac_rejections = dsr_rejections = hac_trials = dsr_trials = 0
    common_scale = math.sqrt(scenario.correlation)
    individual_scale = math.sqrt(1 - scenario.correlation)
    for _ in range(repetitions):
        common = rng.standard_normal(scenario.observations)
        independent = rng.standard_normal((scenario.candidates, scenario.observations))
        values = RETURN_SD * (common_scale * common + individual_scale * independent)
        rankings = np.mean(values, axis=1) / np.std(values, axis=1, ddof=1)
        winner = values[int(np.argmax(rankings))]

        hac = estimate_sharpe_hac(winner, lag=scenario.lag, include_adjacent=False).primary
        if hac.status is HacStatus.OK:
            assert hac.sharpe_ratio is not None and hac.standard_error is not None
            hac_trials += 1
            hac_rejections += int(hac.sharpe_ratio > POSITIVE_Z * hac.standard_error)
        else:
            failures[f"hac:{hac.status.value}"] += 1

        try:
            dsr = compute_deflated_sharpe_ratio(winner.tolist(), n_trials=scenario.candidates)
            score = dsr.deflated_sharpe_ratio
            if not math.isfinite(score):
                raise ValueError("non-finite DSR")
        except (ArithmeticError, ValueError):
            failures["invalid_dsr"] += 1
            continue
        dsr_trials += 1
        dsr_rejections += int(score >= DSR_THRESHOLD)

    dsr_rate = wilson_rate(dsr_rejections, dsr_trials)
    return {
        "scenario": asdict(scenario),
        "failures": dict(failures),
        "uncorrected_hac_false_positive": _rate_json(wilson_rate(hac_rejections, hac_trials)),
        "dsr_false_positive": _rate_json(dsr_rate),
        "criteria": {"dsr_false_positive": classify_rate(dsr_rate, FALSE_POSITIVE_TOLERANCE)},
    }


def run_calibration(*, smoke: bool = False) -> dict:
    repetitions = 12 if smoke else REPLICATIONS
    seeds = np.random.SeedSequence(SEED).spawn(len(SHARPE_SCENARIOS) + len(SELECTION_SCENARIOS))
    generators = iter(np.random.Generator(np.random.PCG64(seed)) for seed in seeds)
    sharpe = [calibrate_sharpe(scenario, repetitions, next(generators)) for scenario in SHARPE_SCENARIOS]
    selection = [calibrate_selection(scenario, repetitions, next(generators)) for scenario in SELECTION_SCENARIOS]
    supported = all(
        not row["failures"] and all(result == "within_tolerance" for result in row["criteria"].values())
        for row in (*sharpe, *selection)
    )
    return {
        "protocol": "plan-05-v1",
        "profile": "smoke" if smoke else "formal",
        "status": "smoke" if smoke else ("within_tolerance" if supported else "not_calibrated"),
        "seed": SEED,
        "numpy": np.__version__,
        "repetitions_per_scenario": repetitions,
        "return_sd": RETURN_SD,
        "nominal_alpha": ALPHA,
        "dsr_threshold": DSR_THRESHOLD,
        "coverage_tolerance": COVERAGE_TOLERANCE,
        "false_positive_tolerance": FALSE_POSITIVE_TOLERANCE,
        "sharpe": sharpe,
        "selection": selection,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smoke", action="store_true", help="exercise the protocol on a smaller sample")
    args = parser.parse_args()
    print(json.dumps(run_calibration(smoke=args.smoke), indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
