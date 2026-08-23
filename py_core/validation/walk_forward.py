"""Walk-forward analysis orchestrator — 批次 2, round 1.

Runs a parameter grid search on each fold's train window, freezes the best-scoring
parameters, evaluates them out-of-sample on that fold's test window, then stitches every
fold's OOS returns into one continuous OOS equity curve/metrics report -- the closest this
module gets to "what would have actually happened live", since at no point does a fold's
test evaluation use a parameter choice informed by data from that same fold's test window.

**Consumes ``walk_forward_splits()``-shaped splits exclusively, not
``purged_kfold_splits()``'s.** This is a deliberate scope boundary, not an oversight: this
repo's strategies compute signals via rolling-window technical indicators
(``py_core/strategies/base.py``), and a rolling window computed across a purged K-Fold
train set's *gaps* would silently average together bars that are not actually temporally
adjacent -- purged K-Fold's classic use case (Lopez de Prado's own) is i.i.d.-ish ML
samples evaluated independently, not a continuous rolling-window signal. Every fold's train
and test windows here are contiguous (enforced, not assumed -- see
``_require_contiguous()``); the ``purge_bars`` gap purged_kfold_splits() applies scattered
throughout a fold's train set only ever appears *between* walk-forward's train and test
blocks, where it doesn't break indicator continuity within either block.
"""

from __future__ import annotations

import math
from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime
from typing import Any

import numpy as np

from py_core.backtests.metrics import compute_metrics
from py_core.backtests.models import BacktestConfig, BacktestMetrics
from py_core.backtests.vectorized_engine import (
    _run_vectorized_backtest_on_df,
    records_to_dataframe,
    resolve_annualization_factor,
)
from py_core.manual_ohlcv import NormalizedOhlcvRecord
from py_core.strategies.base import load_strategy
from py_core.validation.splits import Split


def _default_selection_metric(metrics: BacktestMetrics) -> float:
    return metrics.sharpe_ratio


def _require_contiguous(indices: np.ndarray, label: str) -> None:
    if len(indices) == 0:
        raise ValueError(f"{label}_indices is empty -- every fold needs both a train and a test window")
    if not np.all(np.diff(indices) == 1):
        raise ValueError(
            f"{label}_indices is not contiguous ({indices[:5]}...) -- run_walk_forward_analysis() "
            "only accepts walk_forward_splits()-shaped folds, not purged_kfold_splits()'s "
            "(see module docstring for why the two are not interchangeable here)"
        )


@dataclass(frozen=True, slots=True)
class FoldResult:
    """One fold's outcome: which parameters won the train-window grid search, how they
    scored in-sample, and how that frozen choice performed out-of-sample on test."""

    train_indices: np.ndarray
    test_indices: np.ndarray
    selected_params: dict[str, Any]
    train_score: float
    test_metrics: BacktestMetrics


@dataclass(frozen=True, slots=True)
class WalkForwardReport:
    fold_results: tuple[FoldResult, ...]
    oos_timestamps: list[datetime]
    oos_returns: list[float]
    oos_equity_curve: list[float]
    oos_positions: list[float]
    oos_metrics: BacktestMetrics


def run_walk_forward_analysis(
    config: BacktestConfig,
    records: list[NormalizedOhlcvRecord],
    strategy_spec: str,
    param_grid: list[dict[str, Any]],
    splits: list[Split],
    *,
    selection_metric: Callable[[BacktestMetrics], float] = _default_selection_metric,
    allow_external_strategy: bool = False,
) -> WalkForwardReport:
    """For each split: grid-search ``param_grid`` on the train window (scored by
    ``selection_metric`` applied to each candidate's train-window ``BacktestMetrics``,
    default Sharpe), freeze the winner, evaluate it out-of-sample on the test window, and
    stitch every fold's OOS returns into one continuous curve.

    The frozen winner's OOS signal is generated over the *combined* train+test window (real
    indicator warm-up context, not a cold-started test window) and only then sliced down to
    the test rows for evaluation -- avoiding an artificial warm-up gap at every fold
    boundary. This does cost each fold's first test row its correct position (the
    next-bar-open engine's own ``shift(1)`` has nothing before the sliced series' first row
    to shift from, so it trades flat there regardless of the true prior-bar signal) --  a
    small, well-understood, one-bar-per-fold boundary artifact, not a lookahead violation
    and not something that inflates results (it can only suppress a trade the strategy would
    have made, never fabricate one from future information).

    Raises:
        ValueError: param_grid or splits is empty, or a split's train/test indices are not
            contiguous (see module docstring).
    """
    if not param_grid:
        raise ValueError("param_grid 不能为空")
    if not splits:
        raise ValueError("splits 不能为空")

    df = records_to_dataframe(records)
    annualization_factor = resolve_annualization_factor(config, records)

    fold_results: list[FoldResult] = []
    oos_timestamps: list[datetime] = []
    oos_returns: list[float] = []
    oos_positions: list[float] = []
    total_cost = 0.0

    for split in splits:
        _require_contiguous(split.train_indices, "train")
        _require_contiguous(split.test_indices, "test")

        train_df = df.iloc[split.train_indices[0] : split.train_indices[-1] + 1]

        best_params: dict[str, Any] | None = None
        best_score = -math.inf
        for params in param_grid:
            strategy = load_strategy(strategy_spec, allow_external=allow_external_strategy, **params)
            train_signal = strategy.generate_signals(train_df)
            train_result = _run_vectorized_backtest_on_df(
                config, train_df, train_signal, annualization_factor=annualization_factor
            )
            score = selection_metric(train_result.metrics)
            if best_params is None or score > best_score:
                best_score = score
                best_params = params

        assert best_params is not None  # param_grid is non-empty, so a winner always exists

        combined_df = df.iloc[split.train_indices[0] : split.test_indices[-1] + 1]
        test_df = df.iloc[split.test_indices[0] : split.test_indices[-1] + 1]
        frozen_strategy = load_strategy(strategy_spec, allow_external=allow_external_strategy, **best_params)
        combined_signal = frozen_strategy.generate_signals(combined_df)
        test_signal = combined_signal.reindex(test_df.index)

        test_result = _run_vectorized_backtest_on_df(
            config, test_df, test_signal, annualization_factor=annualization_factor
        )

        fold_results.append(
            FoldResult(
                train_indices=split.train_indices,
                test_indices=split.test_indices,
                selected_params=best_params,
                train_score=best_score,
                test_metrics=test_result.metrics,
            )
        )
        oos_timestamps.extend(test_result.timestamps)
        oos_returns.extend(test_result.returns)
        oos_positions.extend(test_result.positions)
        total_cost += test_result.metrics.cost_impact_total

    equity = config.initial_capital
    oos_equity_curve: list[float] = []
    for r in oos_returns:
        equity *= 1.0 + r
        oos_equity_curve.append(equity)

    oos_metrics = compute_metrics(
        equity_curve=oos_equity_curve,
        returns=oos_returns,
        positions=oos_positions,
        cost_impact_total=total_cost,
        initial_capital=config.initial_capital,
        annualization_factor=annualization_factor,
        risk_free_rate=config.risk_free_rate,
    )

    return WalkForwardReport(
        fold_results=tuple(fold_results),
        oos_timestamps=oos_timestamps,
        oos_returns=oos_returns,
        oos_equity_curve=oos_equity_curve,
        oos_positions=oos_positions,
        oos_metrics=oos_metrics,
    )
