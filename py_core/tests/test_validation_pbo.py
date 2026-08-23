"""Tests for py_core.validation.pbo (批次 2, round 4).

The behavioral control at the bottom is a RELATIVE (paired) comparison, not an absolute
threshold on the noise pool alone -- see pbo.py's module docstring for why: design-time
experimentation showed a pool of purely exchangeable/symmetric noise candidates does NOT
reliably push PBO above 0.5 (it landed at ~0.39-0.45 across several pool sizes), so asserting
that directly would have been asserting something the evidence doesn't actually support.
What IS robust, checked directly before writing this test, is that a pool with genuine,
stable structure shows a clearly LOWER PBO than a same-harness noise pool -- the same
paired-comparison discipline round 1's walk-forward controls already established.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

from py_core.backtests.models import BacktestConfig
from py_core.tests.test_validation_walk_forward import (
    _autocorrelated_prices,
    _random_walk_prices,
    _records_from_prices,
)
from py_core.validation.pbo import PboCombinationResult, _relative_rank, compute_pbo

_NO_EDGE_SPEC = "py_core.tests.test_validation_walk_forward:_DeterministicNoEdgeStrategy"
_PLANTED_EDGE_SPEC = "py_core.tests.test_validation_walk_forward:_PlantedEdgeStrategy"


def _sma_param_grid() -> list[dict[str, int]]:
    return [{"fast_window": f, "slow_window": s} for f in (5, 10, 20) for s in (30, 50) if f < s]


# ---------------------------------------------------------------------------
# _relative_rank(): hand-verified midrank formula
# ---------------------------------------------------------------------------


def test_relative_rank_hand_verified_no_ties() -> None:
    pool = np.array([1.0, 2.0, 3.0, 4.0, 5.0])
    assert _relative_rank(3.0, pool) == pytest.approx(0.5)
    assert _relative_rank(5.0, pool) == pytest.approx(5.0 / 6.0)
    assert _relative_rank(1.0, pool) == pytest.approx(1.0 / 6.0)


def test_relative_rank_hand_verified_with_ties() -> None:
    pool = np.array([1.0, 1.0, 1.0, 2.0])
    assert _relative_rank(1.0, pool) == pytest.approx(2.0 / 5.0)
    assert _relative_rank(2.0, pool) == pytest.approx(4.0 / 5.0)


def test_relative_rank_all_tied_lands_exactly_at_half() -> None:
    pool = np.array([3.0] * 6)
    assert _relative_rank(3.0, pool) == pytest.approx(0.5)


def test_relative_rank_never_exactly_zero_or_one() -> None:
    # A genuine mathematical property (rank always in [1, n]), not a guarded edge case --
    # checked across a small parameter matrix rather than assumed.
    for n in (2, 3, 10, 50):
        pool = np.arange(n, dtype=np.float64)
        assert 0.0 < _relative_rank(0.0, pool) < 1.0
        assert 0.0 < _relative_rank(float(n - 1), pool) < 1.0


# ---------------------------------------------------------------------------
# Mechanics
# ---------------------------------------------------------------------------


def test_compute_pbo_produces_expected_combination_count() -> None:
    records = _records_from_prices(_random_walk_prices(300, seed=1))
    config = BacktestConfig(initial_capital=100_000.0, fee_bps=5.0, slippage_bps=2.0)
    report = compute_pbo(
        config, records, "py_core.strategies.sma_crossover:SmaCrossoverStrategy", _sma_param_grid(), n_groups=6
    )
    assert len(report.combination_results) == math.comb(6, 3)
    assert len(report.logit_distribution) == math.comb(6, 3)
    assert 0.0 <= report.probability_of_backtest_overfitting <= 1.0
    assert all(isinstance(cr, PboCombinationResult) for cr in report.combination_results)


def test_compute_pbo_rejects_odd_n_groups() -> None:
    records = _records_from_prices(_random_walk_prices(300, seed=1))
    config = BacktestConfig(initial_capital=100_000.0)
    with pytest.raises(ValueError, match="n_groups"):
        compute_pbo(
            config, records, "py_core.strategies.sma_crossover:SmaCrossoverStrategy", _sma_param_grid(), n_groups=5
        )


def test_compute_pbo_rejects_too_small_param_grid() -> None:
    records = _records_from_prices(_random_walk_prices(300, seed=1))
    config = BacktestConfig(initial_capital=100_000.0)
    with pytest.raises(ValueError, match="param_grid"):
        compute_pbo(
            config,
            records,
            "py_core.strategies.sma_crossover:SmaCrossoverStrategy",
            [{"fast_window": 5, "slow_window": 20}],
            n_groups=6,
        )


def test_combination_results_are_internally_consistent() -> None:
    records = _records_from_prices(_random_walk_prices(300, seed=1))
    config = BacktestConfig(initial_capital=100_000.0, fee_bps=5.0, slippage_bps=2.0)
    grid = _sma_param_grid()

    report = compute_pbo(
        config, records, "py_core.strategies.sma_crossover:SmaCrossoverStrategy", grid, n_groups=6
    )
    for cr in report.combination_results:
        assert cr.in_sample_winner_params in grid
        assert cr.train_scores[cr.in_sample_winner_index] == max(cr.train_scores)
        expected_rank = _relative_rank(
            cr.test_scores[cr.in_sample_winner_index], np.array(cr.test_scores)
        )
        assert cr.relative_rank == pytest.approx(expected_rank)
        assert cr.logit == pytest.approx(math.log(cr.relative_rank / (1.0 - cr.relative_rank)))


def test_pbo_equals_fraction_of_non_positive_logits() -> None:
    records = _records_from_prices(_random_walk_prices(300, seed=1))
    config = BacktestConfig(initial_capital=100_000.0, fee_bps=5.0, slippage_bps=2.0)
    report = compute_pbo(
        config, records, "py_core.strategies.sma_crossover:SmaCrossoverStrategy", _sma_param_grid(), n_groups=6
    )
    expected = sum(1 for logit in report.logit_distribution if logit <= 0.0) / len(report.logit_distribution)
    assert report.probability_of_backtest_overfitting == pytest.approx(expected)


# ---------------------------------------------------------------------------
# Behavioral control: a pool with genuine, stable structure must show a
# clearly LOWER PBO than a same-harness pool of purely exchangeable noise
# candidates, averaged over independent price series (paired comparison, not
# a single-seed one -- see round 1's walk-forward controls for why).
# ---------------------------------------------------------------------------


def test_genuine_stable_edge_shows_lower_pbo_than_pure_noise_pool_on_average() -> None:
    noise_config = BacktestConfig(initial_capital=100_000.0, fee_bps=5.0, slippage_bps=2.0)
    noise_grid = [{"seed": s} for s in range(10)]

    edge_config = BacktestConfig(initial_capital=100_000.0, fee_bps=2.0, slippage_bps=1.0)
    edge_grid = [{"lookback": lb} for lb in (1, 2, 3, 5, 8)]

    noise_values = []
    edge_values = []
    for seed in range(6):
        noise_records = _records_from_prices(_random_walk_prices(360, seed=100 + seed))
        noise_report = compute_pbo(
            noise_config, noise_records, _NO_EDGE_SPEC, noise_grid, n_groups=6, purge_bars=1,
            allow_external_strategy=True,
        )
        noise_values.append(noise_report.probability_of_backtest_overfitting)

        edge_records = _records_from_prices(_autocorrelated_prices(360, seed=200 + seed))
        edge_report = compute_pbo(
            edge_config, edge_records, _PLANTED_EDGE_SPEC, edge_grid, n_groups=6, purge_bars=1,
            allow_external_strategy=True,
        )
        edge_values.append(edge_report.probability_of_backtest_overfitting)

    mean_noise = sum(noise_values) / len(noise_values)
    mean_edge = sum(edge_values) / len(edge_values)

    assert mean_edge < 0.3, f"a genuine, stable edge should show low PBO -- got {edge_values} (mean {mean_edge:.3f})"
    assert mean_noise > mean_edge + 0.15, (
        f"a pool of purely exchangeable noise candidates should show clearly higher PBO than "
        f"a pool with genuine structure on the same harness -- noise {noise_values} "
        f"(mean {mean_noise:.3f}) vs edge {edge_values} (mean {mean_edge:.3f})"
    )
