"""回测 CLI × 实验登记接线的测试（任务 3.2：登记所有候选，含诊断性回测）。

覆盖：``run`` 与 ``run-risk-aware`` 两条子命令在 ``--strategy``（prefix_differential）
与 ``--signals``（index_only，登记合法但非准入）两条路径上的登记字段、``--trial-id``
重复的失败关闭、``--no-experiment-registry`` 退出、登记文件不可写的失败关闭。
"""

from __future__ import annotations

import json
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

import pytest

from py_core.backtests.cli import build_parser, cmd_run, cmd_run_risk_aware
from py_core.research.experiments import (
    CausalCheck,
    ExperimentRegistry,
    ExperimentStatus,
    is_admission_eligible,
)

STRATEGY_SPEC = "py_core.strategies.sma_crossover:SmaCrossoverStrategy"
BAR_COUNT = 60


def write_ohlcv_csv(path: Path, *, bars: int = BAR_COUNT) -> datetime:
    start = datetime(2024, 1, 1, tzinfo=UTC)
    lines = ["timestamp_utc,open,high,low,close,volume"]
    for day in range(bars):
        stamp = (start + timedelta(days=day)).strftime("%Y-%m-%dT%H:%M:%SZ")
        price = 100.0 + day
        lines.append(f"{stamp},{price},{price},{price},{price},1000")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return start + timedelta(days=bars - 1)


def write_signals_csv(path: Path, bars: int = BAR_COUNT) -> None:
    start = datetime(2024, 1, 1, tzinfo=UTC)
    lines = ["timestamp_utc,signal"]
    for day in range(bars):
        stamp = (start + timedelta(days=day)).strftime("%Y-%m-%dT%H:%M:%SZ")
        lines.append(f"{stamp},1")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def base_args(*, ohlcv: Path, signals: Path, registry: Path, trial_id: str) -> list[str]:
    return [
        "run",
        "--ohlcv",
        str(ohlcv),
        "--signals",
        str(signals),
        "--run-id",
        trial_id,
        "--experiment-registry",
        str(registry),
        "--trial-id",
        trial_id,
    ]


def test_run_with_signals_registers_an_index_only_trial(tmp_path: Any, monkeypatch: pytest.MonkeyPatch) -> None:
    ohlcv = tmp_path / "ohlcv.csv"
    signals = tmp_path / "signals.csv"
    registry_path = tmp_path / "experiments.jsonl"
    data_end = write_ohlcv_csv(ohlcv)
    write_signals_csv(signals)
    monkeypatch.chdir(tmp_path)

    args = build_parser().parse_args(base_args(ohlcv=ohlcv, signals=signals, registry=registry_path, trial_id="t-sig"))
    assert cmd_run(args) == 0

    registry = ExperimentRegistry(registry_path)
    records = registry.all_records()
    assert len(records) == 1
    record = records[0]
    assert record.trial_id == "t-sig"
    assert record.family_id == "signals_csv"
    assert record.status is ExperimentStatus.COMPLETED
    assert record.causal_check is CausalCheck.INDEX_ONLY
    assert record.data_start_utc == datetime(2024, 1, 1, tzinfo=UTC)
    assert record.data_end_utc == data_end
    assert record.eval_start_utc == record.data_start_utc
    assert record.eval_end_utc == record.data_end_utc
    assert record.train_start_utc is None and record.train_end_utc is None
    assert record.random_seed is None
    assert record.recorded_by == "backtests.cli"
    assert record.code_version == "signals_csv"
    assert record.cost_model == "fee_10bps_slippage_5bps"
    params = dict(record.parameters)
    assert params["market"] == "crypto_spot" and params["symbol"] == "BTC/USDT" and params["timeframe"] == "1d"
    assert not is_admission_eligible(record)


def test_run_with_strategy_registers_a_prefix_differential_trial(tmp_path: Any, monkeypatch: pytest.MonkeyPatch) -> None:
    ohlcv = tmp_path / "ohlcv.csv"
    registry_path = tmp_path / "experiments.jsonl"
    write_ohlcv_csv(ohlcv)
    monkeypatch.chdir(tmp_path)

    argv = [
        "run",
        "--ohlcv",
        str(ohlcv),
        "--strategy",
        STRATEGY_SPEC,
        "--strategy-params",
        '{"fast_window": 3, "slow_window": 5}',
        "--run-id",
        "t-strat",
        "--experiment-registry",
        str(registry_path),
        "--trial-id",
        "t-strat",
        "--experiment-family",
        "trend-family",
    ]
    assert cmd_run(build_parser().parse_args(argv)) == 0

    registry = ExperimentRegistry(registry_path)
    assert len(registry.all_records()) == 1
    record = registry.all_records()[0]
    assert record.trial_id == "t-strat"
    assert record.family_id == "trend-family"
    assert record.causal_check is CausalCheck.PREFIX_DIFFERENTIAL
    assert record.code_version.startswith("strategy_sha256:")
    params = dict(record.parameters)
    assert params["fast_window"] == "3" and params["slow_window"] == "5"
    assert is_admission_eligible(record) is False  # 无训练窗口的全样本回测不是准入证据


def test_a_rerun_must_take_a_new_trial_id(tmp_path: Any, monkeypatch: pytest.MonkeyPatch) -> None:
    ohlcv = tmp_path / "ohlcv.csv"
    signals = tmp_path / "signals.csv"
    registry_path = tmp_path / "experiments.jsonl"
    write_ohlcv_csv(ohlcv)
    write_signals_csv(signals)
    monkeypatch.chdir(tmp_path)
    argv = base_args(ohlcv=ohlcv, signals=signals, registry=registry_path, trial_id="t-dup")

    assert cmd_run(build_parser().parse_args(argv)) == 0
    # 重跑换新的 run_id（产物目录不同），但沿用旧 trial_id：登记必须拒绝。
    rerun = list(argv)
    rerun[argv.index("--run-id") + 1] = "t-dup-2"
    assert cmd_run(build_parser().parse_args(rerun)) == 1
    assert len(ExperimentRegistry(registry_path).all_records()) == 1


def test_no_experiment_registry_writes_nothing(tmp_path: Any, monkeypatch: pytest.MonkeyPatch) -> None:
    ohlcv = tmp_path / "ohlcv.csv"
    signals = tmp_path / "signals.csv"
    registry_path = tmp_path / "experiments.jsonl"
    write_ohlcv_csv(ohlcv)
    write_signals_csv(signals)
    monkeypatch.chdir(tmp_path)

    argv = base_args(ohlcv=ohlcv, signals=signals, registry=registry_path, trial_id="t-off")
    argv.append("--no-experiment-registry")
    assert cmd_run(build_parser().parse_args(argv)) == 0
    assert not registry_path.exists()


def test_an_unwritable_registry_fails_loudly_after_artifacts_are_saved(
    tmp_path: Any, monkeypatch: pytest.MonkeyPatch
) -> None:
    ohlcv = tmp_path / "ohlcv.csv"
    signals = tmp_path / "signals.csv"
    # 台账路径指向一个目录：打开追加句柄必然失败（Windows PermissionError /
    # POSIX IsADirectoryError），是跨平台的"登记不可写"故障注入。
    not_a_file = tmp_path / "registry-is-a-directory"
    not_a_file.mkdir()
    write_ohlcv_csv(ohlcv)
    write_signals_csv(signals)
    monkeypatch.chdir(tmp_path)

    argv = base_args(ohlcv=ohlcv, signals=signals, registry=not_a_file, trial_id="t-io")
    assert cmd_run(build_parser().parse_args(argv)) == 1
    # 产物先落盘：失败只拦登记，不销毁已保存的结果。
    assert (tmp_path / ".var" / "backtests" / "t-io").is_dir()


def test_run_risk_aware_registers_the_same_way(tmp_path: Any, monkeypatch: pytest.MonkeyPatch) -> None:
    ohlcv = tmp_path / "ohlcv.csv"
    signals = tmp_path / "signals.csv"
    registry_path = tmp_path / "experiments.jsonl"
    write_ohlcv_csv(ohlcv)
    write_signals_csv(signals)
    monkeypatch.chdir(tmp_path)

    argv = [
        "run-risk-aware",
        "--ohlcv",
        str(ohlcv),
        "--signals",
        str(signals),
        "--run-id",
        "t-risk",
        "--experiment-registry",
        str(registry_path),
        "--trial-id",
        "t-risk",
    ]
    assert cmd_run_risk_aware(build_parser().parse_args(argv)) == 0

    registry = ExperimentRegistry(registry_path)
    assert len(registry.all_records()) == 1
    record = registry.all_records()[0]
    assert record.trial_id == "t-risk"
    assert record.purpose.startswith("run-risk-aware:")
    assert record.causal_check is CausalCheck.INDEX_ONLY
    raw = [json.loads(line) for line in registry_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    assert len(raw) == 1 and raw[0]["status"] == "completed"
