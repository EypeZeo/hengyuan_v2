"""Tests for py_core.backtests.metrics (批次 4 修复回归 — findings 043/049/050/051/052/053).

metrics.py has no test file of its own prior to this one, despite being the source of every
Sharpe/drawdown/Calmar figure in the stack and the default ``selection_metric`` for
walk-forward/CPCV/PBO. Each test below pins the DIRECTION of a fix (not just "no crash"),
reproducing the exact scenario the audit finding used.
"""

from __future__ import annotations

import math

import pytest

from py_core.backtests.metrics import compute_metrics


def test_zero_variance_positive_drift_gets_infinite_sharpe_not_zero() -> None:
    """AUDIT METRIC-ZEROVAR-SHARPE-053: a strategy with a perfectly constant positive
    return every bar used to score sharpe_ratio == 0.0, identical to doing nothing --
    which meant it could never be selected over a no-op by the default selection_metric."""
    n = 20
    returns = [0.001] * n
    equity = [100_000.0 * (1.001**i) for i in range(1, n + 1)]
    positions = [1.0] * n
    metrics = compute_metrics(
        equity_curve=equity,
        returns=returns,
        positions=positions,
        cost_impact_total=0.0,
        initial_capital=100_000.0,
        annualization_factor=365.0,
    )
    assert metrics.sharpe_ratio == float("inf")


def test_zero_variance_zero_drift_sharpe_stays_zero() -> None:
    """The one case with no signal to rank (flat returns, flat excess) must still be 0.0,
    not swept up into the new +-inf branches."""
    n = 10
    returns = [0.0] * n
    equity = [100_000.0] * n
    positions = [0.0] * n
    metrics = compute_metrics(
        equity_curve=equity,
        returns=returns,
        positions=positions,
        cost_impact_total=0.0,
        initial_capital=100_000.0,
        annualization_factor=365.0,
    )
    assert metrics.sharpe_ratio == 0.0


def test_zero_variance_negative_drift_gets_negative_infinite_sharpe() -> None:
    n = 10
    returns = [-0.001] * n
    equity = [100_000.0 * (0.999**i) for i in range(1, n + 1)]
    positions = [1.0] * n
    metrics = compute_metrics(
        equity_curve=equity,
        returns=returns,
        positions=positions,
        cost_impact_total=0.0,
        initial_capital=100_000.0,
        annualization_factor=365.0,
    )
    assert metrics.sharpe_ratio == float("-inf")


def test_loss_from_declining_first_bar_is_measured_against_initial_capital() -> None:
    """AUDIT METRIC-CALMAR-INF-049: peak used to seed from equity_curve[0]. A curve that
    opens with a loss and never recovers above its own first bar used to report max_dd=0.0
    (and calmar_ratio=+inf) despite losing money relative to the capital actually risked."""
    initial_capital = 100_000.0
    # Bar 0 opens already down 40% from initial_capital, then holds flat.
    equity = [60_000.0, 60_000.0, 60_000.0, 60_000.0]
    returns = [-0.4, 0.0, 0.0, 0.0]
    positions = [1.0, 1.0, 1.0, 1.0]
    metrics = compute_metrics(
        equity_curve=equity,
        returns=returns,
        positions=positions,
        cost_impact_total=0.0,
        initial_capital=initial_capital,
        annualization_factor=365.0,
    )
    assert metrics.max_drawdown == pytest.approx(0.4, abs=1e-9)
    # A real loss with a real (nonzero) measured drawdown must not be +inf.
    assert metrics.calmar_ratio < 0.0
    assert math.isfinite(metrics.calmar_ratio)


def test_negative_return_always_shows_a_real_negative_calmar_not_infinite() -> None:
    """Once the peak-baseline fix (METRIC-CALMAR-INF-049) seeds peak from initial_capital,
    any negative annualized_return necessarily corresponds to a real, nonzero measured
    drawdown at some point (equity fell below the starting peak) -- so the "losing money
    with zero measured drawdown -> +inf" failure mode this finding described can no longer
    occur, and calmar_ratio comes out as an ordinary finite negative number instead of
    hitting either +-inf branch."""
    metrics = compute_metrics(
        equity_curve=[90_000.0],
        returns=[-0.1],
        positions=[0.0],
        cost_impact_total=0.0,
        initial_capital=100_000.0,
        annualization_factor=365.0,
    )
    assert metrics.calmar_ratio < 0.0
    assert math.isfinite(metrics.calmar_ratio)


def test_calmar_ratio_is_positive_infinite_for_genuine_no_drawdown_gain() -> None:
    metrics = compute_metrics(
        equity_curve=[101_000.0],
        returns=[0.01],
        positions=[1.0],
        cost_impact_total=0.0,
        initial_capital=100_000.0,
        annualization_factor=365.0,
    )
    assert metrics.calmar_ratio == float("inf")


def test_max_drawdown_is_clamped_to_one_even_when_equity_goes_negative() -> None:
    """AUDIT METRIC-DD-UNBOUNDED-050: with no margin-call cutoff, equity can go negative,
    which used to produce drawdown ratios like 1.05 or 6.0 -- past 100% loss."""
    equity = [100_000.0, 50_000.0, -500_000.0]
    returns = [-0.5, -0.5, -10.0]
    positions = [1.0, 1.0, 1.0]
    metrics = compute_metrics(
        equity_curve=equity,
        returns=returns,
        positions=positions,
        cost_impact_total=0.0,
        initial_capital=100_000.0,
        annualization_factor=365.0,
    )
    assert metrics.max_drawdown == 1.0


def test_annualized_return_is_flat_negative_one_not_complex_on_negative_equity() -> None:
    """AUDIT METRIC-COMPLEX-043: (1+total_return) < 0 raised to a fractional power used to
    silently produce a complex number, which then flowed into calmar_ratio/JSON output."""
    metrics = compute_metrics(
        equity_curve=[100_000.0, -50_000.0],
        returns=[0.0, -1.5],
        positions=[0.0, 1.0],
        cost_impact_total=0.0,
        initial_capital=100_000.0,
        annualization_factor=365.0,
    )
    assert metrics.annualized_return == -1.0
    assert isinstance(metrics.annualized_return, float)  # not complex


def test_turnover_divides_by_number_of_actual_changes_not_bar_count() -> None:
    """AUDIT METRIC-TURNOVER-051: pos_changes has n-1 elements; dividing by n understated
    turnover by (n-1)/n. positions=[1,0,1,0,1] has 4 changes of magnitude 1 each -> 1.0,
    not 0.8."""
    n = 5
    metrics = compute_metrics(
        equity_curve=[100_000.0] * n,
        returns=[0.0] * n,
        positions=[1.0, 0.0, 1.0, 0.0, 1.0],
        cost_impact_total=0.0,
        initial_capital=100_000.0,
        annualization_factor=365.0,
    )
    assert metrics.turnover == pytest.approx(1.0, abs=1e-9)


def test_positions_length_mismatch_is_rejected() -> None:
    """AUDIT METRIC-POSLEN-052: len(positions) was never validated against len(returns),
    unlike equity_curve -- a mismatch silently changed exposure/turnover's denominator."""
    with pytest.raises(ValueError, match="positions"):
        compute_metrics(
            equity_curve=[100_000.0, 101_000.0, 102_000.0],
            returns=[0.0, 0.01, 0.01],
            positions=[1.0],
            cost_impact_total=0.0,
            initial_capital=100_000.0,
            annualization_factor=365.0,
        )
