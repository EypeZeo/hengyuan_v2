"""
Binance 公开 REST 历史 K 线拉取 / 仓库管理 — 本地 CLI 入口（P2-MD-02，批次 1）

独立于 py_core/backtests/cli.py 的小 CLI——两者只通过输出的 ohlcv.csv 文件耦合，不共享任何
代码路径。`fetch` 是单次拉取写平铺文件；`backfill`/`status`/`verify` 是批次 1
`py_core/market_data/warehouse.py`（按月分区 Parquet 仓库）的增量维护入口，两组子命令内部
互不依赖，只是共享同一个 argparse 入口和输出风格。

所有输出均为研究/回测用途的历史行情数据，不代表实盘数据源的授权或点位精度保证。

用法：
    python -m py_core.market_data.cli fetch \\
        --symbol BTCUSDT --interval 1d --start 2024-01-01 --end 2024-06-01 \\
        --output-dir path/to/output_dir

    python -m py_core.market_data.cli backfill \\
        --root path/to/warehouse --symbol BTCUSDT --interval 1d --start 2024-01-01

    python -m py_core.market_data.cli status --root path/to/warehouse --symbol BTCUSDT --interval 1d
    python -m py_core.market_data.cli verify --root path/to/warehouse --symbol BTCUSDT --interval 1d

`fetch` 输出（--output-dir 是目录，原子发布——见 publish_fetch_output() 的实现）：
    <output-dir>/ohlcv.csv        —— timestamp_utc,open,high,low,close,volume，
                                      格式跟 py_core.backtests.cli.load_ohlcv_csv() 的读取端对得上
    <output-dir>/manifest.json    —— 复用既有的 ManualOhlcvImportManifest
    <output-dir>/fetch_meta.json  —— fetch-only 的 provenance（FetchMeta）

`verify` 的退出码约定：0 = 仓库干净（或本来就没有数据），1 = `fetch`/`backfill` 本身执行
失败（网络/校验错误），**也是** 1 = 仓库连续性有问题（存在缺口）——"命令失败"和"命令成功
但报告了一个不连续的仓库"目前共用同一个非零退出码，靠打印内容区分，不是两个独立的错误
通道；这个约定写在这里，避免被脚本调用方误当成"命令本身出错"。
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import hashlib
import json
import shutil
import sys
import uuid
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from py_core.backtests.artifact_export import _write_completion_marker
from py_core.manual_ohlcv import (
    AvailableTimePosture,
    CanonicalMarketSymbol,
    ManualMarket,
    ManualOhlcvImportManifest,
    ManualOhlcvValidationError,
    NormalizedOhlcvRecord,
    OhlcvInputFormat,
    OhlcvTimeframe,
    TimestampPosture,
    manifest_to_dict,
)
from py_core.market_data import warehouse as wh
from py_core.market_data.binance_public_rest import (
    BinancePublicRestError,
    FetchMeta,
    fetch_binance_ohlcv,
)
from py_core.market_data.warehouse_backfill import BackfillError
from py_core.market_data.warehouse_backfill import backfill as run_backfill

NON_AUTH_NOTICE = (
    "\n[NOTICE] All fetched data is for RESEARCH/BACKTESTING USE ONLY.\n"
    "NOT financial advice. NOT a live data feed guarantee. NOT trading authorization.\n"
)


def _parse_utc_date(s: str) -> datetime:
    """Parse a "YYYY-MM-DD" date string as UTC midnight. Deliberately strict (unlike
    py_core.backtests.cli._parse_timestamp_utc's multi-format tolerance) -- this CLI's
    date args are always meant to be whole-day boundaries, not arbitrary timestamps."""
    try:
        return datetime.strptime(s.strip(), "%Y-%m-%d").replace(tzinfo=UTC)
    except ValueError as exc:
        raise ValueError(f"日期必须是 YYYY-MM-DD 格式，收到: {s!r}") from exc


def _write_ohlcv_csv(path: Path, records: list[NormalizedOhlcvRecord]) -> None:
    """写出的格式必须跟 py_core.backtests.cli.load_ohlcv_csv() 的读取端完全对得上。"""
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp_utc", "open", "high", "low", "close", "volume"])
        for r in records:
            writer.writerow(
                [
                    r.event_time_utc.isoformat(),
                    str(r.open_price),
                    str(r.high_price),
                    str(r.low_price),
                    str(r.close_price),
                    str(r.volume),
                ]
            )


def _fetch_meta_to_dict(meta: FetchMeta) -> dict[str, Any]:
    return {
        "request_symbol": meta.request_symbol,
        "request_interval": meta.request_interval,
        "request_start_utc": meta.request_start_utc.isoformat(),
        "request_end_utc": meta.request_end_utc.isoformat(),
        "coverage_end_utc": meta.coverage_end_utc.isoformat(),
        "retrieval_cutoff_utc": meta.retrieval_cutoff_utc.isoformat(),
        "dropped_unclosed_bar_count": meta.dropped_unclosed_bar_count,
        "host": meta.host,
        "page_count": meta.page_count,
        "csv_sha256": meta.csv_sha256,
        "schema_version": meta.schema_version,
        "response_weight_headers": meta.response_weight_headers,
    }


def publish_fetch_output(
    records: list[NormalizedOhlcvRecord],
    meta: FetchMeta,
    output_dir: Path,
    *,
    symbol: str,
    interval: str,
) -> None:
    """原子发布：临时目录写完三个文件 -> output_dir.mkdir(exist_ok=False) 真正原子的排他占位
    （不管目标是否已存在、是否为空，两个平台语义一致，不依赖目录级 rename 在不同操作系统上不
    一致的行为）-> 把临时目录里的文件逐个搬进刚占好位的 output_dir -> 写入 ``.complete`` 标记
    -> 删除空的临时目录。

    任何一步失败，只清理这一次自己创建的临时目录，不碰任何已经存在的东西。

    AUDIT PYPUB-MDCLI-042: 搬运是逐文件 rename，所以在 output_dir 占位与最后一个文件落地之间
    被 kill 会留下一个"存在但不完整"的目录——与 artifact_export.py::export_risk_aware_backtest_artifacts()
    / backtests/cli.py::save_results() 完全同构的窗口（前者的审计 PY-PUB-014 已经记录过这个
    机制）。这里此前是三个同构发布点中唯一没有写 ``.complete`` 标记的一个，消费方
    （``is_complete_run()``）因此无法区分这里的输出是否完整。复用 artifact_export.py 已有的
    ``_write_completion_marker()``，不第三次重新实现同一个修复。

    Raises:
        FileExistsError: output_dir 已经存在（不管是否为空）。
    """
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    tmp_dir = output_dir.parent / f".tmp-{output_dir.name}-{uuid.uuid4().hex}"
    tmp_dir.mkdir(parents=True, exist_ok=False)

    try:
        csv_path = tmp_dir / "ohlcv.csv"
        _write_ohlcv_csv(csv_path, records)
        csv_sha256 = hashlib.sha256(csv_path.read_bytes()).hexdigest()
        meta = dataclasses.replace(meta, csv_sha256=csv_sha256)

        now_utc = datetime.now(UTC)
        manifest = ManualOhlcvImportManifest(
            import_id=uuid.uuid4().hex,
            market=ManualMarket.CRYPTO_SPOT,
            symbol=CanonicalMarketSymbol(symbol),
            timeframe=OhlcvTimeframe(interval),
            source_name="binance_public_rest",
            input_path="ohlcv.csv",
            input_format=OhlcvInputFormat.BINANCE_PUBLIC_REST,
            source_timezone="UTC",
            timestamp_posture=TimestampPosture.OFFSET_PROVIDED_IN_PAYLOAD,
            available_time_posture=AvailableTimePosture.UNKNOWN_MANUAL_IMPORT,
            # 已收盘只能证明数值不会再变，不能证明历史时刻的真实策略确实能以零延迟拿到这条
            # 数据（服务器端索引/API 可用性延迟没有独立验证过）——不过度声明 point-in-time
            # 安全性。假设本身记在 fetch_meta.json 里（隐含于 retrieval_cutoff_utc）。
            point_in_time_safe=False,
            imported_at_utc=now_utc,
            file_sha256=csv_sha256,
            record_count=len(records),
            first_event_time_utc=records[0].event_time_utc if records else None,
            last_event_time_utc=records[-1].event_time_utc if records else None,
        )
        (tmp_dir / "manifest.json").write_text(
            json.dumps(manifest_to_dict(manifest), indent=2, ensure_ascii=False), encoding="utf-8"
        )
        (tmp_dir / "fetch_meta.json").write_text(
            json.dumps(_fetch_meta_to_dict(meta), indent=2, ensure_ascii=False), encoding="utf-8"
        )
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

    # AUDIT PYPUB-MDCLI-042: written LAST, so its presence is proof every other file
    # already landed -- see docstring above and artifact_export.py's PY-PUB-014 comment.
    _write_completion_marker(output_dir)


def cmd_fetch(args: argparse.Namespace) -> int:
    print(NON_AUTH_NOTICE)

    try:
        start = _parse_utc_date(args.start)
        end = _parse_utc_date(args.end)
    except ValueError as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 1

    output_dir = Path(args.output_dir).resolve()

    print(f"[INFO] 拉取 {args.symbol} {args.interval} [{start.isoformat()}, {end.isoformat()})...")
    try:
        records, meta = fetch_binance_ohlcv(
            args.symbol,
            args.interval,
            start,
            end,
            market=ManualMarket(args.market),
            max_requests=args.max_requests,
            strict_coverage=args.strict_coverage,
        )
    except (ValueError, BinancePublicRestError, ManualOhlcvValidationError) as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 1

    print(f"[INFO] 拉到 {len(records)} 条记录（{meta.page_count} 次分页请求）")
    if meta.coverage_end_utc < end:
        print(
            f"[WARNING] 实际覆盖到 {meta.coverage_end_utc.isoformat()}，早于请求的终点 "
            f"{end.isoformat()}——{meta.dropped_unclosed_bar_count} 根末尾 K 线因为尚未收盘"
            "被丢弃。如果需要硬保证覆盖完整区间，加 --strict-coverage 重跑。",
            file=sys.stderr,
        )

    try:
        publish_fetch_output(records, meta, output_dir, symbol=args.symbol, interval=args.interval)
    except FileExistsError as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 1

    print(f"\n[INFO] 结果已保存至: {output_dir}")
    print(NON_AUTH_NOTICE)
    return 0


def _identity_from_args(args: argparse.Namespace) -> tuple[Path, ManualMarket, CanonicalMarketSymbol, OhlcvTimeframe]:
    """--root/--market/--symbol/--interval 是 backfill/status/verify 三个子命令共用的
    身份四元组，集中解析一次，不在每个 cmd_* 里各写一遍。"""
    root = Path(args.root).resolve()
    market = ManualMarket(args.market)
    symbol = CanonicalMarketSymbol(args.symbol)
    timeframe = OhlcvTimeframe(args.interval)
    return root, market, symbol, timeframe


def cmd_backfill(args: argparse.Namespace) -> int:
    print(NON_AUTH_NOTICE)

    try:
        root, market, symbol, timeframe = _identity_from_args(args)
        start = _parse_utc_date(args.start)
        end = _parse_utc_date(args.end) if args.end else datetime.now(UTC)
    except ValueError as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 1

    print(f"[INFO] backfill {symbol.value} {timeframe.value} -> [{start.isoformat()}, {end.isoformat()})")
    try:
        report = run_backfill(root, market, symbol, timeframe, start, end, max_requests=args.max_requests)
    except (
        ValueError,
        BinancePublicRestError,
        ManualOhlcvValidationError,
        BackfillError,
        wh.WarehouseError,
    ) as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 1

    if report.already_current:
        print(f"[INFO] 已是最新（resume 点 {report.resumed_from.isoformat()} >= 请求终点），未发起网络请求")
        print(NON_AUTH_NOTICE)
        return 0

    print(f"[INFO] resume 点: {report.resumed_from.isoformat()}")
    print(f"[INFO] 拉到 {report.fetched_records} 条记录")
    print(
        f"[INFO] 写入 {report.write_report.rows_written} 条，跳过 "
        f"{report.write_report.rows_skipped_duplicate} 条重复"
    )
    if report.fetch_coverage_end_utc is not None and report.fetch_coverage_end_utc < end:
        print(
            f"[WARNING] 实际覆盖到 {report.fetch_coverage_end_utc.isoformat()}，早于请求的终点 "
            f"{end.isoformat()}——末尾 K 线可能因为尚未收盘被丢弃，之后重跑 backfill 会自动"
            "从这里继续。",
            file=sys.stderr,
        )
    print(NON_AUTH_NOTICE)
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    try:
        root, market, symbol, timeframe = _identity_from_args(args)
        cov = wh.coverage(root, market, symbol, timeframe)
    except (ValueError, wh.WarehouseError) as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 1
    if cov is None:
        print(f"[INFO] {symbol.value} {timeframe.value}: 无数据")
        return 0

    print(f"[INFO] {symbol.value} {timeframe.value}:")
    print(f"  record_count : {cov.record_count}")
    print(f"  covered      : [{cov.covered_start_utc.isoformat()}, {cov.covered_end_utc.isoformat()})")
    print(f"  is_contiguous: {cov.is_contiguous}")
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    try:
        root, market, symbol, timeframe = _identity_from_args(args)
        report = wh.scan_gaps(root, market, symbol, timeframe, max_issues=args.max_issues)
    except (ValueError, wh.WarehouseError) as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 1
    if report.total_issue_count == 0:
        print(f"[INFO] {symbol.value} {timeframe.value}: 干净，无缺口")
        return 0

    print(f"[WARNING] {symbol.value} {timeframe.value}: 发现 {report.total_issue_count} 处缺口", file=sys.stderr)
    for issue in report.issues:
        print(f"  缺失于 {issue.at.isoformat()}（分区 {issue.partition}）", file=sys.stderr)
    if report.truncated:
        print(
            f"  ...还有 {report.total_issue_count - len(report.issues)} 处未列出"
            f"（--max-issues {args.max_issues}）",
            file=sys.stderr,
        )
    return 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m py_core.market_data.cli",
        description="P2-MD-02 Binance 公开 REST 历史 K 线拉取 CLI（仅供研究，非交易授权）",
    )
    subparsers = parser.add_subparsers(dest="command")

    fetch_parser = subparsers.add_parser("fetch", help="拉取历史 K 线并原子发布到输出目录")
    fetch_parser.add_argument("--symbol", required=True, help="Binance 现货交易对，如 BTCUSDT")
    fetch_parser.add_argument("--interval", required=True, help="K 线周期，如 1d/1h/15m（不支持 1M）")
    fetch_parser.add_argument(
        "--start", required=True, help="起始日期 YYYY-MM-DD（按 UTC 00:00:00 处理）"
    )
    fetch_parser.add_argument(
        "--end", required=True, help="结束日期 YYYY-MM-DD（按 UTC 00:00:00 处理，半开区间 [start, end)）"
    )
    fetch_parser.add_argument(
        "--output-dir", required=True, help="输出目录（创建 ohlcv.csv/manifest.json/fetch_meta.json）"
    )
    fetch_parser.add_argument(
        "--max-requests", type=int, default=50, help="分页请求次数软上限（默认 50，硬上限 100）"
    )
    fetch_parser.add_argument(
        "--strict-coverage",
        action="store_true",
        help="实际覆盖不到请求区间（末尾 K 线未收盘）时直接报错，而不是打印警告",
    )
    fetch_parser.add_argument("--market", default="crypto_spot", help="市场类型（默认 crypto_spot）")

    def _add_identity_args(p: argparse.ArgumentParser) -> None:
        p.add_argument("--root", required=True, help="仓库根目录（py_core.market_data.warehouse 的 root）")
        p.add_argument("--symbol", required=True, help="Binance 现货交易对，如 BTCUSDT")
        p.add_argument("--interval", required=True, help="K 线周期，如 1d/1h/15m")
        p.add_argument("--market", default="crypto_spot", help="市场类型（默认 crypto_spot）")

    backfill_parser = subparsers.add_parser(
        "backfill", help="从已有覆盖尾部（或 --start，首次回补时）增量拉取到 --end"
    )
    _add_identity_args(backfill_parser)
    backfill_parser.add_argument(
        "--start", required=True, help="起始日期 YYYY-MM-DD（仅在这个 (symbol,interval) 组合还没有任何数据时使用；"
        "已有数据时忽略，从已有覆盖尾部继续——若 --start 早于已有覆盖起点会报错，不静默忽略）"
    )
    backfill_parser.add_argument(
        "--end", default=None, help="结束日期 YYYY-MM-DD（默认当次调用时的 UTC 当前时间）"
    )
    backfill_parser.add_argument(
        "--max-requests", type=int, default=50, help="分页请求次数软上限（默认 50，硬上限 100）"
    )

    status_parser = subparsers.add_parser("status", help="打印一个 (symbol,interval) 组合的覆盖范围")
    _add_identity_args(status_parser)

    verify_parser = subparsers.add_parser(
        "verify", help="扫描缺口；干净退出码 0，有缺口退出码 1（见模块文档的退出码约定）"
    )
    _add_identity_args(verify_parser)
    verify_parser.add_argument(
        "--max-issues", type=int, default=1000, help="最多列出多少条缺口（默认 1000，超过只计数不列出）"
    )

    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    if args.command == "fetch":
        sys.exit(cmd_fetch(args))
    elif args.command == "backfill":
        sys.exit(cmd_backfill(args))
    elif args.command == "status":
        sys.exit(cmd_status(args))
    elif args.command == "verify":
        sys.exit(cmd_verify(args))
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
