"""Tests for exposure-matched allocation references in PLAN-04."""

from __future__ import annotations

from datetime import UTC, datetime, timedelta
from decimal import Decimal

import pytest

from py_core.research.plan04_comparison import (
    ALLOCATION_REFERENCE,
    CashContribution,
    ComparisonBar,
    ComparisonResult,
    ExecutionAssumptions,
    TrialOutcome,
    compare_cash_flows,
    evaluate_exposure_allocations,
    fit_exposure_allocations,
)

_START = datetime(2024, 1, 1, tzinfo=UTC)
_D = Decimal
_INTERVAL = timedelta(hours=4)


def make_bars(
    count: int,
    *,
    start: datetime = _START,
    open_prices: list[Decimal] | None = None,
    close_prices: list[Decimal] | None = None,
) -> tuple[ComparisonBar, ...]:
    opens = open_prices or [_D("10") + _D("0.1") * index for index in range(count)]
    closes = close_prices or opens
    return tuple(
        ComparisonBar(start + index * _INTERVAL, opens[index], closes[index], _D("1"))
        for index in range(count)
    )


def make_contributions(
    bars: tuple[ComparisonBar, ...],
    entries: list[tuple[int, Decimal]],
) -> tuple[CashContribution, ...]:
    return tuple(CashContribution(bars[index].open_time_utc, amount) for index, amount in entries)


def make_execution(**overrides: Decimal) -> ExecutionAssumptions:
    settings = {
        "price_tick": _D("0.1"),
        "quantity_step": _D("0.1"),
        "min_notional": _D("0"),
        "trade_fee_rate": _D("0"),
        "conversion_fee_rate": _D("0"),
        "slippage_rate": _D("0"),
        "strategy_allocation": _D("0.5"),
        "strategy_monthly_cost_cny": _D("0"),
    }
    settings.update(overrides)
    return ExecutionAssumptions(**settings)


def outcomes_by_name(result: ComparisonResult) -> dict[str, TrialOutcome]:
    return {outcome.name: outcome for outcome in result.outcomes}


def test_fit_uses_exact_grid_and_selects_the_closest_reference_allocation() -> None:
    bars = make_bars(300)
    contributions = make_contributions(bars, [(0, _D("10000"))])
    settings = make_execution()

    fit = fit_exposure_allocations(bars, contributions, settings)

    expected_grid = tuple(_D(index) / _D("100") for index in range(51))
    assert tuple(point.allocation for point in fit.grid) == expected_grid
    assert fit.ends_at_utc == bars[-1].open_time_utc + _INTERVAL
    assert fit.execution == settings
    assert {selection.candidate for selection in fit.selections} == {"trend_14d", "trend_28d"}
    for selection in fit.selections:
        expected = min(
            fit.grid,
            key=lambda point: (
                abs(point.mean_base_exposure - selection.calibration_trend_exposure),
                point.allocation,
            ),
        )
        assert selection.allocation == expected.allocation
        assert selection.calibration_reference_exposure == expected.mean_base_exposure


def test_no_trade_calibration_tie_selects_zero_and_preserves_no_trade_exposures() -> None:
    bars = make_bars(8)
    contributions = make_contributions(bars, [(0, _D("100"))])
    settings = make_execution(min_notional=_D("1000000"))

    fit = fit_exposure_allocations(bars, contributions, settings)
    replay = outcomes_by_name(
        compare_cash_flows(bars, contributions, settings, include_allocation_reference=True)
    )

    assert {point.mean_base_exposure for point in fit.grid} == {_D("0")}
    assert {selection.allocation for selection in fit.selections} == {_D("0")}
    assert {selection.calibration_trend_exposure for selection in fit.selections} == {_D("0")}
    assert replay[ALLOCATION_REFERENCE].status == "no_trade"
    for name in ("trend_14d", "trend_28d"):
        assert replay[name].status == "no_trade"
        assert replay[name].fills == ()


def test_evaluation_accepts_the_first_open_at_fit_end_and_keeps_original_baseline() -> None:
    train_bars = make_bars(8)
    train_contributions = make_contributions(train_bars, [(0, _D("100"))])
    settings = make_execution(min_notional=_D("1000000"))
    fit = fit_exposure_allocations(train_bars, train_contributions, settings)

    evaluation_bars = make_bars(8, start=fit.ends_at_utc)
    evaluation_contributions = make_contributions(evaluation_bars, [(0, _D("100"))])
    evaluation = evaluate_exposure_allocations(fit, evaluation_bars, evaluation_contributions)
    expected_baseline = outcomes_by_name(
        compare_cash_flows(
            evaluation_bars,
            evaluation_contributions,
            settings,
            include_allocation_reference=True,
        )
    )[ALLOCATION_REFERENCE]

    assert evaluation.baseline == expected_baseline
    assert len(evaluation.pairs) == 2
    for pair in evaluation.pairs:
        assert pair.selection.candidate == pair.trend.name
        expected_reference_name = "cash_quote" if pair.selection.allocation == 0 else ALLOCATION_REFERENCE
        assert pair.reference.name == expected_reference_name
        assert pair.exposure_gap == abs(
            pair.trend.mean_base_exposure - pair.reference.mean_base_exposure
        )
        assert pair.baseline_exposure_gap == abs(
            pair.trend.mean_base_exposure - evaluation.baseline.mean_base_exposure
        )
        assert pair.closer is (pair.exposure_gap < pair.baseline_exposure_gap)
        assert pair.within_tolerance is (pair.exposure_gap <= _D("0.05"))
        assert pair.pnl_difference_cny == pair.trend.investment_pnl_cny - pair.reference.investment_pnl_cny
        assert pair.drawdown_difference == pair.trend.max_drawdown - pair.reference.max_drawdown


def test_evaluation_rejects_bars_overlapping_training_end() -> None:
    train_bars = make_bars(8)
    train_contributions = make_contributions(train_bars, [(0, _D("100"))])
    fit = fit_exposure_allocations(train_bars, train_contributions, make_execution())

    overlapping_bars = make_bars(2, start=fit.ends_at_utc - _INTERVAL)
    contributions = make_contributions(overlapping_bars, [(0, _D("100"))])
    with pytest.raises(ValueError):
        evaluate_exposure_allocations(fit, overlapping_bars, contributions)


def test_held_out_price_and_funding_changes_do_not_change_fitted_allocations() -> None:
    train_bars = make_bars(300)
    train_contributions = make_contributions(train_bars, [(0, _D("10000"))])
    fit = fit_exposure_allocations(train_bars, train_contributions, make_execution())
    fitted_selections = fit.selections

    held_out_bars = make_bars(180, start=fit.ends_at_utc)
    held_out_contributions = make_contributions(held_out_bars, [(0, _D("10000"))])
    altered_bars = make_bars(
        180,
        start=fit.ends_at_utc,
        open_prices=[_D("50") - _D("0.1") * index for index in range(180)],
        close_prices=[_D("40") - _D("0.1") * index for index in range(180)],
    )
    altered_contributions = make_contributions(altered_bars, [(0, _D("50000"))])

    first = evaluate_exposure_allocations(fit, held_out_bars, held_out_contributions)
    second = evaluate_exposure_allocations(fit, altered_bars, altered_contributions)

    assert fit.selections == fitted_selections
    assert tuple(pair.selection.allocation for pair in first.pairs) == tuple(
        selection.allocation for selection in fitted_selections
    )
    assert tuple(pair.selection.allocation for pair in second.pairs) == tuple(
        selection.allocation for selection in fitted_selections
    )
