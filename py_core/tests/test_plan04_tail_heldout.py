"""Tests for the held-out PLAN-04 tail-rank calibration protocol."""

from __future__ import annotations

import json

import pytest

from py_core.tools import calibrate_plan04_tail_heldout as heldout


def _coverage_fixture(
    *,
    repetitions: int,
    primary_14_day_by_level: dict[int, int] | None = None,
) -> dict[str, heldout.CoverageCounts]:
    primary_index = heldout.BLOCK_LENGTHS.index(heldout.PRIMARY_LENGTH)
    original_level = heldout.ORIGINAL_LEVELS[primary_index]
    coverage: dict[str, heldout.CoverageCounts] = {}
    for scenario in heldout.SCENARIOS:
        rows = []
        for level in heldout.GRID:
            row = [95, 95, 95] if repetitions == 100 else [180, 192, 180]
            if repetitions == 200 and level == original_level:
                row[primary_index] = 190
            if primary_14_day_by_level is not None and scenario.name == heldout.PRIMARY_SCENARIO:
                row[primary_index] = primary_14_day_by_level.get(level, 95)
            rows.append(tuple(row))
        coverage[scenario.name] = tuple(rows)
    return coverage


def test_select_levels_minimizes_worst_deviation_and_breaks_ties_low() -> None:
    coverage = _coverage_fixture(repetitions=200)
    rows_by_scenario = {name: list(rows) for name, rows in coverage.items()}
    # For 7 days, the lower candidate has errors [0, 0, 2000, 0]. The next two
    # candidates both have worst error 1000, so minimax selects the lower one.
    for scenario_index, scenario in enumerate(heldout.SCENARIOS):
        rows = rows_by_scenario[scenario.name]
        for level_index, values in (
            (2, (191, 191, 189, 191)),
            (3, (190, 192, 190, 192)),
            (4, (190, 192, 190, 192)),
        ):
            row = list(rows[level_index])
            row[0] = values[scenario_index]
            rows[level_index] = tuple(row)
        for level_index, value in ((6, 190), (7, 192)):
            row = list(rows[level_index])
            row[2] = value
            rows[level_index] = tuple(row)
    coverage = {name: tuple(rows) for name, rows in rows_by_scenario.items()}

    fit = heldout.select_levels(coverage, repetitions=200)

    assert fit["design_primary_constraint_met"] is True
    assert fit["levels_per_thousand"][0] == heldout.GRID[3]
    assert fit["levels_per_thousand"][0] < heldout.GRID[4]
    assert fit["worst_design_deviation_by_length"] == pytest.approx((0.005, 0.005, 0.005))


def test_fourteen_day_selection_requires_primary_gain_and_falls_back_when_absent() -> None:
    primary_index = heldout.BLOCK_LENGTHS.index(heldout.PRIMARY_LENGTH)
    original_level = heldout.ORIGINAL_LEVELS[primary_index]
    eligible_level = heldout.GRID[8]
    eligible = _coverage_fixture(
        repetitions=100,
        primary_14_day_by_level={original_level: 95, eligible_level: 96},
    )

    fit = heldout.select_levels(eligible, repetitions=100)

    assert fit["design_primary_constraint_met"] is True
    assert fit["levels_per_thousand"][primary_index] == eligible_level

    fallback = _coverage_fixture(repetitions=100)
    fallback_fit = heldout.select_levels(fallback, repetitions=100)

    assert fallback_fit["design_primary_constraint_met"] is False
    assert fallback_fit["levels_per_thousand"][primary_index] == heldout.GRID[0]


def test_smoke_report_separates_design_and_validation_and_pools_counts() -> None:
    report = heldout.run_calibration(smoke=True, workers=1)

    assert report["protocol"] == "plan04-tail-heldout-calibration-v1"
    assert report["profile"] == "smoke"
    assert report["status"] == "smoke"
    assert report["design_seed"] == 20261201
    assert report["validation_seeds"] == (20261202, 20261203)
    assert report["design_seed"] not in report["validation_seeds"]
    assert report["design_repetitions_per_scenario"] == 4
    assert report["validation_repetitions_per_scenario_per_seed"] == 4
    assert report["resamples_per_length"] == 199
    assert report["design_grid_per_thousand"] == heldout.GRID
    assert report["levels_per_thousand_by_method"]["selected"] == report["fit"]["levels_per_thousand"]

    design_counts = report["fit"]["candidate_coverage_counts"]
    assert set(design_counts) == {scenario.name for scenario in heldout.SCENARIOS}
    for rows in design_counts.values():
        assert len(rows) == len(heldout.GRID)
        assert all(len(row) == len(heldout.BLOCK_LENGTHS) for row in rows)
        assert all(0 <= count <= 4 for row in rows for count in row)

    methods = ("baseline", "fixed_rank", "by_length", "selected")
    assert len(report["results"]) == len(heldout.SCENARIOS)
    for result in report["results"]:
        assert [part["seed"] for part in result["by_seed"]] == list(report["validation_seeds"])
        for part in result["by_seed"]:
            for method in methods:
                assert part["methods"][method]["valid_repetitions"] == 4
        for method in methods:
            assert result["pooled"][method]["valid_repetitions"] == 8
            assert result["pooled"][method]["valid_repetitions"] == sum(
                part["methods"][method]["valid_repetitions"] for part in result["by_seed"]
            )
        assert result["paired_primary_coverage"]["valid_repetitions"] == 8

    json.dumps(report, allow_nan=False)
