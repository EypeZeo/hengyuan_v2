"""Probability of Backtest Overfitting (PBO) via CSCV — 批次 2, round 4.

Bailey, Borwein, Lopez de Prado & Zhu (2015), "The Probability of Backtest Overfitting":
given a POOL of N candidate strategy/parameter configurations searched over the same data,
how likely is it that the one variant an in-sample grid search picks as "best" is actually
mediocre or bad out-of-sample -- i.e. that the SELECTION PROCEDURE ITSELF, not any genuine
edge, produced the reported result? This is a different question from round 3's Deflated
Sharpe Ratio (which asks "is this one observed Sharpe too good to be luck, given how many
trials were run") -- PBO instead asks "does picking-the-in-sample-winner, as a *procedure*,
reliably find genuinely good performers, or does it just as often find noise that happened
to look good on this particular historical split".

**Uses Combinatorially Symmetric Cross-Validation (CSCV)**: partition the data into
``n_groups`` equal contiguous blocks (``n_groups`` must be even -- CSCV's own defining
requirement, hence "symmetric"), and for every way of choosing exactly HALF of them as the
"test" half (``cpcv_combinations()`` from round 2's ``cpcv.py``, with
``n_test_groups = n_groups // 2``):

1. Evaluate every ``param_grid`` candidate's performance metric on BOTH the train half and
   the test half (reusing ``cpcv_analysis.py``'s gap-safe ``_evaluate_on_index_subset()`` --
   see that module's docstring for why a non-contiguous train/test half can't be fed to the
   engine directly, and why each candidate's signal is generated once over the whole
   contiguous dataset rather than per-combination on a stitched slice; identical reasoning
   applies unchanged here, this module reuses those private helpers rather than
   re-implementing the same fix a third time).
2. Pick the train-half winner (the candidate an in-sample search would have selected).
3. Find that SAME winner's relative rank among ALL candidates' TEST-half performance
   (midrank for ties -- ``_relative_rank()``) -- ``omega_c`` in (0, 1). ``omega_c > 0.5``
   means the in-sample winner also did relatively well out-of-sample (no red flag);
   ``omega_c <= 0.5`` means it did relatively POORLY out-of-sample despite looking best
   in-sample (an overfitting red flag).
4. ``logit_c = ln(omega_c / (1 - omega_c))``.

This module deliberately reuses ``cpcv_combinations()`` but NOT ``cpcv_paths()`` -- PBO
evaluates each combination's train/test halves independently and ranks within them; it has
no use for round 2's path-reconstruction machinery, which exists to stitch combinations into
continuous OOS equity curves, a different purpose CSCV doesn't need.

**PBO = the fraction of combinations where ``logit_c <= 0``** -- literally "how often does
the in-sample-best choice turn out to be at-or-below-median out-of-sample". Close to 0: the
selection procedure reliably picks genuinely good performers. Close to 1: the selection
procedure is essentially picking noise dressed up as the in-sample winner.

**A finding worth recording before trusting a test suite around this: PBO for a pool of
purely exchangeable/symmetric noise candidates is NOT reliably above 0.5.** The intuitive
expectation going into this round was "a pool where every candidate has zero genuine edge
should show elevated (>0.5) PBO" -- checked directly via a pool of
``_DeterministicNoEdgeStrategy`` instances (differing only by an arbitrary hash seed, hence
genuinely interchangeable) at pool sizes 10/30/60, averaged over 6 independent price series
each: the mean landed at ~0.39-0.45 in every case, never reliably crossing 0.5, and did not
trend upward with a larger pool (more "hunting" did not, on its own, push PBO higher for
*symmetric* noise). This makes sense on reflection: PBO detects whether the selection
procedure exploits sample-specific structure that fails to generalize -- for candidates that
are truly interchangeable, the relationship between in-sample rank and out-of-sample rank is
close to *independent*, not systematically inverted, so PBO centers near (but somewhat below,
in this specific setup) 0.5 rather than climbing toward 1. What DOES reliably and robustly
separate "good selection process" from "one degree away from picking noise" is a RELATIVE
comparison against a pool with genuine, stable structure (a real edge shows dramatically
LOWER PBO, ~0.12 vs the noise pool's ~0.40 in the same experiment) -- the test suite is built
around that paired comparison, mirroring round 1's own positive-vs-no-edge paired-comparison
discipline, rather than an absolute "noise must exceed 0.5" claim that the evidence does not
actually support.
"""

from __future__ import annotations

import math
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

import numpy as np

from py_core.backtests.models import BacktestConfig, BacktestMetrics
from py_core.backtests.vectorized_engine import (
    records_to_dataframe,
    resolve_annualization_factor,
)
from py_core.manual_ohlcv import NormalizedOhlcvRecord
from py_core.strategies.base import load_strategy
from py_core.validation.cpcv import cpcv_combinations
from py_core.validation.cpcv_analysis import _evaluate_on_index_subset


def _default_selection_metric(metrics: BacktestMetrics) -> float:
    return metrics.sharpe_ratio


def _relative_rank(value: float, pool: np.ndarray) -> float:
    """Midrank-based relative rank of ``value`` within ``pool`` (which must include
    ``value`` itself), mapped to (0, 1): ``rank = less + (equal + 1) / 2`` (average rank of
    the tied group, 1-indexed), ``omega = rank / (n + 1)``. Never exactly 0 or 1 for any
    valid input -- a genuine mathematical property of this formula (``rank`` always lies in
    ``[1, n]``), not a guarded edge case.
    """
    less = int(np.sum(pool < value))
    equal = int(np.sum(pool == value))
    n = len(pool)
    rank = less + (equal + 1) / 2.0
    return rank / (n + 1)


@dataclass(frozen=True, slots=True)
class PboCombinationResult:
    """One CSCV combination's outcome: which candidate the train half would have selected,
    and where that same candidate ranked on the test half among the full pool."""

    combination_index: int
    test_groups: tuple[int, ...]
    in_sample_winner_index: int
    in_sample_winner_params: dict[str, Any]
    train_scores: tuple[float, ...]
    test_scores: tuple[float, ...]
    relative_rank: float
    logit: float


@dataclass(frozen=True, slots=True)
class PboReport:
    combination_results: tuple[PboCombinationResult, ...]
    probability_of_backtest_overfitting: float
    logit_distribution: tuple[float, ...]


def compute_pbo(
    config: BacktestConfig,
    records: list[NormalizedOhlcvRecord],
    strategy_spec: str,
    param_grid: list[dict[str, Any]],
    *,
    n_groups: int,
    purge_bars: int = 0,
    embargo_bars: int = 0,
    selection_metric: Callable[[BacktestMetrics], float] = _default_selection_metric,
    allow_external_strategy: bool = False,
) -> PboReport:
    """See module docstring for the CSCV algorithm and what PBO means.

    Raises:
        ValueError: n_groups is odd (CSCV requires an exact train/test half split),
            param_grid has fewer than 2 candidates (a relative rank is meaningless with
            only one), or any of cpcv_combinations()'s own validation
            (n_groups/purge_bars/embargo_bars/n_samples) or
            _evaluate_on_index_subset()'s (an empty train or test half).
    """
    if n_groups % 2 != 0:
        raise ValueError(
            f"n_groups must be even for CSCV's symmetric train/test half split, got {n_groups}"
        )
    if len(param_grid) < 2:
        raise ValueError(f"param_grid 至少需要 2 个候选才能计算相对排名，收到 {len(param_grid)} 个")

    df = records_to_dataframe(records)
    n_samples = len(df)
    annualization_factor = resolve_annualization_factor(config, records)
    n_test_groups = n_groups // 2

    combinations = cpcv_combinations(
        n_samples, n_groups, n_test_groups, purge_bars=purge_bars, embargo_bars=embargo_bars
    )

    # One signal per candidate, over the whole dataset, ever -- see cpcv_analysis.py's
    # module docstring for why (identical reasoning: a combination's train/test half is a
    # non-contiguous union of blocks, so an indicator can't be safely recomputed on it).
    signals_by_param_index = []
    for params in param_grid:
        strategy = load_strategy(strategy_spec, allow_external=allow_external_strategy, **params)
        signals_by_param_index.append(strategy.generate_signals(df))

    combination_results: list[PboCombinationResult] = []
    for combo in combinations:
        combined_test_indices = np.sort(
            np.concatenate([combo.test_indices_by_group[g] for g in combo.test_groups])
        )

        train_scores: list[float] = []
        test_scores: list[float] = []
        for signal in signals_by_param_index:
            train_eval = _evaluate_on_index_subset(
                config, df, signal, combo.train_indices, annualization_factor=annualization_factor
            )
            test_eval = _evaluate_on_index_subset(
                config, df, signal, combined_test_indices, annualization_factor=annualization_factor
            )
            train_scores.append(selection_metric(train_eval.metrics))
            test_scores.append(selection_metric(test_eval.metrics))

        test_scores_arr = np.array(test_scores)
        winner_index = int(np.argmax(train_scores))
        omega = _relative_rank(test_scores_arr[winner_index], test_scores_arr)
        logit = math.log(omega / (1.0 - omega))

        combination_results.append(
            PboCombinationResult(
                combination_index=combo.combination_index,
                test_groups=combo.test_groups,
                in_sample_winner_index=winner_index,
                in_sample_winner_params=param_grid[winner_index],
                train_scores=tuple(train_scores),
                test_scores=tuple(test_scores),
                relative_rank=omega,
                logit=logit,
            )
        )

    logits = tuple(cr.logit for cr in combination_results)
    pbo = sum(1 for logit in logits if logit <= 0.0) / len(logits)

    return PboReport(
        combination_results=tuple(combination_results),
        probability_of_backtest_overfitting=pbo,
        logit_distribution=logits,
    )
