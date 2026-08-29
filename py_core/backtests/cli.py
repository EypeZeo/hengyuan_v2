"""
向量化 OHLCV 回测 — 本地 CLI 示例入口（P2-BT-01 / P2-STRAT-01）

独立脚本，不依赖 app/interfaces/cli/main.py。
所有输出均为回测估算值，不代表财务建议或交易授权。

用法（--signals 与 --strategy 二选一）：
    python -m research.backtests.cli run \\
        --ohlcv path/to/ohlcv.csv \\
        --signals path/to/signals.csv \\
        --initial-capital 100000 \\
        --fee-bps 10 \\
        --slippage-bps 5

    python -m research.backtests.cli run \\
        --ohlcv path/to/ohlcv.csv \\
        --strategy py_core.strategies.sma_crossover:SmaCrossoverStrategy \\
        --strategy-params '{"fast_window": 5, "slow_window": 20}'

OHLCV CSV 格式（必须包含以下列，timestamp 为 UTC ISO 8601）：
    timestamp_utc,open,high,low,close,volume

Signals CSV 格式（仅需 timestamp_utc 与 signal 两列，signal 值 0 或 1）：
    timestamp_utc,signal
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import shutil
import sys
import uuid
from datetime import UTC, datetime
from decimal import Decimal
from pathlib import Path
from typing import Any

import pandas as pd

from py_core.backtests.artifact_export import (
    _dumps,
    _dumps_line,
    _write_completion_marker,
    export_risk_aware_backtest_artifacts,
)
from py_core.backtests.models import BacktestConfig, BacktestResult
from py_core.backtests.risk_integration import run_risk_aware_backtest
from py_core.backtests.vectorized_engine import (
    _run_vectorized_backtest_on_df,
    records_to_dataframe,
    resolve_annualization_factor,
)
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)
from py_core.risk.risk_config import RiskConfig
from py_core.strategies.base import load_strategy

NON_AUTH_NOTICE = (
    "\n[NOTICE] All outputs are BACKTESTING ESTIMATES ONLY.\n"
    "NOT financial advice. NOT trading authorization.\n"
    "NOT dry-run readiness. NOT live readiness.\n"
)


def validate_run_id(run_id: str) -> None:
    """校验 run_id 可以安全拼进输出文件路径。

    Raises:
        ValueError: run_id 为空，或包含 "/"、"\\" 或 ".."。
    """
    if not run_id:
        raise ValueError("run_id 不能为空")
    if "/" in run_id or "\\" in run_id or ".." in run_id:
        raise ValueError(f"run_id 不能包含路径分隔符或 '..'，收到: {run_id!r}")


def _parse_strategy_params(raw: str | None) -> dict[str, Any]:
    """解析 --strategy-params 传入的 JSON 对象字符串。None/空字符串视为无参数。

    改用单个 JSON 对象而不是重复的 "key=value" 参数——后者对重复 key、空 key、科学计数、
    布尔值这些情况的类型推断没有明确定义，JSON 把类型解析完全交给 json.loads()，没有歧义。

    Raises:
        ValueError: 不是合法 JSON，或合法 JSON 但顶层不是对象（dict）。
    """
    if not raw:
        return {}
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ValueError(f"--strategy-params 不是合法的 JSON: {exc}") from exc
    if not isinstance(parsed, dict):
        raise ValueError(
            f"--strategy-params 必须是一个 JSON 对象（dict），收到: {type(parsed).__name__}"
        )
    return parsed


def ohlcv_fingerprint(ohlcv_path: Path, records: list[NormalizedOhlcvRecord]) -> dict[str, Any]:
    """OHLCV 数据的可复现性指纹。优先用源文件内容的 SHA-256；文件不存在时（理论上不会发生，
    调用方在这之前已经检查过路径存在）退化成"记录数 + 首尾时间戳"摘要。"""
    if ohlcv_path.exists():
        digest = hashlib.sha256(ohlcv_path.read_bytes()).hexdigest()
        return {"kind": "file_sha256", "path": str(ohlcv_path), "sha256": digest}
    first_ts = records[0].event_time_utc.isoformat() if records else None
    last_ts = records[-1].event_time_utc.isoformat() if records else None
    return {
        "kind": "record_summary",
        "record_count": len(records),
        "first_timestamp_utc": first_ts,
        "last_timestamp_utc": last_ts,
    }


def strategy_source_fingerprint(strategy_spec: str | None) -> dict[str, Any] | None:
    """尝试定位 strategy_spec 对应模块的磁盘文件并算 SHA-256。定位不到时返回一个明确标注
    "source_available": False 的字典，不是报错——不是每个可 import 的策略模块都保证有对应
    的 .py 文件路径。strategy_spec 为 None（走 --signals 路径）时直接返回 None。"""
    if strategy_spec is None:
        return None
    module_path = strategy_spec.split(":", 1)[0] if ":" in strategy_spec else strategy_spec
    try:
        spec = importlib.util.find_spec(module_path)
    except (ImportError, ValueError):
        spec = None
    if spec is None or spec.origin is None:
        return {"spec": strategy_spec, "source_available": False}
    source_path = Path(spec.origin)
    if not source_path.is_file():
        return {"spec": strategy_spec, "source_available": False}
    digest = hashlib.sha256(source_path.read_bytes()).hexdigest()
    return {
        "spec": strategy_spec,
        "source_available": True,
        "source_path": str(source_path),
        "sha256": digest,
    }


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


def load_signals_from_strategy(
    strategy_spec: str,
    strategy_params: dict[str, Any],
    df: pd.DataFrame,
    *,
    allow_external: bool,
) -> pd.Series[Any]:
    """用策略框架在已经构建好的 df 上生成 signal Series。

    df 由调用方构建一次并传入（不在这里再调用 records_to_dataframe()）——避免策略生成和
    回测执行各自独立构建一次同样的 DataFrame。
    """
    strategy = load_strategy(strategy_spec, allow_external=allow_external, **strategy_params)
    return strategy.generate_signals(df)


def save_results(
    result: BacktestResult,
    output_dir: Path,
    *,
    strategy_spec: str | None = None,
    strategy_params: dict[str, Any] | None = None,
    signals_path: str | None = None,
    ohlcv_fingerprint_info: dict[str, Any] | None = None,
    strategy_fingerprint_info: dict[str, Any] | None = None,
) -> None:
    """将回测结果序列化写入 output_dir，原子发布。

    先在 output_dir 的同级临时目录下把所有文件写完；全部成功后用
    output_dir.mkdir(exist_ok=False) 做一次真正原子的排他占位（不管 output_dir 是否已存在、
    是否为空，两个平台语义一致——之前用"检查已存在且非空，再 rename"的写法在 POSIX 上对一个
    已存在的空目录会被静默替换掉，跟 Windows 上 rename 对已存在空目录直接报错的行为不一致，
    是一个真实的 check-then-act 竞争窗口，不只是理论上的），再把临时目录里的文件逐个搬进去。
    中途失败/被杀不会在 output_dir 留下"写了一半"的残留；已存在的目标目录也不会被静默覆盖。

    Raises:
        FileExistsError: output_dir 已存在（不管是否为空）。
    """
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    tmp_dir = output_dir.parent / f".tmp-{output_dir.name}-{uuid.uuid4().hex}"
    tmp_dir.mkdir(parents=True, exist_ok=False)

    try:
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
        # AUDIT PYJSON-CLI-041: this used to be bare json.dumps(metrics_dict, indent=2) --
        # metrics.calmar_ratio (or, since the batch-4 metrics.py fixes, sharpe_ratio too) can
        # be float("inf")/float("-inf") whenever a strategy has no measured drawdown or zero
        # return variance, and json.dumps serializes that as the bare token `Infinity`/
        # `-Infinity`, which is not valid JSON (RFC 8259 has no such literal). That made
        # every artifact from such a run rejected by any strict parser while looking fine to
        # Python's own json.load. save_results() is reached from cmd_run(), a live CLI path,
        # so this was not a theoretical residue -- artifact_export.py already closed this
        # exact gap with _dumps()/_dumps_line() (which replace non-finite floats with `null`
        # and set allow_nan=False as a safety net); this function now reuses those same
        # helpers instead of a second, incomplete implementation of the same fix.
        (tmp_dir / "metrics.json").write_text(_dumps(metrics_dict), encoding="utf-8")

        # validation_report.json
        vr_dict: dict[str, Any] = {
            "timestamp_monotonic": result.validation_report.timestamp_monotonic,
            "no_nan_close": result.validation_report.no_nan_close,
            "no_future_shift_detected": result.validation_report.no_future_shift_detected,
            "bar_count": result.validation_report.bar_count,
            "is_valid": result.validation_report.is_valid,
            "issues": result.validation_report.issues,
        }
        (tmp_dir / "validation_report.json").write_text(_dumps(vr_dict), encoding="utf-8")

        # equity_curve.jsonl
        with (tmp_dir / "equity_curve.jsonl").open("w", encoding="utf-8") as f:
            for ts, eq in zip(result.timestamps, result.equity_curve, strict=True):
                f.write(_dumps_line({"timestamp_utc": ts.isoformat(), "equity": eq}) + "\n")

        # positions.jsonl
        with (tmp_dir / "positions.jsonl").open("w", encoding="utf-8") as f:
            for ts, pos in zip(result.timestamps, result.positions, strict=True):
                f.write(_dumps_line({"timestamp_utc": ts.isoformat(), "position": pos}) + "\n")

        # trades.jsonl
        with (tmp_dir / "trades.jsonl").open("w", encoding="utf-8") as f:
            for tr in result.fills_approx:
                f.write(
                    _dumps_line(
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

        # run_manifest.json — provenance for reproducibility/audit (P2-STRAT-01)
        manifest: dict[str, Any] = {
            "created_at": datetime.now(UTC).isoformat(),
            "output_label": result.output_label,
            "non_auth_assertion": result.non_auth_assertion,
            "strategy_spec": strategy_spec,
            "strategy_params": strategy_params,
            "signals_path": signals_path,
            "ohlcv_fingerprint": ohlcv_fingerprint_info,
            "strategy_fingerprint": strategy_fingerprint_info,
            "python_version": sys.version,
            "pandas_version": pd.__version__,
        }
        (tmp_dir / "run_manifest.json").write_text(_dumps(manifest), encoding="utf-8")
    except Exception:
        shutil.rmtree(tmp_dir, ignore_errors=True)
        raise

    try:
        output_dir.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        shutil.rmtree(tmp_dir, ignore_errors=True)
        raise FileExistsError(f"输出目录已存在，拒绝覆盖: {output_dir}") from None

    for f in tmp_dir.iterdir():
        f.rename(output_dir / f.name)
    tmp_dir.rmdir()
    # Audit PY-PUB-014: same per-file rename loop as artifact_export.py, so the same
    # partially-published window. The marker is written last and is what
    # is_complete_run() checks.
    _write_completion_marker(output_dir)


def cmd_run_risk_aware(args: argparse.Namespace) -> int:
    """执行风险感知回测子命令（P2-RM-03）。"""
    print(NON_AUTH_NOTICE)
    print("[NOTICE] run-risk-aware — NON-AUTHORIZING RESEARCH USE ONLY\n")

    ohlcv_path = Path(args.ohlcv).resolve()
    if not ohlcv_path.exists():
        print(f"[ERROR] OHLCV 文件不存在: {ohlcv_path}", file=sys.stderr)
        return 1

    if bool(args.strategy) == bool(args.signals):
        print("[ERROR] 必须且只能提供 --strategy 或 --signals 之一", file=sys.stderr)
        return 1

    print(f"[INFO] 加载 OHLCV: {ohlcv_path}")
    records = load_ohlcv_csv(
        ohlcv_path,
        market=args.market,
        symbol=args.symbol,
        timeframe=args.timeframe,
    )
    print(f"[INFO] 已加载 {len(records)} 条 OHLCV 记录")

    strategy_params: dict[str, Any] = {}
    signals_path_str: str | None = None
    if args.strategy:
        strategy_params = _parse_strategy_params(args.strategy_params)
        df = records_to_dataframe(records)
        signals = load_signals_from_strategy(
            args.strategy, strategy_params, df, allow_external=args.allow_external_strategy
        )
        print(f"[INFO] 使用策略生成 Signals: {args.strategy}")
    else:
        signals_path = Path(args.signals).resolve()
        if not signals_path.exists():
            print(f"[ERROR] Signals 文件不存在: {signals_path}", file=sys.stderr)
            return 1
        signals_path_str = str(signals_path)
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

    stop_distance_fraction: Decimal | None = None
    if args.stop_distance_fraction is not None:
        stop_distance_fraction = Decimal(str(args.stop_distance_fraction))
        # Audit PY-RISK-005: the old --stop-distance flag was documented in price
        # units, so an existing invocation would pass something like 500 here. Reject
        # it loudly rather than silently sizing the position ~1000x differently --
        # the whole point of the rename is that the wrong unit can no longer pass
        # through unnoticed.
        if not (Decimal("0") < stop_distance_fraction <= Decimal("1")):
            print(
                f"[ERROR] --stop-distance-fraction 必须在 (0, 1] 区间内，"
                f"当前值 {stop_distance_fraction}。它是价格的比例（0.02 = 2%），"
                f"不是绝对报价金额。",
                file=sys.stderr,
            )
            return 2

    print("[INFO] 运行风险感知回测...")
    result = run_risk_aware_backtest(config, records, signals, risk_config, stop_distance_fraction)

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
    validate_run_id(run_id)
    output_root = Path(getattr(args, "output", ".var/risk-aware-backtests"))
    run_dir = export_risk_aware_backtest_artifacts(
        run_id=run_id,
        result=result,
        config=config,
        risk_config=risk_config,
        output_dir=output_root,
        stop_distance_fraction=stop_distance_fraction,
        ohlcv_path=str(ohlcv_path),
        signals_path=signals_path_str or "",
        strategy_spec=args.strategy,
        strategy_params=strategy_params if args.strategy else None,
        ohlcv_fingerprint_info=ohlcv_fingerprint(ohlcv_path, records),
        strategy_fingerprint_info=strategy_source_fingerprint(args.strategy),
    )
    print(f"\n[INFO] Artifacts 已保存至: {run_dir}")
    print(NON_AUTH_NOTICE)
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    """执行回测子命令。"""
    print(NON_AUTH_NOTICE)

    ohlcv_path = Path(args.ohlcv).resolve()
    if not ohlcv_path.exists():
        print(f"[ERROR] OHLCV 文件不存在: {ohlcv_path}", file=sys.stderr)
        return 1

    if bool(args.strategy) == bool(args.signals):
        print("[ERROR] 必须且只能提供 --strategy 或 --signals 之一", file=sys.stderr)
        return 1

    print(f"[INFO] 加载 OHLCV: {ohlcv_path}")
    records = load_ohlcv_csv(
        ohlcv_path,
        market=args.market,
        symbol=args.symbol,
        timeframe=args.timeframe,
    )
    print(f"[INFO] 已加载 {len(records)} 条 OHLCV 记录")

    # 只构建一次 df——策略生成信号和回测执行都用这同一份，不重复调用
    # records_to_dataframe()。
    df = records_to_dataframe(records)

    strategy_params: dict[str, Any] = {}
    signals_path_str: str | None = None
    if args.strategy:
        strategy_params = _parse_strategy_params(args.strategy_params)
        signals = load_signals_from_strategy(
            args.strategy, strategy_params, df, allow_external=args.allow_external_strategy
        )
        print(f"[INFO] 使用策略生成 Signals: {args.strategy}")
    else:
        signals_path = Path(args.signals).resolve()
        if not signals_path.exists():
            print(f"[ERROR] Signals 文件不存在: {signals_path}", file=sys.stderr)
            return 1
        signals_path_str = str(signals_path)
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

    annualization_factor = resolve_annualization_factor(config, records)
    if args.annualization_factor is None:
        print(
            f"[INFO] 年化因子自动推导: {annualization_factor:g}"
            f"（market={args.market}, timeframe={args.timeframe}）"
        )
    else:
        print(f"[INFO] 年化因子（显式指定）: {annualization_factor:g}")

    print("[INFO] 运行回测...")
    result = _run_vectorized_backtest_on_df(
        config, df, signals, annualization_factor=annualization_factor
    )

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
    validate_run_id(run_id)
    output_dir = Path(".var") / "backtests" / run_id
    save_results(
        result,
        output_dir,
        strategy_spec=args.strategy,
        strategy_params=strategy_params if args.strategy else None,
        signals_path=signals_path_str,
        ohlcv_fingerprint_info=ohlcv_fingerprint(ohlcv_path, records),
        strategy_fingerprint_info=strategy_source_fingerprint(args.strategy),
    )
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
    run_parser.add_argument(
        "--signals", default=None, help="Signals CSV 文件路径（与 --strategy 二选一）"
    )
    run_parser.add_argument(
        "--strategy",
        default=None,
        help='策略 spec，形如 "py_core.strategies.sma_crossover:SmaCrossoverStrategy"'
        "（与 --signals 二选一）",
    )
    run_parser.add_argument(
        "--strategy-params",
        default=None,
        help='策略构造参数，JSON 对象字符串，例如 \'{"fast_window": 5, "slow_window": 20}\'',
    )
    run_parser.add_argument(
        "--allow-external-strategy",
        action="store_true",
        help="允许加载 py_core.strategies 命名空间之外的策略模块（默认不允许）",
    )
    run_parser.add_argument(
        "--initial-capital", type=float, default=100000.0, help="初始资本（默认 100000）"
    )
    run_parser.add_argument("--fee-bps", type=float, default=10.0, help="手续费 bps（默认 10）")
    run_parser.add_argument("--slippage-bps", type=float, default=5.0, help="滑点 bps（默认 5）")
    run_parser.add_argument(
        "--risk-free-rate", type=float, default=0.0, help="年化无风险利率（默认 0.0）"
    )
    run_parser.add_argument(
        "--annualization-factor",
        type=float,
        default=None,
        help="年化因子（默认自动从 --market + --timeframe 推导：crypto_spot 按 7×24 全年，"
        "如 1d→365、1h→8760；显式传值可覆盖）",
    )
    run_parser.add_argument("--market", default="crypto_spot", help="市场类型（默认 crypto_spot）")
    run_parser.add_argument("--symbol", default="BTC/USDT", help="交易对符号（默认 BTC/USDT）")
    run_parser.add_argument("--timeframe", default="1d", help="时间周期（默认 1d）")
    run_parser.add_argument("--run-id", default=None, help="自定义 run_id（用于输出目录）")

    # ── run-risk-aware subcommand ────────────────────────────────────────────
    rra_parser = subparsers.add_parser("run-risk-aware", help="运行风险感知回测（P2-RM-03）")
    rra_parser.add_argument("--ohlcv", required=True, help="OHLCV CSV 文件路径")
    rra_parser.add_argument(
        "--signals", default=None, help="Signals CSV 文件路径（与 --strategy 二选一）"
    )
    rra_parser.add_argument(
        "--strategy",
        default=None,
        help='策略 spec，形如 "py_core.strategies.sma_crossover:SmaCrossoverStrategy"'
        "（与 --signals 二选一）",
    )
    rra_parser.add_argument(
        "--strategy-params",
        default=None,
        help='策略构造参数，JSON 对象字符串，例如 \'{"fast_window": 5, "slow_window": 20}\'',
    )
    rra_parser.add_argument(
        "--allow-external-strategy",
        action="store_true",
        help="允许加载 py_core.strategies 命名空间之外的策略模块（默认不允许）",
    )
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
        "--stop-distance-fraction",
        type=float,
        default=None,
        help="止损距离，表示为价格的比例（0 < x <= 1，例如 0.02 = 2%%；可选）。"
             "注意：不是绝对报价金额 —— 见 PY-RISK-005。",
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
