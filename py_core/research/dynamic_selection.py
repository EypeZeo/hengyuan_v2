"""Chronological model refitting and conditional sign randomization."""

from __future__ import annotations

from dataclasses import dataclass
from itertools import pairwise

import numpy as np


@dataclass(frozen=True, slots=True)
class WalkForwardPaths:
    selected_models: np.ndarray
    refitted_slopes: np.ndarray
    fold_payoffs: np.ndarray
    mean_payoff: np.ndarray


@dataclass(frozen=True, slots=True)
class DynamicSelectionTest:
    mean_payoff: float
    p_value: float
    selected_models: tuple[int, ...]
    switch_count: int


def _real_values(values: np.ndarray) -> np.ndarray:
    if np.ma.is_masked(values):
        raise ValueError("predictors and returns must be complete")
    panel = np.asarray(values)
    if panel.dtype.kind not in "biuf":
        raise ValueError("predictors and returns must be real numbers")
    with np.errstate(over="ignore", invalid="ignore"):
        panel = np.asarray(panel, dtype=np.float64)
    if not np.all(np.isfinite(panel)):
        raise ValueError("predictors and returns must be finite")
    return panel


def _slopes(cross_product: np.ndarray, squared_sum: np.ndarray) -> np.ndarray:
    if not np.all(np.isfinite(cross_product)) or not np.all(np.isfinite(squared_sum)):
        raise ValueError("regression products must be finite")
    slopes = np.divide(cross_product, squared_sum, out=np.zeros_like(cross_product), where=squared_sum > 0)
    if not np.all(np.isfinite(slopes)):
        raise ValueError("regression slopes must be finite")
    return slopes


def evaluate_walk_forward(
    predictors: np.ndarray,
    response_paths: np.ndarray,
    *,
    train_periods: int,
    validation_periods: int,
    test_periods: int,
) -> WalkForwardPaths:
    """Fit, select, refit, and evaluate each path with complete rolling test folds.

    Predictors have shape (periods, candidates) and are available before the
    matching return. Responses have shape (paths, periods). Each test fold uses
    only the preceding training and validation periods for its choice and fit.
    """
    x = _real_values(predictors)
    y = _real_values(response_paths)
    if x.ndim != 2 or x.shape[1] == 0 or y.ndim != 2 or y.shape[0] == 0 or y.shape[1] != x.shape[0]:
        raise ValueError("predictors and response paths must share a complete period axis")
    windows = (train_periods, validation_periods, test_periods)
    if any(isinstance(size, bool) or not isinstance(size, (int, np.integer)) or size < 1 for size in windows):
        raise ValueError("window lengths must be positive integers")
    pretest_periods = train_periods + validation_periods
    folds = (x.shape[0] - pretest_periods) // test_periods
    if folds < 1:
        raise ValueError("at least one complete test fold is required")

    selected = np.empty((y.shape[0], folds), dtype=np.intp)
    refitted = np.empty((y.shape[0], folds))
    payoffs = np.empty((y.shape[0], folds))
    positive = (x > 0).astype(np.float64)
    negative = (x < 0).astype(np.float64)
    with np.errstate(over="ignore", invalid="ignore", divide="ignore", under="ignore"):
        for fold in range(folds):
            test_start = pretest_periods + fold * test_periods
            validation_start = test_start - validation_periods
            train = slice(test_start - pretest_periods, validation_start)
            validation = slice(validation_start, test_start)
            refit = slice(train.start, test_start)
            test = slice(test_start, test_start + test_periods)

            train_slopes = _slopes(y[:, train] @ x[train], np.sum(x[train] ** 2, axis=0))
            up = y[:, validation] @ positive[validation]
            down = y[:, validation] @ negative[validation]
            scores = np.where(train_slopes > 0, up, np.where(train_slopes < 0, down, 0.0))
            if not np.all(np.isfinite(scores)):
                raise ValueError("validation payoffs must be finite")
            winner = np.argmax(scores, axis=1)
            selected[:, fold] = winner

            chosen = x[refit][:, winner].T
            slope = _slopes(np.sum(y[:, refit] * chosen, axis=1), np.sum(chosen**2, axis=1))
            refitted[:, fold] = slope
            test_x = x[test][:, winner].T
            hold = ((slope[:, None] > 0) & (test_x > 0)) | ((slope[:, None] < 0) & (test_x < 0))
            payoffs[:, fold] = np.sum(np.where(hold, y[:, test], 0.0), axis=1)
        mean_payoff = np.sum(payoffs, axis=1) / (folds * test_periods)
    if not np.all(np.isfinite(mean_payoff)):
        raise ValueError("test payoffs must be finite")
    return WalkForwardPaths(selected, refitted, payoffs, mean_payoff)


def run_dynamic_selection_test(
    predictors: np.ndarray,
    returns: np.ndarray,
    *,
    train_periods: int,
    validation_periods: int,
    test_periods: int,
    n_randomizations: int,
    rng: np.random.Generator,
) -> DynamicSelectionTest:
    """Re-run the selection algorithm under the conditional independent-sign null."""
    observed = _real_values(returns)
    if observed.ndim != 1:
        raise ValueError("observed returns must be a vector")
    if (
        isinstance(n_randomizations, bool)
        or not isinstance(n_randomizations, (int, np.integer))
        or n_randomizations < 1
    ):
        raise ValueError("at least one randomization is required")
    signs = 2 * rng.integers(0, 2, size=(n_randomizations, observed.size), dtype=np.int8) - 1
    paths = np.empty((1 + n_randomizations, observed.size))
    paths[0] = observed
    paths[1:] = signs * np.abs(observed)
    evaluated = evaluate_walk_forward(
        predictors, paths, train_periods=train_periods, validation_periods=validation_periods, test_periods=test_periods
    )
    mean_payoff = float(evaluated.mean_payoff[0])
    exceedances = int(np.count_nonzero(evaluated.mean_payoff[1:] >= mean_payoff))
    selected = tuple(int(value) for value in evaluated.selected_models[0])
    switches = sum(a != b for a, b in pairwise(selected))
    return DynamicSelectionTest(mean_payoff, (1 + exceedances) / (1 + n_randomizations), selected, switches)
