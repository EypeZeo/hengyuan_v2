"""PLAN-04 多种子、可行性优先选秩的测试。"""

from __future__ import annotations

from dataclasses import asdict

import pytest

from py_core.tools.calibrate_plan04_tail import SCENARIOS
from py_core.tools.calibrate_plan04_tail_feasible import (
    DESIGN_SEEDS,
    VALIDATION_SEEDS,
    run_calibration,
    select_feasible_levels,
)
from py_core.tools.calibrate_plan04_tail_heldout import GRID
from py_core.tools.calibrate_plan05 import (
    COVERAGE_TOLERANCE,
    FALSE_POSITIVE_TOLERANCE,
    classify_rate,
    wilson_rate,
)


def summary(coverage: float, false_positive: float = 0.01, stable_false_positive: float = 0.01) -> dict:
    blocks = []
    for _ in range(3):
        coverage_rate = wilson_rate(round(1000 * coverage), 1000)
        false_rate = wilson_rate(round(1000 * false_positive), 1000)
        blocks.append({
            "block_length": 7,
            "coverage": asdict(coverage_rate),
            "coverage_criterion": classify_rate(coverage_rate, COVERAGE_TOLERANCE),
            "false_positive": asdict(false_rate),
            "false_positive_criterion": classify_rate(false_rate, FALSE_POSITIVE_TOLERANCE),
        })
    stable_rate = wilson_rate(round(1000 * stable_false_positive), 1000)
    return {
        "valid_repetitions": 1000,
        "failures": {},
        "blocks": blocks,
        "stable_false_positive": asdict(stable_rate),
        "stable_false_positive_criterion": classify_rate(stable_rate, FALSE_POSITIVE_TOLERANCE),
    }


def summaries(coverages: list[float], *, primary_original: float | None = None,
              primary_candidates: list[float] | None = None) -> dict:
    result = {}
    for scenario in SCENARIOS:
        if scenario.name == "ar_positive_null" and primary_original is not None:
            candidates = [primary_original, *([primary_original] * (len(GRID) - 1))]
            if primary_candidates is not None:
                for index, value in enumerate(primary_candidates):
                    candidates[index + 6] = value
        else:
            candidates = coverages
        pooled = [summary(value) for value in candidates]
        by_seed = {
            101: [summary(value) for value in candidates],
            102: [summary(value) for value in candidates],
        }
        result[scenario.name] = {"pooled": pooled, "by_seed": by_seed}
    return result


def test_lowest_feasible_rank_wins_except_for_the_required_primary_gain() -> None:
    fit = select_feasible_levels(summaries([0.955] * len(GRID), primary_original=0.945,
                                          primary_candidates=[0.965] * (len(GRID) - 6)))
    for scenario in SCENARIOS:
        expected = (925, 955, 925) if scenario.name == "ar_positive_null" else (925, 925, 925)
        assert fit["levels_by_scenario"][scenario.name] == expected
    assert fit["fallback_cells"] == []
    assert fit["design_primary_constraint_met"] is True


def test_an_infeasible_cell_records_a_fallback_instead_of_faking_feasibility() -> None:
    fit = select_feasible_levels(summaries([0.99] * len(GRID)))
    assert fit["fallback_cells"]
    assert fit["fallback_cells"][0]["reason"] == "no candidate met every pooled and per-seed tolerance"
    assert fit["design_primary_constraint_met"] is False


def test_primary_gain_is_required_even_when_other_cells_are_feasible() -> None:
    fit = select_feasible_levels(summaries([0.955] * len(GRID)))
    assert fit["design_primary_constraint_met"] is False
    assert fit["primary_candidate_levels"] == ()


def test_smoke_run_uses_only_new_seeds_and_is_process_deterministic() -> None:
    first = run_calibration(smoke=True, workers=1)
    second = run_calibration(smoke=True, workers=2)
    assert first == second
    assert first["design_seeds"] == DESIGN_SEEDS == (20261206, 20261207, 20261208, 20261209)
    assert first["validation_seeds"] == VALIDATION_SEEDS == (20261210, 20261211)
    assert not set(DESIGN_SEEDS) & set(VALIDATION_SEEDS)
    assert first["fit"]["validation_seeds_used_for_selection"] is False
    assert len(first["results"]) == len(SCENARIOS)
    assert first["status"] == "smoke"


def test_workers_must_be_a_positive_integer() -> None:
    with pytest.raises(ValueError, match="positive integer"):
        run_calibration(smoke=True, workers=0)
