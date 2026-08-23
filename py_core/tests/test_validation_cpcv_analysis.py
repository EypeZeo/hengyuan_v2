"""Tests for py_core.validation.cpcv_analysis (批次 2, round 2).

Reuses the SAME control strategies test_validation_walk_forward.py already defined and
validated for round 1 (_DeterministicNoEdgeStrategy, _PlantedEdgeStrategy,
_autocorrelated_prices) rather than redefining them -- both rounds' orchestrators ultimately
call the same _run_vectorized_backtest_on_df() per contiguous run, so the same engine-timing
reasoning documented there (why the exploitable sign of a planted edge depends on the
engine's execution timing, not just the data-generating process) applies unchanged here.

The gap-correctness fix cpcv_analysis.py's module docstring describes (never feeding a
row subset with an internal purge gap into the engine directly) is tested precisely and
directly against the private helpers that implement it (_contiguous_runs(),
_evaluate_on_index_subset()) rather than indirectly through an elaborate full-pipeline
scenario -- a hand-verified exact-value check of the fix itself is a stronger, more precise
regression guard than trying to construct a full CPCV run sensitive to the same bug.
"""

from __future__ import annotations

import math

import numpy as np
import pandas as pd
import pytest

from py_core.backtests.models import BacktestConfig
from py_core.backtests.vectorized_engine import records_to_dataframe
from py_core.tests.test_validation_walk_forward import (
    _autocorrelated_prices,
    _random_walk_prices,
    _records_from_prices,
)
from py_core.validation.cpcv_analysis import (
    CombinationResult,
    CpcvPathResult,
    _contiguous_runs,
    _evaluate_on_index_subset,
    run_cpcv_analysis,
)

_NO_EDGE_SPEC = "py_core.tests.test_validation_walk_forward:_DeterministicNoEdgeStrategy"
_PLANTED_EDGE_SPEC = "py_core.tests.test_validation_walk_forward:_PlantedEdgeStrategy"


def _sma_param_grid() -> list[dict[str, int]]:
    return [{"fast_window": f, "slow_window": s} for f in (5, 10, 20) for s in (30, 50) if f < s]


# ---------------------------------------------------------------------------
# The gap-correctness fix itself: direct, hand-verified tests of the two
# private helpers that implement it.
# ---------------------------------------------------------------------------


def test_contiguous_runs_splits_ascending_indices_at_every_non_unit_gap() -> None:
    runs = _contiguous_runs(np.array([0, 1, 2, 5, 6, 9], dtype=np.int64))
    assert [r.tolist() for r in runs] == [[0, 1, 2], [5, 6], [9]]


def test_contiguous_runs_single_contiguous_block_is_one_run() -> None:
    runs = _contiguous_runs(np.arange(10, dtype=np.int64))
    assert len(runs) == 1
    assert runs[0].tolist() == list(range(10))


def test_contiguous_runs_empty_input_is_no_runs() -> None:
    assert _contiguous_runs(np.array([], dtype=np.int64)) == []


def test_evaluate_on_index_subset_never_computes_a_spurious_cross_gap_return() -> None:
    """The exact scenario the module docstring's correctness argument is about: an extreme
    price spike sits ONLY in the rows a purge gap excludes from index_subset. If this helper
    fed the gappy subset directly into _run_vectorized_backtest_on_df() (positional
    .shift(-1), no notion of real elapsed time), the row just before the gap would compute
    its forward return against the row just after -- i.e. against the spike or across it --
    fabricating a huge return that never really happened at any true adjacent-bar pair.
    Splitting into contiguous runs first (what this helper actually does) means the spike
    rows, being entirely excluded from index_subset, never influence any computed return at
    all."""
    prices = [100.0, 101.0, 99.0, 102.0, 100_000.0, 100_000.0, 103.0, 101.0, 104.0, 102.0]
    records = _records_from_prices(np.array(prices))
    df_index = pd.DatetimeIndex([r.event_time_utc for r in records])
    df = records_to_dataframe(records)
    signal = pd.Series([1.0] * len(prices), index=df.index)
    config = BacktestConfig(initial_capital=10_000.0, annualization_factor=252.0)

    # Excludes indices 4, 5 -- the two spike rows -- exactly like a purge gap would.
    index_subset = np.array([0, 1, 2, 3, 6, 7, 8, 9], dtype=np.int64)
    result = _evaluate_on_index_subset(config, df, signal, index_subset, annualization_factor=252.0)

    assert len(result.returns) == len(index_subset)
    assert max(abs(r) for r in result.returns) < 0.1, (
        f"a return anywhere near the spike's magnitude leaked through the purge gap: {result.returns}"
    )
    # Run boundaries (see module docstring: an accepted, non-inflating artifact) --
    # the run [0,1,2,3]'s last row and the run [6,7,8,9]'s last row both trade flat/zero,
    # since neither has a true next bar within its own run.
    assert result.returns[3] == pytest.approx(0.0)
    assert result.returns[-1] == pytest.approx(0.0)
    assert df_index[0] == df.index[0]  # sanity: records_to_dataframe() preserved row order


def test_evaluate_on_index_subset_rejects_empty_subset() -> None:
    records = _records_from_prices(_random_walk_prices(20, seed=1))
    df = records_to_dataframe(records)
    signal = pd.Series([1.0] * len(df), index=df.index)
    config = BacktestConfig(initial_capital=10_000.0)
    with pytest.raises(ValueError, match="为空"):
        _evaluate_on_index_subset(
            config, df, signal, np.array([], dtype=np.int64), annualization_factor=252.0
        )


# ---------------------------------------------------------------------------
# Mechanics: combination/path counts, empty-input rejection, empty-train detection
# ---------------------------------------------------------------------------


def test_run_cpcv_analysis_produces_expected_combination_and_path_counts() -> None:
    records = _records_from_prices(_random_walk_prices(300, seed=1))
    config = BacktestConfig(initial_capital=100_000.0, fee_bps=5.0, slippage_bps=2.0)

    report = run_cpcv_analysis(
        config,
        records,
        "py_core.strategies.sma_crossover:SmaCrossoverStrategy",
        _sma_param_grid(),
        n_groups=5,
        n_test_groups=2,
    )
    assert len(report.combination_results) == math.comb(5, 2)
    assert len(report.path_results) == math.comb(4, 1)
    assert len(report.oos_sharpe_distribution) == len(report.path_results)
    assert all(isinstance(cr, CombinationResult) for cr in report.combination_results)
    assert all(isinstance(pr, CpcvPathResult) for pr in report.path_results)


def test_run_cpcv_analysis_rejects_empty_param_grid() -> None:
    records = _records_from_prices(_random_walk_prices(300, seed=1))
    config = BacktestConfig(initial_capital=100_000.0)
    with pytest.raises(ValueError, match="param_grid"):
        run_cpcv_analysis(
            config,
            records,
            "py_core.strategies.sma_crossover:SmaCrossoverStrategy",
            [],
            n_groups=4,
            n_test_groups=2,
        )


def test_combination_results_carry_actual_selected_params_from_the_grid() -> None:
    records = _records_from_prices(_random_walk_prices(300, seed=1))
    config = BacktestConfig(initial_capital=100_000.0, fee_bps=5.0, slippage_bps=2.0)
    grid = _sma_param_grid()

    report = run_cpcv_analysis(
        config,
        records,
        "py_core.strategies.sma_crossover:SmaCrossoverStrategy",
        grid,
        n_groups=4,
        n_test_groups=2,
    )
    for cr in report.combination_results:
        assert cr.selected_params in grid
        assert set(cr.test_metrics_by_group.keys()) == set(cr.test_groups)


def test_aggressive_purge_that_empties_a_combinations_train_set_raises_clear_error() -> None:
    records = _records_from_prices(_random_walk_prices(20, seed=1))
    config = BacktestConfig(initial_capital=100_000.0)
    with pytest.raises(ValueError, match="为空"):
        run_cpcv_analysis(
            config,
            records,
            "py_core.strategies.sma_crossover:SmaCrossoverStrategy",
            _sma_param_grid(),
            n_groups=4,
            n_test_groups=1,
            purge_bars=100,
        )


# ---------------------------------------------------------------------------
# Negative control: a strategy with a provably zero real-world edge must not
# show a systematically positive OOS Sharpe distribution.
# ---------------------------------------------------------------------------


def test_no_edge_strategy_does_not_show_systematic_oos_edge_across_many_seeds() -> None:
    config = BacktestConfig(initial_capital=100_000.0, fee_bps=5.0, slippage_bps=2.0)
    param_grid = [{"seed": s} for s in range(5)]

    mean_sharpes = []
    for price_seed in range(6):
        records = _records_from_prices(_random_walk_prices(300, seed=100 + price_seed))
        report = run_cpcv_analysis(
            config,
            records,
            _NO_EDGE_SPEC,
            param_grid,
            n_groups=5,
            n_test_groups=2,
            purge_bars=1,
            allow_external_strategy=True,
        )
        mean_sharpes.append(sum(report.oos_sharpe_distribution) / len(report.oos_sharpe_distribution))

    mean_of_means = sum(mean_sharpes) / len(mean_sharpes)
    assert mean_of_means < 0.3, (
        f"no-edge strategy showed a suspiciously positive mean CPCV OOS Sharpe: {mean_sharpes}"
    )


# ---------------------------------------------------------------------------
# Positive control: a strategy with a genuine, deterministically planted edge
# must show a clearly positive OOS Sharpe distribution.
# ---------------------------------------------------------------------------


def test_planted_edge_strategy_shows_clearly_positive_oos_edge() -> None:
    config = BacktestConfig(initial_capital=100_000.0, fee_bps=2.0, slippage_bps=1.0)
    param_grid = [{"lookback": lb} for lb in (1, 2, 3, 5)]

    mean_sharpes = []
    for price_seed in range(5):
        records = _records_from_prices(_autocorrelated_prices(400, seed=200 + price_seed))
        report = run_cpcv_analysis(
            config,
            records,
            _PLANTED_EDGE_SPEC,
            param_grid,
            n_groups=5,
            n_test_groups=2,
            purge_bars=1,
            allow_external_strategy=True,
        )
        mean_sharpes.append(sum(report.oos_sharpe_distribution) / len(report.oos_sharpe_distribution))

    mean_of_means = sum(mean_sharpes) / len(mean_sharpes)
    assert mean_of_means > 0.5, (
        f"a strategy exploiting a genuine, deterministically planted edge should show a "
        f"clearly positive mean CPCV OOS Sharpe -- got {mean_sharpes} (mean {mean_of_means:.3f})"
    )
