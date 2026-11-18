"""PLAN-04 有向秩修正的单元测试与独立种子 smoke 验证。"""

from __future__ import annotations

import pytest

from py_core.tools.calibrate_plan04_tail import SCENARIOS
from py_core.tools.calibrate_plan04_tail_heldout import GRID, TARGET_PER_THOUSAND
from py_core.tools.calibrate_plan04_tail_robust import (
    DESIGN_SEED,
    VALIDATION_SEEDS,
    run_calibration,
    select_targeted_levels,
)

HELDOUT = (955, 965, 965)


def coverage_grid(*, gaussian14: dict[int, int], mixed7: dict[int, int]) -> dict:
    target = TARGET_PER_THOUSAND
    result = {}
    for scenario in SCENARIOS:
        counts = [[target, target, target] for _ in GRID]
        for index, level in enumerate(GRID):
            counts[index][1] = gaussian14.get(level, target)
            counts[index][0] = mixed7.get(level, target)
        result[scenario.name] = tuple(tuple(row) for row in counts)
    return result


def test_only_the_two_registered_cells_change() -> None:
    coverage = coverage_grid(gaussian14={950: TARGET_PER_THOUSAND, 965: TARGET_PER_THOUSAND + 500},
                             mixed7={970: TARGET_PER_THOUSAND, 955: TARGET_PER_THOUSAND - 400})
    selected = select_targeted_levels(coverage, HELDOUT, 1)
    for scenario in SCENARIOS:
        if scenario.name == "gaussian_null":
            assert selected[scenario.name] == (955, 925, 965)
        elif scenario.name == "ar_mixed_alternative":
            assert selected[scenario.name] == (985, 965, 965)
        else:
            assert selected[scenario.name] == HELDOUT


def test_direction_never_crosses_the_heldout_rank() -> None:
    coverage = coverage_grid(gaussian14={level: TARGET_PER_THOUSAND + 100 for level in GRID},
                             mixed7={level: TARGET_PER_THOUSAND - 100 for level in GRID})
    selected = select_targeted_levels(coverage, HELDOUT, 1)
    for scenario in SCENARIOS:
        levels = selected[scenario.name]
        if scenario.name == "gaussian_null":
            assert levels[1] <= HELDOUT[1]
        if scenario.name == "ar_mixed_alternative":
            assert levels[0] >= HELDOUT[0]
        assert levels[2] == HELDOUT[2]


def test_ties_follow_the_required_correction_direction() -> None:
    coverage = coverage_grid(
        gaussian14={955: TARGET_PER_THOUSAND + 100, 960: TARGET_PER_THOUSAND + 100},
        mixed7={960: TARGET_PER_THOUSAND - 100, 965: TARGET_PER_THOUSAND - 100},
    )
    selected = select_targeted_levels(coverage, HELDOUT, 1)
    assert selected["gaussian_null"][1] == 925
    assert selected["ar_mixed_alternative"][0] == 985


def test_invalid_inputs_are_rejected() -> None:
    coverage = coverage_grid(gaussian14={}, mixed7={})
    with pytest.raises(ValueError, match="one rank per block"):
        select_targeted_levels(coverage, (950, 950), 1)
    with pytest.raises(ValueError, match="positive"):
        select_targeted_levels(coverage, HELDOUT, 0)


def test_smoke_run_uses_new_validation_seeds_and_keeps_selection_out_of_validation() -> None:
    report = run_calibration(smoke=True, workers=1)
    assert report["protocol"] == "plan04-tail-robust-calibration-v1"
    assert report["profile"] == "smoke"
    assert report["status"] == "smoke"
    assert report["design_seed"] == DESIGN_SEED
    assert report["validation_seeds"] == VALIDATION_SEEDS == (20261204, 20261205)
    assert DESIGN_SEED not in VALIDATION_SEEDS
    assert report["fit"]["validation_seeds_used_for_selection"] is False
    assert len(report["results"]) == len(SCENARIOS)
    for row in report["results"]:
        assert len(row["by_seed"]) == 2
        assert all(part["robust_within_point_tolerances"] is not None for part in row["by_seed"])


def test_workers_must_be_a_positive_integer() -> None:
    with pytest.raises(ValueError, match="positive integer"):
        run_calibration(smoke=True, workers=0)
