"""Recheck the frozen PLAN-04 matched-reference tail statistic offline."""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass
from datetime import UTC, datetime
from decimal import Decimal, InvalidOperation
from pathlib import Path

import numpy as np

from py_core.research.plan04_paired_tail import (
    bootstrap_tail_bands,
    classify_tail_direction,
)
from py_core.research.plan04_risk import DailyNav, daily_simple_returns
from py_core.tools.compare_plan04_exposure import DAILY_NAV_PROTOCOL

_PROTOCOL = "plan04-paired-tail-v1"
_SYMBOLS = ("BTCUSDT", "ETHUSDT")
_MONTHLY_CNY = ("0", "200", "500")
_CANDIDATES = ("trend_14d", "trend_28d")
_START = datetime(2025, 1, 1, tzinfo=UTC)
_END = datetime(2026, 1, 1, tzinfo=UTC)
_BLOCK_LENGTHS = (7, 14, 28)
_PRIMARY_LENGTH = 14
_SEED = 20261127
_RESAMPLES = 4999


@dataclass(frozen=True, slots=True)
class PairLabel:
    symbol: str
    monthly_cny: str
    candidate: str


def _daily_column(rows: list[dict[str, str]]) -> tuple[float, ...]:
    points = tuple(DailyNav(datetime.fromisoformat(row["as_of_utc"]), Decimal(row["nav"])) for row in rows)
    if len(points) != 366 or points[0].as_of_utc != _START or points[-1].as_of_utc != _END:
        raise ValueError("daily NAV must cover every UTC date in the complete 2025 evaluation")
    return tuple(float(value) for value in daily_simple_returns(points))


def _load_family(paths: list[Path]) -> tuple[list[PairLabel], np.ndarray, list[dict[str, str]]]:
    labels = [
        PairLabel(symbol, monthly, candidate)
        for symbol in _SYMBOLS
        for monthly in _MONTHLY_CNY
        for candidate in _CANDIDATES
    ]
    columns: dict[PairLabel, tuple[tuple[float, ...], tuple[float, ...]]] = {}
    sources: dict[str, dict[str, str]] = {}
    for path in paths:
        report = json.loads(path.read_text(encoding="utf-8-sig"))
        symbol = report["symbol"]
        if report["protocol"] != DAILY_NAV_PROTOCOL or symbol not in _SYMBOLS or symbol in sources:
            raise ValueError("provide separate BTCUSDT and ETHUSDT exports with --export-daily-nav")
        sources[symbol] = {
            "symbol": symbol,
            "calibration_data_version": report["calibration_data_version"],
            "evaluation_data_version": report["evaluation_data_version"],
        }
        for scenario in report["scenarios"]:
            for row in scenario["daily_nav"]:
                label = PairLabel(symbol, scenario["monthly_cny"], row["candidate"])
                if label not in labels or label in columns:
                    raise ValueError(f"unexpected or repeated daily NAV pair {label!r}")
                columns[label] = (_daily_column(row["reference"]), _daily_column(row["trend"]))
    if set(columns) != set(labels):
        raise ValueError("daily NAV exports must contain all twelve planned pairs")
    matrix = np.asarray([column for label in labels for column in columns[label]], dtype=np.float64).T
    return labels, matrix, [sources[symbol] for symbol in _SYMBOLS]


def run_review(paths: list[Path]) -> dict:
    labels, matrix, sources = _load_family(paths)
    seeds = np.random.SeedSequence(_SEED).spawn(len(_BLOCK_LENGTHS))
    results = [
        bootstrap_tail_bands(
            matrix,
            block_length=length,
            n_resamples=_RESAMPLES,
            rng=np.random.Generator(np.random.PCG64(seed)),
        )
        for length, seed in zip(_BLOCK_LENGTHS, seeds, strict=True)
    ]
    primary = results[_BLOCK_LENGTHS.index(_PRIMARY_LENGTH)]
    pairs = []
    for index, label in enumerate(labels):
        bands = [result.bands[index] for result in results]
        estimate = primary.bands[index]
        pairs.append({
            **asdict(label),
            "trend_tail": estimate.trend_tail,
            "reference_tail": estimate.reference_tail,
            "difference": estimate.difference,
            "bands": [
                {"block_length": length, "lower": band.lower, "upper": band.upper}
                for length, band in zip(_BLOCK_LENGTHS, bands, strict=True)
            ],
            "direction": classify_tail_direction(bands),
        })
    return {
        "protocol": _PROTOCOL,
        "numpy": np.__version__,
        "seed": _SEED,
        "resamples_per_length": _RESAMPLES,
        "primary_block_length": _PRIMARY_LENGTH,
        "daily_observations": matrix.shape[0],
        "tail_observations": primary.tail_observations,
        "sources": sources,
        "family_bands": [
            {"block_length": length, "half_width": result.half_width}
            for length, result in zip(_BLOCK_LENGTHS, results, strict=True)
        ],
        "pairs": pairs,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs=2, type=Path, help="BTCUSDT and ETHUSDT daily NAV export files")
    args = parser.parse_args()
    try:
        report = run_review(args.reports)
    except (OSError, ValueError, KeyError, TypeError, InvalidOperation, OverflowError) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
