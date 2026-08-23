"""Unit tests for py_core.validation.walk_forward (批次 2, round 1).

The two controls at the bottom of this file are the ones the batch-2 plan calls out
explicitly: a known-zero-edge strategy must not show a systematically positive OOS result,
and a strategy with a genuine, deterministic, plantable edge must show a clearly positive
one -- proving the harness rejects noise without also being too blunt an instrument to ever
detect real signal.
"""

from __future__ import annotations

import hashlib
from datetime import UTC, datetime, timedelta
from decimal import Decimal
from typing import Any

import numpy as np
import pandas as pd
import pytest

from py_core.backtests.models import BacktestConfig, BacktestMetrics
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)
from py_core.strategies.base import Strategy, validate_signal_output
from py_core.validation.splits import Split, walk_forward_splits
from py_core.validation.walk_forward import FoldResult, run_walk_forward_analysis

_BTC = CanonicalMarketSymbol("BTCUSDT")
_1D = OhlcvTimeframe("1d")
_CRYPTO = ManualMarket.CRYPTO_SPOT
_BASE = datetime(2024, 1, 1, tzinfo=UTC)


def _records_from_prices(prices: np.ndarray) -> list[NormalizedOhlcvRecord]:
    records = []
    for i, p in enumerate(prices):
        p = float(p)
        records.append(
            NormalizedOhlcvRecord(
                market=_CRYPTO,
                symbol=_BTC,
                timeframe=_1D,
                event_time_utc=_BASE + timedelta(days=i),
                open_price=Decimal(str(round(p, 6))),
                high_price=Decimal(str(round(p * 1.001, 6))),
                low_price=Decimal(str(round(p * 0.999, 6))),
                close_price=Decimal(str(round(p, 6))),
                volume=Decimal(1000),
            )
        )
    return records


def _random_walk_prices(n: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return 100.0 * np.exp(np.cumsum(rng.normal(0.0, 0.01, n)))


def _sma_param_grid() -> list[dict[str, Any]]:
    return [{"fast_window": f, "slow_window": s} for f in (5, 10, 20) for s in (30, 50) if f < s]


# ---------------------------------------------------------------------------
# Mechanics: fold count, contiguity enforcement, empty-input rejection
# ---------------------------------------------------------------------------


def test_run_walk_forward_analysis_produces_one_fold_result_per_split(tmp_path=None) -> None:
    records = _records_from_prices(_random_walk_prices(300, seed=1))
    config = BacktestConfig(initial_capital=100_000.0, fee_bps=5.0, slippage_bps=2.0)
    splits = walk_forward_splits(300, train_window=100, test_window=50)

    report = run_walk_forward_analysis(
        config, records, "py_core.strategies.sma_crossover:SmaCrossoverStrategy", _sma_param_grid(), splits
    )
    assert len(report.fold_results) == len(splits)
    assert all(isinstance(fr, FoldResult) for fr in report.fold_results)
    assert len(report.oos_returns) == sum(len(s.test_indices) for s in splits)


def test_run_walk_forward_analysis_rejects_empty_param_grid() -> None:
    records = _records_from_prices(_random_walk_prices(300, seed=1))
    config = BacktestConfig(initial_capital=100_000.0)
    splits = walk_forward_splits(300, train_window=100, test_window=50)
    with pytest.raises(ValueError, match="param_grid"):
        run_walk_forward_analysis(config, records, "py_core.strategies.sma_crossover:SmaCrossoverStrategy", [], splits)


def test_run_walk_forward_analysis_rejects_empty_splits() -> None:
    records = _records_from_prices(_random_walk_prices(300, seed=1))
    config = BacktestConfig(initial_capital=100_000.0)
    with pytest.raises(ValueError, match="splits"):
        run_walk_forward_analysis(
            config, records, "py_core.strategies.sma_crossover:SmaCrossoverStrategy", _sma_param_grid(), []
        )


def test_run_walk_forward_analysis_rejects_non_contiguous_split() -> None:
    """purged_kfold_splits()-shaped folds must be refused, not silently misinterpreted --
    see walk_forward.py's module docstring for why the two are not interchangeable."""
    records = _records_from_prices(_random_walk_prices(300, seed=1))
    config = BacktestConfig(initial_capital=100_000.0)
    non_contiguous_split = Split(
        train_indices=np.array([0, 1, 2, 50, 51, 52], dtype=np.int64),
        test_indices=np.array([100, 101, 102], dtype=np.int64),
    )
    with pytest.raises(ValueError, match="not contiguous"):
        run_walk_forward_analysis(
            config,
            records,
            "py_core.strategies.sma_crossover:SmaCrossoverStrategy",
            _sma_param_grid(),
            [non_contiguous_split],
        )


def test_fold_results_carry_the_actual_selected_params_used_for_test(tmp_path=None) -> None:
    records = _records_from_prices(_random_walk_prices(300, seed=1))
    config = BacktestConfig(initial_capital=100_000.0, fee_bps=5.0, slippage_bps=2.0)
    splits = walk_forward_splits(300, train_window=100, test_window=50)
    grid = _sma_param_grid()

    report = run_walk_forward_analysis(
        config, records, "py_core.strategies.sma_crossover:SmaCrossoverStrategy", grid, splits
    )
    for fr in report.fold_results:
        assert fr.selected_params in grid
        assert isinstance(fr.test_metrics, BacktestMetrics)


# ---------------------------------------------------------------------------
# Negative control: a strategy with a provably zero real-world edge must not
# show a systematically positive OOS result.
# ---------------------------------------------------------------------------


class _DeterministicNoEdgeStrategy(Strategy):
    """A signal that looks random but is actually a deterministic function of each row's
    own timestamp (via a cryptographic hash) -- not sequential RNG state, so it stays
    lookahead-safe (assert_no_lookahead-compatible: the same timestamp always produces the
    same bit regardless of how much of the df is visible) while carrying zero real
    correlation with any market pattern by construction."""

    def __init__(self, seed: int = 0) -> None:
        self.seed = seed

    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        bits = []
        for ts in df.index:
            digest = hashlib.sha256(f"{self.seed}:{ts.value}".encode()).digest()
            bits.append(float(digest[0] & 1))
        signal = pd.Series(bits, index=df.index)
        validate_signal_output(signal, df)
        return signal


def test_no_edge_strategy_does_not_show_systematic_oos_edge_across_many_seeds() -> None:
    """Run the deterministic-noise strategy through walk-forward on several independent
    random-walk price series (no planted pattern in any of them) and confirm the OOS Sharpe
    is not systematically positive -- checked across multiple seeds, not just one run, since
    a single run's Sharpe being merely negative-by-chance would prove nothing about the
    harness itself."""
    config = BacktestConfig(initial_capital=100_000.0, fee_bps=5.0, slippage_bps=2.0)
    param_grid = [{"seed": s} for s in range(5)]

    oos_sharpes = []
    for price_seed in range(8):
        records = _records_from_prices(_random_walk_prices(300, seed=100 + price_seed))
        splits = walk_forward_splits(300, train_window=100, test_window=50)
        report = run_walk_forward_analysis(
            config,
            records,
            "py_core.tests.test_validation_walk_forward:_DeterministicNoEdgeStrategy",
            param_grid,
            splits,
            allow_external_strategy=True,
        )
        oos_sharpes.append(report.oos_metrics.sharpe_ratio)

    mean_sharpe = sum(oos_sharpes) / len(oos_sharpes)
    # A real edge should show up as a consistently, meaningfully positive mean Sharpe across
    # independent runs. Zero edge plus per-bar transaction costs should average out at or
    # below zero -- allow generous slack (a real strategy worth trusting clears this by a
    # wide margin, see the positive-control test below) rather than pin an exact threshold.
    assert mean_sharpe < 0.3, f"no-edge strategy showed a suspiciously positive mean OOS Sharpe: {oos_sharpes}"


# ---------------------------------------------------------------------------
# Positive control: a strategy with a genuine, deterministically planted edge
# must show a clearly positive OOS result -- proving the harness can detect
# real signal, not just always report ~zero/negative.
# ---------------------------------------------------------------------------


class _PlantedEdgeStrategy(Strategy):
    """Buy iff the trailing `lookback`-day cumulative return was positive.

    Exploits _autocorrelated_prices() below's AR(1)-in-returns process -- but note the sign
    here is deliberately momentum-shaped, not mean-reversion-shaped, even though the
    underlying price process has a *negative* return autocorrelation coefficient. This is
    not a mistake: the engine's own timing (positions = signal.shift(1), so a signal
    computed through bar T-1 earns fwd_return[T] ~ return[T+1], a **two**-bar-ahead
    relationship from the signal's own information cutoff) turns an odd-lag negative AR(1)
    coefficient into a same-sign (positive) correlation at the lag the engine actually
    trades -- (-k)^2 is positive. Verified directly (not just derived on paper, after an
    initial paper-derivation here got the sign backwards and this test failed loudly rather
    than silently): empirically measuring corr(trailing_return[T-1], fwd_return[T]) on this
    exact process at lookback in (1,2,3,5) confirmed positive correlation throughout. This
    is the single most important thing to get right when hand-constructing a "known good"
    control for a backtest engine -- the exploitable sign depends on the *engine's* execution
    timing, not just the data-generating process's own textbook description.
    """

    def __init__(self, lookback: int = 3) -> None:
        if lookback <= 0:
            raise ValueError(f"lookback must be > 0, got {lookback}")
        self.lookback = lookback

    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        trailing_return = df["close"].pct_change(self.lookback)
        signal = (trailing_return > 0).astype(float)
        signal = signal.mask(trailing_return.isna(), other=float("nan"))
        validate_signal_output(signal, df)
        return signal


def _autocorrelated_prices(n: int, seed: int, *, ar1_coefficient: float = 0.6) -> np.ndarray:
    """AR(1)-in-returns: return[t] = -ar1_coefficient * return[t-1] + noise. A textbook,
    deterministically exploitable pattern -- see _PlantedEdgeStrategy's docstring for why
    the engine ends up trading the resulting *even*-lag (positive) correlation, not the
    process's own odd-lag (negative) one."""
    rng = np.random.default_rng(seed)
    returns = np.zeros(n)
    prev_return = 0.0
    for i in range(n):
        noise = rng.normal(0.0, 0.01)
        returns[i] = -ar1_coefficient * prev_return + noise
        prev_return = returns[i]
    return 100.0 * np.exp(np.cumsum(returns))


def test_planted_edge_strategy_shows_clearly_positive_oos_edge() -> None:
    config = BacktestConfig(initial_capital=100_000.0, fee_bps=2.0, slippage_bps=1.0)
    param_grid = [{"lookback": lb} for lb in (1, 2, 3, 5)]

    oos_sharpes = []
    for price_seed in range(5):
        records = _records_from_prices(_autocorrelated_prices(400, seed=200 + price_seed))
        splits = walk_forward_splits(400, train_window=150, test_window=50)
        report = run_walk_forward_analysis(
            config,
            records,
            "py_core.tests.test_validation_walk_forward:_PlantedEdgeStrategy",
            param_grid,
            splits,
            allow_external_strategy=True,
        )
        oos_sharpes.append(report.oos_metrics.sharpe_ratio)

    mean_sharpe = sum(oos_sharpes) / len(oos_sharpes)
    assert mean_sharpe > 1.0, (
        f"a strategy exploiting a genuine, deterministically planted edge should show a "
        f"clearly positive mean OOS Sharpe -- got {oos_sharpes} (mean {mean_sharpe:.3f}); "
        "if this fails, the harness may be too conservative to ever validate a real strategy"
    )


def test_positive_control_beats_no_edge_control_on_the_same_harness() -> None:
    """Paired comparison, same price series feeding both strategies per seed, averaged over
    several independent seeds -- **not** a single-seed comparison. A first version of this
    test compared exactly one draw of each and was genuinely flaky: on some individual seeds
    the no-edge strategy's own in-sample grid search (picking the best-looking of 5 random
    hash seeds on each fold's train data) got mildly lucky and beat the planted-edge
    strategy on that one draw -- precisely the "one lucky/unlucky historical path" problem
    this whole validation package exists to guard against, so a single-draw comparison test
    would have been a bad advertisement for its own premise. Averaging over multiple seeds
    is the fix, not a wider tolerance band."""
    config = BacktestConfig(initial_capital=100_000.0, fee_bps=2.0, slippage_bps=1.0)

    margins = []
    for seed in range(6):
        records = _records_from_prices(_autocorrelated_prices(400, seed=300 + seed))
        splits = walk_forward_splits(400, train_window=150, test_window=50)

        planted_report = run_walk_forward_analysis(
            config,
            records,
            "py_core.tests.test_validation_walk_forward:_PlantedEdgeStrategy",
            [{"lookback": lb} for lb in (1, 2, 3, 5)],
            splits,
            allow_external_strategy=True,
        )
        no_edge_report = run_walk_forward_analysis(
            config,
            records,
            "py_core.tests.test_validation_walk_forward:_DeterministicNoEdgeStrategy",
            [{"seed": s} for s in range(5)],
            splits,
            allow_external_strategy=True,
        )
        margins.append(planted_report.oos_metrics.sharpe_ratio - no_edge_report.oos_metrics.sharpe_ratio)

    mean_margin = sum(margins) / len(margins)
    assert mean_margin > 0.3, (
        f"planted-edge strategy should consistently beat the no-edge strategy on the same "
        f"data, averaged across seeds -- per-seed margins {margins} (mean {mean_margin:.3f})"
    )
