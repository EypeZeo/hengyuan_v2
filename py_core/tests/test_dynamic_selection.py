"""Tests for chronological model selection and conditional sign randomization."""

from __future__ import annotations

import json
import math

import numpy as np
import pytest

from py_core.research import dynamic_selection
from py_core.research.dynamic_selection import (
    evaluate_walk_forward,
    run_dynamic_selection_test,
)
from py_core.tools import calibrate_dynamic_selection


class FixedSignGenerator:
    def __init__(self, bits: np.ndarray) -> None:
        self.bits = np.asarray(bits, dtype=np.int8)

    def integers(self, low: int, high: int, *, size: tuple[int, int], dtype: type) -> np.ndarray:
        assert low == 0
        assert high == 2
        assert tuple(size) == self.bits.shape
        return self.bits.astype(dtype, copy=True)


def _selection_fixture() -> tuple[np.ndarray, np.ndarray]:
    predictors = np.column_stack(
        (
            np.array([1, 1, 1, -1, -1, -1, -1, -1, -1], dtype=np.float64),
            np.array([1, 1, 1, 1, 1, -1, -1, -1, -1], dtype=np.float64),
        )
    )
    response_paths = np.vstack((np.ones(9), np.zeros(9)))
    return predictors, response_paths


def _scalar_walk_forward(
    predictors: np.ndarray,
    response_paths: np.ndarray,
    *,
    train_periods: int,
    validation_periods: int,
    test_periods: int,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Small loop-based reference for training, selection, refitting, and test payoff."""
    periods, candidates = predictors.shape
    fold_count = (periods - train_periods - validation_periods) // test_periods
    selected = np.empty((response_paths.shape[0], fold_count), dtype=np.intp)
    refitted = np.empty((response_paths.shape[0], fold_count), dtype=np.float64)
    payoffs = np.empty((response_paths.shape[0], fold_count), dtype=np.float64)

    for path_index, response in enumerate(response_paths):
        for fold in range(fold_count):
            test_start = train_periods + validation_periods + fold * test_periods
            validation_start = test_start - validation_periods
            train_start = validation_start - train_periods
            train_slopes = []
            for candidate in range(candidates):
                cross_product = sum(
                    response[period] * predictors[period, candidate]
                    for period in range(train_start, validation_start)
                )
                squared_sum = sum(
                    predictors[period, candidate] ** 2 for period in range(train_start, validation_start)
                )
                train_slopes.append(cross_product / squared_sum if squared_sum else 0.0)

            validation_scores = []
            for candidate, slope in enumerate(train_slopes):
                if slope > 0:
                    score = sum(
                        response[period]
                        for period in range(validation_start, test_start)
                        if predictors[period, candidate] > 0
                    )
                elif slope < 0:
                    score = sum(
                        response[period]
                        for period in range(validation_start, test_start)
                        if predictors[period, candidate] < 0
                    )
                else:
                    score = 0.0
                validation_scores.append(score)
            winner = max(range(candidates), key=validation_scores.__getitem__)
            selected[path_index, fold] = winner

            cross_product = sum(
                response[period] * predictors[period, winner] for period in range(train_start, test_start)
            )
            squared_sum = sum(predictors[period, winner] ** 2 for period in range(train_start, test_start))
            slope = cross_product / squared_sum if squared_sum else 0.0
            refitted[path_index, fold] = slope

            payoff = 0.0
            for period in range(test_start, test_start + test_periods):
                feature = predictors[period, winner]
                if (slope > 0 and feature > 0) or (slope < 0 and feature < 0):
                    payoff += response[period]
            payoffs[path_index, fold] = payoff

    means = np.sum(payoffs, axis=1) / (fold_count * test_periods)
    return selected, refitted, payoffs, means


def test_walk_forward_matches_scalar_oracle_across_paths_folds_and_ties() -> None:
    predictors, response_paths = _selection_fixture()
    expected = _scalar_walk_forward(
        predictors,
        response_paths,
        train_periods=3,
        validation_periods=2,
        test_periods=2,
    )

    actual = evaluate_walk_forward(
        predictors,
        response_paths,
        train_periods=3,
        validation_periods=2,
        test_periods=2,
    )

    np.testing.assert_array_equal(actual.selected_models, expected[0])
    np.testing.assert_allclose(actual.refitted_slopes, expected[1])
    np.testing.assert_allclose(actual.fold_payoffs, expected[2])
    np.testing.assert_allclose(actual.mean_payoff, expected[3])
    np.testing.assert_array_equal(actual.selected_models[0], [1, 0])
    np.testing.assert_array_equal(actual.selected_models[1], [0, 0])
    np.testing.assert_array_equal(actual.refitted_slopes[1], [0.0, 0.0])
    np.testing.assert_array_equal(actual.fold_payoffs[1], [0.0, 0.0])


def test_future_return_changes_do_not_change_earlier_choice_or_refit() -> None:
    predictors = np.column_stack((np.ones(9), np.zeros(9)))
    original_returns = np.ones(9)
    changed_returns = original_returns.copy()
    changed_returns[5] = -3.0
    original = evaluate_walk_forward(
        predictors,
        original_returns[None, :],
        train_periods=3,
        validation_periods=2,
        test_periods=2,
    )
    changed = evaluate_walk_forward(
        predictors,
        changed_returns[None, :],
        train_periods=3,
        validation_periods=2,
        test_periods=2,
    )

    assert original.selected_models[0, 0] == changed.selected_models[0, 0] == 0
    assert original.refitted_slopes[0, 0] == changed.refitted_slopes[0, 0]
    assert original.fold_payoffs[0, 0] != changed.fold_payoffs[0, 0]


def test_randomization_refits_each_sign_path_and_counts_equal_payoffs(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    predictors, fixture_paths = _selection_fixture()
    observed = fixture_paths[0]
    bits = np.vstack((np.ones(9), np.zeros(9), np.arange(9) % 2, np.ones(9))).astype(np.int8)
    rng = FixedSignGenerator(bits)
    randomized_paths = (2 * bits - 1) * np.abs(observed)[None, :]
    all_paths = np.vstack((observed, randomized_paths))
    expected = _scalar_walk_forward(
        predictors,
        all_paths,
        train_periods=3,
        validation_periods=2,
        test_periods=2,
    )
    evaluated_paths: list[np.ndarray] = []
    original_evaluate = dynamic_selection.evaluate_walk_forward

    def record_paths(x: np.ndarray, paths: np.ndarray, **kwargs: int) -> dynamic_selection.WalkForwardPaths:
        evaluated_paths.append(np.array(paths, copy=True))
        return original_evaluate(x, paths, **kwargs)

    monkeypatch.setattr(dynamic_selection, "evaluate_walk_forward", record_paths)
    actual = run_dynamic_selection_test(
        predictors,
        observed,
        train_periods=3,
        validation_periods=2,
        test_periods=2,
        n_randomizations=bits.shape[0],
        rng=rng,  # type: ignore[arg-type]
    )
    observed_payoff = expected[3][0]
    exceedances = np.count_nonzero(expected[3][1:] >= observed_payoff)

    assert len(evaluated_paths) == 1
    np.testing.assert_array_equal(evaluated_paths[0], all_paths)
    assert actual.mean_payoff == pytest.approx(observed_payoff)
    assert actual.p_value == pytest.approx((1 + exceedances) / (1 + bits.shape[0]))
    assert actual.p_value >= 0.6
    assert actual.selected_models == tuple(int(value) for value in expected[0][0])
    assert actual.switch_count == sum(a != b for a, b in zip(actual.selected_models, actual.selected_models[1:]))


def test_zero_returns_produce_tied_randomization_payoffs_and_p_value_one() -> None:
    predictors, _ = _selection_fixture()
    result = run_dynamic_selection_test(
        predictors,
        np.zeros(9),
        train_periods=3,
        validation_periods=2,
        test_periods=2,
        n_randomizations=5,
        rng=np.random.default_rng(41),
    )

    assert result.mean_payoff == 0.0
    assert result.p_value == 1.0
    assert result.selected_models == (0, 0)
    assert result.switch_count == 0


@pytest.mark.parametrize(
    ("predictors", "paths"),
    [
        (np.ones(9), np.ones((1, 9))),
        (np.ones((9, 2)), np.ones(9)),
        (np.ones((9, 2)), np.ones((1, 8))),
        (np.full((9, 2), np.nan), np.ones((1, 9))),
        (np.ones((9, 2), dtype=np.complex128), np.ones((1, 9))),
        (np.ma.array(np.ones((9, 2)), mask=False), np.ma.array(np.ones((1, 9)), mask=[[False] * 8 + [True]])),
    ],
)
def test_walk_forward_rejects_invalid_or_incomplete_panels(predictors: np.ndarray, paths: np.ndarray) -> None:
    with pytest.raises(ValueError):
        evaluate_walk_forward(
            predictors,
            paths,
            train_periods=3,
            validation_periods=2,
            test_periods=2,
        )


@pytest.mark.parametrize(
    ("train_periods", "validation_periods", "test_periods"),
    [(0, 2, 2), (3, True, 2), (3, 2, 0), (3, 2, 2.0), (5, 3, 2)],
)
def test_walk_forward_rejects_invalid_windows_or_missing_complete_fold(
    train_periods: object, validation_periods: object, test_periods: object
) -> None:
    predictors, paths = _selection_fixture()
    with pytest.raises(ValueError):
        evaluate_walk_forward(
            predictors,
            paths,
            train_periods=train_periods,  # type: ignore[arg-type]
            validation_periods=validation_periods,  # type: ignore[arg-type]
            test_periods=test_periods,  # type: ignore[arg-type]
        )


@pytest.mark.parametrize("n_randomizations", [0, -1, True, 1.5])
def test_randomization_rejects_invalid_counts(n_randomizations: object) -> None:
    predictors, paths = _selection_fixture()
    with pytest.raises(ValueError, match="at least one randomization"):
        run_dynamic_selection_test(
            predictors,
            paths[0],
            train_periods=3,
            validation_periods=2,
            test_periods=2,
            n_randomizations=n_randomizations,  # type: ignore[arg-type]
            rng=np.random.default_rng(7),
        )


def test_dynamic_selection_smoke_calibration_is_repeatable_and_never_claims_formal_pass() -> None:
    first = calibrate_dynamic_selection.run_calibration(smoke=True)
    second = calibrate_dynamic_selection.run_calibration(smoke=True)

    assert first == second
    assert first["protocol"] == "plan-05-dynamic-selection-v1"
    assert first["seed"] == calibrate_dynamic_selection.SEED == 20261125
    assert first["profile"] == "smoke"
    assert first["status"] == "smoke"
    assert first["repetitions_per_scenario"] == 12
    assert first["randomizations_per_repetition"] == 19
    assert len(first["results"]) == len(calibrate_dynamic_selection.SCENARIOS) == 4
    for row in first["results"]:
        assert row["valid_repetitions"] + sum(row["failures"].values()) == 12
        assert row["rejection_rate"] is not None
        assert math.isfinite(row["rejection_rate"]["fraction"])
        assert math.isfinite(row["mean_oos_payoff"])
    json.dumps(first, allow_nan=False)
