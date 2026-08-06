"""
Binance 公开 REST 历史 K 线拉取 — 本地 CLI 入口（P2-MD-02）

独立于 py_core/backtests/cli.py 的小 CLI，只做"拉数据存文件"这一件事——两个 CLI 之间只通过
输出的 ohlcv.csv 文件耦合，不共享任何代码路径。

所有输出均为研究/回测用途的历史行情数据，不代表实盘数据源的授权或点位精度保证。

用法：
    python -m py_core.market_data.cli fetch \\
        --symbol BTCUSDT --interval 1d \\
        --start 2024-01-01 --end 2024-06-01 \\
        --output-dir path/to/output_dir

输出（--output-dir 是目录，原子发布——见 publish_fetch_output() 的实现）：
    <output-dir>/ohlcv.csv        —— timestamp_utc,open,high,low,close,volume，
                                      格式跟 py_core.backtests.cli.load_ohlcv_csv() 的读取端对得上
    <output-dir>/manifest.json    —— 复用既有的 ManualOhlcvImportManifest
    <output-dir>/fetch_meta.json  —— fetch-only 的 provenance（FetchMeta）
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

from py_core.market_data.binance_public_rest import (
    BinancePublicRestError,
    FetchMeta,
    fetch_binance_ohlcv,
)

NON_AUTH_NOTICE = (
    "\n[NOTICE] All fetched data is for RESEARCH/BACKTESTING USE ONLY.\n"
    "NOT financial advice. NOT a live data feed guarantee. NOT trading authorization.\n"
)


def _parse_utc_date(s: str) -> datetime:
    """Parse a "YYYY-MM-DD" date string as UTC midnight. Deliberately strict (unlike
    py_core.backtests.cli._parse_timestamp_utc's multi-format tolerance) -- this CLI's
    date args are always meant to be whole-day boundaries, not arbitrary timestamps."""
    try:
        parsed = datetime.strptime(s.strip(), "%Y-%m-%d")
    except ValueError as exc:
        raise ValueError(f"日期必须是 YYYY-MM-DD 格式，收到: {s!r}") from exc
    return parsed.replace(tzinfo=UTC)


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
    一致的行为）-> 把临时目录里的文件逐个搬进刚占好位的 output_dir -> 删除空的临时目录。

    任何一步失败，只清理这一次自己创建的临时目录，不碰任何已经存在的东西。

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

    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    if args.command == "fetch":
        sys.exit(cmd_fetch(args))
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
