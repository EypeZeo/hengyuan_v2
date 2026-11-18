"""Run the offline PLAN-04 funding scenarios on synthetic data or a local historical snapshot."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict
from datetime import UTC, datetime, timedelta
from decimal import Decimal
from itertools import pairwise
from pathlib import Path

from py_core.research.experiments import (
    ExperimentRecord,
    ExperimentRegistry,
    ExperimentStatus,
    params_from_mapping,
    require_valid_experiment_record,
)
from py_core.research.plan04_comparison import (
    ALLOCATION_REFERENCE,
    TRIALS,
    CashContribution,
    ComparisonBar,
    ComparisonResult,
    ExecutionAssumptions,
    TrialOutcome,
    compare_cash_flows,
)
from py_core.research.plan04_history import HistoricalMarket, load_historical_market
from py_core.strategies.trend import MAX_HOLD_BARS_4H_21D

_PROTOCOL = "plan04-matched-funding-v1"
_ALLOCATION_PROTOCOL = "plan04-allocation-reference-v1"
_EXECUTION = ExecutionAssumptions(
    price_tick=Decimal("0.01"),
    quantity_step=Decimal("0.0001"),
    min_notional=Decimal(5),
    conversion_fee_rate=Decimal("0.002"),
    slippage_rate=Decimal("0.0005"),
    strategy_monthly_cost_cny=Decimal(30),
)
_MONTHLY_CNY = (Decimal(0), Decimal(200), Decimal(500))
_INITIAL_CNY = Decimal(1000)


def synthetic_market() -> tuple[ComparisonBar, ...]:
    """Create 150 days with rising and falling prices and a changing conversion rate."""
    start = datetime(2025, 1, 1, tzinfo=UTC)
    levels = (100, 172, 88, 196, 148)
    bars: list[ComparisonBar] = []
    previous = Decimal(levels[0])
    for index in range(150 * 6):
        leg, position = divmod(index, 240)
        step = Decimal(levels[leg + 1] - levels[leg]) / 240
        close = Decimal(levels[leg]) + step * (position + 1)
        stamp = start + timedelta(hours=4 * index)
        rate = Decimal(7) + Decimal("0.02") * (stamp.month - 1)
        bars.append(ComparisonBar(stamp, previous, close, rate))
        previous = close
    return tuple(bars)


def funding_schedule(bars: tuple[ComparisonBar, ...], monthly: Decimal) -> tuple[CashContribution, ...]:
    start = CashContribution(bars[0].open_time_utc, _INITIAL_CNY)
    if monthly == 0:
        return (start,)
    later = tuple(
        CashContribution(current.open_time_utc, monthly)
        for previous, current in pairwise(bars)
        if previous.open_time_utc.month != current.open_time_utc.month
    )
    return (start, *later)


def _delta(left: Decimal | None, right: Decimal | None) -> Decimal | None:
    return left - right if left is not None and right is not None else None


def _relative_metrics(
    outcome: TrialOutcome, reference: TrialOutcome, *, include_exposure: bool
) -> dict[str, Decimal | str | None]:
    metrics = {
        "candidate": outcome.name,
        "reference": reference.name,
        "terminal_cny_difference": _delta(outcome.terminal_cny, reference.terminal_cny),
        "all_in_pnl_cny_difference": _delta(outcome.all_in_pnl_cny, reference.all_in_pnl_cny),
        "daily_mean_return_difference": _delta(outcome.daily_mean_return, reference.daily_mean_return),
        "daily_sharpe_difference": _delta(outcome.daily_sharpe, reference.daily_sharpe),
    }

    if include_exposure:
        metrics["max_drawdown_difference"] = _delta(outcome.max_drawdown, reference.max_drawdown)
        metrics["mean_base_exposure_difference"] = _delta(outcome.mean_base_exposure, reference.mean_base_exposure)
    return metrics


def _scenario_report(monthly: Decimal, result: ComparisonResult, *, include_exposure: bool) -> dict:
    report = asdict(result)
    if not include_exposure:
        for outcome in report["outcomes"]:
            del outcome["mean_base_exposure"]
    return {
        "monthly_cny": monthly,
        **report,
        "comparisons": [
            _relative_metrics(outcome, reference, include_exposure=include_exposure)
            for outcome in result.outcomes
            if outcome.name.startswith("trend_")
            for reference in result.outcomes
            if not reference.name.startswith("trend_")
        ],
    }


def _trial_records(
    run_id: str,
    bars: tuple[ComparisonBar, ...],
    cases: tuple[tuple[Decimal, ComparisonResult], ...],
    execution: ExecutionAssumptions,
    market: HistoricalMarket | None,
    protocol: str,
) -> tuple[ExperimentRecord, ...]:
    observed = datetime.now(UTC)
    start, end = bars[0].open_time_utc, bars[-1].open_time_utc + timedelta(hours=4)
    windows = dict(TRIALS) | {ALLOCATION_REFERENCE: None}
    records = []
    for monthly, result in cases:
        for outcome in result.outcomes:
            trend_bars = windows[outcome.name]
            parameters = {
                **{key: str(value) for key, value in asdict(execution).items()},
                "candidate": outcome.name,
                "initial_cny": str(_INITIAL_CNY),
                "monthly_cny": str(monthly),
                "timeframe": "4h",
                "trend_bars": trend_bars,
                "max_hold_bars": MAX_HOLD_BARS_4H_21D if trend_bars is not None else None,
            }
            if market is not None:
                parameters["symbol"] = market.symbol
                parameters["max_rate_age_hours"] = market.max_rate_age_hours
            record = ExperimentRecord(
                trial_id=f"{run_id}-{monthly}-{outcome.name}",
                family_id="STR-TREND-01",
                purpose="historical matched contribution diagnostic" if market else "synthetic matched contribution comparison",
                status=outcome.status,
                parameters=params_from_mapping(parameters),
                data_version=market.data_version if market else "synthetic-market-v1",
                data_start_utc=start,
                data_end_utc=end,
                code_version=protocol,
                cost_model="spot-fee-slippage-conversion-external",
                eval_start_utc=start,
                eval_end_utc=end,
                causal_check=outcome.causal_check,
                observed_at_utc=observed,
                recorded_by="compare_plan04",
                failure_reason=outcome.failure_reason,
            )
            require_valid_experiment_record(record)
            records.append(record)
    return tuple(records)


def serialize_report_value(value: object) -> str:
    if isinstance(value, Decimal):
        return str(value)
    if isinstance(value, datetime):
        return value.isoformat()
    raise TypeError(f"cannot serialize {type(value).__name__}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-id", required=True, help="unique prefix for the registered trials")
    parser.add_argument("--registry", type=Path, default=Path(".var/research/experiments.jsonl"))
    parser.add_argument("--historical-manifest", type=Path, help="validated local market and exchange-rate files")
    parser.add_argument(
        "--allocation-reference", action="store_true", help="add buy and hold at the strategy's funding allocation"
    )
    args = parser.parse_args()
    protocol = _ALLOCATION_PROTOCOL if args.allocation_reference else _PROTOCOL
    market = None
    if args.historical_manifest is not None:
        try:
            market = load_historical_market(args.historical_manifest)
        except (OSError, ValueError, OverflowError) as error:
            parser.error(str(error))
    bars = market.bars if market else synthetic_market()
    execution = market.execution if market else _EXECUTION
    cases = tuple(
        (
            monthly,
            compare_cash_flows(
                bars, funding_schedule(bars, monthly), execution,
                include_allocation_reference=args.allocation_reference,
            ),
        )
        for monthly in _MONTHLY_CNY
    )
    records = _trial_records(args.run_id, bars, cases, execution, market, protocol)
    args.registry.parent.mkdir(parents=True, exist_ok=True)
    registry = ExperimentRegistry(args.registry)
    if any(record.trial_id in registry.trial_ids() for record in records):
        parser.error("this run ID is already registered")
    for record in records:
        registry.register(record)

    failed = any(record.status is ExperimentStatus.FAILED for record in records)
    report = {
        "protocol": protocol,
        "data": "historical" if market else "synthetic",
        "bars": len(bars),
        "execution": asdict(execution),
        "status": "failed_trials" if failed else "completed",
        "scenarios": [
            _scenario_report(monthly, result, include_exposure=args.allocation_reference)
            for monthly, result in cases
        ],
    }
    if market is not None:
        report.update(
            symbol=market.symbol,
            data_version=market.data_version,
            max_rate_age_hours=market.max_rate_age_hours,
            start_utc=bars[0].open_time_utc,
            end_utc=bars[-1].open_time_utc + timedelta(hours=4),
        )
    print(json.dumps(report, indent=2, default=serialize_report_value, allow_nan=False))
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
