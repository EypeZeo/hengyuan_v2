"""PLAN-04 robust tail-rank calibration for the two registered coverage deviations.

Protocol ``plan04-tail-robust-calibration-v1``:

1. Reuse the held-out design sample and :func:`select_levels` verbatim to obtain the
   previous selected ranks. Validation data never enters selection.
2. For ``gaussian_null`` at 14 days (previous pooled Wilson upper bound 98.57%),
   candidates may only move the rank DOWN (narrower band, reducing over-coverage).
   For ``ar_mixed_alternative`` at 7 days (previous pooled Wilson lower bound 91.85%),
   candidates may only move the rank UP (wider band, reducing under-coverage).
   Every other scenario/length keeps the previous selected rank. Within each
   direction, choose the smallest absolute deviation from the frozen 95.5% target;
   ties prefer the direction of the required correction.
3. Freeze those scenario-specific ranks, then evaluate them on two NEW independent
   seeds (20261204 and 20261205), never the held-out seeds. All original coverage,
   false-positive, per-seed point-estimate and paired-primary criteria are unchanged.

The research default remains the original rank unless every acceptance check passes;
a smoke run or a failed validation never changes it. The module is intentionally a
separate experiment so the previous calibration modules and reports stay immutable.
"""

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
    RESAMPLES,
    SCENARIOS,
    ReplicationMetrics,
    TailScenario,
    summarize_replications,
    summary_within_tolerance,
)
from py_core.tools.calibrate_plan04_tail_heldout import (
    BY_LENGTH_LEVELS,
    DESIGN_REPETITIONS,
    DESIGN_SEED,
    FIXED_LEVELS,
    GRID,
    ORIGINAL_LEVELS,
    PRIMARY_SCENARIO,
    TARGET_PER_THOUSAND,
    VALIDATION_REPETITIONS,
    PairedReplication,
    compare_primary_coverage,
    ranked_replications,
    select_levels,
)
from py_core.tools.calibrate_plan05 import COVERAGE_TOLERANCE, FALSE_POSITIVE_TOLERANCE

VALIDATION_SEEDS = (20261204, 20261205)
_METHODS = ("baseline", "fixed_rank", "by_length", "heldout_selected", "robust")
_OVER_COVERED_SCENARIO = "gaussian_null"
_OVER_COVERED_LENGTH = 14
_UNDER_COVERED_SCENARIO = "ar_mixed_alternative"
_UNDER_COVERED_LENGTH = 7

type Replication = tuple[ReplicationMetrics, ...] | str
type CoverageCounts = tuple[tuple[int, ...], ...]


def select_targeted_levels(
    coverage: Mapping[str, CoverageCounts],
    heldout_levels: Sequence[int],
    repetitions: int,
) -> dict[str, tuple[int, ...]]:
    """Choose only direction-constrained adjustments from the design coverage grid.

    Ties prefer a lower rank for the over-covered case and a higher rank for the
    under-covered case. All other cells return ``heldout_levels`` unchanged.
    """
    if len(heldout_levels) != len(BLOCK_LENGTHS):
        raise ValueError("heldout_levels must have one rank per block length")
    if isinstance(repetitions, bool) or not isinstance(repetitions, int) or repetitions <= 0:
        raise ValueError("repetitions must be a positive integer")
    over_index = BLOCK_LENGTHS.index(_OVER_COVERED_LENGTH)
    under_index = BLOCK_LENGTHS.index(_UNDER_COVERED_LENGTH)
    base_over = GRID.index(heldout_levels[over_index])
    base_under = GRID.index(heldout_levels[under_index])
    target = TARGET_PER_THOUSAND * repetitions

    selected: dict[str, tuple[int, ...]] = {}
    for scenario in SCENARIOS:
        counts = coverage[scenario.name]
        if len(counts) != len(GRID) or any(
            len(row) != len(BLOCK_LENGTHS)
            or any(isinstance(count, bool) or not isinstance(count, int) or not 0 <= count <= repetitions for count in row)
            for row in counts
        ):
            raise ValueError(f"{scenario.name} must provide integer coverage counts within the repetition total")
        levels = list(heldout_levels)
        for length_index in range(len(BLOCK_LENGTHS)):
            if scenario.name == _OVER_COVERED_SCENARIO and length_index == over_index:
                candidates = range(base_over + 1)
                # Same deviation -> lower level (stronger narrowing correction).
                chosen = min(
                    candidates,
                    key=lambda i: (abs(1000 * counts[i][length_index] - target), GRID[i]),
                )
            elif scenario.name == _UNDER_COVERED_SCENARIO and length_index == under_index:
                candidates = range(base_under, len(GRID))
                # Same deviation -> higher level (stronger widening correction).
                chosen = min(
                    candidates,
                    key=lambda i: (abs(1000 * counts[i][length_index] - target), -GRID[i]),
                )
            else:
                chosen = GRID.index(heldout_levels[length_index])
            levels[length_index] = GRID[chosen]
        selected[scenario.name] = tuple(levels)
    return selected


def _simulate(
    seed: int,
    repetitions: int,
    resamples: int,
    levels: Sequence[tuple[int, ...]] | Mapping[str, Sequence[tuple[int, ...]]],
    *,
    pool: ProcessPoolExecutor | None,
    progress: bool,
) -> dict[str, list[Replication]]:
    data: dict[str, list[Replication]] = {}
    streams = np.random.SeedSequence(seed).spawn(len(SCENARIOS))
    for scenario, stream in zip(SCENARIOS, streams, strict=True):
        method_levels = levels[scenario.name] if isinstance(levels, Mapping) else levels
        outcomes = ranked_replications(
            scenario, stream, repetitions=repetitions, resamples=resamples,
            levels_by_method=tuple(method_levels), pool=pool,
        )
        rows = []
        for count, outcome in enumerate(outcomes, start=1):
            rows.append(outcome)
            if progress and count % 50 == 0:
                print(f"{seed} {scenario.name} {count}/{repetitions}", file=sys.stderr, flush=True)
        data[scenario.name] = rows
    return data


def _design_coverage(rows: Iterable[Replication]) -> CoverageCounts:
    counts = np.zeros((len(GRID), len(BLOCK_LENGTHS)), dtype=np.int64)
    for row in rows:
        if isinstance(row, str):
            raise TypeError(f"design estimation failed: {row}")
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
            lower <= block["coverage"]["fraction"] <= upper
            and block["false_positive"]["fraction"] <= fp_upper
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
            "robust_within_point_tolerances": _within_point_tolerances(methods["robust"]),
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
    """Run design selection and two independent validations with no reuse for tuning."""
    if isinstance(workers, bool) or not isinstance(workers, int) or workers < 1:
        raise ValueError("workers must be a positive integer")
    design_n, validation_n, resamples = (4, 4, 199) if smoke else (DESIGN_REPETITIONS, VALIDATION_REPETITIONS, RESAMPLES)
    grid_levels = tuple((level,) * len(BLOCK_LENGTHS) for level in GRID)
    executor = ProcessPoolExecutor(max_workers=workers) if workers > 1 else nullcontext()
    with executor as pool:
        design = _simulate(DESIGN_SEED, design_n, resamples, grid_levels, pool=pool, progress=not smoke)
        coverage = {name: _design_coverage(rows) for name, rows in design.items()}
        base_fit = select_levels(coverage, design_n)
        heldout_levels = base_fit["levels_per_thousand"]
        robust_by_scenario = select_targeted_levels(coverage, heldout_levels, design_n)
        fit = {
            **base_fit,
            "robust_levels_by_scenario": robust_by_scenario,
            "validation_seeds_used_for_selection": False,
        }
        validation = {
            seed: _simulate(
                seed,
                validation_n,
                resamples,
                {
                    scenario.name: (
                        ORIGINAL_LEVELS,
                        FIXED_LEVELS,
                        BY_LENGTH_LEVELS,
                        heldout_levels,
                        robust_by_scenario[scenario.name],
                    )
                    for scenario in SCENARIOS
                },
                pool=pool,
                progress=not smoke,
            )
            for seed in VALIDATION_SEEDS
        }
    results = [_validation_summary(scenario, validation) for scenario in SCENARIOS]
    primary = next(row for row in results if row["scenario"] == PRIMARY_SCENARIO)
    passed = (
        fit["design_primary_constraint_met"]
        and all(
            summary_within_tolerance(row["pooled"]["robust"])
            and all(part["robust_within_point_tolerances"] for part in row["by_seed"])
            for row in results
        )
        and primary["paired_primary_coverage"]["criterion"] == "within_tolerance"
    )
    return {
        "protocol": "plan04-tail-robust-calibration-v1",
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
        "levels_per_thousand_by_method": {
            "baseline": ORIGINAL_LEVELS,
            "fixed_rank": FIXED_LEVELS,
            "by_length": BY_LENGTH_LEVELS,
            "heldout_selected": heldout_levels,
            "robust_by_scenario": robust_by_scenario,
        },
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
    except (ValueError, TypeError, RuntimeError) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
