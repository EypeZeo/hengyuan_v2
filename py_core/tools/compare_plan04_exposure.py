"""Fit PLAN-04 buy-and-hold allocations and evaluate them on a later market snapshot."""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict
from datetime import UTC, datetime, timedelta
from decimal import Decimal
from pathlib import Path

from py_core.research.experiments import (
    ExperimentRecord,
    ExperimentRegistry,
    params_from_mapping,
    require_valid_experiment_record,
)
from py_core.research.plan04_comparison import (
    EXPOSURE_MATCH_TOLERANCE,
    ExposureEvaluation,
    ExposureFit,
    evaluate_exposure_allocations,
    fit_exposure_allocations,
)
from py_core.research.plan04_history import HistoricalMarket, load_historical_market
from py_core.tools.compare_plan04 import funding_schedule, serialize_report_value

_PROTOCOL = "plan04-exposure-match-v1"
_RISK_PROTOCOL = "plan04-matched-risk-v1"
_MONTHLY_CNY = (Decimal(0), Decimal(200), Decimal(500))
ExposureCase = tuple[Decimal, ExposureFit, ExposureEvaluation]


def _records(
    run_id: str,
    calibration: HistoricalMarket,
    evaluation: HistoricalMarket,
    cases: tuple[ExposureCase, ...],
    protocol: str,
) -> tuple[ExperimentRecord, ...]:
    observed = datetime.now(UTC)
    start = evaluation.bars[0].open_time_utc
    end = evaluation.bars[-1].open_time_utc + timedelta(hours=4)
    execution = {key: str(value) for key, value in asdict(evaluation.execution).items()}
    records = []
    for monthly, fit, evaluated in cases:
        trials = [("baseline", evaluated.baseline, fit.execution.strategy_allocation)]
        for pair in evaluated.pairs:
            trials.extend((
                (pair.selection.candidate, pair.trend, None),
                (f"{pair.selection.candidate}-matched", pair.reference, pair.selection.allocation),
            ))
        for trial, outcome, deposit_allocation in trials:
            parameters = {
                **execution,
                "symbol": evaluation.symbol,
                "monthly_cny": str(monthly),
                "initial_cny": "1000",
                "candidate": outcome.name,
                "role": trial,
                "deposit_allocation": str(deposit_allocation) if deposit_allocation is not None else None,
                "calibration_data_version": calibration.data_version,
                "calibration_start_utc": calibration.bars[0].open_time_utc.isoformat(),
                "calibration_end_utc": fit.ends_at_utc.isoformat(),
            }
            record = ExperimentRecord(
                trial_id=f"{run_id}-{monthly}-{trial}",
                family_id="STR-TREND-01",
                purpose="historical evaluation with calibration-matched reference exposure",
                status=outcome.status,
                parameters=params_from_mapping(parameters),
                data_version=evaluation.data_version,
                data_start_utc=start,
                data_end_utc=end,
                code_version=protocol,
                cost_model="spot-fee-slippage-conversion-external",
                eval_start_utc=start,
                eval_end_utc=end,
                causal_check=outcome.causal_check,
                observed_at_utc=observed,
                recorded_by="compare_plan04_exposure",
                failure_reason=outcome.failure_reason,
            )
            require_valid_experiment_record(record)
            records.append(record)
    return tuple(records)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--calibration-manifest", type=Path, required=True)
    parser.add_argument("--evaluation-manifest", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--registry", type=Path, default=Path(".var/research/experiments.jsonl"))
    parser.add_argument("--risk-diagnostics", action="store_true", help="add daily drawdown and tail return diagnostics")
    args = parser.parse_args()
    protocol = _RISK_PROTOCOL if args.risk_diagnostics else _PROTOCOL
    try:
        calibration = load_historical_market(args.calibration_manifest)
        fits = tuple(
            fit_exposure_allocations(
                calibration.bars, funding_schedule(calibration.bars, monthly), calibration.execution
            )
            for monthly in _MONTHLY_CNY
        )
        evaluation = load_historical_market(args.evaluation_manifest)
        if (evaluation.symbol, evaluation.execution) != (calibration.symbol, calibration.execution):
            raise ValueError("calibration and evaluation must use the same market and execution assumptions")
        if evaluation.data_version == calibration.data_version:
            raise ValueError("calibration and evaluation must have distinct data versions")
        cases = tuple(
            (
                monthly,
                fit,
                evaluate_exposure_allocations(
                    fit,
                    evaluation.bars,
                    funding_schedule(evaluation.bars, monthly),
                    include_risk_diagnostics=args.risk_diagnostics,
                ),
            )
            for monthly, fit in zip(_MONTHLY_CNY, fits, strict=True)
        )
        records = _records(args.run_id, calibration, evaluation, cases, protocol)
    except (OSError, ValueError, OverflowError) as error:
        parser.error(str(error))

    args.registry.parent.mkdir(parents=True, exist_ok=True)
    registry = ExperimentRegistry(args.registry)
    if any(record.trial_id in registry.trial_ids() for record in records):
        parser.error("this run ID is already registered")
    for record in records:
        registry.register(record)

    scenarios = []
    for monthly, fit, evaluated in cases:
        outcome = asdict(evaluated)
        if not args.risk_diagnostics:
            del outcome["risk_diagnostics"]
        scenarios.append({"monthly_cny": monthly, "grid": [asdict(point) for point in fit.grid], **outcome})
    report = {
        "protocol": protocol,
        "symbol": evaluation.symbol,
        "calibration_data_version": calibration.data_version,
        "evaluation_data_version": evaluation.data_version,
        "calibration_bars": len(calibration.bars),
        "evaluation_bars": len(evaluation.bars),
        "tolerance": EXPOSURE_MATCH_TOLERANCE,
        "execution": asdict(evaluation.execution),
        "scenarios": scenarios,
    }
    print(json.dumps(report, indent=2, default=serialize_report_value, allow_nan=False))


if __name__ == "__main__":
    main()
