"""Compare prespecified tail-band rank adjustments on independent simulated paths."""

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

from py_core.research.plan04_paired_tail import bootstrap_tail_band_levels
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
_MIN_LONG_REDUCTION_PERCENT = 1
_LONG_SCENARIOS = ("gaussian_null", "student_t5_null")
_LONG_LENGTH = 28


@dataclass(frozen=True, slots=True)
class RankProtocol:
    name: str
    seed: int
    repetitions: int
    smoke_resamples: int
    methods: tuple[tuple[str, tuple[int, ...]], ...]


ORIGINAL_LEVELS = (950, 950, 950)
FIXED_LEVELS = (965, 965, 965)
BY_LENGTH_LEVELS = (960, 965, 980)
_FIXED_PROTOCOL = RankProtocol(
    "plan04-tail-rank-calibration-v1", SEED, REPETITIONS, 39,
    (("baseline", ORIGINAL_LEVELS), ("adjusted", FIXED_LEVELS)),
)
_LENGTH_PROTOCOL = RankProtocol(
    "plan04-tail-length-calibration-v1", 20261130, 800, 99,
    (("baseline", ORIGINAL_LEVELS), ("fixed_rank", FIXED_LEVELS), ("adjusted", BY_LENGTH_LEVELS)),
)


@dataclass(frozen=True, slots=True)
class PairedReplication:
    baseline: ReplicationMetrics
    adjusted: ReplicationMetrics


def _replication(
    job: tuple[TailScenario, np.random.SeedSequence, int, tuple[tuple[int, ...], ...]],
) -> tuple[ReplicationMetrics, ...] | str:
    scenario, seed, n_resamples, levels_by_length = job
    data_seed, draws_seed = seed.spawn(2)
    try:
        values = simulated_returns(scenario, np.random.Generator(np.random.PCG64(data_seed)))
        bands = tuple(
            bootstrap_tail_band_levels(
                values,
                block_length=length,
                n_resamples=n_resamples,
                rng=np.random.Generator(np.random.PCG64(draw_seed)),
                levels_per_thousand=levels,
            )
            for length, draw_seed, levels in zip(
                BLOCK_LENGTHS, draws_seed.spawn(len(BLOCK_LENGTHS)), levels_by_length, strict=True
            )
        )
        truth = true_differences(scenario)
        return tuple(assess_replication(truth, method) for method in zip(*bands, strict=True))
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


def ranked_replications(
    scenario: TailScenario,
    seed: np.random.SeedSequence,
    *,
    repetitions: int,
    resamples: int,
    levels_by_method: tuple[tuple[int, ...], ...],
    pool: ProcessPoolExecutor | None,
) -> Iterable[tuple[ReplicationMetrics, ...] | str]:
    """Evaluate each method on the same simulated paths and the same draws per length."""
    levels_by_length = tuple(zip(*levels_by_method, strict=True))
    jobs = ((scenario, child, resamples, levels_by_length) for child in seed.spawn(repetitions))
    return pool.map(_replication, jobs) if pool is not None else map(_replication, jobs)


def compare_long_false_positives(
    fixed: Sequence[ReplicationMetrics], adjusted: Sequence[ReplicationMetrics]
) -> dict:
    """Count changes in 28-day false positives on shared replications."""
    index = BLOCK_LENGTHS.index(_LONG_LENGTH)
    fixed_only = adjusted_only = 0
    for prior, current in zip(fixed, adjusted, strict=True):
        prior_positive = prior.false_positive[index]
        current_positive = current.false_positive[index]
        fixed_only += prior_positive and not current_positive
        adjusted_only += current_positive and not prior_positive
    valid = len(fixed)
    net = fixed_only - adjusted_only
    passed = valid > 0 and 100 * net >= _MIN_LONG_REDUCTION_PERCENT * valid
    return {
        "block_length": _LONG_LENGTH,
        "valid_repetitions": valid,
        "fixed_rank_only": fixed_only,
        "adjusted_only": adjusted_only,
        "net_reduction": net / valid if valid else None,
        "criterion": "within_tolerance" if passed else "not_met",
    }


def _summarize(
    scenario: TailScenario,
    outcomes: Iterable[tuple[ReplicationMetrics, ...] | str],
    *,
    methods: tuple[str, ...],
    progress_total: int | None,
) -> dict:
    completed: dict[str, list[ReplicationMetrics]] = {method: [] for method in methods}
    failures = []
    for count, outcome in enumerate(outcomes, start=1):
        if isinstance(outcome, str):
            failures.append(outcome)
        else:
            for method, metrics in zip(methods, outcome, strict=True):
                completed[method].append(metrics)
        if progress_total is not None and count % 50 == 0:
            print(f"{scenario.name} {count}/{progress_total}", file=sys.stderr, flush=True)
    paired = tuple(
        PairedReplication(baseline, adjusted)
        for baseline, adjusted in zip(completed["baseline"], completed["adjusted"], strict=True)
    )
    row = {
        "scenario": asdict(scenario),
        **{method: summarize_replications(scenario, [*rows, *failures]) for method, rows in completed.items()},
        "paired_primary_coverage": compare_primary_coverage(paired),
    }
    if "fixed_rank" in completed and scenario.name in _LONG_SCENARIOS:
        row["paired_long_false_positive"] = compare_long_false_positives(completed["fixed_rank"], completed["adjusted"])
    return row


def _run(protocol: RankProtocol, *, smoke: bool, workers: int) -> dict:
    if isinstance(workers, bool) or not isinstance(workers, int) or workers < 1:
        raise ValueError("workers must be a positive integer")
    repeats, resamples = (4, protocol.smoke_resamples) if smoke else (protocol.repetitions, RESAMPLES)
    methods, levels = zip(*protocol.methods, strict=True)
    scenario_seeds = np.random.SeedSequence(protocol.seed).spawn(len(SCENARIOS))
    results = []
    executor = ProcessPoolExecutor(max_workers=workers) if workers > 1 else nullcontext()
    with executor as pool:
        for scenario, seed in zip(SCENARIOS, scenario_seeds, strict=True):
            outcomes = ranked_replications(
                scenario, seed, repetitions=repeats, resamples=resamples, levels_by_method=levels, pool=pool
            )
            results.append(_summarize(scenario, outcomes, methods=methods, progress_total=None if smoke else repeats))
    primary = next(row for row in results if row["scenario"]["name"] == PRIMARY_SCENARIO)
    passed = (
        all(summary_within_tolerance(row["adjusted"]) for row in results)
        and primary["paired_primary_coverage"]["criterion"] == "within_tolerance"
    )
    rank_metadata = {"baseline_rank_level": 0.95, "adjusted_rank_level": 0.965}
    if protocol is _LENGTH_PROTOCOL:
        passed = passed and all(
            row["paired_long_false_positive"]["criterion"] == "within_tolerance"
            for row in results if row["scenario"]["name"] in _LONG_SCENARIOS
        )
        rank_metadata = {
            "levels_per_thousand_by_method": dict(protocol.methods),
            "minimum_long_false_positive_reduction": _MIN_LONG_REDUCTION_PERCENT / 100,
        }
    return {
        "protocol": protocol.name,
        "profile": "smoke" if smoke else "formal",
        "status": "smoke" if smoke else ("within_tolerance" if passed else "not_calibrated"),
        "numpy": np.__version__,
        "seed": protocol.seed,
        "observations": OBSERVATIONS,
        "pairs": PAIRS,
        "repetitions_per_scenario": repeats,
        "resamples_per_length": resamples,
        "block_lengths": list(BLOCK_LENGTHS),
        "primary_block_length": PRIMARY_LENGTH,
        "primary_scenario": PRIMARY_SCENARIO,
        **rank_metadata,
        "coverage_tolerance": list(COVERAGE_TOLERANCE),
        "false_positive_tolerance": list(FALSE_POSITIVE_TOLERANCE),
        "minimum_primary_gain": _MIN_NET_GAIN_PERCENT / 100,
        "results": results,
    }


def run_calibration(*, smoke: bool = False, workers: int = 1) -> dict:
    return _run(_FIXED_PROTOCOL, smoke=smoke, workers=workers)


def run_length_calibration(*, smoke: bool = False, workers: int = 1) -> dict:
    return _run(_LENGTH_PROTOCOL, smoke=smoke, workers=workers)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smoke", action="store_true", help="run four smaller replications per scenario")
    parser.add_argument("--by-length", action="store_true", help="run the prespecified block-length adjustment")
    parser.add_argument("--workers", type=int, default=min(6, os.cpu_count() or 1))
    args = parser.parse_args()
    try:
        run = run_length_calibration if args.by_length else run_calibration
        report = run(smoke=args.smoke, workers=args.workers)
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
