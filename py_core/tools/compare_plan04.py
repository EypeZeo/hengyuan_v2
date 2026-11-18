"""Run the offline PLAN-04 funding scenarios on a deterministic synthetic market."""

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
    TRIALS,
    CashContribution,
    ComparisonBar,
    ComparisonResult,
    ExecutionAssumptions,
    TrialOutcome,
    compare_cash_flows,
)
from py_core.strategies.trend import MAX_HOLD_BARS_4H_21D

_PROTOCOL = "plan04-matched-funding-v1"
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


def _relative_metrics(outcome: TrialOutcome, reference: TrialOutcome) -> dict[str, Decimal | str | None]:
    return {
        "candidate": outcome.name,
        "reference": reference.name,
        "terminal_cny_difference": _delta(outcome.terminal_cny, reference.terminal_cny),
        "all_in_pnl_cny_difference": _delta(outcome.all_in_pnl_cny, reference.all_in_pnl_cny),
        "daily_mean_return_difference": _delta(outcome.daily_mean_return, reference.daily_mean_return),
        "daily_sharpe_difference": _delta(outcome.daily_sharpe, reference.daily_sharpe),
    }


def _trial_records(
    run_id: str,
    bars: tuple[ComparisonBar, ...],
    cases: tuple[tuple[Decimal, ComparisonResult], ...],
) -> tuple[ExperimentRecord, ...]:
    observed = datetime.now(UTC)
    start, end = bars[0].open_time_utc, bars[-1].open_time_utc + timedelta(hours=4)
    windows = dict(TRIALS)
    records = []
    for monthly, result in cases:
        for outcome in result.outcomes:
            trend_bars = windows[outcome.name]
            parameters = {
                **{key: str(value) for key, value in asdict(_EXECUTION).items()},
                "candidate": outcome.name,
                "initial_cny": str(_INITIAL_CNY),
                "monthly_cny": str(monthly),
                "timeframe": "4h",
                "trend_bars": trend_bars,
                "max_hold_bars": MAX_HOLD_BARS_4H_21D if trend_bars is not None else None,
            }
            record = ExperimentRecord(
                trial_id=f"{run_id}-{monthly}-{outcome.name}",
                family_id="STR-TREND-01",
                purpose="synthetic matched contribution comparison",
                status=outcome.status,
                parameters=params_from_mapping(parameters),
                data_version="synthetic-market-v1",
                data_start_utc=start,
                data_end_utc=end,
                code_version=_PROTOCOL,
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


def _serialize(value: object) -> str:
    if isinstance(value, Decimal):
        return str(value)
    if isinstance(value, datetime):
        return value.isoformat()
    raise TypeError(f"cannot serialize {type(value).__name__}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-id", required=True, help="unique prefix for the registered trials")
    parser.add_argument("--registry", type=Path, default=Path(".var/research/experiments.jsonl"))
    args = parser.parse_args()
    bars = synthetic_market()
    cases = tuple(
        (monthly, compare_cash_flows(bars, funding_schedule(bars, monthly), _EXECUTION))
        for monthly in _MONTHLY_CNY
    )
    records = _trial_records(args.run_id, bars, cases)
    args.registry.parent.mkdir(parents=True, exist_ok=True)
    registry = ExperimentRegistry(args.registry)
    if any(record.trial_id in registry.trial_ids() for record in records):
        parser.error("this run ID is already registered")
    for record in records:
        registry.register(record)

    failed = any(record.status is ExperimentStatus.FAILED for record in records)
    report = {
        "protocol": _PROTOCOL,
        "data": "synthetic",
        "bars": len(bars),
        "execution": asdict(_EXECUTION),
        "status": "failed_trials" if failed else "completed",
        "scenarios": [
            {
                "monthly_cny": monthly,
                **asdict(result),
                "comparisons": [
                    _relative_metrics(outcome, reference)
                    for outcome in result.outcomes
                    if outcome.name.startswith("trend_")
                    for reference in result.outcomes
                    if not reference.name.startswith("trend_")
                ],
            }
            for monthly, result in cases
        ],
    }
    print(json.dumps(report, indent=2, default=_serialize, allow_nan=False))
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
