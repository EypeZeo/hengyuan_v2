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
REPETITIONS = 200


def coverage_grid(
    *,
    gaussian14: dict[int, int],
    mixed7: dict[int, int],
    repetitions: int = REPETITIONS,
    default_count: int = 0,
) -> dict:
    assert 0 <= default_count <= repetitions
    assert all(0 <= count <= repetitions for count in (*gaussian14.values(), *mixed7.values()))
    result = {}
    for scenario in SCENARIOS:
        counts = [[default_count, default_count, default_count] for _ in GRID]
        for index, level in enumerate(GRID):
            counts[index][1] = gaussian14.get(level, default_count)
            counts[index][0] = mixed7.get(level, default_count)
        result[scenario.name] = tuple(tuple(row) for row in counts)
    return result


def test_scaled_target_selects_the_nearest_rank_in_both_directions_and_leaves_other_cells_unchanged() -> None:
    gaussian14 = dict(zip(GRID, (140, 150, 160, 170, 180, 185, 190, 191, 200, 200, 200, 200, 200), strict=True))
    mixed7 = dict(zip(GRID, (120, 125, 130, 135, 140, 145, 150, 160, 170, 180, 191, 192, 200), strict=True))
    coverage = coverage_grid(gaussian14=gaussian14, mixed7=mixed7)

    assert 1_000 * 191 == TARGET_PER_THOUSAND * REPETITIONS
    selected = select_targeted_levels(coverage, HELDOUT, REPETITIONS)

    for scenario in SCENARIOS:
        if scenario.name == "gaussian_null":
            assert selected[scenario.name] == (955, 960, 965)
        elif scenario.name == "ar_mixed_alternative":
            assert selected[scenario.name] == (975, 965, 965)
        else:
            assert selected[scenario.name] == HELDOUT


def test_direction_never_crosses_the_heldout_rank() -> None:
    coverage = coverage_grid(
        gaussian14={level: 120 + 5 * index for index, level in enumerate(GRID)},
        mixed7={level: 130 + 5 * index for index, level in enumerate(GRID)},
    )
    selected = select_targeted_levels(coverage, HELDOUT, REPETITIONS)
    for scenario in SCENARIOS:
        levels = selected[scenario.name]
        if scenario.name == "gaussian_null":
            assert levels[1] <= HELDOUT[1]
        if scenario.name == "ar_mixed_alternative":
            assert levels[0] >= HELDOUT[0]
        assert levels[2] == HELDOUT[2]


def test_ties_follow_the_required_correction_direction() -> None:
    coverage = coverage_grid(
        gaussian14=dict(zip(GRID, (150, 155, 160, 165, 170, 175, 190, 192, 200, 200, 200, 200, 200), strict=True)),
        mixed7=dict(zip(GRID, (150, 155, 160, 165, 170, 175, 180, 185, 188, 189, 190, 192, 200), strict=True)),
    )
    selected = select_targeted_levels(coverage, HELDOUT, REPETITIONS)
    assert abs(1_000 * coverage["gaussian_null"][GRID.index(955)][1] - TARGET_PER_THOUSAND * REPETITIONS) == abs(
        1_000 * coverage["gaussian_null"][GRID.index(960)][1] - TARGET_PER_THOUSAND * REPETITIONS
    )
    assert abs(1_000 * coverage["ar_mixed_alternative"][GRID.index(975)][0] - TARGET_PER_THOUSAND * REPETITIONS) == abs(
        1_000 * coverage["ar_mixed_alternative"][GRID.index(980)][0] - TARGET_PER_THOUSAND * REPETITIONS
    )
    assert selected["gaussian_null"][1] == 955
    assert selected["ar_mixed_alternative"][0] == 980


def test_invalid_inputs_are_rejected() -> None:
    coverage = coverage_grid(gaussian14={}, mixed7={})
    with pytest.raises(ValueError, match="one rank per block"):
        select_targeted_levels(coverage, (950, 950), REPETITIONS)
    with pytest.raises(ValueError, match="positive"):
        select_targeted_levels(coverage, HELDOUT, 0)


def test_coverage_counts_must_be_within_the_repetition_total() -> None:
    coverage = coverage_grid(gaussian14={}, mixed7={})
    malformed = dict(coverage)
    rows = list(malformed[SCENARIOS[0].name])
    rows[0] = (REPETITIONS + 1, 0, 0)
    malformed[SCENARIOS[0].name] = tuple(rows)

    with pytest.raises(ValueError, match="integer coverage counts within the repetition total"):
        select_targeted_levels(malformed, HELDOUT, REPETITIONS)


def test_coverage_rows_must_match_the_number_of_block_lengths() -> None:
    coverage = coverage_grid(gaussian14={}, mixed7={})
    malformed = dict(coverage)
    rows = list(malformed[SCENARIOS[0].name])
    rows[0] = (0, 0)
    malformed[SCENARIOS[0].name] = tuple(rows)

    with pytest.raises(ValueError, match="integer coverage counts within the repetition total"):
        select_targeted_levels(malformed, HELDOUT, REPETITIONS)


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


def test_smoke_run_also_works_with_a_process_pool() -> None:
    report = run_calibration(smoke=True, workers=2)
    assert report["status"] == "smoke"
    assert report["validation_seeds"] == VALIDATION_SEEDS
    assert len(report["results"]) == len(SCENARIOS)


def test_workers_must_be_a_positive_integer() -> None:
    with pytest.raises(ValueError, match="positive integer"):
        run_calibration(smoke=True, workers=0)
