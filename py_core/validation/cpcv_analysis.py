"""CPCV orchestrator — 批次 2, round 2.

**Why signal generation happens ONCE per candidate parameter set, over the FULL dataset,
never per-combination on a stitched train slice.** A CPCV combination's train set is a
union of possibly several disjoint blocks (see ``cpcv.py``'s module docstring) -- exactly
the same "a rolling-window indicator can't be safely computed across a purge gap" problem
``walk_forward.py``'s own module docstring already rejected ``purged_kfold_splits()`` for.
Unlike a live/streaming walk-forward though, CPCV's whole point is that the complete
dataset is available upfront -- so instead of ever feeding a gappy slice into
``strategy.generate_signals()``, each candidate parameter set's signal is computed exactly
once over the ENTIRE contiguous dataset (a technical indicator is a fixed, causal,
no-lookahead formula -- computing it over more of the series than any one combination needs
is not itself a leakage risk; only USING future rows' signal value to score PAST rows would
be) and that one signal series is then *sliced* (never recomputed) to score whichever row
subset a given combination's train or test set calls for. This is also a large efficiency
win: ``n_params`` signal computations total, not ``n_params * C(n_groups, n_test_groups)``.

**Why train/test scoring never feeds a gappy row subset into ``_run_vectorized_backtest_on_df()``
directly.** That function's forward-return computation (``open[T+1] / open[T] - 1``) is
computed via ``.shift(-1)`` on WHATEVER ROWS ARE PASSED IN, positionally -- it has no notion
of the underlying timestamps' real spacing. Passed a subset with an internal gap (e.g. a
CPCV combination's train set, which purges out excluded blocks from the middle), it would
silently compute a bar's forward return using the row that happens to be NEXT IN THE ARRAY
as if it were the true next bar in time, even though a purged block sits between them in
real time -- fabricating a return that never happened. ``_evaluate_on_index_subset()``
avoids this by splitting any row subset into maximal contiguous runs first, backtesting
each run independently (so a forward return is only ever computed between two rows that
really are adjacent in time), then concatenating each run's own returns/positions/timestamps
and computing ONE combined ``BacktestMetrics`` over the concatenation --
``compute_metrics()`` itself has no cross-bar adjacency assumption (it only aggregates a
list of already-correct per-bar values), so concatenating post-hoc is safe even though
feeding the gappy slice pre-hoc into the engine would not have been. This mirrors, and is
provably necessary for the same reason as, how ``walk_forward.py`` already stitches separate
folds' OOS results together rather than ever re-running the engine on a concatenated
multi-fold dataframe.

One accepted, unavoidable, and non-inflating consequence: every contiguous run's own first
bar trades flat (no prior signal within that run to shift from) and last bar always reports
a zero forward return (no next bar within that run) -- the exact same one-bar-per-boundary
artifact ``walk_forward.py``'s own docstring already documents and accepts for fold
boundaries, just potentially several times per combination here instead of once per fold.

**Why an empty train subset is a hard error, not a silently-produced garbage metric.**
``splits.py``'s own docstring already flags that an aggressive ``purge_bars``/``embargo_bars``
choice can legitimately empty a fold's train set, and says the walk-forward-style
orchestrator consuming it "will need to detect and refuse rather than silently proceed" --
``_evaluate_on_index_subset()`` does exactly that (raises ``ValueError`` on an empty subset)
for the same reason here.
"""

from __future__ import annotations

import math
from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime
from typing import Any

import numpy as np
import pandas as pd

from py_core.backtests.metrics import compute_metrics
from py_core.backtests.models import BacktestConfig, BacktestMetrics
from py_core.backtests.vectorized_engine import (
    _run_vectorized_backtest_on_df,
    records_to_dataframe,
    resolve_annualization_factor,
)
from py_core.manual_ohlcv import NormalizedOhlcvRecord
from py_core.strategies.base import checked_signals, load_strategy
from py_core.validation.cpcv import cpcv_combinations, cpcv_paths


def _default_selection_metric(metrics: BacktestMetrics) -> float:
    return metrics.sharpe_ratio


@dataclass(frozen=True, slots=True)
class _SubsetEvalResult:
    metrics: BacktestMetrics
    timestamps: list[datetime]
    returns: list[float]
    positions: list[float]


def _contiguous_runs(indices: np.ndarray) -> list[np.ndarray]:
    """Split ascending positional indices into maximal contiguous runs, e.g.
    ``[0,1,2,5,6,9] -> [[0,1,2],[5,6],[9]]``."""
    if len(indices) == 0:
        return []
    breaks = np.where(np.diff(indices) != 1)[0] + 1
    return np.split(indices, breaks)


def _evaluate_on_index_subset(
    config: BacktestConfig,
    df: pd.DataFrame,
    signal: pd.Series[Any],
    index_subset: np.ndarray,
    *,
    annualization_factor: float,
) -> _SubsetEvalResult:
    """See module docstring for why this splits into contiguous runs rather than feeding
    ``index_subset`` into ``_run_vectorized_backtest_on_df()`` directly.

    Raises:
        ValueError: index_subset is empty.
    """
    runs = _contiguous_runs(index_subset)
    if not runs:
        raise ValueError(
            "index_subset 为空，无法评估 -- 可能是 purge_bars/embargo_bars 设置过于激进，"
            "把某个 combination 的整个 train（或 test）集合清空了"
        )

    all_timestamps: list[datetime] = []
    all_returns: list[float] = []
    all_positions: list[float] = []
    total_cost = 0.0
    for run in runs:
        run_df = df.iloc[run]
        run_signal = signal.reindex(run_df.index)
        result = _run_vectorized_backtest_on_df(
            config, run_df, run_signal, annualization_factor=annualization_factor
        )
        all_timestamps.extend(result.timestamps)
        all_returns.extend(result.returns)
        all_positions.extend(result.positions)
        total_cost += result.metrics.cost_impact_total

    equity = config.initial_capital
    equity_curve: list[float] = []
    for r in all_returns:
        equity *= 1.0 + r
        equity_curve.append(equity)

    metrics = compute_metrics(
        equity_curve=equity_curve,
        returns=all_returns,
        positions=all_positions,
        cost_impact_total=total_cost,
        initial_capital=config.initial_capital,
        annualization_factor=annualization_factor,
        risk_free_rate=config.risk_free_rate,
    )
    return _SubsetEvalResult(
        metrics=metrics, timestamps=all_timestamps, returns=all_returns, positions=all_positions
    )


@dataclass(frozen=True, slots=True)
class CombinationResult:
    """One combination's outcome: which parameters won its train-set grid search, and how
    that frozen choice scored on each of the combination's test groups."""

    combination_index: int
    test_groups: tuple[int, ...]
    selected_params: dict[str, Any]
    train_score: float
    test_metrics_by_group: dict[int, BacktestMetrics]


@dataclass(frozen=True, slots=True)
class CpcvPathResult:
    """One reconstructed full-length OOS path (see ``cpcv.py``'s ``cpcv_paths()``)."""

    path_index: int
    oos_timestamps: list[datetime]
    oos_returns: list[float]
    oos_equity_curve: list[float]
    oos_positions: list[float]
    oos_metrics: BacktestMetrics


@dataclass(frozen=True, slots=True)
class CpcvReport:
    combination_results: tuple[CombinationResult, ...]
    path_results: tuple[CpcvPathResult, ...]
    oos_sharpe_distribution: tuple[float, ...]


def run_cpcv_analysis(
    config: BacktestConfig,
    records: list[NormalizedOhlcvRecord],
    strategy_spec: str,
    param_grid: list[dict[str, Any]],
    *,
    n_groups: int,
    n_test_groups: int,
    purge_bars: int = 0,
    embargo_bars: int = 0,
    selection_metric: Callable[[BacktestMetrics], float] = _default_selection_metric,
    allow_external_strategy: bool = False,
) -> CpcvReport:
    """Grid-search-then-freeze-then-OOS-evaluate, generalized from ``walk_forward.py``'s
    single chronological path to CPCV's combinatorial reconstruction of many OOS paths (see
    ``cpcv.py``'s module docstring for why this exists and how path reconstruction works).

    Raises:
        ValueError: param_grid empty, or any of cpcv_combinations()/cpcv_paths()'s own
            validation (n_groups/n_test_groups/purge_bars/embargo_bars/n_samples), or a
            combination's train (or test) subset ends up empty (see module docstring).
    """
    if not param_grid:
        raise ValueError("param_grid 不能为空")

    df = records_to_dataframe(records)
    n_samples = len(df)
    annualization_factor = resolve_annualization_factor(config, records)

    combinations = cpcv_combinations(
        n_samples, n_groups, n_test_groups, purge_bars=purge_bars, embargo_bars=embargo_bars
    )
    paths = cpcv_paths(n_groups, n_test_groups)

    # See module docstring: one signal per candidate param set, over the whole dataset, ever.
    signals_by_param_index: list[pd.Series[Any]] = []
    for params in param_grid:
        strategy = load_strategy(strategy_spec, allow_external=allow_external_strategy, **params)
        signal, _ = checked_signals(strategy, df)  # SAFE-05：每组参数在全量数据上过一次因果门禁
        signals_by_param_index.append(signal)

    combination_results: list[CombinationResult] = []
    test_result_by_combo_and_group: dict[tuple[int, int], _SubsetEvalResult] = {}

    for combo in combinations:
        best_params_index: int | None = None
        best_score = -math.inf
        for pi, signal in enumerate(signals_by_param_index):
            train_eval = _evaluate_on_index_subset(
                config, df, signal, combo.train_indices, annualization_factor=annualization_factor
            )
            score = selection_metric(train_eval.metrics)
            if best_params_index is None or score > best_score:
                best_score = score
                best_params_index = pi

        assert best_params_index is not None  # param_grid is non-empty, so a winner always exists
        winning_signal = signals_by_param_index[best_params_index]

        test_metrics_by_group: dict[int, BacktestMetrics] = {}
        for g, test_idx in combo.test_indices_by_group.items():
            test_eval = _evaluate_on_index_subset(
                config, df, winning_signal, test_idx, annualization_factor=annualization_factor
            )
            test_metrics_by_group[g] = test_eval.metrics
            test_result_by_combo_and_group[(combo.combination_index, g)] = test_eval

        combination_results.append(
            CombinationResult(
                combination_index=combo.combination_index,
                test_groups=combo.test_groups,
                selected_params=param_grid[best_params_index],
                train_score=best_score,
                test_metrics_by_group=test_metrics_by_group,
            )
        )

    path_results: list[CpcvPathResult] = []
    for path in paths:
        oos_timestamps: list[datetime] = []
        oos_returns: list[float] = []
        oos_positions: list[float] = []
        total_cost = 0.0
        for g in range(n_groups):
            combo_index = path.combination_for_group[g]
            group_result = test_result_by_combo_and_group[(combo_index, g)]
            oos_timestamps.extend(group_result.timestamps)
            oos_returns.extend(group_result.returns)
            oos_positions.extend(group_result.positions)
            total_cost += group_result.metrics.cost_impact_total

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
        path_results.append(
            CpcvPathResult(
                path_index=path.path_index,
                oos_timestamps=oos_timestamps,
                oos_returns=oos_returns,
                oos_equity_curve=oos_equity_curve,
                oos_positions=oos_positions,
                oos_metrics=oos_metrics,
            )
        )

    return CpcvReport(
        combination_results=tuple(combination_results),
        path_results=tuple(path_results),
        oos_sharpe_distribution=tuple(pr.oos_metrics.sharpe_ratio for pr in path_results),
    )
