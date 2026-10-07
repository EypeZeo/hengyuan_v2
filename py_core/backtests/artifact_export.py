"""
风险感知回测 Artifact 导出 — P2-RM-03

将 RiskAwareBacktestResult 序列化为结构化 artifact 目录。
所有输出均为研究用途的回测估算值，不具授权效力。

输出文件结构：
    <output_dir>/<run_id>/
        config.json
        base_metrics.json
        risk_metrics.json
        risk_equity_curve.jsonl
        risk_positions.jsonl
        risk_decisions.jsonl
        summary.json
        validation_report.json

所有文件均包含 non_authorizing=true 字段。
不含 API key、账户真值或任何敏感信息。
"""

from __future__ import annotations

import json
import math
import os
import shutil
import sys
import uuid
from datetime import UTC, datetime
from decimal import Decimal
from pathlib import Path
from typing import Any

import pandas as pd

from py_core.backtests.models import BacktestConfig, BacktestMetrics, ValidationReport
from py_core.backtests.risk_integration import RiskAwareBacktestResult
from py_core.risk.risk_config import RiskConfig
from py_core.risk.risk_decision_artifact import RiskDecisionArtifact, RiskDecisionStatus

_NON_AUTH_LABEL = "risk-aware backtest estimates only — NON-AUTHORIZING RESEARCH USE ONLY"


# ── JSON serialization helpers ───────────────────────────────────────────────


def _decimal_to_float(obj: Any) -> Any:
    """将 Decimal 转为 float 用于 JSON 序列化（稳定、无精度放大）。"""
    if isinstance(obj, Decimal):
        return float(obj)
    raise TypeError(f"Object of type {type(obj).__name__!r} is not JSON serializable")


def _finite_or_none(obj: Any) -> Any:
    """Recursively replace non-finite floats with ``None``.

    Audit PY-JSON-024: ``metrics.calmar_ratio`` is ``float("inf")`` whenever a
    strategy had no drawdown, and ``json.dumps`` serialises that as the bare token
    ``Infinity`` — which is **not valid JSON** (RFC 8259 has no such literal).  Every
    artifact from a no-drawdown run was therefore rejected by any strict parser,
    silently, while looking fine to Python's own ``json.load``.

    ``None``/``null`` is the honest encoding: the ratio is undefined, not enormous.
    Consumers already have to handle a missing value, and ``null`` says so in a way
    every JSON parser agrees on.  Kept at the serialisation boundary on purpose —
    ``inf`` is mathematically meaningful in the in-memory model and stays there.
    """
    if isinstance(obj, float):
        return obj if math.isfinite(obj) else None
    if isinstance(obj, dict):
        return {k: _finite_or_none(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_finite_or_none(v) for v in obj]
    return obj


def _dumps(obj: Any) -> str:
    """带 Decimal 支持的 JSON 序列化，indent=2。

    ``allow_nan=False`` 是安全网而不是主要机制：``_finite_or_none`` 已经把
    非有限值换成 ``None``，这里让任何漏网的 inf/nan 直接抛错，而不是
    静默写出非法 JSON（审计 PY-JSON-024）。
    """
    return json.dumps(
        _finite_or_none(obj), indent=2, default=_decimal_to_float, ensure_ascii=False,
        allow_nan=False,
    )


def _dumps_line(obj: Any) -> str:
    """带 Decimal 支持的单行 JSON 序列化（用于 JSONL）。"""
    return json.dumps(
        _finite_or_none(obj), default=_decimal_to_float, ensure_ascii=False, allow_nan=False,
    )


# ── per-artifact serializers ─────────────────────────────────────────────────


def serialize_config(
    run_id: str,
    config: BacktestConfig,
    risk_config: RiskConfig,
    *,
    stop_distance_fraction: Decimal | None = None,
    ohlcv_path: str = "",
    signals_path: str = "",
    strategy_spec: str | None = None,
    strategy_params: dict[str, Any] | None = None,
    ohlcv_fingerprint_info: dict[str, Any] | None = None,
    strategy_fingerprint_info: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """序列化运行配置快照。不含 API key 或 secret。

    strategy_spec/strategy_params/ohlcv_fingerprint_info/strategy_fingerprint_info 是
    P2-STRAT-01 新增的可复现性 provenance 字段——"这次跑的到底是哪份 OHLCV 数据、哪个
    策略、哪个版本、什么参数"，走 --signals 路径时均为 None。
    """
    return {
        "run_id": run_id,
        "created_at": datetime.now(UTC).isoformat(),
        "output_label": _NON_AUTH_LABEL,
        "non_authorizing": True,
        "inputs": {
            "ohlcv_path": ohlcv_path,
            "signals_path": signals_path,
        },
        "provenance": {
            "strategy_spec": strategy_spec,
            "strategy_params": strategy_params,
            "ohlcv_fingerprint": ohlcv_fingerprint_info,
            "strategy_fingerprint": strategy_fingerprint_info,
            "python_version": sys.version,
            "pandas_version": pd.__version__,
        },
        "backtest_config": {
            "initial_capital": config.initial_capital,
            "fee_bps": config.fee_bps,
            "slippage_bps": config.slippage_bps,
            "risk_free_rate": config.risk_free_rate,
            "annualization_factor": config.annualization_factor,
        },
        "risk_config": {
            "risk_fraction": config_decimal_str(risk_config.risk_fraction),
            "max_position_fraction": config_decimal_str(risk_config.max_position_fraction),
            "max_notional": config_decimal_str(risk_config.max_notional),
            "max_risk_per_trade": config_decimal_str(risk_config.max_risk_per_trade),
            "volatility_estimate": (
                config_decimal_str(risk_config.volatility_estimate)
                if risk_config.volatility_estimate is not None
                else None
            ),
            "stop_distance_fraction": (
                config_decimal_str(stop_distance_fraction) if stop_distance_fraction is not None else None
            ),
        },
    }


def config_decimal_str(d: Decimal) -> str:
    """将 Decimal 规范化为字符串，供 config 快照使用（保持可读性，避免科学计数法）。"""
    return format(d.normalize(), "f")


def serialize_metrics(metrics: BacktestMetrics, *, non_authorizing: bool = True) -> dict[str, Any]:
    """序列化 BacktestMetrics 到 JSON 可序列化 dict。"""
    return {
        "output_label": metrics.output_label,
        "non_authorizing": non_authorizing,
        "total_return": metrics.total_return,
        "annualized_return": metrics.annualized_return,
        "annualized_volatility": metrics.annualized_volatility,
        "sharpe_ratio": metrics.sharpe_ratio,
        "max_drawdown": metrics.max_drawdown,
        "calmar_ratio": metrics.calmar_ratio,
        "win_rate": metrics.win_rate,
        "exposure": metrics.exposure,
        "turnover": metrics.turnover,
        "cost_impact_bps": metrics.cost_impact_bps,
        "cost_impact_total": metrics.cost_impact_total,
    }


def serialize_causal_fields(report: ValidationReport) -> dict[str, Any]:
    """因果核验字段（SAFE-05）。validation_report.json 的两个写出点共用这一份，避免漂移。"""
    return {
        "causal_check": report.causal_check,
        "causal_points": report.causal_points,
        "causal_context": report.causal_context,
    }


def serialize_validation_report(report: ValidationReport) -> dict[str, Any]:
    """序列化 ValidationReport 到 JSON 可序列化 dict。"""
    return {
        "non_authorizing": True,
        "timestamp_monotonic": report.timestamp_monotonic,
        "no_nan_close": report.no_nan_close,
        "no_future_shift_detected": report.no_future_shift_detected,
        "bar_count": report.bar_count,
        "is_valid": report.is_valid,
        "issues": report.issues,
        **serialize_causal_fields(report),
    }


def serialize_risk_decision_line(
    decision: RiskDecisionArtifact,
    timestamp: datetime,
) -> dict[str, Any]:
    """序列化单条 RiskDecisionArtifact 为 JSONL 行。

    按 P2-RM-03 schema 包含：
    timestamp, status, proposed_position_size, capped_position_size,
    risk_used, cap_reason, warnings, non_authorizing.
    """
    return {
        "timestamp": timestamp.isoformat(),
        "status": str(decision.status),
        "proposed_position_size": float(decision.proposed_position_size),
        "capped_position_size": float(decision.capped_position_size),
        "risk_used": float(decision.risk_used),
        "cap_reason": decision.cap_reason,
        "warnings": list(decision.warnings),
        "non_authorizing": decision.non_authorizing,
    }


def compute_decision_counts(
    decisions: list[RiskDecisionArtifact],
) -> dict[str, int]:
    """统计每种 RiskDecisionStatus 的数量。"""
    counts: dict[str, int] = {
        str(RiskDecisionStatus.ACCEPTED): 0,
        str(RiskDecisionStatus.CAPPED): 0,
        str(RiskDecisionStatus.REJECTED): 0,
    }
    for d in decisions:
        key = str(d.status)
        counts[key] = counts.get(key, 0) + 1
    return counts


def serialize_summary(
    run_id: str,
    result: RiskAwareBacktestResult,
) -> dict[str, Any]:
    """序列化 summary.json，包含基础与风险调整指标对比及决策计数。"""
    base_m = result.base_result.metrics
    risk_m = result.risk_metrics
    return {
        "run_id": run_id,
        "created_at": datetime.now(UTC).isoformat(),
        "output_label": _NON_AUTH_LABEL,
        "non_authorizing": True,
        "base_total_return": base_m.total_return,
        "risk_total_return": risk_m.total_return,
        "base_max_drawdown": base_m.max_drawdown,
        "risk_max_drawdown": risk_m.max_drawdown,
        "base_sharpe": base_m.sharpe_ratio,
        "risk_sharpe": risk_m.sharpe_ratio,
        "decision_counts": compute_decision_counts(result.risk_decisions),
    }


# ── main export function ─────────────────────────────────────────────────────


def export_risk_aware_backtest_artifacts(
    run_id: str,
    result: RiskAwareBacktestResult,
    config: BacktestConfig,
    risk_config: RiskConfig,
    output_dir: Path,
    *,
    stop_distance_fraction: Decimal | None = None,
    ohlcv_path: str = "",
    signals_path: str = "",
    strategy_spec: str | None = None,
    strategy_params: dict[str, Any] | None = None,
    ohlcv_fingerprint_info: dict[str, Any] | None = None,
    strategy_fingerprint_info: dict[str, Any] | None = None,
) -> Path:
    """将 RiskAwareBacktestResult 序列化为完整 artifact 目录。

    原子发布：先在 output_dir 的同级临时目录下把所有文件写完；全部成功后用
    run_dir.mkdir(exist_ok=False) 做一次真正原子的排他占位（不管 run_dir 是否已存在、是否为空，
    两个平台语义一致——之前"检查已存在且非空，再 rename"的写法在 POSIX 上对一个已存在的空目录
    会被静默替换掉，是一个真实的 check-then-act 竞争窗口，不只是理论上的），再把临时目录里的
    文件逐个搬进去，最后写入 ``.complete`` 标记。已存在的目标目录不会被静默覆盖。

    完整性判定以 ``.complete`` 标记为准（审计 PY-PUB-014）：搬运是逐文件 rename，
    所以在占位与标记之间被 kill 会留下一个"存在但不完整"的 run_dir。标记最后写入，
    因此它的存在即证明其余文件都已就位。消费方必须用 :func:`is_complete_run` 判断，
    不能仅凭目录存在。

    所有文件包含 non_authorizing=true。

    Returns:
        创建的 run 目录绝对路径。

    Raises:
        ValueError: result 不符合 non_authorizing 约束（不应发生，仅防御）。
        FileExistsError: 目标 run 目录已存在（不管是否为空）。
        OSError: 无法创建输出目录。
    """
    if not result.non_authorizing:
        raise ValueError("result.non_authorizing 必须为 True")

    run_dir = output_dir / run_id
    output_dir.mkdir(parents=True, exist_ok=True)
    tmp_dir = output_dir / f".tmp-{run_id}-{uuid.uuid4().hex}"
    tmp_dir.mkdir(parents=True, exist_ok=False)

    try:
        # config.json
        config_dict = serialize_config(
            run_id=run_id,
            config=config,
            risk_config=risk_config,
            stop_distance_fraction=stop_distance_fraction,
            ohlcv_path=ohlcv_path,
            signals_path=signals_path,
            strategy_spec=strategy_spec,
            strategy_params=strategy_params,
            ohlcv_fingerprint_info=ohlcv_fingerprint_info,
            strategy_fingerprint_info=strategy_fingerprint_info,
        )
        (tmp_dir / "config.json").write_text(_dumps(config_dict), encoding="utf-8")

        # base_metrics.json
        base_metrics_dict = serialize_metrics(result.base_result.metrics, non_authorizing=True)
        (tmp_dir / "base_metrics.json").write_text(_dumps(base_metrics_dict), encoding="utf-8")

        # risk_metrics.json
        risk_metrics_dict = serialize_metrics(result.risk_metrics, non_authorizing=True)
        (tmp_dir / "risk_metrics.json").write_text(_dumps(risk_metrics_dict), encoding="utf-8")

        # validation_report.json
        vr_dict = serialize_validation_report(result.base_result.validation_report)
        (tmp_dir / "validation_report.json").write_text(_dumps(vr_dict), encoding="utf-8")

        # risk_equity_curve.jsonl
        with (tmp_dir / "risk_equity_curve.jsonl").open("w", encoding="utf-8") as f:
            for ts, eq in zip(result.timestamps, result.risk_equity_curve, strict=True):
                f.write(_dumps_line({"timestamp_utc": ts.isoformat(), "equity": eq}) + "\n")

        # risk_positions.jsonl
        with (tmp_dir / "risk_positions.jsonl").open("w", encoding="utf-8") as f:
            for ts, pos in zip(result.timestamps, result.risk_positions_fraction, strict=True):
                f.write(_dumps_line({"timestamp_utc": ts.isoformat(), "exposure_fraction": pos}) + "\n")

        # risk_decisions.jsonl
        with (tmp_dir / "risk_decisions.jsonl").open("w", encoding="utf-8") as f:
            for ts, decision in zip(result.timestamps, result.risk_decisions, strict=True):
                line = serialize_risk_decision_line(decision, ts)
                f.write(_dumps_line(line) + "\n")

        # summary.json
        summary_dict = serialize_summary(run_id=run_id, result=result)
        (tmp_dir / "summary.json").write_text(_dumps(summary_dict), encoding="utf-8")
    except Exception:
        shutil.rmtree(tmp_dir, ignore_errors=True)
        raise

    try:
        run_dir.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        shutil.rmtree(tmp_dir, ignore_errors=True)
        raise FileExistsError(f"输出目录已存在，拒绝覆盖: {run_dir}") from None

    for f in tmp_dir.iterdir():
        f.rename(run_dir / f.name)
    tmp_dir.rmdir()

    # AUDIT PY-PUB-014: everything above is a per-file rename loop, so a crash or
    # SIGKILL between run_dir.mkdir() and here leaves run_dir EXISTING and holding a
    # SUBSET of the artifact set -- indistinguishable, to a reader, from a complete
    # run. The docstring's "中途失败/被杀不会留下写了一半的残留" was true of the tmp
    # directory but not of the destination.
    #
    # A directory rename is not available as a fix: the exclusive
    # mkdir(exist_ok=False) placeholder above is what closes the check-then-act race
    # the previous revision had, and os.rename onto an existing empty directory
    # silently succeeds on POSIX -- which is exactly the bug that placeholder
    # replaced. The marker is the standard answer: it is written LAST, so its
    # presence is proof every other file already landed.
    _write_completion_marker(run_dir)
    return run_dir.resolve()


COMPLETION_MARKER = ".complete"


def _fsync_dir(path: Path) -> None:
    """Flush a directory entry to disk; a no-op where the platform forbids it."""
    try:
        fd = os.open(path, os.O_RDONLY)
    except OSError:
        return  # Windows cannot open a directory this way; the rename is durable enough there
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


def _write_completion_marker(run_dir: Path) -> None:
    marker = run_dir / COMPLETION_MARKER
    with marker.open("w", encoding="utf-8") as f:
        f.write("complete\n")
        f.flush()
        os.fsync(f.fileno())
    _fsync_dir(run_dir)


def is_complete_run(run_dir: Path) -> bool:
    """True iff ``run_dir`` holds a fully-published artifact set (audit PY-PUB-014).

    Consumers MUST check this before reading a run directory.  A directory without
    the marker is a partially-published run left behind by a crash: its files are
    individually valid but the set is incomplete, and which files are missing is not
    predictable.
    """
    return (run_dir / COMPLETION_MARKER).is_file()
