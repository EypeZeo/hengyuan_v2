"""Calibrate PLAN-05 paired stationary bootstrap in fixed synthetic scenarios."""

from __future__ import annotations

import argparse
import json
import math
from collections import Counter
from dataclasses import asdict, dataclass

import numpy as np

from py_core.research.paired_bootstrap import compare_paired_mean, evaluate_fixed_family
from py_core.tools.calibrate_plan05 import (
    ALPHA,
    COVERAGE_TOLERANCE,
    FALSE_POSITIVE_TOLERANCE,
    RETURN_SD,
    classify_rate,
    wilson_rate,
)

SEED = 20261121
OBSERVATIONS = 512
BLOCK_LENGTH = 16
REPETITIONS = 2000
RESAMPLES = 399


@dataclass(frozen=True, slots=True)
class Scenario:
    name: str
    candidates: int = 1
    correlation: float = 0.6
    phi: float = 0.0
    student_df: int | None = None
    excess_mean: float = 0.0


SCENARIOS = (
    Scenario("paired_gaussian_null"),
    Scenario("paired_ar_positive_null", phi=0.5),
    Scenario("paired_heavy_tail_null", student_df=5),
    Scenario("paired_ar_positive_alternative", phi=0.5, excess_mean=0.001),
    Scenario("fixed_family_independent_null", candidates=6, correlation=0.0),
    Scenario("fixed_family_correlated_ar_null", candidates=6, phi=0.5),
)


def simulated_panel(scenario: Scenario, rng: np.random.Generator) -> np.ndarray:
    shape = (OBSERVATIONS, 1 + scenario.candidates + 1)
    if scenario.student_df is None:
        shocks = rng.standard_normal(shape)
    else:
        df = scenario.student_df
        shocks = rng.standard_t(df, shape) * math.sqrt((df - 2) / df)
    common = shocks[:, :1]
    values = math.sqrt(scenario.correlation) * common + math.sqrt(1 - scenario.correlation) * shocks[:, 1:]
    if scenario.candidates > 1:
        # An independent benchmark lets the candidate correlation affect the excess returns.
        values[:, 0] = shocks[:, 1]
    if scenario.phi:
        innovation_scale = math.sqrt(1 - scenario.phi**2)
        for t in range(1, OBSERVATIONS):
            values[t] = scenario.phi * values[t - 1] + innovation_scale * values[t]
    values *= RETURN_SD
    values[:, 1] += scenario.excess_mean
    return values


def calibrate_scenario(
    scenario: Scenario, repetitions: int, n_resamples: int, seed: np.random.SeedSequence
) -> dict:
    data_seed, resampling_seed = seed.spawn(2)
    data_rng = np.random.Generator(np.random.PCG64(data_seed))
    resampling_rng = np.random.Generator(np.random.PCG64(resampling_seed))
    failures: Counter[str] = Counter()
    covered = rejected = 0
    for _ in range(repetitions):
        panel = simulated_panel(scenario, data_rng)
        try:
            if scenario.candidates == 1:
                interval = compare_paired_mean(
                    panel, block_length=BLOCK_LENGTH, n_resamples=n_resamples, rng=resampling_rng, alpha=ALPHA
                )
                covered += int(interval.lower <= scenario.excess_mean <= interval.upper)
                rejected += int(interval.one_sided_lower > 0.0)
            else:
                result = evaluate_fixed_family(
                    panel, block_length=BLOCK_LENGTH, n_resamples=n_resamples, rng=resampling_rng
                )
                rejected += int(result.p_value <= ALPHA)
        except ValueError as error:
            failures[str(error)] += 1

    valid = repetitions - sum(failures.values())
    coverage = wilson_rate(covered, valid) if scenario.candidates == 1 else None
    positive_rate = wilson_rate(rejected, valid)
    metric = "positive_rejection" if scenario.candidates == 1 else "family_false_positive"
    criteria = {}
    if scenario.candidates == 1:
        criteria["coverage"] = classify_rate(coverage, COVERAGE_TOLERANCE)
    if scenario.excess_mean == 0:
        criteria[metric] = classify_rate(positive_rate, FALSE_POSITIVE_TOLERANCE)
    return {
        "scenario": asdict(scenario),
        "failures": dict(failures),
        "coverage": asdict(coverage) if coverage else None,
        metric: asdict(positive_rate) if positive_rate else None,
        "criteria": criteria,
    }


def run_calibration(*, smoke: bool = False) -> dict:
    repetitions, n_resamples = (12, 39) if smoke else (REPETITIONS, RESAMPLES)
    seeds = np.random.SeedSequence(SEED).spawn(len(SCENARIOS))
    rows = [
        calibrate_scenario(scenario, repetitions, n_resamples, seed)
        for scenario, seed in zip(SCENARIOS, seeds, strict=True)
    ]
    supported = all(
        not row["failures"] and all(result == "within_tolerance" for result in row["criteria"].values())
        for row in rows
    )
    return {
        "protocol": "plan-05-paired-v1",
        "profile": "smoke" if smoke else "formal",
        "status": "smoke" if smoke else ("within_tolerance" if supported else "not_calibrated"),
        "seed": SEED,
        "numpy": np.__version__,
        "observations": OBSERVATIONS,
        "expected_block_length": BLOCK_LENGTH,
        "repetitions_per_scenario": repetitions,
        "resamples_per_repetition": n_resamples,
        "return_sd": RETURN_SD,
        "nominal_alpha": ALPHA,
        "coverage_tolerance": COVERAGE_TOLERANCE,
        "false_positive_tolerance": FALSE_POSITIVE_TOLERANCE,
        "results": rows,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smoke", action="store_true", help="exercise the protocol on a smaller sample")
    args = parser.parse_args()
    print(json.dumps(run_calibration(smoke=args.smoke), indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
