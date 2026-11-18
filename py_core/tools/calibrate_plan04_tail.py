"""Calibrate PLAN-04 paired tail bands against simulated families with known truth."""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from collections import Counter
from collections.abc import Iterable
from concurrent.futures import ProcessPoolExecutor
from contextlib import nullcontext
from dataclasses import asdict, dataclass

import numpy as np

from py_core.research.plan04_paired_tail import TailFamilyBands, bootstrap_tail_bands
from py_core.tools.calibrate_plan05 import (
    COVERAGE_TOLERANCE,
    FALSE_POSITIVE_TOLERANCE,
    Rate,
    classify_rate,
    wilson_rate,
)

SEED = 20261128
OBSERVATIONS = 365
PAIRS = 12
REPETITIONS = 400
RESAMPLES = 4999
BLOCK_LENGTHS = (7, 14, 28)
PRIMARY_LENGTH = 14


@dataclass(frozen=True, slots=True)
class TailScenario:
    name: str
    phi: float = 0
    student_df: int | None = None
    shifted_pairs: int = 0
    shift: float = 0


SCENARIOS = (
    TailScenario("gaussian_null"),
    TailScenario("ar_positive_null", phi=0.5),
    TailScenario("student_t5_null", student_df=5),
    TailScenario("ar_mixed_alternative", phi=0.5, shifted_pairs=6, shift=-0.006),
)


@dataclass(frozen=True, slots=True)
class ReplicationMetrics:
    coverage: tuple[bool, ...]
    false_positive: tuple[bool, ...]
    stable_false_positive: bool
    any_detected: bool
    all_detected: bool
    half_widths: tuple[float, ...]


def true_differences(scenario: TailScenario) -> np.ndarray:
    truth = np.zeros(PAIRS)
    truth[:scenario.shifted_pairs] = scenario.shift
    return truth


def simulated_returns(scenario: TailScenario, rng: np.random.Generator) -> np.ndarray:
    common = rng.standard_normal((OBSERVATIONS, 1, 1))
    paired = rng.standard_normal((OBSERVATIONS, PAIRS, 1))
    individual = rng.standard_normal((OBSERVATIONS, PAIRS, 2))
    values = math.sqrt(0.3) * common + math.sqrt(0.3) * paired + math.sqrt(0.4) * individual
    if scenario.phi:
        innovation_scale = math.sqrt(1 - scenario.phi**2)
        for day in range(1, OBSERVATIONS):
            values[day] = scenario.phi * values[day - 1] + innovation_scale * values[day]
    if scenario.student_df is not None:
        df = scenario.student_df
        values *= np.sqrt((df - 2) / rng.chisquare(df, size=(OBSERVATIONS, 1, 1)))
    values *= 0.01
    values[:, :, 1] += true_differences(scenario)
    return values.reshape(OBSERVATIONS, PAIRS * 2)


def assess_replication(truth: np.ndarray, results: tuple[TailFamilyBands, ...]) -> ReplicationMetrics:
    lower = np.array([[band.lower for band in result.bands] for result in results])
    upper = np.array([[band.upper for band in result.bands] for result in results])
    if truth.ndim != 1 or lower.shape != (len(BLOCK_LENGTHS), truth.size):
        raise ValueError("each block length must report a band for every true difference")
    coverage = np.all((lower <= truth) & (truth <= upper), axis=1)
    negative, positive = upper < 0, lower > 0
    nulls = truth == 0
    false_positive = np.any((negative | positive)[:, nulls], axis=1)
    stable_negative, stable_positive = np.all(negative, axis=0), np.all(positive, axis=0)
    stable_false_positive = np.any((stable_negative | stable_positive)[nulls])
    detected = ((truth < 0) & stable_negative) | ((truth > 0) & stable_positive)
    return ReplicationMetrics(
        coverage=tuple(bool(value) for value in coverage),
        false_positive=tuple(bool(value) for value in false_positive),
        stable_false_positive=bool(stable_false_positive),
        any_detected=bool(np.any(detected)),
        all_detected=bool(np.any(~nulls) and np.all(detected[~nulls])),
        half_widths=tuple(result.half_width for result in results),
    )


def _replication(job: tuple[TailScenario, np.random.SeedSequence, int]) -> ReplicationMetrics | str:
    scenario, seed, n_resamples = job
    data_seed, draws_seed = seed.spawn(2)
    try:
        values = simulated_returns(scenario, np.random.Generator(np.random.PCG64(data_seed)))
        results = tuple(
            bootstrap_tail_bands(values, block_length=length, n_resamples=n_resamples,
                                 rng=np.random.Generator(np.random.PCG64(draw_seed)))
            for length, draw_seed in zip(BLOCK_LENGTHS, draws_seed.spawn(len(BLOCK_LENGTHS)), strict=True)
        )
        return assess_replication(true_differences(scenario), results)
    except (ValueError, FloatingPointError) as error:
        return str(error)


def _rate_json(rate: Rate | None) -> dict | None:
    return asdict(rate) if rate is not None else None


def summarize_replications(
    scenario: TailScenario, outcomes: Iterable[ReplicationMetrics | str], *, progress_total: int | None = None
) -> dict:
    failures: Counter[str] = Counter()
    completed = []
    for count, outcome in enumerate(outcomes, start=1):
        if isinstance(outcome, str):
            failures[outcome] += 1
        else:
            completed.append(outcome)
        if progress_total is not None and count % 50 == 0:
            print(f"{scenario.name} {count}/{progress_total}", file=sys.stderr, flush=True)
    valid = len(completed)
    blocks = []
    for index, length in enumerate(BLOCK_LENGTHS):
        coverage = wilson_rate(sum(row.coverage[index] for row in completed), valid)
        false_positive = wilson_rate(sum(row.false_positive[index] for row in completed), valid)
        blocks.append({
            "block_length": length,
            "coverage": _rate_json(coverage),
            "false_positive": _rate_json(false_positive),
            "mean_half_width": math.fsum(row.half_widths[index] for row in completed) / valid if valid else None,
            "coverage_criterion": classify_rate(coverage, COVERAGE_TOLERANCE),
            "false_positive_criterion": classify_rate(false_positive, FALSE_POSITIVE_TOLERANCE),
        })
    stable_false_positive = wilson_rate(sum(row.stable_false_positive for row in completed), valid)
    result = {
        "scenario": asdict(scenario),
        "valid_repetitions": valid,
        "failures": dict(failures),
        "blocks": blocks,
        "stable_false_positive": _rate_json(stable_false_positive),
        "stable_false_positive_criterion": classify_rate(stable_false_positive, FALSE_POSITIVE_TOLERANCE),
    }
    if scenario.shifted_pairs:
        result["any_detected"] = _rate_json(wilson_rate(sum(row.any_detected for row in completed), valid))
        result["all_detected"] = _rate_json(wilson_rate(sum(row.all_detected for row in completed), valid))
    return result


def summary_within_tolerance(summary: dict) -> bool:
    return (
        not summary["failures"]
        and summary["stable_false_positive_criterion"] == "within_tolerance"
        and all(
            block["coverage_criterion"] == "within_tolerance"
            and block["false_positive_criterion"] == "within_tolerance"
            for block in summary["blocks"]
        )
    )


def run_calibration(*, smoke: bool = False, workers: int = 1) -> dict:
    if isinstance(workers, bool) or not isinstance(workers, int) or workers < 1:
        raise ValueError("workers must be a positive integer")
    repeats, resamples = (4, 39) if smoke else (REPETITIONS, RESAMPLES)
    scenario_seeds = np.random.SeedSequence(SEED).spawn(len(SCENARIOS))
    results = []
    executor = ProcessPoolExecutor(max_workers=workers) if workers > 1 else nullcontext()
    with executor as pool:
        for scenario, seed in zip(SCENARIOS, scenario_seeds, strict=True):
            jobs = ((scenario, rep_seed, resamples) for rep_seed in seed.spawn(repeats))
            outcomes = pool.map(_replication, jobs) if pool is not None else map(_replication, jobs)
            results.append(summarize_replications(
                scenario, outcomes, progress_total=None if smoke else REPETITIONS
            ))

    passed = all(summary_within_tolerance(row) for row in results)
    return {
        "protocol": "plan04-tail-coverage-v1",
        "profile": "smoke" if smoke else "formal",
        "status": "smoke" if smoke else ("within_tolerance" if passed else "not_calibrated"),
        "numpy": np.__version__,
        "seed": SEED,
        "observations": OBSERVATIONS,
        "pairs": PAIRS,
        "repetitions_per_scenario": repeats,
        "resamples_per_length": resamples,
        "block_lengths": list(BLOCK_LENGTHS),
        "primary_block_length": PRIMARY_LENGTH,
        "coverage_tolerance": list(COVERAGE_TOLERANCE),
        "false_positive_tolerance": list(FALSE_POSITIVE_TOLERANCE),
        "results": results,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smoke", action="store_true", help="run four smaller replications per scenario")
    parser.add_argument("--workers", type=int, default=min(6, os.cpu_count() or 1))
    args = parser.parse_args()
    try:
        report = run_calibration(smoke=args.smoke, workers=args.workers)
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
