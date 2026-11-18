"""Compare a prespecified tail-band rank adjustment on independent simulated paths."""

from __future__ import annotations

import argparse
import json
import os
import sys
from collections.abc import Iterable, Sequence
from concurrent.futures import ProcessPoolExecutor
from contextlib import nullcontext
from dataclasses import asdict, dataclass

import numpy as np

from py_core.research.plan04_paired_tail import bootstrap_tail_band_variants
from py_core.tools.calibrate_plan04_tail import (
    BLOCK_LENGTHS,
    OBSERVATIONS,
    PAIRS,
    PRIMARY_LENGTH,
    RESAMPLES,
    SCENARIOS,
    ReplicationMetrics,
    TailScenario,
    assess_replication,
    simulated_returns,
    summarize_replications,
    summary_within_tolerance,
    true_differences,
)
from py_core.tools.calibrate_plan05 import (
    COVERAGE_TOLERANCE,
    FALSE_POSITIVE_TOLERANCE,
    wilson_rate,
)

SEED = 20261129
REPETITIONS = 600
PRIMARY_SCENARIO = "ar_positive_null"
_MIN_NET_GAIN_PERCENT = 1


@dataclass(frozen=True, slots=True)
class PairedReplication:
    baseline: ReplicationMetrics
    adjusted: ReplicationMetrics


def _replication(job: tuple[TailScenario, np.random.SeedSequence, int]) -> PairedReplication | str:
    scenario, seed, n_resamples = job
    data_seed, draws_seed = seed.spawn(2)
    try:
        values = simulated_returns(scenario, np.random.Generator(np.random.PCG64(data_seed)))
        bands = tuple(
            bootstrap_tail_band_variants(
                values,
                block_length=length,
                n_resamples=n_resamples,
                rng=np.random.Generator(np.random.PCG64(draw_seed)),
            )
            for length, draw_seed in zip(BLOCK_LENGTHS, draws_seed.spawn(len(BLOCK_LENGTHS)), strict=True)
        )
        baseline, adjusted = zip(*bands, strict=True)
        truth = true_differences(scenario)
        return PairedReplication(assess_replication(truth, baseline), assess_replication(truth, adjusted))
    except (ValueError, FloatingPointError) as error:
        return str(error)


def compare_primary_coverage(outcomes: Sequence[PairedReplication]) -> dict:
    """Measure new coverage relative to the same original-method replications."""
    index = BLOCK_LENGTHS.index(PRIMARY_LENGTH)
    baseline_only = sum(row.baseline.coverage[index] and not row.adjusted.coverage[index] for row in outcomes)
    adjusted_only = sum(row.adjusted.coverage[index] and not row.baseline.coverage[index] for row in outcomes)
    net = adjusted_only - baseline_only
    rate = wilson_rate(adjusted_only, baseline_only + adjusted_only)
    passed = bool(outcomes) and 100 * net >= _MIN_NET_GAIN_PERCENT * len(outcomes) and rate is not None and rate.lower > 0.5
    return {
        "valid_repetitions": len(outcomes),
        "baseline_only": baseline_only,
        "adjusted_only": adjusted_only,
        "net_gain": net / len(outcomes) if outcomes else None,
        "discordant_adjusted_coverage": asdict(rate) if rate is not None else None,
        "criterion": "within_tolerance" if passed else "not_met",
    }


def _summarize(
    scenario: TailScenario, outcomes: Iterable[PairedReplication | str], *, smoke: bool
) -> dict:
    completed = []
    baseline = []
    adjusted = []
    for count, outcome in enumerate(outcomes, start=1):
        if isinstance(outcome, str):
            baseline.append(outcome)
            adjusted.append(outcome)
        else:
            completed.append(outcome)
            baseline.append(outcome.baseline)
            adjusted.append(outcome.adjusted)
        if not smoke and count % 50 == 0:
            print(f"{scenario.name} {count}/{REPETITIONS}", file=sys.stderr, flush=True)
    return {
        "scenario": asdict(scenario),
        "baseline": summarize_replications(scenario, baseline),
        "adjusted": summarize_replications(scenario, adjusted),
        "paired_primary_coverage": compare_primary_coverage(completed),
    }


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
            results.append(_summarize(scenario, outcomes, smoke=smoke))
    primary = next(row for row in results if row["scenario"]["name"] == PRIMARY_SCENARIO)
    passed = (
        all(summary_within_tolerance(row["adjusted"]) for row in results)
        and primary["paired_primary_coverage"]["criterion"] == "within_tolerance"
    )
    return {
        "protocol": "plan04-tail-rank-calibration-v1",
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
        "primary_scenario": PRIMARY_SCENARIO,
        "baseline_rank_level": 0.95,
        "adjusted_rank_level": 0.965,
        "coverage_tolerance": list(COVERAGE_TOLERANCE),
        "false_positive_tolerance": list(FALSE_POSITIVE_TOLERANCE),
        "minimum_primary_gain": _MIN_NET_GAIN_PERCENT / 100,
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
