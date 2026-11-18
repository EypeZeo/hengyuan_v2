"""Independently validate the fixed PLAN-05 studentized paired interval protocol."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from dataclasses import asdict

import numpy as np

from py_core.research.paired_bootstrap import (
    compare_paired_mean,
    compare_paired_mean_studentized,
)
from py_core.tools.calibrate_paired_bootstrap import (
    BLOCK_LENGTH,
    OBSERVATIONS,
    RESAMPLES,
    SCENARIOS,
    Scenario,
    simulated_panel,
)
from py_core.tools.calibrate_plan05 import (
    ALPHA,
    COVERAGE_TOLERANCE,
    FALSE_POSITIVE_TOLERANCE,
    classify_rate,
    wilson_rate,
)

SEED = 20261122
REPETITIONS = 3000
PREWHITENED_SEED = 20261123
PREWHITENED_REPETITIONS = 6000
VARIANCE_LAG = 8
MIN_COVERAGE_GAIN = 0.02
TARGET = "paired_ar_positive_null"
PAIR_SCENARIOS = SCENARIOS[:4]


def _summarize(covered: int, rejected: int, width: float, valid: int) -> dict:
    coverage = wilson_rate(covered, valid)
    rejection = wilson_rate(rejected, valid)
    return {
        "coverage": asdict(coverage) if coverage else None,
        "positive_rejection": asdict(rejection) if rejection else None,
        "mean_width": width / valid if valid else None,
    }


def calibrate_scenario(
    scenario: Scenario, repetitions: int, n_resamples: int, seed: np.random.SeedSequence, *, prewhiten: bool = False
) -> dict:
    data_seed, draw_seed = seed.spawn(2)
    data_rng = np.random.Generator(np.random.PCG64(data_seed))
    seed_rng = np.random.Generator(np.random.PCG64(draw_seed))
    failures: Counter[str] = Counter()
    basic_covered = studentized_covered = basic_rejected = studentized_rejected = 0
    basic_width = studentized_width = 0.0
    studentized_only = basic_only = 0
    for _ in range(repetitions):
        panel = simulated_panel(scenario, data_rng)
        draw = int(seed_rng.integers(2**63))
        # Two generators initialized identically supply the same shared row draws.
        try:
            basic = compare_paired_mean(
                panel, block_length=BLOCK_LENGTH, n_resamples=n_resamples, alpha=ALPHA,
                rng=np.random.Generator(np.random.PCG64(draw)),
            )
            studentized = compare_paired_mean_studentized(
                panel, block_length=BLOCK_LENGTH, variance_lag=VARIANCE_LAG, n_resamples=n_resamples,
                alpha=ALPHA, rng=np.random.Generator(np.random.PCG64(draw)), prewhiten=prewhiten,
            )
        except ValueError as error:
            failures[str(error)] += 1
            continue

        basic_hit = basic.lower <= scenario.excess_mean <= basic.upper
        studentized_hit = studentized.lower <= scenario.excess_mean <= studentized.upper
        basic_covered += int(basic_hit)
        studentized_covered += int(studentized_hit)
        basic_rejected += int(basic.one_sided_lower > 0.0)
        studentized_rejected += int(studentized.one_sided_lower > 0.0)
        basic_width += basic.upper - basic.lower
        studentized_width += studentized.upper - studentized.lower
        basic_only += int(basic_hit and not studentized_hit)
        studentized_only += int(studentized_hit and not basic_hit)

    valid = repetitions - sum(failures.values())
    coverage = wilson_rate(studentized_covered, valid)
    rejection = wilson_rate(studentized_rejected, valid)
    discordant = wilson_rate(studentized_only, basic_only + studentized_only)
    gain = (studentized_covered - basic_covered) / valid if valid else None
    criteria = {"coverage": classify_rate(coverage, COVERAGE_TOLERANCE)}
    if scenario.excess_mean == 0:
        criteria["false_positive"] = classify_rate(rejection, FALSE_POSITIVE_TOLERANCE)
    if scenario.name == TARGET:
        criteria["coverage_gain"] = (
            "within_tolerance"
            if gain is not None and gain >= MIN_COVERAGE_GAIN and discordant is not None and discordant.lower > 0.5
            else "not_confirmed"
        )
    return {
        "scenario": asdict(scenario),
        "failures": dict(failures),
        "basic": _summarize(basic_covered, basic_rejected, basic_width, valid),
        "studentized": _summarize(studentized_covered, studentized_rejected, studentized_width, valid),
        "paired_gain": gain,
        "basic_only": basic_only,
        "studentized_only": studentized_only,
        "studentized_share_of_discordant_coverage": asdict(discordant) if discordant else None,
        "criteria": criteria,
    }


def run_calibration(*, smoke: bool = False, prewhiten: bool = False) -> dict:
    seed = PREWHITENED_SEED if prewhiten else SEED
    planned_repetitions = PREWHITENED_REPETITIONS if prewhiten else REPETITIONS
    repetitions, resamples = (12, 39) if smoke else (planned_repetitions, RESAMPLES)
    seeds = np.random.SeedSequence(seed).spawn(len(PAIR_SCENARIOS))
    rows = [
        calibrate_scenario(scenario, repetitions, resamples, scenario_seed, prewhiten=prewhiten)
        for scenario, scenario_seed in zip(PAIR_SCENARIOS, seeds, strict=True)
    ]
    supported = all(
        not row["failures"] and all(value == "within_tolerance" for value in row["criteria"].values()) for row in rows
    )
    return {
        "protocol": "plan-05-prewhitened-paired-v1" if prewhiten else "plan-05-studentized-paired-v1",
        "profile": "smoke" if smoke else "formal",
        "status": "smoke" if smoke else ("within_tolerance" if supported else "not_calibrated"),
        "seed": seed,
        "numpy": np.__version__,
        "observations": OBSERVATIONS,
        "repetitions_per_scenario": repetitions,
        "resamples_per_repetition": resamples,
        "expected_block_length": BLOCK_LENGTH,
        "variance_lag": VARIANCE_LAG,
        "target": TARGET,
        "min_coverage_gain": MIN_COVERAGE_GAIN,
        "coverage_tolerance": COVERAGE_TOLERANCE,
        "false_positive_tolerance": FALSE_POSITIVE_TOLERANCE,
        "results": rows,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smoke", action="store_true", help="exercise the protocol on a smaller sample")
    parser.add_argument("--prewhiten", action="store_true", help="run the independent AR(1) prewhitening protocol")
    args = parser.parse_args()
    print(json.dumps(run_calibration(smoke=args.smoke, prewhiten=args.prewhiten), indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
