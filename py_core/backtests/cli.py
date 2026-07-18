"""
向量化 OHLCV 回测 — 本地 CLI 示例入口（P2-BT-01）

独立脚本，不依赖 app/interfaces/cli/main.py。
所有输出均为回测估算值，不代表财务建议或交易授权。

用法：
    python -m research.backtests.cli run \\
        --ohlcv path/to/ohlcv.csv \\
        --signals path/to/signals.csv \\
        --initial-capital 100000 \\
        --fee-bps 10 \\
        --slippage-bps 5

OHLCV CSV 格式（必须包含以下列，timestamp 为 UTC ISO 8601）：
    timestamp_utc,open,high,low,close,volume

Signals CSV 格式（仅需 timestamp_utc 与 signal 两列，signal 值 0 或 1）：
    timestamp_utc,signal
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
import uuid
from datetime import UTC, datetime
from decimal import Decimal
from pathlib import Path
from typing import Any

import pandas as pd
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)

from py_core.backtests.artifact_export import export_risk_aware_backtest_artifacts
from py_core.backtests.models import BacktestConfig, BacktestResult
from py_core.backtests.risk_integration import run_risk_aware_backtest
from py_core.backtests.vectorized_engine import run_vectorized_backtest
from py_core.risk.risk_config import RiskConfig

NON_AUTH_NOTICE = (
    "\n[NOTICE] All outputs are BACKTESTING ESTIMATES ONLY.\n"
    "NOT financial advice. NOT trading authorization.\n"
    "NOT dry-run readiness. NOT live readiness.\n"
)


def _parse_timestamp_utc(s: str) -> datetime:
    """Parse ISO 8601 timestamp string, return UTC-aware datetime."""
    s = s.strip()
    # Try with offset
    for fmt in (
        "%Y-%m-%dT%H:%M:%S%z",
        "%Y-%m-%dT%H:%M:%S.%f%z",
        "%Y-%m-%d %H:%M:%S%z",
        "%Y-%m-%dT%H:%M:%SZ",
    ):
        try:
            dt = datetime.strptime(s, fmt)
            return dt.astimezone(UTC)
        except ValueError:
            continue
    # Try naive → assume UTC
    for fmt in (
        "%Y-%m-%dT%H:%M:%S",
        "%Y-%m-%dT%H:%M:%S.%f",
        "%Y-%m-%d %H:%M:%S",
        "%Y-%m-%d",
    ):
        try:
            dt = datetime.strptime(s, fmt)
            return dt.replace(tzinfo=UTC)
        except ValueError:
            continue
    raise ValueError(f"无法解析时间戳: {s!r}")


def load_ohlcv_csv(
    path: Path,
    *,
    market: str = "crypto_spot",
    symbol: str = "BTC/USDT",
    timeframe: str = "1d",
) -> list[NormalizedOhlcvRecord]:
    """从 CSV 文件加载 NormalizedOhlcvRecord 列表。

    CSV 必须包含以下列：timestamp_utc, open, high, low, close, volume
    """
    records: list[NormalizedOhlcvRecord] = []
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            records.append(
                NormalizedOhlcvRecord(
                    market=ManualMarket(market),
                    symbol=CanonicalMarketSymbol(symbol),
                    timeframe=OhlcvTimeframe(timeframe),
                    event_time_utc=_parse_timestamp_utc(row["timestamp_utc"]),
                    open_price=Decimal(row["open"]),
                    high_price=Decimal(row["high"]),
                    low_price=Decimal(row["low"]),
                    close_price=Decimal(row["close"]),
                    volume=Decimal(row["volume"]),
                )
            )
    if not records:
        raise ValueError(f"OHLCV 文件无有效记录: {path}")
    return records


def load_signals_csv(
    path: Path,
    ohlcv_records: list[NormalizedOhlcvRecord],
) -> pd.Series[Any]:
    """从 CSV 文件加载 signal Series，索引为 UTC DatetimeIndex。

    CSV 必须包含以下列：timestamp_utc, signal
    """
    rows: list[dict[str, Any]] = []
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(
                {
                    "timestamp_utc": _parse_timestamp_utc(row["timestamp_utc"]),
                    "signal": float(row["signal"]),
                }
            )
    if not rows:
        raise ValueError(f"Signals 文件无有效记录: {path}")

    df_sig = pd.DataFrame(rows)
    df_sig = df_sig.set_index("timestamp_utc")
    df_sig.index = pd.DatetimeIndex(df_sig.index)
    signals: pd.Series[Any] = df_sig["signal"]
    return signals


def save_results(result: BacktestResult, output_dir: Path) -> None:
    """将回测结果序列化写入 output_dir。"""
    output_dir.mkdir(parents=True, exist_ok=True)

    # metrics.json
    metrics_dict: dict[str, Any] = {
        "output_label": result.output_label,
        "non_auth_assertion": result.non_auth_assertion,
        "total_return": result.metrics.total_return,
        "annualized_return": result.metrics.annualized_return,
        "annualized_volatility": result.metrics.annualized_volatility,
        "sharpe_ratio": result.metrics.sharpe_ratio,
        "max_drawdown": result.metrics.max_drawdown,
        "calmar_ratio": result.metrics.calmar_ratio,
        "win_rate": result.metrics.win_rate,
        "exposure": result.metrics.exposure,
        "turnover": result.metrics.turnover,
        "cost_impact_bps": result.metrics.cost_impact_bps,
        "cost_impact_total": result.metrics.cost_impact_total,
    }
    (output_dir / "metrics.json").write_text(json.dumps(metrics_dict, indent=2), encoding="utf-8")

    # validation_report.json
    vr_dict: dict[str, Any] = {
        "timestamp_monotonic": result.validation_report.timestamp_monotonic,
        "no_nan_close": result.validation_report.no_nan_close,
        "no_future_shift_detected": result.validation_report.no_future_shift_detected,
        "bar_count": result.validation_report.bar_count,
        "is_valid": result.validation_report.is_valid,
        "issues": result.validation_report.issues,
    }
    (output_dir / "validation_report.json").write_text(
        json.dumps(vr_dict, indent=2), encoding="utf-8"
    )

    # equity_curve.jsonl
    with (output_dir / "equity_curve.jsonl").open("w", encoding="utf-8") as f:
        for ts, eq in zip(result.timestamps, result.equity_curve, strict=True):
            f.write(json.dumps({"timestamp_utc": ts.isoformat(), "equity": eq}) + "\n")

    # positions.jsonl
    with (output_dir / "positions.jsonl").open("w", encoding="utf-8") as f:
        for ts, pos in zip(result.timestamps, result.positions, strict=True):
            f.write(json.dumps({"timestamp_utc": ts.isoformat(), "position": pos}) + "\n")

    # trades.jsonl
    with (output_dir / "trades.jsonl").open("w", encoding="utf-8") as f:
        for tr in result.fills_approx:
            f.write(
                json.dumps(
                    {
                        "bar_index": tr.bar_index,
                        "timestamp_utc": tr.timestamp_utc.isoformat(),
                        "direction": tr.direction,
                        "execution_price": tr.execution_price,
                        "position_before": tr.position_before,
                        "position_after": tr.position_after,
                        "position_change": tr.position_change,
                        "estimated_cost_rate": tr.estimated_cost_rate,
                        "note": tr.note,
                    }
                )
                + "\n"
            )


def cmd_run_risk_aware(args: argparse.Namespace) -> int:
    """执行风险感知回测子命令（P2-RM-03）。"""
    print(NON_AUTH_NOTICE)
    print("[NOTICE] run-risk-aware — NON-AUTHORIZING RESEARCH USE ONLY\n")

    ohlcv_path = Path(args.ohlcv).resolve()
    signals_path = Path(args.signals).resolve()
    if not ohlcv_path.exists():
        print(f"[ERROR] OHLCV 文件不存在: {ohlcv_path}", file=sys.stderr)
        return 1
    if not signals_path.exists():
        print(f"[ERROR] Signals 文件不存在: {signals_path}", file=sys.stderr)
        return 1

    print(f"[INFO] 加载 OHLCV: {ohlcv_path}")
    records = load_ohlcv_csv(
        ohlcv_path,
        market=args.market,
        symbol=args.symbol,
        timeframe=args.timeframe,
    )
    print(f"[INFO] 已加载 {len(records)} 条 OHLCV 记录")

    print(f"[INFO] 加载 Signals: {signals_path}")
    signals = load_signals_csv(signals_path, records)
    print(f"[INFO] 已加载 {len(signals)} 条 Signal")

    config = BacktestConfig(
        initial_capital=args.initial_capital,
        fee_bps=args.fee_bps,
        slippage_bps=args.slippage_bps,
    )

    risk_config = RiskConfig(
        risk_fraction=Decimal(str(args.risk_fraction)),
        max_position_fraction=Decimal(str(args.max_position_fraction)),
        max_notional=Decimal(str(args.max_notional)),
        max_risk_per_trade=Decimal(str(args.max_risk_per_trade)),
    )

    stop_distance: Decimal | None = None
    if args.stop_distance is not None:
        stop_distance = Decimal(str(args.stop_distance))

    print("[INFO] 运行风险感知回测...")
    result = run_risk_aware_backtest(config, records, signals, risk_config, stop_distance)

    # 输出绩效摘要
    bm = result.base_result.metrics
    rm = result.risk_metrics
    from py_core.risk.risk_decision_artifact import RiskDecisionStatus

    decision_counts = {
        str(RiskDecisionStatus.ACCEPTED): sum(
            1 for d in result.risk_decisions if d.status == RiskDecisionStatus.ACCEPTED
        ),
        str(RiskDecisionStatus.CAPPED): sum(
            1 for d in result.risk_decisions if d.status == RiskDecisionStatus.CAPPED
        ),
        str(RiskDecisionStatus.REJECTED): sum(
            1 for d in result.risk_decisions if d.status == RiskDecisionStatus.REJECTED
        ),
    }

    print("\n===== 风险感知回测绩效摘要 =====")
    print(f"  output_label         : {result.output_label}")
    print(f"  non_authorizing      : {result.non_authorizing}")
    print(f"  bar_count            : {result.base_result.validation_report.bar_count}")
    print("  --- 基础回测 ---")
    print(f"  base total_return    : {bm.total_return:.4%}")
    print(f"  base sharpe_ratio    : {bm.sharpe_ratio:.4f}")
    print(f"  base max_drawdown    : {bm.max_drawdown:.4%}")
    print("  --- 风险调整 ---")
    print(f"  risk total_return    : {rm.total_return:.4%}")
    print(f"  risk sharpe_ratio    : {rm.sharpe_ratio:.4f}")
    print(f"  risk max_drawdown    : {rm.max_drawdown:.4%}")
    print("  --- 决策统计 ---")
    for status, count in decision_counts.items():
        print(f"  {status:10s}          : {count}")
    print("================================")

    # 导出 artifacts
    run_id = getattr(args, "run_id", None) or uuid.uuid4().hex[:8]
    output_root = Path(getattr(args, "output", ".var/risk-aware-backtests"))
    run_dir = export_risk_aware_backtest_artifacts(
        run_id=run_id,
        result=result,
        config=config,
        risk_config=risk_config,
        output_dir=output_root,
        stop_distance=stop_distance,
        ohlcv_path=str(ohlcv_path),
        signals_path=str(signals_path),
    )
    print(f"\n[INFO] Artifacts 已保存至: {run_dir}")
    print(NON_AUTH_NOTICE)
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    """执行回测子命令。"""
    print(NON_AUTH_NOTICE)

    ohlcv_path = Path(args.ohlcv).resolve()
    signals_path = Path(args.signals).resolve()
    if not ohlcv_path.exists():
        print(f"[ERROR] OHLCV 文件不存在: {ohlcv_path}", file=sys.stderr)
        return 1
    if not signals_path.exists():
        print(f"[ERROR] Signals 文件不存在: {signals_path}", file=sys.stderr)
        return 1

    print(f"[INFO] 加载 OHLCV: {ohlcv_path}")
    records = load_ohlcv_csv(
        ohlcv_path,
        market=args.market,
        symbol=args.symbol,
        timeframe=args.timeframe,
    )
    print(f"[INFO] 已加载 {len(records)} 条 OHLCV 记录")

    print(f"[INFO] 加载 Signals: {signals_path}")
    signals = load_signals_csv(signals_path, records)
    print(f"[INFO] 已加载 {len(signals)} 条 Signal")

    config = BacktestConfig(
        initial_capital=args.initial_capital,
        fee_bps=args.fee_bps,
        slippage_bps=args.slippage_bps,
        risk_free_rate=args.risk_free_rate,
        annualization_factor=args.annualization_factor,
    )

    print("[INFO] 运行回测...")
    result = run_vectorized_backtest(config, records, signals)

    # 输出绩效摘要
    m = result.metrics
    print("\n===== 回测绩效摘要 =====")
    print(f"  output_label         : {result.output_label}")
    print(f"  bar_count            : {result.validation_report.bar_count}")
    print(f"  total_return         : {m.total_return:.4%}")
    print(f"  annualized_return    : {m.annualized_return:.4%}")
    print(f"  annualized_volatility: {m.annualized_volatility:.4%}")
    print(f"  sharpe_ratio         : {m.sharpe_ratio:.4f}")
    print(f"  max_drawdown         : {m.max_drawdown:.4%}")
    print(f"  calmar_ratio         : {m.calmar_ratio:.4f}")
    print(f"  win_rate             : {m.win_rate:.4%}")
    print(f"  exposure             : {m.exposure:.4%}")
    print(f"  turnover             : {m.turnover:.4f}")
    print(f"  cost_impact_bps      : {m.cost_impact_bps:.4f}")
    print(f"  cost_impact_total    : {m.cost_impact_total:.2f}")
    print(f"  fills_approx count   : {len(result.fills_approx)}")
    print("========================")

    # 保存结果
    run_id = getattr(args, "run_id", None) or uuid.uuid4().hex[:8]
    output_dir = Path(".var") / "backtests" / run_id
    save_results(result, output_dir)
    print(f"\n[INFO] 结果已保存至: {output_dir.resolve()}")
    print(NON_AUTH_NOTICE)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m research.backtests.cli",
        description="P2-BT-01 向量化 OHLCV 回测 CLI（仅供研究，非交易授权）",
    )
    subparsers = parser.add_subparsers(dest="command")

    run_parser = subparsers.add_parser("run", help="运行回测")
    run_parser.add_argument("--ohlcv", required=True, help="OHLCV CSV 文件路径")
    run_parser.add_argument("--signals", required=True, help="Signals CSV 文件路径")
    run_parser.add_argument(
        "--initial-capital", type=float, default=100000.0, help="初始资本（默认 100000）"
    )
    run_parser.add_argument("--fee-bps", type=float, default=10.0, help="手续费 bps（默认 10）")
    run_parser.add_argument("--slippage-bps", type=float, default=5.0, help="滑点 bps（默认 5）")
    run_parser.add_argument(
        "--risk-free-rate", type=float, default=0.0, help="年化无风险利率（默认 0.0）"
    )
    run_parser.add_argument(
        "--annualization-factor", type=int, default=252, help="年化因子（默认 252）"
    )
    run_parser.add_argument("--market", default="crypto_spot", help="市场类型（默认 crypto_spot）")
    run_parser.add_argument("--symbol", default="BTC/USDT", help="交易对符号（默认 BTC/USDT）")
    run_parser.add_argument("--timeframe", default="1d", help="时间周期（默认 1d）")
    run_parser.add_argument("--run-id", default=None, help="自定义 run_id（用于输出目录）")

    # ── run-risk-aware subcommand ────────────────────────────────────────────
    rra_parser = subparsers.add_parser("run-risk-aware", help="运行风险感知回测（P2-RM-03）")
    rra_parser.add_argument("--ohlcv", required=True, help="OHLCV CSV 文件路径")
    rra_parser.add_argument("--signals", required=True, help="Signals CSV 文件路径")
    rra_parser.add_argument(
        "--initial-capital", type=float, default=100000.0, help="初始资本（默认 100000）"
    )
    rra_parser.add_argument("--fee-bps", type=float, default=10.0, help="手续费 bps（默认 10）")
    rra_parser.add_argument("--slippage-bps", type=float, default=5.0, help="滑点 bps（默认 5）")
    rra_parser.add_argument(
        "--risk-fraction",
        type=float,
        default=0.02,
        help="每次交易风险比例（默认 0.02）",
    )
    rra_parser.add_argument(
        "--max-position-fraction",
        type=float,
        default=0.20,
        help="最大仓位比例（默认 0.20）",
    )
    rra_parser.add_argument(
        "--max-notional",
        type=float,
        default=20000.0,
        help="最大名义金额（默认 20000）",
    )
    rra_parser.add_argument(
        "--max-risk-per-trade",
        type=float,
        default=0.01,
        help="每笔最大风险（默认 0.01）",
    )
    rra_parser.add_argument(
        "--stop-distance",
        type=float,
        default=None,
        help="止损距离（价格单位，可选）",
    )
    rra_parser.add_argument(
        "--output",
        default=".var/risk-aware-backtests",
        help="输出根目录（默认 .var/risk-aware-backtests）",
    )
    rra_parser.add_argument("--market", default="crypto_spot", help="市场类型（默认 crypto_spot）")
    rra_parser.add_argument("--symbol", default="BTC/USDT", help="交易对符号（默认 BTC/USDT）")
    rra_parser.add_argument("--timeframe", default="1d", help="时间周期（默认 1d）")
    rra_parser.add_argument("--run-id", default=None, help="自定义 run_id（用于输出目录）")

    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    if args.command == "run":
        sys.exit(cmd_run(args))
    elif args.command == "run-risk-aware":
        sys.exit(cmd_run_risk_aware(args))
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
