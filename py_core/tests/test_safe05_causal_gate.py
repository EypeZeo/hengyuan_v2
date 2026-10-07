"""SAFE-05: the research causal gate and the pre-reindex input check.

Before SAFE-05 ``no_future_shift_detected`` was constant True: both engines reindexed the signal
onto the OHLCV index first and then compared that already-aligned index, and
``assert_no_lookahead`` had no production call site, so a strategy peeking ``close.shift(-1)``
ran through every entry point with ``is_valid=True``. These tests pin the repaired behaviour, one
negative control per entry point, and every control is paired with an honest strategy that must
still pass.

All outputs are backtesting estimates only.
NOT financial advice. NOT trading authorization.
"""

from __future__ import annotations

import ast
import json
from datetime import UTC, datetime, timedelta
from decimal import Decimal
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd
import pytest

import py_core
from py_core.backtests.artifact_export import serialize_validation_report
from py_core.backtests.cli import build_parser, cmd_run, cmd_run_risk_aware
from py_core.backtests.models import BacktestConfig, ValidationReport
from py_core.backtests.risk_integration import run_risk_aware_backtest
from py_core.backtests.vectorized_engine import (
    records_to_dataframe,
    run_vectorized_backtest,
    validate_inputs,
)
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)
from py_core.risk.risk_config import RiskConfig
from py_core.strategies.base import (
    DEFAULT_CAUSAL_POINTS,
    LookaheadBiasError,
    Strategy,
    causal_gate,
    checked_signals,
)
from py_core.strategies.momentum import MomentumStrategy
from py_core.strategies.sma_crossover import SmaCrossoverStrategy
from py_core.strategy_spec.schema import StrategySpecError, parse_spec
from py_core.strategy_spec.validation_sweep import run_validation_sweep
from py_core.validation.cpcv_analysis import run_cpcv_analysis
from py_core.validation.pbo import compute_pbo
from py_core.validation.splits import walk_forward_splits
from py_core.validation.walk_forward import run_walk_forward_analysis

_BASE_DT = datetime(2024, 1, 1, tzinfo=UTC)


# ---------------------------------------------------------------------------
# Data and strategies
# ---------------------------------------------------------------------------


def _records(n: int = 300, seed: int = 7) -> list[NormalizedOhlcvRecord]:
    """Deterministic random-walk OHLCV with distinct open/high/low/close."""
    rng = np.random.default_rng(seed)
    closes = 100.0 * np.exp(np.cumsum(rng.normal(0.0, 0.01, n)))
    out: list[NormalizedOhlcvRecord] = []
    prev = float(closes[0])
    for i, c in enumerate(closes):
        close = float(c)
        out.append(
            NormalizedOhlcvRecord(
                market=ManualMarket.CRYPTO_SPOT,
                symbol=CanonicalMarketSymbol("BTC/USDT"),
                timeframe=OhlcvTimeframe("1d"),
                event_time_utc=_BASE_DT + timedelta(days=i),
                open_price=Decimal(f"{prev:.6f}"),
                high_price=Decimal(f"{max(prev, close) * 1.001:.6f}"),
                low_price=Decimal(f"{min(prev, close) * 0.999:.6f}"),
                close_price=Decimal(f"{close:.6f}"),
                volume=Decimal(1000),
            )
        )
        prev = close
    return out


def _cfg() -> BacktestConfig:
    return BacktestConfig(initial_capital=100_000.0, fee_bps=5.0, slippage_bps=2.0)


def _risk_cfg() -> RiskConfig:
    return RiskConfig(
        risk_fraction=Decimal("0.5"),
        max_position_fraction=Decimal("1.0"),
        max_notional=Decimal(1000000),
        max_risk_per_trade=Decimal(1000000),
    )


class PeekNextClose(Strategy):
    """Leak 1: buys when the NEXT bar's close is higher (``shift(-1)``)."""

    def __init__(self, variant: int = 0) -> None:
        self.variant = variant

    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        future = df["close"].shift(-1)
        signal = (future > df["close"] * (1.0 + self.variant * 1e-4)).astype(float)
        return signal.mask(future.isna())


class FullSampleZScore(Strategy):
    """Leak 2: z-score against the FULL sample mean/std."""

    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        z = (df["close"] - df["close"].mean()) / df["close"].std()
        return (z > 0).astype(float)


class FullSampleMax(Strategy):
    """Leak 3: position size normalised by the maximum close over the FULL sample."""

    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        return df["close"] / df["close"].max()


class AllNan(Strategy):
    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        return pd.Series(float("nan"), index=df.index)


class ShiftedIndex(Strategy):
    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        return pd.Series(0.0, index=df.index + timedelta(days=1))


class CountingSma(SmaCrossoverStrategy):
    calls = 0

    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        type(self).calls += 1
        return super().generate_signals(df)


_PEEK = "py_core.tests.test_safe05_causal_gate:PeekNextClose"
_HONEST = "py_core.strategies.sma_crossover:SmaCrossoverStrategy"
_PEEK_GRID = [{"variant": 0}, {"variant": 1}]
_HONEST_GRID = [{"fast_window": 5, "slow_window": 20}, {"fast_window": 8, "slow_window": 30}]


def _honest() -> SmaCrossoverStrategy:
    return SmaCrossoverStrategy(fast_window=5, slow_window=20)


# ---------------------------------------------------------------------------
# The gate itself
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("leak", [PeekNextClose, FullSampleZScore, FullSampleMax])
def test_gate_rejects_each_realistic_leak(leak: type[Strategy]) -> None:
    df = records_to_dataframe(_records())
    with pytest.raises(LookaheadBiasError):
        checked_signals(leak(), df)


def test_lookahead_bias_error_is_a_value_error() -> None:
    assert issubclass(LookaheadBiasError, ValueError)


@pytest.mark.parametrize("strategy", [_honest(), MomentumStrategy(lookback=10)])
def test_gate_passes_honest_strategies_and_records_evidence(strategy: Strategy) -> None:
    df = records_to_dataframe(_records())
    signal, evidence = checked_signals(strategy, df)

    assert signal.index.equals(df.index)
    assert evidence.method == "prefix_differential"
    assert evidence.points_requested == DEFAULT_CAUSAL_POINTS == 8
    assert evidence.points_compared == 8
    positions = list(evidence.sample_positions)
    assert positions == sorted(set(positions))
    # the last bar can never reveal a future dependency, the one before it is the sharpest probe
    assert positions[-1] == len(df) - 2
    assert evidence.bar_count == len(df)
    assert len(evidence.data_digest) == 16
    assert evidence.point_in_time is None
    assert evidence.experiment_id is None
    assert evidence.blind_set_hash is None


def test_gate_samples_start_after_the_warmup() -> None:
    df = records_to_dataframe(_records())
    _, evidence = checked_signals(_honest(), df)
    assert evidence.sample_positions[0] == 19  # slow_window=20 -> first valid signal at position 19


def test_gate_with_no_testable_position_says_so() -> None:
    df = records_to_dataframe(_records(50))
    _, evidence = checked_signals(AllNan(), df)
    assert evidence.points_compared == 0
    assert evidence.note != ""


def test_gate_records_the_reserved_slots_without_using_them() -> None:
    df = records_to_dataframe(_records(80))
    _, evidence = checked_signals(
        _honest(),
        df,
        point_in_time={"t_event": "x", "t_available": "y"},
        experiment_id="exp-001",
        blind_set_hash="abc123",
    )
    context = evidence.to_context()
    assert context["reserved"] == {
        "point_in_time": {"t_event": "x", "t_available": "y"},
        "experiment_id": "exp-001",
        "blind_set_hash": "abc123",
    }
    assert context["method"] == "prefix_differential"
    assert context["sample_positions"] == list(evidence.sample_positions)
    json.dumps(context)  # must be JSON-serializable as is


def test_gate_rejects_non_positive_points() -> None:
    df = records_to_dataframe(_records(50))
    with pytest.raises(ValueError):
        checked_signals(_honest(), df, points=0)


def test_gate_still_validates_the_output_shape() -> None:
    df = records_to_dataframe(_records(50))
    with pytest.raises(ValueError) as excinfo:
        checked_signals(ShiftedIndex(), df)
    assert not isinstance(excinfo.value, LookaheadBiasError)


def test_causal_gate_returns_evidence_only() -> None:
    df = records_to_dataframe(_records(80))
    evidence = causal_gate(_honest(), df, points=4)
    assert evidence.points_compared == 4


def test_evidence_covers_windows_of_the_gated_frame_and_skips_the_differential() -> None:
    df = records_to_dataframe(_records())
    CountingSma.calls = 0
    strategy = CountingSma(fast_window=5, slow_window=20)
    _, evidence = checked_signals(strategy, df)
    gate_calls = CountingSma.calls
    assert gate_calls == 1 + evidence.points_compared

    window = df.iloc[50:150]
    signal, same = checked_signals(strategy, window, evidence=evidence)
    assert same is evidence
    assert signal.index.equals(window.index)
    assert CountingSma.calls == gate_calls + 1  # one evaluation, no differential


def test_evidence_does_not_cover_other_data() -> None:
    df = records_to_dataframe(_records())
    _, evidence = checked_signals(_honest(), df)

    tampered = df.iloc[50:150].copy()
    tampered.iloc[10, tampered.columns.get_loc("close")] *= 1.5
    with pytest.raises(LookaheadBiasError):
        checked_signals(_honest(), tampered, evidence=evidence)

    foreign = records_to_dataframe(_records(seed=99)).iloc[50:150]
    with pytest.raises(LookaheadBiasError):
        checked_signals(_honest(), foreign, evidence=evidence)


# ---------------------------------------------------------------------------
# Engines: the alignment check now looks at the raw signal index, before reindex
# ---------------------------------------------------------------------------


def _signals_and_records(n: int = 120) -> tuple[list[NormalizedOhlcvRecord], pd.Series[Any]]:
    records = _records(n)
    df = records_to_dataframe(records)
    return records, _honest().generate_signals(df)


def test_engine_rejects_a_signal_index_shifted_by_a_day() -> None:
    records, signals = _signals_and_records()
    shifted = signals.copy()
    shifted.index = shifted.index + timedelta(days=1)
    with pytest.raises(ValueError, match="不在 OHLCV index"):
        run_vectorized_backtest(_cfg(), records, shifted)


def test_engine_rejects_a_timezone_mismatched_signal_index() -> None:
    records, signals = _signals_and_records()
    naive = signals.copy()
    naive.index = naive.index.tz_localize(None)
    with pytest.raises(ValueError, match="不在 OHLCV index"):
        run_vectorized_backtest(_cfg(), records, naive)


def test_engine_rejects_duplicate_signal_timestamps() -> None:
    records, signals = _signals_and_records()
    duplicated = pd.concat([signals, signals.iloc[:1]])
    with pytest.raises(ValueError, match="重复"):
        run_vectorized_backtest(_cfg(), records, duplicated)


def test_engine_allows_a_sparse_signal_subset_but_labels_it_index_only() -> None:
    records, signals = _signals_and_records()
    result = run_vectorized_backtest(_cfg(), records, signals.iloc[::3])
    report = result.validation_report
    assert report.is_valid
    assert report.no_future_shift_detected is True
    assert report.causal_check == "index_only"
    assert report.causal_points == 0


def test_engine_report_carries_the_causal_evidence() -> None:
    records = _records()
    df = records_to_dataframe(records)
    signal, evidence = checked_signals(_honest(), df)
    result = run_vectorized_backtest(_cfg(), records, signal, causal_evidence=evidence)
    report = result.validation_report
    assert report.causal_check == "prefix_differential"
    assert report.causal_points == evidence.points_compared
    assert report.causal_context == evidence.to_context()


def test_validate_inputs_looks_at_the_raw_index() -> None:
    records, signals = _signals_and_records()
    df = records_to_dataframe(records)
    shifted = signals.copy()
    shifted.index = shifted.index + timedelta(days=1)

    bad = validate_inputs(df, shifted)
    assert bad.no_future_shift_detected is False
    assert bad.is_valid is False

    good = validate_inputs(df, signals)
    assert good.no_future_shift_detected is True
    assert good.causal_check == "index_only"


def test_risk_aware_engine_rejects_a_shifted_signal_index() -> None:
    records, signals = _signals_and_records()
    shifted = signals.copy()
    shifted.index = shifted.index + timedelta(days=1)
    with pytest.raises(ValueError, match="不在 OHLCV index"):
        run_risk_aware_backtest(_cfg(), records, shifted, _risk_cfg())


def test_risk_aware_engine_report_carries_the_causal_evidence() -> None:
    records = _records()
    df = records_to_dataframe(records)
    signal, evidence = checked_signals(_honest(), df)
    result = run_risk_aware_backtest(_cfg(), records, signal, _risk_cfg(), causal_evidence=evidence)
    assert result.base_result.validation_report.causal_check == "prefix_differential"


# ---------------------------------------------------------------------------
# Every entry point refuses a peeking strategy and still accepts an honest one
# ---------------------------------------------------------------------------


def test_walk_forward_rejects_a_peeking_strategy_and_accepts_an_honest_one() -> None:
    records = _records()
    splits = walk_forward_splits(300, train_window=100, test_window=50)
    with pytest.raises(LookaheadBiasError):
        run_walk_forward_analysis(
            _cfg(), records, _PEEK, _PEEK_GRID, splits, allow_external_strategy=True
        )
    report = run_walk_forward_analysis(_cfg(), records, _HONEST, _HONEST_GRID, splits)
    assert len(report.fold_results) == len(splits)


def test_cpcv_rejects_a_peeking_strategy_and_accepts_an_honest_one() -> None:
    records = _records()
    with pytest.raises(LookaheadBiasError):
        run_cpcv_analysis(
            _cfg(),
            records,
            _PEEK,
            _PEEK_GRID,
            n_groups=6,
            n_test_groups=2,
            allow_external_strategy=True,
        )
    report = run_cpcv_analysis(_cfg(), records, _HONEST, _HONEST_GRID, n_groups=6, n_test_groups=2)
    assert report.combination_results


def test_pbo_rejects_a_peeking_strategy_and_accepts_an_honest_one() -> None:
    records = _records()
    with pytest.raises(LookaheadBiasError):
        compute_pbo(
            _cfg(), records, _PEEK, _PEEK_GRID, n_groups=6, allow_external_strategy=True
        )
    report = compute_pbo(_cfg(), records, _HONEST, _HONEST_GRID, n_groups=6)
    assert 0.0 <= report.probability_of_backtest_overfitting <= 1.0


_SWEEP_SPEC = """
spec_version = 1
name = "safe05_sweep"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "fast"
op = "sma"
input = "close"
window = 5

[[indicators]]
id = "slow"
op = "sma"
input = "close"
window = 20

[[indicators]]
id = "entry"
op = "gt"
left = "fast"
right = "slow"

[signal]
node = "entry"
mode = "boolean"
"""


def test_validation_sweep_rejects_a_peeking_spec_strategy(monkeypatch: pytest.MonkeyPatch) -> None:
    """A parsed spec cannot peek (every operator is causal), so a leak is simulated by patching
    ``SpecStrategy.generate_signals`` -- the sweep must still refuse to continue."""
    from py_core.strategies.spec_strategy import SpecStrategy

    def leaky(self: SpecStrategy, df: pd.DataFrame) -> pd.Series[Any]:
        future = df["close"].shift(-1)
        return (future > df["close"]).astype(float).mask(future.isna())

    monkeypatch.setattr(SpecStrategy, "generate_signals", leaky)
    spec = parse_spec(_SWEEP_SPEC)
    sweep = {("fast", "param"): [5, 10], ("slow", "param"): [20, 30]}
    splits = walk_forward_splits(300, train_window=100, test_window=50)
    with pytest.raises(LookaheadBiasError):
        run_validation_sweep(
            _cfg(), _records(), spec, sweep, walk_forward_splits=splits, cpcv_n_groups=4
        )


# ---------------------------------------------------------------------------
# The CLI: both commands, strategy path and signals-file path
# ---------------------------------------------------------------------------


def _write_ohlcv_csv(path: Path, records: list[NormalizedOhlcvRecord]) -> None:
    lines = ["timestamp_utc,open,high,low,close,volume"]
    for r in records:
        lines.append(
            ",".join(
                [
                    r.event_time_utc.strftime("%Y-%m-%dT%H:%M:%SZ"),
                    str(r.open_price),
                    str(r.high_price),
                    str(r.low_price),
                    str(r.close_price),
                    str(r.volume),
                ]
            )
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _write_signals_csv(path: Path, signals: pd.Series[Any]) -> None:
    lines = ["timestamp_utc,signal"]
    for ts, value in signals.fillna(0.0).items():
        lines.append(f"{pd.Timestamp(ts).strftime('%Y-%m-%dT%H:%M:%SZ')},{float(value)}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _validation_report_json(root: Path, run_id: str) -> dict[str, Any]:
    path = root / ".var" / "backtests" / run_id / "validation_report.json"
    return json.loads(path.read_text(encoding="utf-8"))  # type: ignore[no-any-return]


def test_cli_run_refuses_a_peeking_strategy(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.chdir(tmp_path)
    csv = tmp_path / "ohlcv.csv"
    _write_ohlcv_csv(csv, _records())
    args = build_parser().parse_args(
        ["run", "--ohlcv", str(csv), "--strategy", _PEEK, "--allow-external-strategy", "--run-id", "peek"]
    )
    assert cmd_run(args) == 1
    assert "因果核验失败" in capsys.readouterr().err
    assert not (tmp_path / ".var" / "backtests" / "peek").exists()


def test_cli_run_records_the_prefix_differential_evidence(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.chdir(tmp_path)
    csv = tmp_path / "ohlcv.csv"
    _write_ohlcv_csv(csv, _records())
    args = build_parser().parse_args(
        [
            "run",
            "--ohlcv",
            str(csv),
            "--strategy",
            _HONEST,
            "--strategy-params",
            '{"fast_window": 5, "slow_window": 20}',
            "--run-id",
            "honest",
        ]
    )
    assert cmd_run(args) == 0
    report = _validation_report_json(tmp_path, "honest")
    assert report["causal_check"] == "prefix_differential"
    assert report["causal_points"] == 8
    assert report["causal_context"]["method"] == "prefix_differential"

    args = build_parser().parse_args(
        [
            "run",
            "--ohlcv",
            str(csv),
            "--strategy",
            _HONEST,
            "--strategy-params",
            '{"fast_window": 5, "slow_window": 20}',
            "--causal-points",
            "4",
            "--run-id",
            "honest4",
        ]
    )
    assert cmd_run(args) == 0
    assert _validation_report_json(tmp_path, "honest4")["causal_points"] == 4


def test_cli_run_labels_a_precomputed_signals_file_index_only(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.chdir(tmp_path)
    records = _records()
    csv = tmp_path / "ohlcv.csv"
    _write_ohlcv_csv(csv, records)
    signals_csv = tmp_path / "signals.csv"
    _write_signals_csv(signals_csv, _honest().generate_signals(records_to_dataframe(records)))
    args = build_parser().parse_args(
        ["run", "--ohlcv", str(csv), "--signals", str(signals_csv), "--run-id", "file"]
    )
    assert cmd_run(args) == 0
    out = capsys.readouterr().out
    assert "index_only" in out
    report = _validation_report_json(tmp_path, "file")
    assert report["causal_check"] == "index_only"
    assert report["causal_points"] == 0


def test_cli_run_risk_aware_refuses_a_peeking_strategy(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.chdir(tmp_path)
    csv = tmp_path / "ohlcv.csv"
    _write_ohlcv_csv(csv, _records())
    args = build_parser().parse_args(
        [
            "run-risk-aware",
            "--ohlcv",
            str(csv),
            "--strategy",
            _PEEK,
            "--allow-external-strategy",
            "--run-id",
            "peek-ra",
        ]
    )
    assert cmd_run_risk_aware(args) == 1
    assert "因果核验失败" in capsys.readouterr().err


# ---------------------------------------------------------------------------
# Parse-time rejection of a future-referencing operator
# ---------------------------------------------------------------------------

_LAG_SPEC = """
spec_version = 1
name = "lag_test"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "past"
op = "lag"
input = "close"
n = {n}

[[indicators]]
id = "entry"
op = "gt"
left = "close"
right = "past"

[signal]
node = "entry"
mode = "boolean"
"""


@pytest.mark.parametrize("n", [0, -1, -5])
def test_schema_rejects_a_lag_that_does_not_look_back(n: int) -> None:
    with pytest.raises(LookaheadBiasError) as excinfo:
        parse_spec(_LAG_SPEC.format(n=n))
    # also a StrategySpecError, so every existing handler of spec errors still applies
    assert isinstance(excinfo.value, StrategySpecError)


def test_schema_accepts_a_lag_of_one_bar() -> None:
    assert parse_spec(_LAG_SPEC.format(n=1)).name == "lag_test"


# ---------------------------------------------------------------------------
# The report and its single serializer
# ---------------------------------------------------------------------------


def test_validation_report_defaults_say_the_gate_did_not_run() -> None:
    report = ValidationReport(
        timestamp_monotonic=True, no_nan_close=True, no_future_shift_detected=True, bar_count=1
    )
    assert report.causal_check == "not_run"
    assert report.causal_points == 0
    assert report.causal_context == {}


def test_serializer_exposes_the_causal_fields() -> None:
    report = ValidationReport(
        timestamp_monotonic=True,
        no_nan_close=True,
        no_future_shift_detected=True,
        bar_count=3,
        causal_check="index_only",
    )
    payload = serialize_validation_report(report)
    assert payload["causal_check"] == "index_only"
    assert payload["causal_points"] == 0
    assert payload["causal_context"] == {}


# ---------------------------------------------------------------------------
# No production path may bypass the gate
# ---------------------------------------------------------------------------


def test_no_production_code_calls_generate_signals_outside_the_gate() -> None:
    root = Path(py_core.__file__).resolve().parent
    offenders: list[str] = []
    for path in sorted(root.rglob("*.py")):
        rel = path.relative_to(root)
        if rel.parts[0] == "tests" or ".venv" in rel.parts or "site-packages" in rel.parts:
            continue
        if rel.as_posix() == "strategies/base.py":
            continue
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if (
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Attribute)
                and node.func.attr == "generate_signals"
            ):
                offenders.append(f"{rel.as_posix()}:{node.lineno}")
    assert offenders == [], f"generate_signals() must only be called through the gate: {offenders}"
