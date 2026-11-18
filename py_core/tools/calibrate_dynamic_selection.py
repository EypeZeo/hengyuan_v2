"""Calibrate a fixed dynamic model-selection procedure on synthetic paths."""

from __future__ import annotations

import argparse
import json
import math
from collections import Counter
from dataclasses import asdict, dataclass

import numpy as np

from py_core.research.dynamic_selection import run_dynamic_selection_test
from py_core.tools.calibrate_plan05 import (
    ALPHA,
    FALSE_POSITIVE_TOLERANCE,
    RETURN_SD,
    classify_rate,
    wilson_rate,
)

SEED = 20261125
REPETITIONS = 2400
RANDOMIZATIONS = 199
PERIODS = 512
CANDIDATES = 6
TRAIN_PERIODS = 96
VALIDATION_PERIODS = 32
TEST_PERIODS = 32
VOLATILITY_BLOCK = 64
POWER_TOLERANCE = (0.70, 1.0)


@dataclass(frozen=True, slots=True)
class Scenario:
    name: str
    predictor_phi: float = 0.0
    predictor_correlation: float = 0.0
    alternating_volatility: bool = False
    student_df: int | None = None
    switching_coefficient: float = 0.0


SCENARIOS = (
    Scenario("gaussian_null"),
    Scenario("dependent_predictors_volatility_null", 0.5, 0.6, alternating_volatility=True),
    Scenario("heavy_tail_null", 0.5, 0.6, student_df=5),
    Scenario("switching_predictor_alternative", 0.5, 0.6, switching_coefficient=0.006),
)


def simulated_path(scenario: Scenario, rng: np.random.Generator) -> tuple[np.ndarray, np.ndarray]:
    innovations = rng.standard_normal((PERIODS, CANDIDATES + 1))
    correlation = scenario.predictor_correlation
    x = math.sqrt(correlation) * innovations[:, :1] + math.sqrt(1 - correlation) * innovations[:, 1:]
    if scenario.predictor_phi:
        phi = scenario.predictor_phi
        innovation_scale = math.sqrt(1 - phi**2)
        for t in range(1, PERIODS):
            x[t] = phi * x[t - 1] + innovation_scale * x[t]

    if scenario.student_df is None:
        y = rng.standard_normal(PERIODS)
    else:
        df = scenario.student_df
        y = rng.standard_t(df, PERIODS) * math.sqrt((df - 2) / df)
    y *= RETURN_SD
    if scenario.alternating_volatility:
        y *= np.where((np.arange(PERIODS) // VOLATILITY_BLOCK) % 2 == 0, 0.5, 1.5)
    if scenario.switching_coefficient:
        half = PERIODS // 2
        y[:half] += scenario.switching_coefficient * x[:half, 0]
        y[half:] += scenario.switching_coefficient * x[half:, 1]
    return x, y


def calibrate_scenario(
    scenario: Scenario, repetitions: int, randomizations: int, seed: np.random.SeedSequence
) -> dict:
    data_seed, sign_seed = seed.spawn(2)
    data_rng = np.random.Generator(np.random.PCG64(data_seed))
    sign_rng = np.random.Generator(np.random.PCG64(sign_seed))
    failures: Counter[str] = Counter()
    rejected = switched = switches = 0
    payoff_sum = 0.0
    for _ in range(repetitions):
        predictors, returns = simulated_path(scenario, data_rng)
        try:
            result = run_dynamic_selection_test(
                predictors, returns, train_periods=TRAIN_PERIODS, validation_periods=VALIDATION_PERIODS,
                test_periods=TEST_PERIODS, n_randomizations=randomizations, rng=sign_rng,
            )
        except ValueError as error:
            failures[str(error)] += 1
            continue
        rejected += int(result.p_value <= ALPHA)
        switched += int(result.switch_count > 0)
        switches += result.switch_count
        payoff_sum += result.mean_payoff

    valid = repetitions - sum(failures.values())
    rate = wilson_rate(rejected, valid)
    is_alternative = scenario.switching_coefficient != 0
    tolerance = POWER_TOLERANCE if is_alternative else FALSE_POSITIVE_TOLERANCE
    return {
        "scenario": asdict(scenario),
        "failures": dict(failures),
        "valid_repetitions": valid,
        "rejection_rate": asdict(rate) if rate else None,
        "rejection_metric": "power" if is_alternative else "false_positive",
        "criterion": classify_rate(rate, tolerance),
        "runs_with_switch": switched,
        "switch_fraction": switched / valid if valid else None,
        "mean_switches": switches / valid if valid else None,
        "mean_oos_payoff": payoff_sum / valid if valid else None,
    }


def run_calibration(*, smoke: bool = False) -> dict:
    repetitions, randomizations = (12, 19) if smoke else (REPETITIONS, RANDOMIZATIONS)
    seeds = np.random.SeedSequence(SEED).spawn(len(SCENARIOS))
    rows = [
        calibrate_scenario(scenario, repetitions, randomizations, seed)
        for scenario, seed in zip(SCENARIOS, seeds, strict=True)
    ]
    supported = all(not row["failures"] and row["criterion"] == "within_tolerance" for row in rows)
    return {
        "protocol": "plan-05-dynamic-selection-v1",
        "profile": "smoke" if smoke else "formal",
        "status": "smoke" if smoke else ("within_tolerance" if supported else "not_calibrated"),
        "seed": SEED,
        "numpy": np.__version__,
        "periods": PERIODS,
        "candidates": CANDIDATES,
        "train_periods": TRAIN_PERIODS,
        "validation_periods": VALIDATION_PERIODS,
        "test_periods": TEST_PERIODS,
        "repetitions_per_scenario": repetitions,
        "randomizations_per_repetition": randomizations,
        "nominal_alpha": ALPHA,
        "return_sd": RETURN_SD,
        "false_positive_tolerance": FALSE_POSITIVE_TOLERANCE,
        "power_tolerance": POWER_TOLERANCE,
        "results": rows,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smoke", action="store_true", help="exercise the protocol on a smaller sample")
    args = parser.parse_args()
    print(json.dumps(run_calibration(smoke=args.smoke), indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
