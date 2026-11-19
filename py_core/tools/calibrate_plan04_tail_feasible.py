"""PLAN-04 multi-seed, feasibility-first tail-rank calibration.

Protocol ``plan04-tail-feasible-calibration-v1``:

- Four NEW design seeds (20261206..20261209) are evaluated independently. No seed
  used by an earlier calibration is reused for design or validation.
- For every scenario/length, a candidate is feasible only when the pooled Wilson
  coverage and false-positive criteria and every design seed's point-estimate
  coverage, false-positive and stable-false-positive limits all hold. Select the
  LOWEST feasible rank (least unnecessary widening); otherwise retain the closest
  fallback and mark that cell infeasible. The primary scenario/length additionally
  requires at least one candidate with a design coverage gain of at least 1% over
  the original rank.
- Two NEW validation seeds (20261210 and 20261211) are used only after selection is
  frozen. They never influence the chosen ranks. All previous coverage, false-
  positive, per-seed point-estimate and paired-primary criteria remain unchanged.

This protocol is deliberately different from the previous directed correction: no
cell receives a hand-written direction or a validation-driven override. A failed
validation remains ``not_calibrated`` and never changes the research default.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from collections.abc import Mapping, Sequence
from concurrent.futures import ProcessPoolExecutor
from contextlib import nullcontext
from typing import Any

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
    FIXED_LEVELS,
    GRID,
    ORIGINAL_LEVELS,
    PRIMARY_LENGTH,
    PRIMARY_SCENARIO,
    TARGET_PER_THOUSAND,
    PairedReplication,
    compare_primary_coverage,
    ranked_replications,
)
from py_core.tools.calibrate_plan05 import COVERAGE_TOLERANCE, FALSE_POSITIVE_TOLERANCE

DESIGN_SEEDS = (20261206, 20261207, 20261208, 20261209)
VALIDATION_SEEDS = (20261210, 20261211)
DESIGN_REPETITIONS = 150
VALIDATION_REPETITIONS = 400
_METHODS = ("baseline", "fixed_rank", "by_length", "feasible")

type Replication = tuple[ReplicationMetrics, ...] | str
type CandidateSummaries = dict[str, Any]


def _block_is_feasible(summary: dict, by_seed: Sequence[dict], block_index: int) -> bool:
    lower, upper = COVERAGE_TOLERANCE
    fp_upper = FALSE_POSITIVE_TOLERANCE[1]
    block = summary["blocks"][block_index]
    if block["coverage_criterion"] != "within_tolerance" or block["false_positive_criterion"] != "within_tolerance":
        return False
    if summary["stable_false_positive_criterion"] != "within_tolerance":
        return False
    for seed_summary in by_seed:
        seed_block = seed_summary["blocks"][block_index]
        if not lower <= seed_block["coverage"]["fraction"] <= upper:
            return False
        if seed_block["false_positive"]["fraction"] > fp_upper:
            return False
        if seed_summary["stable_false_positive"]["fraction"] > fp_upper:
            return False
    return True


def select_feasible_levels(summaries: Mapping[str, CandidateSummaries]) -> dict:
    """Select the lowest feasible rank per scenario/length, recording every fallback."""
    primary_index = BLOCK_LENGTHS.index(PRIMARY_LENGTH)
    original_primary = GRID.index(ORIGINAL_LEVELS[primary_index])
    primary_pooled = summaries[PRIMARY_SCENARIO]["pooled"]
    original_coverage = primary_pooled[original_primary]["blocks"][primary_index]["coverage"]["fraction"]
    primary_candidates = [
        index
        for index, summary in enumerate(primary_pooled)
        if index > original_primary
        and summary["blocks"][primary_index]["coverage"]["fraction"] - original_coverage >= 0.01
    ]
    selected: dict[str, tuple[int, ...]] = {}
    fallback_cells: list[dict[str, Any]] = []
    for scenario in SCENARIOS:
        pooled = summaries[scenario.name]["pooled"]
        seed_summaries = list(summaries[scenario.name]["by_seed"].values())
        levels = list(ORIGINAL_LEVELS)
        for block_index, length in enumerate(BLOCK_LENGTHS):
            candidates = [
                index
                for index in range(len(GRID))
                if _block_is_feasible(
                    pooled[index],
                    [seed_summary[index] for seed_summary in seed_summaries],
                    block_index,
                )
            ]
            if scenario.name == PRIMARY_SCENARIO and block_index == primary_index:
                candidates = [index for index in candidates if index in primary_candidates]
            if candidates:
                # Feasibility first: least unnecessary widening, not a post-hoc direction.
                chosen = min(candidates, key=lambda index: GRID[index])
            else:
                chosen = min(
                    range(len(GRID)),
                    key=lambda index: (
                        abs(pooled[index]["blocks"][block_index]["coverage"]["fraction"] - TARGET_PER_THOUSAND / 1000),
                        GRID[index],
                    ),
                )
                fallback_cells.append({
                    "scenario": scenario.name,
                    "block_length": length,
                    "fallback_level": GRID[chosen],
                    "reason": "no candidate met every pooled and per-seed tolerance",
                })
            levels[block_index] = GRID[chosen]
        selected[scenario.name] = tuple(levels)
    return {
        "levels_by_scenario": selected,
        "fallback_cells": fallback_cells,
        "design_primary_constraint_met": bool(primary_candidates),
        "primary_candidate_levels": tuple(GRID[index] for index in primary_candidates),
    }


def _simulate(
    seed: int,
    repetitions: int,
    resamples: int,
    levels_by_method: Sequence[tuple[int, ...]] | Mapping[str, Sequence[tuple[int, ...]]],
    *,
    pool: ProcessPoolExecutor | None,
    progress: bool,
) -> dict[str, list[Replication]]:
    data: dict[str, list[Replication]] = {}
    streams = np.random.SeedSequence(seed).spawn(len(SCENARIOS))
    for scenario, stream in zip(SCENARIOS, streams, strict=True):
        method_levels = levels_by_method[scenario.name] if isinstance(levels_by_method, Mapping) else levels_by_method
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


def _candidate_summaries(scenario: TailScenario, rows: Sequence[Replication]) -> list[dict]:
    return [
        summarize_replications(scenario, (row if isinstance(row, str) else row[index] for row in rows))
        for index in range(len(GRID))
    ]


def _design_summaries(design: Mapping[int, Mapping[str, list[Replication]]]) -> dict[str, CandidateSummaries]:
    result: dict[str, CandidateSummaries] = {}
    for scenario in SCENARIOS:
        by_seed = {
            seed: _candidate_summaries(scenario, scenarios[scenario.name])
            for seed, scenarios in design.items()
        }
        pooled_rows = [row for scenarios in design.values() for row in scenarios[scenario.name]]
        result[scenario.name] = {
            "pooled": _candidate_summaries(scenario, pooled_rows),
            "by_seed": by_seed,
        }
    return result


def _summarize(scenario: TailScenario, rows: Sequence[Replication]) -> dict:
    return {
        method: summarize_replications(scenario, (row if isinstance(row, str) else row[index] for row in rows))
        for index, method in enumerate(_METHODS)
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
            "feasible_within_point_tolerances": _within_point_tolerances(methods["feasible"]),
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
    """Run independent multi-seed design selection and independent validation."""
    if isinstance(workers, bool) or not isinstance(workers, int) or workers < 1:
        raise ValueError("workers must be a positive integer")
    design_n, validation_n, resamples = (4, 4, 199) if smoke else (DESIGN_REPETITIONS, VALIDATION_REPETITIONS, RESAMPLES)
    grid_levels = tuple((level,) * len(BLOCK_LENGTHS) for level in GRID)
    executor = ProcessPoolExecutor(max_workers=workers) if workers > 1 else nullcontext()
    with executor as pool:
        design = {
            seed: _simulate(seed, design_n, resamples, grid_levels, pool=pool, progress=not smoke)
            for seed in DESIGN_SEEDS
        }
        summaries = _design_summaries(design)
        fit = select_feasible_levels(summaries)
        fit["validation_seeds_used_for_selection"] = False
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
                        fit["levels_by_scenario"][scenario.name],
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
        and not fit["fallback_cells"]
        and all(
            summary_within_tolerance(row["pooled"]["feasible"])
            and all(part["feasible_within_point_tolerances"] for part in row["by_seed"])
            for row in results
        )
        and primary["paired_primary_coverage"]["criterion"] == "within_tolerance"
    )
    return {
        "protocol": "plan04-tail-feasible-calibration-v1",
        "profile": "smoke" if smoke else "formal",
        "status": "smoke" if smoke else ("within_tolerance" if passed else "not_calibrated"),
        "numpy": np.__version__,
        "observations": OBSERVATIONS,
        "pairs": PAIRS,
        "block_lengths": BLOCK_LENGTHS,
        "resamples_per_length": resamples,
        "design_seeds": DESIGN_SEEDS,
        "design_repetitions_per_scenario_per_seed": design_n,
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
            "feasible_by_scenario": fit["levels_by_scenario"],
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
