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
from datetime import UTC, datetime
from decimal import Decimal
from pathlib import Path
from typing import Any

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


def _dumps(obj: Any) -> str:
    """带 Decimal 支持的 JSON 序列化，indent=2。"""
    return json.dumps(obj, indent=2, default=_decimal_to_float, ensure_ascii=False)


def _dumps_line(obj: Any) -> str:
    """带 Decimal 支持的单行 JSON 序列化（用于 JSONL）。"""
    return json.dumps(obj, default=_decimal_to_float, ensure_ascii=False)


# ── per-artifact serializers ─────────────────────────────────────────────────


def serialize_config(
    run_id: str,
    config: BacktestConfig,
    risk_config: RiskConfig,
    *,
    stop_distance: Decimal | None = None,
    ohlcv_path: str = "",
    signals_path: str = "",
) -> dict[str, Any]:
    """序列化运行配置快照。不含 API key 或 secret。"""
    return {
        "run_id": run_id,
        "created_at": datetime.now(UTC).isoformat(),
        "output_label": _NON_AUTH_LABEL,
        "non_authorizing": True,
        "inputs": {
            "ohlcv_path": ohlcv_path,
            "signals_path": signals_path,
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
            "stop_distance": (
                config_decimal_str(stop_distance) if stop_distance is not None else None
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
    stop_distance: Decimal | None = None,
    ohlcv_path: str = "",
    signals_path: str = "",
) -> Path:
    """将 RiskAwareBacktestResult 序列化为完整 artifact 目录。

    在 output_dir/<run_id>/ 下创建所有 artifact 文件。
    所有文件包含 non_authorizing=true。

    Returns:
        创建的 run 目录绝对路径。

    Raises:
        ValueError: result 不符合 non_authorizing 约束（不应发生，仅防御）。
        OSError: 无法创建输出目录。
    """
    if not result.non_authorizing:
        raise ValueError("result.non_authorizing 必须为 True")

    run_dir = output_dir / run_id
    run_dir.mkdir(parents=True, exist_ok=True)

    # config.json
    config_dict = serialize_config(
        run_id=run_id,
        config=config,
        risk_config=risk_config,
        stop_distance=stop_distance,
        ohlcv_path=ohlcv_path,
        signals_path=signals_path,
    )
    (run_dir / "config.json").write_text(_dumps(config_dict), encoding="utf-8")

    # base_metrics.json
    base_metrics_dict = serialize_metrics(result.base_result.metrics, non_authorizing=True)
    (run_dir / "base_metrics.json").write_text(_dumps(base_metrics_dict), encoding="utf-8")

    # risk_metrics.json
    risk_metrics_dict = serialize_metrics(result.risk_metrics, non_authorizing=True)
    (run_dir / "risk_metrics.json").write_text(_dumps(risk_metrics_dict), encoding="utf-8")

    # validation_report.json
    vr_dict = serialize_validation_report(result.base_result.validation_report)
    (run_dir / "validation_report.json").write_text(_dumps(vr_dict), encoding="utf-8")

    # risk_equity_curve.jsonl
    with (run_dir / "risk_equity_curve.jsonl").open("w", encoding="utf-8") as f:
        for ts, eq in zip(result.timestamps, result.risk_equity_curve, strict=True):
            f.write(_dumps_line({"timestamp_utc": ts.isoformat(), "equity": eq}) + "\n")

    # risk_positions.jsonl
    with (run_dir / "risk_positions.jsonl").open("w", encoding="utf-8") as f:
        for ts, pos in zip(result.timestamps, result.risk_positions_fraction, strict=True):
            f.write(_dumps_line({"timestamp_utc": ts.isoformat(), "exposure_fraction": pos}) + "\n")

    # risk_decisions.jsonl
    with (run_dir / "risk_decisions.jsonl").open("w", encoding="utf-8") as f:
        for ts, decision in zip(result.timestamps, result.risk_decisions, strict=True):
            line = serialize_risk_decision_line(decision, ts)
            f.write(_dumps_line(line) + "\n")

    # summary.json
    summary_dict = serialize_summary(run_id=run_id, result=result)
    (run_dir / "summary.json").write_text(_dumps(summary_dict), encoding="utf-8")

    return run_dir.resolve()
