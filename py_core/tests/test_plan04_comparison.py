"""Tests for matched cash-flow replay of the PLAN-04 trend candidates."""

from __future__ import annotations

from datetime import UTC, datetime, timedelta
from decimal import Decimal

import pytest

from py_core.research.plan04_comparison import (
    CashContribution,
    ComparisonBar,
    ComparisonResult,
    ExecutionAssumptions,
    TrialOutcome,
    compare_cash_flows,
)

_START = datetime(2024, 1, 1, tzinfo=UTC)
_D = Decimal


def make_bars(
    count: int,
    *,
    open_prices: list[Decimal] | None = None,
    close_prices: list[Decimal] | None = None,
    quote_cny_rates: list[Decimal] | None = None,
) -> tuple[ComparisonBar, ...]:
    opens = open_prices or [_D("10")] * count
    closes = close_prices or opens
    rates = quote_cny_rates or [_D("1")] * count
    return tuple(
        ComparisonBar(
            _START + timedelta(hours=4 * index),
            opens[index],
            closes[index],
            rates[index],
        )
        for index in range(count)
    )


def make_contributions(
    bars: tuple[ComparisonBar, ...],
    entries: list[tuple[int, Decimal]],
) -> tuple[CashContribution, ...]:
    return tuple(CashContribution(bars[index].open_time_utc, amount) for index, amount in entries)


def execution(**overrides: Decimal) -> ExecutionAssumptions:
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


def test_timed_deposits_are_equal_across_references_and_flat_valuation_has_no_pnl() -> None:
    bars = make_bars(9)
    contributions = make_contributions(bars, [(0, _D("100")), (3, _D("200")), (6, _D("300"))])

    result = compare_cash_flows(bars, contributions, execution())
    allocated_result = compare_cash_flows(
        bars, contributions, execution(), include_allocation_reference=True
    )

    outcomes = outcomes_by_name(result)
    assert tuple(outcomes) == ("cash_cny", "cash_quote", "buy_hold", "trend_14d", "trend_28d")
    assert tuple(outcomes_by_name(allocated_result)) == (
        "cash_cny",
        "cash_quote",
        "buy_hold",
        "buy_hold_allocated",
        "trend_14d",
        "trend_28d",
    )
    for outcome in outcomes.values():
        assert outcome.contributed_cny == _D("600")
        assert outcome.terminal_cny == _D("600")
        assert outcome.investment_pnl_cny == _D("0")
        assert outcome.all_in_pnl_cny == _D("0")
    assert outcomes["cash_cny"].status == "completed"
    assert outcomes["cash_quote"].status == "completed"
    assert outcomes["buy_hold"].status == "completed"
    for name in ("trend_14d", "trend_28d"):
        assert outcomes[name].status == "no_trade"
        assert outcomes[name].fills == ()


def test_allocated_reference_keeps_reserve_and_carries_rounding_dust_between_deposits() -> None:
    bars = make_bars(
        4,
        open_prices=[_D("20"), _D("25"), _D("30"), _D("35")],
        close_prices=[_D("20"), _D("30"), _D("30"), _D("40")],
        quote_cny_rates=[_D("2")] * 4,
    )
    contributions = make_contributions(bars, [(0, _D("100")), (2, _D("100"))])

    result = compare_cash_flows(
        bars,
        contributions,
        execution(),
        include_allocation_reference=True,
    )

    allocated = outcomes_by_name(result)["buy_hold_allocated"]
    assert [fill.side for fill in allocated.fills] == ["buy", "buy"]
    assert [fill.open_time_utc for fill in allocated.fills] == [
        bars[0].open_time_utc,
        bars[2].open_time_utc,
    ]
    assert [fill.quantity for fill in allocated.fills] == [_D("1.2"), _D("0.8")]
    assert allocated.rejected_orders == 0
    assert allocated.contributed_cny == _D("200")
    assert allocated.terminal_cny == _D("264")
    expected_exposure = (
        _D("24") / _D("50")
        + _D("36") / _D("62")
        + _D("60") / _D("112")
        + _D("80") / _D("132")
    ) / _D("4")
    assert float(allocated.mean_base_exposure) == pytest.approx(float(expected_exposure))


def test_rejected_allocated_budget_carries_until_a_later_contribution_open() -> None:
    prices = [_D("20"), _D("10"), _D("20"), _D("20")]
    bars = make_bars(
        4,
        open_prices=prices,
        close_prices=prices,
        quote_cny_rates=[_D("2")] * 4,
    )
    contributions = make_contributions(bars, [(0, _D("100")), (2, _D("100"))])

    result = compare_cash_flows(
        bars,
        contributions,
        execution(min_notional=_D("25")),
        include_allocation_reference=True,
    )

    allocated = outcomes_by_name(result)["buy_hold_allocated"]
    assert allocated.status == "completed"
    assert allocated.rejected_orders == 1
    assert len(allocated.fills) == 1
    assert allocated.fills[0].open_time_utc == bars[2].open_time_utc
    assert allocated.fills[0].quantity == _D("2.5")
    assert allocated.fills[0].price * allocated.fills[0].quantity == _D("50")
    assert allocated.terminal_cny == _D("200")


def test_allocated_reference_applies_conversion_slippage_and_base_fee_in_quote_fx() -> None:
    bars = make_bars(
        1,
        open_prices=[_D("20")],
        close_prices=[_D("20.2")],
        quote_cny_rates=[_D("2")],
    )
    contributions = make_contributions(bars, [(0, _D("100"))])
    settings = execution(
        price_tick=_D("0.1"),
        quantity_step=_D("0.1"),
        min_notional=_D("24.24"),
        trade_fee_rate=_D("0.01"),
        conversion_fee_rate=_D("0.01"),
        slippage_rate=_D("0.01"),
    )

    result = compare_cash_flows(
        bars,
        contributions,
        settings,
        include_allocation_reference=True,
    )

    allocated = outcomes_by_name(result)["buy_hold_allocated"]
    assert len(allocated.fills) == 1
    fill = allocated.fills[0]
    assert fill.side == "buy"
    assert fill.price == _D("20.2")
    assert fill.quantity == _D("1.2")
    assert fill.price * fill.quantity == settings.min_notional
    assert fill.fee == _D("0.012")
    assert allocated.conversion_fees_cny == _D("1")
    assert allocated.trade_fees_cny == _D("0.4848")
    assert allocated.terminal_cny == _D("98.5152")


def test_buy_and_sell_fills_obey_steps_minimum_notional_and_fee_assets() -> None:
    count = 296
    prices = [_D("10") + _D("0.1") * index for index in range(count)]
    bars = make_bars(count, open_prices=prices)
    contributions = make_contributions(bars, [(0, _D("1000"))])
    settings = execution(min_notional=_D("497.65"), trade_fee_rate=_D("0.001"))

    result = compare_cash_flows(bars, contributions, settings)

    outcomes = outcomes_by_name(result)
    assert outcomes["trend_14d"].status == "completed"
    assert outcomes["trend_28d"].status == "completed"
    for name, trend_bars in (("trend_14d", 84), ("trend_28d", 168)):
        fills = outcomes[name].fills
        assert [fill.side for fill in fills] == ["buy", "sell"]
        buy, sell = fills
        assert buy.open_time_utc == bars[trend_bars + 1].open_time_utc
        assert sell.open_time_utc == bars[trend_bars + 126 + 1].open_time_utc
        assert buy.price % settings.price_tick == 0
        assert buy.quantity % settings.quantity_step == 0
        assert buy.price * buy.quantity >= settings.min_notional
        assert sell.price % settings.price_tick == 0
        assert sell.quantity % settings.quantity_step == 0
        assert sell.price * sell.quantity >= settings.min_notional
        assert buy.fee == buy.quantity * settings.trade_fee_rate
        assert sell.fee == sell.quantity * sell.price * settings.trade_fee_rate
    assert outcomes["trend_28d"].fills[0].price * outcomes["trend_28d"].fills[0].quantity == settings.min_notional


def test_contribution_during_an_open_trend_does_not_trigger_an_extra_buy() -> None:
    count = 212
    prices = [_D("10") + _D("0.1") * index for index in range(count)]
    bars = make_bars(count, open_prices=prices)
    contributions = make_contributions(bars, [(0, _D("1000")), (150, _D("1000"))])

    result = compare_cash_flows(bars, contributions, execution())

    trend = outcomes_by_name(result)["trend_14d"]
    assert [fill.side for fill in trend.fills] == ["buy", "sell"]
    assert trend.fills[0].open_time_utc == bars[85].open_time_utc
    assert trend.fills[1].open_time_utc == bars[211].open_time_utc


def test_quote_fx_and_external_cost_apply_only_to_the_relevant_outcomes() -> None:
    count = 220
    rates = [_D("1") if index < 150 else _D("2") for index in range(count)]
    bars = make_bars(count, quote_cny_rates=rates)
    contributions = make_contributions(bars, [(0, _D("1000"))])
    settings = execution(
        conversion_fee_rate=_D("0.01"),
        strategy_monthly_cost_cny=_D("5"),
    )

    outcomes = outcomes_by_name(compare_cash_flows(bars, contributions, settings))

    assert outcomes["cash_cny"].terminal_cny == _D("1000")
    assert outcomes["cash_quote"].terminal_cny == _D("1980")
    assert outcomes["buy_hold"].terminal_cny == _D("1980")
    for name in ("cash_quote", "buy_hold", "trend_14d", "trend_28d"):
        assert outcomes[name].conversion_fees_cny == _D("10")
    assert outcomes["cash_cny"].conversion_fees_cny == _D("0")
    for name in ("cash_cny", "cash_quote", "buy_hold"):
        assert outcomes[name].external_cost_cny == _D("0")
    for name in ("trend_14d", "trend_28d"):
        assert outcomes[name].status == "no_trade"
        assert outcomes[name].external_cost_cny == _D("10")
        assert outcomes[name].all_in_pnl_cny == outcomes[name].investment_pnl_cny - _D("10")
        assert outcomes[name].fills == ()


def test_insufficient_notional_records_rejected_orders_without_dropping_trials() -> None:
    count = 296
    prices = [_D("10") + _D("0.1") * index for index in range(count)]
    bars = make_bars(count, open_prices=prices)
    contributions = make_contributions(bars, [(0, _D("1000"))])
    settings = execution(min_notional=_D("10000"))

    outcomes = outcomes_by_name(compare_cash_flows(bars, contributions, settings))

    for name in ("cash_cny", "cash_quote"):
        assert outcomes[name].status == "completed"
    for name in ("buy_hold", "trend_14d", "trend_28d"):
        assert outcomes[name].status == "no_trade"
        assert outcomes[name].fills == ()
        assert outcomes[name].rejected_orders >= 1


@pytest.mark.parametrize("malformed", ["naive_bar_time", "zero_funding", "off_grid_funding"])
def test_malformed_dates_or_funding_are_rejected(malformed: str) -> None:
    bars = make_bars(3)
    contributions = make_contributions(bars, [(0, _D("100"))])
    if malformed == "naive_bar_time":
        naive_time = bars[0].open_time_utc.replace(tzinfo=None)
        bars = (
            ComparisonBar(naive_time, bars[0].open_price, bars[0].close_price, bars[0].quote_cny_rate),
            *bars[1:],
        )
        contributions = (CashContribution(naive_time, _D("100")),)
    elif malformed == "zero_funding":
        contributions = (CashContribution(bars[0].open_time_utc, _D("0")),)
    else:
        contributions = (
            contributions[0],
            CashContribution(bars[1].open_time_utc + timedelta(hours=1), _D("10")),
        )

    with pytest.raises(ValueError):
        compare_cash_flows(bars, contributions, execution())
