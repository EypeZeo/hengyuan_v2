"""Replay the two STR-TREND-01 trials and three references on one funding schedule."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from datetime import datetime, time, timedelta
from decimal import ROUND_CEILING, ROUND_FLOOR, Decimal, DecimalException
from itertools import pairwise
from statistics import mean, stdev

import pandas as pd

from py_core.research.experiments import CausalCheck, ExperimentStatus
from py_core.research.ledgers import (
    AllInCnyAccount,
    BalanceState,
    FlowEvent,
    FlowEventKind,
    LedgerEvent,
    LedgerEventKind,
    PerformanceAccount,
    TradingAssetAccount,
)
from py_core.strategies.base import check_signal_prefixes
from py_core.strategies.trend import (
    TREND_BARS_4H_14D,
    TREND_BARS_4H_28D,
    TrendFollowingStrategy,
)

_ZERO = Decimal(0)
_ONE = Decimal(1)
_INTERVAL = timedelta(hours=4)
_CNY, _QUOTE, _BASE = "CNY", "quote", "base"
ALLOCATION_REFERENCE = "buy_hold_allocated"
_PASSIVE_TRIALS = (("cash_cny", None), ("cash_quote", None), ("buy_hold", None))
_TREND_TRIALS = (("trend_14d", TREND_BARS_4H_14D), ("trend_28d", TREND_BARS_4H_28D))
TRIALS = (*_PASSIVE_TRIALS, *_TREND_TRIALS)
_TRIALS_WITH_ALLOCATION = (*_PASSIVE_TRIALS, (ALLOCATION_REFERENCE, None), *_TREND_TRIALS)


@dataclass(frozen=True, slots=True)
class ComparisonBar:
    open_time_utc: datetime
    open_price: Decimal
    close_price: Decimal
    quote_cny_rate: Decimal


@dataclass(frozen=True, slots=True)
class CashContribution:
    open_time_utc: datetime
    amount_cny: Decimal


@dataclass(frozen=True, slots=True)
class ExecutionAssumptions:
    price_tick: Decimal
    quantity_step: Decimal
    min_notional: Decimal
    trade_fee_rate: Decimal = Decimal("0.001")
    conversion_fee_rate: Decimal = _ZERO
    slippage_rate: Decimal = _ZERO
    strategy_allocation: Decimal = Decimal("0.5")
    strategy_monthly_cost_cny: Decimal = _ZERO


@dataclass(frozen=True, slots=True)
class Fill:
    open_time_utc: datetime
    side: str
    price: Decimal
    quantity: Decimal
    fee: Decimal


@dataclass(frozen=True, slots=True)
class TrialOutcome:
    name: str
    status: ExperimentStatus
    contributed_cny: Decimal
    causal_check: CausalCheck
    terminal_cny: Decimal | None = None
    investment_pnl_cny: Decimal | None = None
    all_in_pnl_cny: Decimal | None = None
    terminal_nav: Decimal | None = None
    max_drawdown: Decimal | None = None
    trade_fees_cny: Decimal | None = None
    conversion_fees_cny: Decimal | None = None
    external_cost_cny: Decimal | None = None
    daily_observations: int = 0
    daily_mean_return: Decimal | None = None
    daily_sharpe: Decimal | None = None
    fills: tuple[Fill, ...] = ()
    rejected_orders: int = 0
    failure_reason: str | None = None
    mean_base_exposure: Decimal | None = None


@dataclass(frozen=True, slots=True)
class ComparisonResult:
    contributions: tuple[CashContribution, ...]
    outcomes: tuple[TrialOutcome, ...]


def _decimal(value: Decimal, field: str, minimum: Decimal, maximum: Decimal | None = None) -> None:
    if not isinstance(value, Decimal) or not value.is_finite() or value < minimum:
        raise ValueError(f"{field} must be a finite Decimal >= {minimum}")
    if maximum is not None and value >= maximum:
        raise ValueError(f"{field} must be below {maximum}")


def _positive(value: Decimal, field: str) -> None:
    _decimal(value, field, _ZERO)
    if value == 0:
        raise ValueError(f"{field} must be positive")


def _validate(
    bars: tuple[ComparisonBar, ...],
    contributions: tuple[CashContribution, ...],
    execution: ExecutionAssumptions,
) -> None:
    if not bars or not contributions:
        raise ValueError("bars and contributions must be nonempty")
    for bar in bars:
        if bar.open_time_utc.utcoffset() != timedelta(0):
            raise ValueError("bar timestamps must be UTC")
        _positive(bar.open_price, "open_price")
        _positive(bar.close_price, "close_price")
        _positive(bar.quote_cny_rate, "quote_cny_rate")
    if any(current.open_time_utc - prior.open_time_utc != _INTERVAL for prior, current in pairwise(bars)):
        raise ValueError("bars must be consecutive and 4 hours apart")
    if contributions[0].open_time_utc != bars[0].open_time_utc:
        raise ValueError("the first contribution must occur at the first bar open")
    times = {bar.open_time_utc for bar in bars}
    previous = None
    for contribution in contributions:
        current = contribution.open_time_utc
        if current.utcoffset() != timedelta(0) or current not in times or (previous is not None and current <= previous):
            raise ValueError("contributions must be strictly ordered UTC bar opens")
        _positive(contribution.amount_cny, "amount_cny")
        previous = current
    validate_execution_assumptions(execution)


def validate_execution_assumptions(execution: ExecutionAssumptions) -> None:
    """Reject execution assumptions outside the supported cash and order models."""
    _positive(execution.price_tick, "price_tick")
    _positive(execution.quantity_step, "quantity_step")
    _decimal(execution.min_notional, "min_notional", _ZERO)
    _decimal(execution.trade_fee_rate, "trade_fee_rate", _ZERO, _ONE)
    _decimal(execution.slippage_rate, "slippage_rate", _ZERO, _ONE)
    _decimal(execution.conversion_fee_rate, "conversion_fee_rate", _ZERO, _ONE)
    _positive(execution.strategy_allocation, "strategy_allocation")
    if execution.strategy_allocation > _ONE:
        raise ValueError("strategy_allocation must be at most 1")
    _decimal(execution.strategy_monthly_cost_cny, "strategy_monthly_cost_cny", _ZERO)


def _target_for_each_open(bars: tuple[ComparisonBar, ...], trend_bars: int | None) -> tuple[tuple[bool, ...], CausalCheck]:
    if trend_bars is None:
        return (False,) * len(bars), CausalCheck.INDEX_ONLY
    frame = pd.DataFrame(
        {"close": [float(bar.close_price) for bar in bars]},
        index=pd.DatetimeIndex(bar.open_time_utc for bar in bars),
    )
    signal, positions = check_signal_prefixes(TrendFollowingStrategy(trend_bars=trend_bars), frame)
    causal = CausalCheck.PREFIX_DIFFERENTIAL if positions else CausalCheck.INDEX_ONLY
    return tuple(signal.shift(1, fill_value=0).eq(1.0)), causal


def _round_to_step(value: Decimal, step: Decimal, rounding: str = ROUND_FLOOR) -> Decimal:
    return (value / step).to_integral_value(rounding=rounding) * step


def _order_fill(
    bar: ComparisonBar,
    execution: ExecutionAssumptions,
    side: str,
    available: Decimal,
) -> Fill | None:
    if side == "buy":
        price = _round_to_step(bar.open_price * (_ONE + execution.slippage_rate), execution.price_tick, ROUND_CEILING)
        quantity = (available // (price * execution.quantity_step)) * execution.quantity_step
    else:
        price = _round_to_step(bar.open_price * (_ONE - execution.slippage_rate), execution.price_tick)
        quantity = _round_to_step(available, execution.quantity_step)
    notional = quantity * price
    if quantity <= 0 or price <= 0 or notional < execution.min_notional:
        return None
    fee_basis = quantity if side == "buy" else notional
    return Fill(bar.open_time_utc, side, price, quantity, fee_basis * execution.trade_fee_rate)


def _value_cny(balance: BalanceState, price: Decimal, rate: Decimal) -> Decimal:
    return balance.available(_CNY) + (balance.available(_QUOTE) + balance.available(_BASE) * price) * rate


def _replay_trial(
    name: str,
    trend_bars: int | None,
    bars: tuple[ComparisonBar, ...],
    funding_by_open: dict[datetime, Decimal],
    execution: ExecutionAssumptions,
    *,
    measure_exposure: bool,
) -> TrialOutcome:
    open_targets, causal = _target_for_each_open(bars, trend_bars)
    assets = TradingAssetAccount()
    performance = PerformanceAccount()
    owner = AllInCnyAccount()
    fills: list[Fill] = []
    max_drawdown = trade_fees = conversion_fees = _ZERO
    # The allocated budget carries unspent buy funds across deposits without using the cash reserve.
    allocated_quote = exposure_sum = _ZERO
    is_allocated_hold = name == ALLOCATION_REFERENCE
    rejected_orders = 0
    previous_month = None
    previous_target = exit_pending = False
    daily_returns: list[Decimal] = []
    previous_daily_nav = _ONE if bars[0].open_time_utc.time() == time.min else None

    for index, (bar, target) in enumerate(zip(bars, open_targets, strict=True)):
        month = bar.open_time_utc.year, bar.open_time_utc.month
        if trend_bars is not None and month != previous_month and execution.strategy_monthly_cost_cny:
            owner.record_external_cost(f"run-{index}", execution.strategy_monthly_cost_cny)
        previous_month = month

        open_value = _value_cny(assets.state(), bar.open_price, bar.quote_cny_rate)
        performance.mark(open_value)
        max_drawdown = max(max_drawdown, performance.drawdown() or _ZERO)
        funding = funding_by_open.get(bar.open_time_utc)
        if funding is not None:
            deposit_id = f"deposit-{index}"
            performance.apply_flow(FlowEvent(deposit_id, FlowEventKind.DEPOSIT, funding))
            owner.record_deposit(deposit_id, funding)
            asset, units = _CNY, funding
            if name != "cash_cny":
                fee_cny = funding * execution.conversion_fee_rate
                conversion_fees += fee_cny
                asset, units = _QUOTE, (funding - fee_cny) / bar.quote_cny_rate
            assets.apply(LedgerEvent(deposit_id, LedgerEventKind.DEPOSIT, asset, quantity=units))
            if is_allocated_hold:
                allocated_quote += units * execution.strategy_allocation

        balance = assets.state()
        cash = balance.available(_QUOTE)
        holding = balance.available(_BASE)
        side, available = None, _ZERO
        if name == "buy_hold" and funding is not None:
            side, available = "buy", cash
        elif is_allocated_hold and funding is not None:
            side, available = "buy", min(cash, allocated_quote)
        elif trend_bars is not None:
            sellable = holding >= execution.quantity_step
            exit_pending = sellable and (exit_pending or not target)
            if exit_pending:
                side, available = "sell", holding
            elif target and not previous_target:
                equity = cash + holding * bar.open_price
                side, available = "buy", min(cash, equity * execution.strategy_allocation)
        previous_target = target

        if side is not None:
            fill = _order_fill(bar, execution, side, available)
            if fill is None:
                rejected_orders += 1
            else:
                is_buy = side == "buy"
                fee_asset = _BASE if is_buy else _QUOTE
                notional = fill.quantity * fill.price
                assets.apply(LedgerEvent(
                    f"fill-{index}",
                    LedgerEventKind.BUY_FILL if is_buy else LedgerEventKind.SELL_FILL,
                    _BASE,
                    asset_quantity=fill.quantity,
                    counter_asset=_QUOTE,
                    counter_quantity=notional,
                    fee_asset=fee_asset if fill.fee else None,
                    fee_quantity=fill.fee or None,
                ))
                if is_allocated_hold:
                    allocated_quote -= notional
                fills.append(fill)
                trade_fees += fill.fee * (fill.price if is_buy else _ONE) * bar.quote_cny_rate
                exit_pending = False

        close_balance = assets.state()
        terminal_value = _value_cny(close_balance, bar.close_price, bar.quote_cny_rate)
        if measure_exposure and terminal_value > 0:
            base_value = close_balance.available(_BASE) * bar.close_price * bar.quote_cny_rate
            exposure_sum += base_value / terminal_value
        performance.mark(terminal_value)
        max_drawdown = max(max_drawdown, performance.drawdown() or _ZERO)
        if (bar.open_time_utc + _INTERVAL).time() == time.min:
            nav = performance.nav()
            if previous_daily_nav and nav is not None:
                daily_returns.append(nav / previous_daily_nav - _ONE)
            previous_daily_nav = nav

    daily_mean = mean(daily_returns) if daily_returns else None
    daily_std = stdev(daily_returns) if len(daily_returns) > 1 else _ZERO
    daily_sharpe = None
    if daily_mean is not None and daily_std > Decimal("1e-12"):
        daily_sharpe = daily_mean / daily_std * Decimal(365).sqrt()
    owner.mark_total_value_cny(terminal_value)
    summary = owner.summary()
    return TrialOutcome(
        name=name,
        status=ExperimentStatus.COMPLETED if fills or name.startswith("cash_") else ExperimentStatus.NO_TRADE,
        contributed_cny=performance.contributions(),
        causal_check=causal,
        terminal_cny=terminal_value,
        investment_pnl_cny=summary["investment_pnl_cny"],
        all_in_pnl_cny=summary["all_in_pnl_cny"],
        terminal_nav=performance.nav(),
        max_drawdown=max_drawdown,
        trade_fees_cny=trade_fees,
        conversion_fees_cny=conversion_fees,
        external_cost_cny=summary["external_costs_cny"],
        daily_observations=len(daily_returns),
        daily_mean_return=daily_mean,
        daily_sharpe=daily_sharpe,
        fills=tuple(fills),
        rejected_orders=rejected_orders,
        mean_base_exposure=exposure_sum / len(bars) if measure_exposure else None,
    )


def compare_cash_flows(
    bars: Sequence[ComparisonBar],
    contributions: Sequence[CashContribution],
    execution: ExecutionAssumptions,
    *,
    include_allocation_reference: bool = False,
) -> ComparisonResult:
    """Replay independent accounts in fixed trial order and retain failed trials."""
    market = tuple(bars)
    schedule = tuple(contributions)
    _validate(market, schedule, execution)
    funding = {item.open_time_utc: item.amount_cny for item in schedule}
    total_contributed = sum(funding.values(), _ZERO)
    outcomes: list[TrialOutcome] = []
    trials = _TRIALS_WITH_ALLOCATION if include_allocation_reference else TRIALS
    for name, trend_bars in trials:
        try:
            outcomes.append(_replay_trial(
                name, trend_bars, market, funding, execution, measure_exposure=include_allocation_reference
            ))
        except (ValueError, DecimalException) as error:
            outcomes.append(TrialOutcome(
                name, ExperimentStatus.FAILED, total_contributed, CausalCheck.NOT_RUN, failure_reason=str(error)
            ))
    return ComparisonResult(schedule, tuple(outcomes))
