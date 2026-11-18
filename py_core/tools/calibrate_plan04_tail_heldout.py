"""Select shared tail ranks on simulated paths and evaluate on independent seeds."""

from __future__ import annotations

import argparse
import json
import os
import sys
from collections.abc import Iterable, Mapping, Sequence
from concurrent.futures import ProcessPoolExecutor
from contextlib import nullcontext

import numpy as np

from py_core.tools.calibrate_plan04_tail import (
    BLOCK_LENGTHS,
    OBSERVATIONS,
    PAIRS,
    PRIMARY_LENGTH,
    RESAMPLES,
    SCENARIOS,
    ReplicationMetrics,
    TailScenario,
    summarize_replications,
    summary_within_tolerance,
)
from py_core.tools.calibrate_plan04_tail_ranked import (
    BY_LENGTH_LEVELS,
    FIXED_LEVELS,
    ORIGINAL_LEVELS,
    PRIMARY_SCENARIO,
    PairedReplication,
    compare_primary_coverage,
    ranked_replications,
)
from py_core.tools.calibrate_plan05 import COVERAGE_TOLERANCE, FALSE_POSITIVE_TOLERANCE

DESIGN_SEED = 20261201
VALIDATION_SEEDS = (20261202, 20261203)
DESIGN_REPETITIONS = 300
VALIDATION_REPETITIONS = 400
GRID = tuple(range(925, 986, 5))
TARGET_PER_THOUSAND = 955
_METHODS = ("baseline", "fixed_rank", "by_length", "selected")

type Replication = tuple[ReplicationMetrics, ...] | str
type CoverageCounts = tuple[tuple[int, ...], ...]


def select_levels(coverage: Mapping[str, CoverageCounts], repetitions: int) -> dict:
    """Minimize each length's worst design coverage deviation on the frozen grid."""
    primary_index = BLOCK_LENGTHS.index(PRIMARY_LENGTH)
    original_index = GRID.index(ORIGINAL_LEVELS[primary_index])
    primary_counts = coverage[PRIMARY_SCENARIO]
    original = primary_counts[original_index][primary_index]
    primary_candidates = [
        i for i in range(len(GRID)) if 100 * (primary_counts[i][primary_index] - original) >= repetitions
    ]
    selected = []
    deviations = []
    for length_index in range(len(BLOCK_LENGTHS)):
        candidates = primary_candidates if length_index == primary_index and primary_candidates else range(len(GRID))
        errors = [
            max(abs(1000 * counts[i][length_index] - TARGET_PER_THOUSAND * repetitions) for counts in coverage.values())
            for i in range(len(GRID))
        ]
        i = min(candidates, key=lambda index: (errors[index], GRID[index]))
        selected.append(GRID[i])
        deviations.append(errors[i] / (1000 * repetitions))
    return {
        "levels_per_thousand": tuple(selected),
        "worst_design_deviation_by_length": deviations,
        "design_primary_constraint_met": bool(primary_candidates),
        "candidate_coverage_counts": dict(coverage),
    }


def _simulate(
    seed: int,
    repetitions: int,
    resamples: int,
    levels: tuple[tuple[int, ...], ...],
    *,
    pool: ProcessPoolExecutor | None,
    progress: bool,
) -> dict[str, list[Replication]]:
    data = {}
    streams = np.random.SeedSequence(seed).spawn(len(SCENARIOS))
    for scenario, stream in zip(SCENARIOS, streams, strict=True):
        rows = []
        outcomes = ranked_replications(
            scenario, stream, repetitions=repetitions, resamples=resamples, levels_by_method=levels, pool=pool
        )
        for count, outcome in enumerate(outcomes, start=1):
            rows.append(outcome)
            if progress and count % 50 == 0:
                print(f"{seed} {scenario.name} {count}/{repetitions}", file=sys.stderr, flush=True)
        data[scenario.name] = rows
    return data


def _design_coverage(rows: Iterable[Replication]) -> CoverageCounts:
    counts = np.zeros((len(GRID), len(BLOCK_LENGTHS)), dtype=np.int64)
    for row in rows:
        match row:
            case str(reason):
                raise RuntimeError(f"design estimation failed: {reason}")
            case _:
                counts += np.asarray([metrics.coverage for metrics in row])
    return tuple(tuple(map(int, count)) for count in counts)


def _summarize(scenario: TailScenario, rows: Sequence[Replication]) -> dict:
    return {
        method: summarize_replications(scenario, (row if isinstance(row, str) else row[i] for row in rows))
        for i, method in enumerate(_METHODS)
    }


def _within_point_tolerances(summary: dict) -> bool:
    if summary["failures"] or not summary["valid_repetitions"]:
        return False
    lower, upper = COVERAGE_TOLERANCE
    fp_upper = FALSE_POSITIVE_TOLERANCE[1]
    return (
        summary["stable_false_positive"]["fraction"] <= fp_upper
        and all(
            lower <= block["coverage"]["fraction"] <= upper and block["false_positive"]["fraction"] <= fp_upper
            for block in summary["blocks"]
        )
    )


def _validation_summary(scenario: TailScenario, by_seed: Mapping[int, dict[str, list[Replication]]]) -> dict:
    parts = []
    pooled = []
    for seed, scenarios in by_seed.items():
        rows = scenarios[scenario.name]
        methods = _summarize(scenario, rows)
        parts.append({
            "seed": seed,
            "selected_within_point_tolerances": _within_point_tolerances(methods["selected"]),
            "methods": methods,
        })
        pooled.extend(rows)
    paired = [PairedReplication(row[0], row[-1]) for row in pooled if not isinstance(row, str)]
    return {
        "scenario": scenario.name,
        "by_seed": parts,
        "pooled": _summarize(scenario, pooled),
        "paired_primary_coverage": compare_primary_coverage(paired),
    }


def run_calibration(*, smoke: bool = False, workers: int = 1) -> dict:
    if type(workers) is not int or workers < 1:
        raise ValueError("workers must be a positive integer")
    design_n, validation_n, resamples = (
        (4, 4, 199) if smoke else (DESIGN_REPETITIONS, VALIDATION_REPETITIONS, RESAMPLES)
    )
    grid_levels = tuple((level,) * len(BLOCK_LENGTHS) for level in GRID)
    executor = ProcessPoolExecutor(max_workers=workers) if workers > 1 else nullcontext()
    with executor as pool:
        design = _simulate(DESIGN_SEED, design_n, resamples, grid_levels, pool=pool, progress=not smoke)
        fit = select_levels({name: _design_coverage(rows) for name, rows in design.items()}, design_n)
        levels = (ORIGINAL_LEVELS, FIXED_LEVELS, BY_LENGTH_LEVELS, fit["levels_per_thousand"])
        if not smoke:
            print(f"Selected levels per thousand: {fit['levels_per_thousand']}", file=sys.stderr, flush=True)
        validation = {
            seed: _simulate(seed, validation_n, resamples, levels, pool=pool, progress=not smoke)
            for seed in VALIDATION_SEEDS
        }
    results = [_validation_summary(scenario, validation) for scenario in SCENARIOS]
    primary = next(row for row in results if row["scenario"] == PRIMARY_SCENARIO)
    passed = (
        fit["design_primary_constraint_met"]
        and all(
            summary_within_tolerance(row["pooled"]["selected"])
            and all(part["selected_within_point_tolerances"] for part in row["by_seed"])
            for row in results
        )
        and primary["paired_primary_coverage"]["criterion"] == "within_tolerance"
    )
    return {
        "protocol": "plan04-tail-heldout-calibration-v1",
        "profile": "smoke" if smoke else "formal",
        "status": "smoke" if smoke else ("within_tolerance" if passed else "not_calibrated"),
        "numpy": np.__version__,
        "observations": OBSERVATIONS,
        "pairs": PAIRS,
        "block_lengths": BLOCK_LENGTHS,
        "resamples_per_length": resamples,
        "design_seed": DESIGN_SEED,
        "design_repetitions_per_scenario": design_n,
        "design_grid_per_thousand": GRID,
        "design_target_coverage": TARGET_PER_THOUSAND / 1000,
        "validation_seeds": VALIDATION_SEEDS,
        "validation_repetitions_per_scenario_per_seed": validation_n,
        "coverage_tolerance": COVERAGE_TOLERANCE,
        "false_positive_tolerance": FALSE_POSITIVE_TOLERANCE,
        "levels_per_thousand_by_method": dict(zip(_METHODS, levels, strict=True)),
        "fit": fit,
        "results": results,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smoke", action="store_true", help="run four replications per scenario and seed")
    parser.add_argument("--workers", type=int, default=min(6, os.cpu_count() or 1))
    args = parser.parse_args()
    try:
        report = run_calibration(smoke=args.smoke, workers=args.workers)
    except (ValueError, RuntimeError) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
