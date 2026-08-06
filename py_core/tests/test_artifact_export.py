"""
tests/unit/research/backtests/test_artifact_export.py

P2-RM-03 artifact_export 模块单元测试。

所有测试均为研究用途验证，不代表交易授权、实盘结果或干跑批准。
"""

from __future__ import annotations

import json
from datetime import UTC, datetime
from decimal import Decimal
from pathlib import Path
from typing import Any
from unittest import mock

import pandas as pd
import pytest
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)
from py_core.backtests.artifact_export import (
    _decimal_to_float,
    _dumps,
    _dumps_line,
    compute_decision_counts,
    config_decimal_str,
    export_risk_aware_backtest_artifacts,
    serialize_config,
    serialize_metrics,
    serialize_risk_decision_line,
    serialize_summary,
    serialize_validation_report,
)
from py_core.backtests.models import (
    BacktestConfig,
    BacktestMetrics,
    ValidationReport,
)
from py_core.backtests.risk_integration import RiskAwareBacktestResult, run_risk_aware_backtest
from py_core.risk.risk_config import RiskConfig
from py_core.risk.risk_decision_artifact import (
    RiskDecisionArtifact,
    RiskDecisionStatus,
)

# ─────────────────────────────── helpers ─────────────────────────────────────


def _rec(day: int, close_p: float) -> NormalizedOhlcvRecord:
    ts = datetime(2024, 1, day, tzinfo=UTC)
    return NormalizedOhlcvRecord(
        market=ManualMarket.CRYPTO_SPOT,
        symbol=CanonicalMarketSymbol("BTC/USDT"),
        timeframe=OhlcvTimeframe("1d"),
        event_time_utc=ts,
        open_price=Decimal(str(close_p)),
        high_price=Decimal(str(close_p)),
        low_price=Decimal(str(close_p)),
        close_price=Decimal(str(close_p)),
        volume=Decimal("1000"),
    )


def _decision(
    status: RiskDecisionStatus = RiskDecisionStatus.ACCEPTED,
    cap_reason: str | None = None,
    warnings: list[str] | None = None,
) -> RiskDecisionArtifact:
    return RiskDecisionArtifact(
        proposed_position_size=Decimal("1000"),
        capped_position_size=Decimal("1000"),
        risk_used=Decimal("200"),
        cap_reason=cap_reason,
        status=status,
        warnings=warnings or [],
    )


def _metrics(label: str = "backtesting estimates only") -> BacktestMetrics:
    return BacktestMetrics(
        total_return=0.05,
        annualized_return=0.04,
        annualized_volatility=0.12,
        sharpe_ratio=0.33,
        max_drawdown=-0.08,
        calmar_ratio=0.5,
        win_rate=0.55,
        exposure=0.6,
        turnover=0.3,
        cost_impact_bps=8.0,
        cost_impact_total=80.0,
        output_label=label,
    )


def _vr() -> ValidationReport:
    return ValidationReport(
        timestamp_monotonic=True,
        no_nan_close=True,
        no_future_shift_detected=True,
        bar_count=5,
    )


def _cfg(**kwargs: Any) -> BacktestConfig:
    defaults: dict[str, Any] = {
        "initial_capital": 100000.0,
        "fee_bps": 10.0,
        "slippage_bps": 5.0,
    }
    defaults.update(kwargs)
    return BacktestConfig(**defaults)


def _risk_cfg(**kwargs: Any) -> RiskConfig:
    defaults: dict[str, Any] = {
        "risk_fraction": Decimal("0.02"),
        "max_position_fraction": Decimal("0.20"),
        "max_notional": Decimal("20000"),
        "max_risk_per_trade": Decimal("0.01"),
    }
    defaults.update(kwargs)
    return RiskConfig(**defaults)


def _make_raa_result() -> tuple[RiskAwareBacktestResult, BacktestConfig, RiskConfig]:
    """用 run_risk_aware_backtest 生成真实的 RiskAwareBacktestResult。"""
    records: list[NormalizedOhlcvRecord] = [_rec(i + 1, 45000.0 + i * 100) for i in range(5)]
    signals: pd.Series[float] = pd.Series(
        [1.0, 1.0, 0.0, 1.0, 1.0],
        index=pd.DatetimeIndex([r.event_time_utc for r in records]),
    )
    config: BacktestConfig = _cfg()
    risk_config: RiskConfig = _risk_cfg()
    result: RiskAwareBacktestResult = run_risk_aware_backtest(config, records, signals, risk_config)
    return result, config, risk_config


# ─────────────────────────── _decimal_to_float ───────────────────────────────


def test_decimal_to_float_converts() -> None:
    assert _decimal_to_float(Decimal("1.5")) == 1.5


def test_decimal_to_float_raises_on_non_decimal() -> None:
    with pytest.raises(TypeError):
        _decimal_to_float("not a decimal")


# ─────────────────────────── _dumps / _dumps_line ────────────────────────────


def test_dumps_handles_decimal() -> None:
    result: str = _dumps({"value": Decimal("3.14")})
    data = json.loads(result)
    assert abs(data["value"] - 3.14) < 1e-10


def test_dumps_line_is_single_line() -> None:
    result: str = _dumps_line({"key": "value"})
    assert "\n" not in result


def test_dumps_line_handles_decimal() -> None:
    result: str = _dumps_line({"d": Decimal("0.1")})
    assert json.loads(result)["d"] == pytest.approx(0.1)


# ─────────────────────────── config_decimal_str ──────────────────────────────


def test_config_decimal_str_normalizes() -> None:
    # trailing zeros stripped
    assert config_decimal_str(Decimal("1.00")) == "1"
    assert config_decimal_str(Decimal("0.020")) == "0.02"


# ─────────────────────────── serialize_config ────────────────────────────────


def test_serialize_config_contains_non_authorizing() -> None:
    cfg: BacktestConfig = _cfg()
    risk_cfg: RiskConfig = _risk_cfg()
    result: dict[str, Any] = serialize_config("abc123", cfg, risk_cfg)
    assert result["non_authorizing"] is True


def test_serialize_config_output_label() -> None:
    cfg: BacktestConfig = _cfg()
    risk_cfg: RiskConfig = _risk_cfg()
    result: dict[str, Any] = serialize_config("abc123", cfg, risk_cfg)
    assert "NON-AUTHORIZING" in result["output_label"]


def test_serialize_config_run_id() -> None:
    cfg: BacktestConfig = _cfg()
    risk_cfg: RiskConfig = _risk_cfg()
    result: dict[str, Any] = serialize_config("myrunid", cfg, risk_cfg)
    assert result["run_id"] == "myrunid"


def test_serialize_config_stop_distance() -> None:
    cfg: BacktestConfig = _cfg()
    risk_cfg: RiskConfig = _risk_cfg()
    result: dict[str, Any] = serialize_config("x", cfg, risk_cfg, stop_distance=Decimal("500"))
    assert result["risk_config"]["stop_distance"] == "500"


def test_serialize_config_stop_distance_none() -> None:
    cfg: BacktestConfig = _cfg()
    risk_cfg: RiskConfig = _risk_cfg()
    result: dict[str, Any] = serialize_config("x", cfg, risk_cfg)
    assert result["risk_config"]["stop_distance"] is None


def test_serialize_config_paths() -> None:
    cfg: BacktestConfig = _cfg()
    risk_cfg: RiskConfig = _risk_cfg()
    result: dict[str, Any] = serialize_config(
        "x", cfg, risk_cfg, ohlcv_path="/a/b/ohlcv.csv", signals_path="/c/sig.csv"
    )
    assert result["inputs"]["ohlcv_path"] == "/a/b/ohlcv.csv"
    assert result["inputs"]["signals_path"] == "/c/sig.csv"


# ─────────────────────────── serialize_metrics ───────────────────────────────


def test_serialize_metrics_has_non_authorizing() -> None:
    m: BacktestMetrics = _metrics()
    result: dict[str, Any] = serialize_metrics(m)
    assert result["non_authorizing"] is True


def test_serialize_metrics_total_return() -> None:
    m: BacktestMetrics = _metrics()
    result: dict[str, Any] = serialize_metrics(m)
    assert result["total_return"] == pytest.approx(0.05)


def test_serialize_metrics_json_serializable() -> None:
    m: BacktestMetrics = _metrics()
    result: dict[str, Any] = serialize_metrics(m)
    # should not raise
    json.dumps(result)


# ─────────────────────────── serialize_validation_report ─────────────────────


def test_serialize_validation_report_fields() -> None:
    vr: ValidationReport = _vr()
    result: dict[str, Any] = serialize_validation_report(vr)
    assert result["non_authorizing"] is True
    assert result["is_valid"] is True
    assert result["bar_count"] == 5


def test_serialize_validation_report_json_serializable() -> None:
    vr: ValidationReport = _vr()
    result: dict[str, Any] = serialize_validation_report(vr)
    json.dumps(result)


# ─────────────────────────── serialize_risk_decision_line ────────────────────


def test_serialize_risk_decision_line_schema() -> None:
    ts = datetime(2024, 1, 1, tzinfo=UTC)
    d: RiskDecisionArtifact = _decision(RiskDecisionStatus.ACCEPTED)
    result: dict[str, Any] = serialize_risk_decision_line(d, ts)
    assert result["non_authorizing"] is True
    assert result["status"] in (
        str(RiskDecisionStatus.ACCEPTED),
        str(RiskDecisionStatus.CAPPED),
        str(RiskDecisionStatus.REJECTED),
    )
    assert "proposed_position_size" in result
    assert "capped_position_size" in result
    assert "risk_used" in result
    assert "cap_reason" in result
    assert "warnings" in result
    assert "timestamp" in result


def test_serialize_risk_decision_line_timestamp_iso() -> None:
    ts = datetime(2024, 1, 5, tzinfo=UTC)
    d: RiskDecisionArtifact = _decision()
    result: dict[str, Any] = serialize_risk_decision_line(d, ts)
    # ISO 8601 with UTC offset
    assert "2024-01-05" in result["timestamp"]


def test_serialize_risk_decision_line_json_serializable() -> None:
    ts = datetime(2024, 1, 1, tzinfo=UTC)
    d: RiskDecisionArtifact = _decision(
        RiskDecisionStatus.CAPPED, cap_reason="max_position_fraction"
    )
    result: dict[str, Any] = serialize_risk_decision_line(d, ts)
    json.dumps(result)


def test_serialize_risk_decision_line_rejected() -> None:
    ts = datetime(2024, 1, 1, tzinfo=UTC)
    d: RiskDecisionArtifact = _decision(RiskDecisionStatus.REJECTED, cap_reason="no_position")
    result: dict[str, Any] = serialize_risk_decision_line(d, ts)
    assert result["status"] == str(RiskDecisionStatus.REJECTED)
    assert result["cap_reason"] == "no_position"


# ─────────────────────────── compute_decision_counts ─────────────────────────


def test_compute_decision_counts_empty() -> None:
    counts: dict[str, int] = compute_decision_counts([])
    assert counts[str(RiskDecisionStatus.ACCEPTED)] == 0
    assert counts[str(RiskDecisionStatus.CAPPED)] == 0
    assert counts[str(RiskDecisionStatus.REJECTED)] == 0


def test_compute_decision_counts_mixed() -> None:
    decisions: list[RiskDecisionArtifact] = [
        _decision(RiskDecisionStatus.ACCEPTED),
        _decision(RiskDecisionStatus.ACCEPTED),
        _decision(RiskDecisionStatus.CAPPED, cap_reason="x"),
        _decision(RiskDecisionStatus.REJECTED),
    ]
    counts: dict[str, int] = compute_decision_counts(decisions)
    assert counts[str(RiskDecisionStatus.ACCEPTED)] == 2
    assert counts[str(RiskDecisionStatus.CAPPED)] == 1
    assert counts[str(RiskDecisionStatus.REJECTED)] == 1


# ─────────────────────────── serialize_summary ───────────────────────────────


def test_serialize_summary_non_authorizing() -> None:
    result, config, risk_config = _make_raa_result()
    summary: dict[str, Any] = serialize_summary("testrun", result)
    assert summary["non_authorizing"] is True


def test_serialize_summary_output_label() -> None:
    result, config, risk_config = _make_raa_result()
    summary: dict[str, Any] = serialize_summary("testrun", result)
    assert "NON-AUTHORIZING" in summary["output_label"]


def test_serialize_summary_decision_counts_keys() -> None:
    result, config, risk_config = _make_raa_result()
    summary: dict[str, Any] = serialize_summary("testrun", result)
    counts = summary["decision_counts"]
    assert str(RiskDecisionStatus.ACCEPTED) in counts
    assert str(RiskDecisionStatus.CAPPED) in counts
    assert str(RiskDecisionStatus.REJECTED) in counts


def test_serialize_summary_decision_counts_sum() -> None:
    result, config, risk_config = _make_raa_result()
    summary: dict[str, Any] = serialize_summary("testrun", result)
    counts = summary["decision_counts"]
    total: int = sum(counts.values())
    assert total == len(result.risk_decisions)


def test_serialize_summary_run_id() -> None:
    result, config, risk_config = _make_raa_result()
    summary: dict[str, Any] = serialize_summary("myrun123", result)
    assert summary["run_id"] == "myrun123"


def test_serialize_summary_json_serializable() -> None:
    result, config, risk_config = _make_raa_result()
    summary: dict[str, Any] = serialize_summary("testrun", result)
    json.dumps(summary)


# ─────────────────────────── export_risk_aware_backtest_artifacts ─────────────


def test_export_creates_run_dir(tmp_path: Path) -> None:
    result, config, risk_config = _make_raa_result()
    run_dir: Path = export_risk_aware_backtest_artifacts(
        run_id="testrun",
        result=result,
        config=config,
        risk_config=risk_config,
        output_dir=tmp_path,
    )
    assert run_dir.is_dir()
    assert run_dir.name == "testrun"


def test_export_all_files_present(tmp_path: Path) -> None:
    result, config, risk_config = _make_raa_result()
    run_dir: Path = export_risk_aware_backtest_artifacts(
        run_id="testrun",
        result=result,
        config=config,
        risk_config=risk_config,
        output_dir=tmp_path,
    )
    expected_files: set[str] = {
        "config.json",
        "base_metrics.json",
        "risk_metrics.json",
        "risk_equity_curve.jsonl",
        "risk_positions.jsonl",
        "risk_decisions.jsonl",
        "summary.json",
        "validation_report.json",
    }
    actual_files: set[str] = {f.name for f in run_dir.iterdir() if f.is_file()}
    assert expected_files == actual_files


def test_export_config_json_non_authorizing(tmp_path: Path) -> None:
    result, config, risk_config = _make_raa_result()
    export_risk_aware_backtest_artifacts(
        run_id="x",
        result=result,
        config=config,
        risk_config=risk_config,
        output_dir=tmp_path,
    )
    data = json.loads((tmp_path / "x" / "config.json").read_text(encoding="utf-8"))
    assert data["non_authorizing"] is True
    assert "NON-AUTHORIZING" in data["output_label"]


def test_export_summary_json_non_authorizing(tmp_path: Path) -> None:
    result, config, risk_config = _make_raa_result()
    export_risk_aware_backtest_artifacts(
        run_id="x",
        result=result,
        config=config,
        risk_config=risk_config,
        output_dir=tmp_path,
    )
    data = json.loads((tmp_path / "x" / "summary.json").read_text(encoding="utf-8"))
    assert data["non_authorizing"] is True


def test_export_risk_decisions_jsonl_line_count(tmp_path: Path) -> None:
    result, config, risk_config = _make_raa_result()
    export_risk_aware_backtest_artifacts(
        run_id="x",
        result=result,
        config=config,
        risk_config=risk_config,
        output_dir=tmp_path,
    )
    lines: list[str] = (
        (tmp_path / "x" / "risk_decisions.jsonl").read_text(encoding="utf-8").strip().split("\n")
    )
    assert len(lines) == len(result.risk_decisions)


def test_export_risk_decisions_jsonl_schema(tmp_path: Path) -> None:
    result, config, risk_config = _make_raa_result()
    export_risk_aware_backtest_artifacts(
        run_id="x",
        result=result,
        config=config,
        risk_config=risk_config,
        output_dir=tmp_path,
    )
    lines: list[str] = (
        (tmp_path / "x" / "risk_decisions.jsonl").read_text(encoding="utf-8").strip().split("\n")
    )
    required_keys: set[str] = {
        "timestamp",
        "status",
        "proposed_position_size",
        "capped_position_size",
        "risk_used",
        "cap_reason",
        "warnings",
        "non_authorizing",
    }
    for line in lines:
        row = json.loads(line)
        assert required_keys.issubset(row.keys())
        assert row["non_authorizing"] is True


def test_export_risk_equity_curve_jsonl_line_count(tmp_path: Path) -> None:
    result, config, risk_config = _make_raa_result()
    export_risk_aware_backtest_artifacts(
        run_id="x",
        result=result,
        config=config,
        risk_config=risk_config,
        output_dir=tmp_path,
    )
    lines: list[str] = (
        (tmp_path / "x" / "risk_equity_curve.jsonl").read_text(encoding="utf-8").strip().split("\n")
    )
    assert len(lines) == len(result.risk_equity_curve)


def test_export_base_metrics_json_serializable(tmp_path: Path) -> None:
    result, config, risk_config = _make_raa_result()
    export_risk_aware_backtest_artifacts(
        run_id="x",
        result=result,
        config=config,
        risk_config=risk_config,
        output_dir=tmp_path,
    )
    data = json.loads((tmp_path / "x" / "base_metrics.json").read_text(encoding="utf-8"))
    assert data["non_authorizing"] is True
    assert "total_return" in data


def test_export_validation_report_present(tmp_path: Path) -> None:
    result, config, risk_config = _make_raa_result()
    export_risk_aware_backtest_artifacts(
        run_id="x",
        result=result,
        config=config,
        risk_config=risk_config,
        output_dir=tmp_path,
    )
    data = json.loads((tmp_path / "x" / "validation_report.json").read_text(encoding="utf-8"))
    assert data["non_authorizing"] is True
    assert "is_valid" in data


def test_export_idempotent_different_run_ids(tmp_path: Path) -> None:
    result, config, risk_config = _make_raa_result()
    export_risk_aware_backtest_artifacts(
        run_id="run1",
        result=result,
        config=config,
        risk_config=risk_config,
        output_dir=tmp_path,
    )
    export_risk_aware_backtest_artifacts(
        run_id="run2",
        result=result,
        config=config,
        risk_config=risk_config,
        output_dir=tmp_path,
    )
    assert (tmp_path / "run1").is_dir()
    assert (tmp_path / "run2").is_dir()


def test_export_refuses_to_overwrite_existing_nonempty_run_dir(tmp_path: Path) -> None:
    result, config, risk_config = _make_raa_result()
    export_risk_aware_backtest_artifacts(
        run_id="dup",
        result=result,
        config=config,
        risk_config=risk_config,
        output_dir=tmp_path,
    )
    with pytest.raises(FileExistsError):
        export_risk_aware_backtest_artifacts(
            run_id="dup",
            result=result,
            config=config,
            risk_config=risk_config,
            output_dir=tmp_path,
        )


def test_export_refuses_to_overwrite_existing_empty_run_dir(tmp_path: Path) -> None:
    # 回归用例：之前"已存在且非空才拒绝"的写法在 POSIX 上对着一个已存在的空目录会被
    # rename 静默替换掉——跟 Windows 上 rename 对已存在空目录直接报错不一致，是真实的
    # 平台相关 TOCTOU，不只是理论风险。现在不管目标目录是否为空，已存在就必须拒绝。
    result, config, risk_config = _make_raa_result()
    run_dir = tmp_path / "dup"
    run_dir.mkdir(parents=True)
    assert list(run_dir.iterdir()) == []
    with pytest.raises(FileExistsError):
        export_risk_aware_backtest_artifacts(
            run_id="dup",
            result=result,
            config=config,
            risk_config=risk_config,
            output_dir=tmp_path,
        )


def test_export_leaves_no_partial_output_on_failure(tmp_path: Path) -> None:
    result, config, risk_config = _make_raa_result()
    run_id = "flaky"

    real_dumps = json.dumps
    call_count = {"n": 0}

    def flaky_dumps(*args: Any, **kwargs: Any) -> str:
        call_count["n"] += 1
        if call_count["n"] == 3:
            raise RuntimeError("boom")
        return real_dumps(*args, **kwargs)

    with mock.patch("py_core.backtests.artifact_export.json.dumps", side_effect=flaky_dumps):
        with pytest.raises(RuntimeError):
            export_risk_aware_backtest_artifacts(
                run_id=run_id,
                result=result,
                config=config,
                risk_config=risk_config,
                output_dir=tmp_path,
            )

    assert not (tmp_path / run_id).exists()
    assert list(tmp_path.glob(".tmp-*")) == []


def test_export_rejects_non_authorizing_false(tmp_path: Path) -> None:
    """export 应拒绝 non_authorizing=False（防御检查，正常不会触发）。"""
    import copy

    result, config, risk_config = _make_raa_result()
    # 用 copy + 直接赋值绕过 __post_init__ 强制置 False，模拟防御检查
    bad_result = copy.copy(result)
    bad_result.non_authorizing = False
    with pytest.raises(ValueError, match="non_authorizing"):
        export_risk_aware_backtest_artifacts(
            run_id="bad",
            result=bad_result,
            config=config,
            risk_config=risk_config,
            output_dir=tmp_path,
        )
