"""
Binance 公开 REST 历史 K 线拉取 — P2-MD-02

只调用 Binance 公开、无需签名的市场数据端点（GET /api/v3/klines），不涉及账户、API Key 或任何
私有签名 REST（那部分仍然是 docs/BINANCE_PRIVATE_REST_L4_SPEC.md 里"Draft, Not Implemented"的
范围，这个模块跟那个完全无关）。产出跟 py_core.manual_ohlcv 里既有的 NormalizedOhlcvRecord 完全
一样的形状，回测引擎不需要知道数据是从 CSV 读的还是从这里拉的。

单实例单次调用、无跨调用共享可变状态、不共享 urllib opener/文件句柄；未来若要并行拉取多个
symbol/区间，应该用多进程隔离，每个 worker 写各自独立的临时文件和输出目录。

只返回**已收盘**的 K 线（见 fetch_binance_ohlcv 的 retrieval_cutoff 逻辑）；即便如此，"已收盘"
只能证明这根 K 线的数值不会再变，不能证明一个真实策略在那个历史时刻点确实能以零延迟拿到这条数据
（服务器端索引/API 可用性延迟没有独立验证过）——调用方不应该把这里的数据当作绝对的
point-in-time-safe 证据，manifest 里也如实标注这一点（point_in_time_safe=False）。

所有输出均为研究/回测用途，不代表交易信号或数据源的实盘授权。
"""

from __future__ import annotations

import http.client
import itertools
import json
import re
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import UTC, datetime
from decimal import Decimal, InvalidOperation

from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    ManualOhlcvValidationError,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
    validate_ohlcv_batch,
    validate_ohlcv_record,
)

_KLINES_URL = "https://data-api.binance.vision/api/v3/klines"  # Binance 官方推荐的公共市场数据
# 域名（不是交易用的 api.binance.com）。限流仍然按实际收到的响应处理，不假设有独立预算——这只是
# Binance 推荐的用法，不是这个模块能验证的硬保证。
_MAX_LIMIT_PER_REQUEST = 1000
_ABSOLUTE_MAX_REQUESTS_CEILING = 100  # 10 万根 K 线。本轮不做流式 writer，这个上限必须匹配
# "list 一次性建好、不做流式"这个既有前提能接受的内存规模，不能单独放大。
_MAX_RESPONSE_BYTES = 2_000_000
_MAX_ERROR_BODY_BYTES = 2_000
_KLINE_ROW_FIELD_COUNT = 12  # Binance klines 单行的真实字段数（本模块只用到其中 7 个：
# open_time/open/high/low/close/volume/close_time，但完整性校验要看全部 12 个，防 schema 漂移/伪造）。
_REQUEST_TIMEOUT_SECONDS = 10.0
_USER_AGENT = "hengyuan-v2-py_core-market-data/1 (+https://github.com/EypeZeo/hengyuan_v2)"

# "1M"（Binance 日历月线）故意不支持：py_core.manual_ohlcv.OhlcvTimeframe 的校验正则
# (^[1-9]\d*[mhdw]$，大小写不敏感、规范化成小写) 会把 "1M" 变成 "1m"，跟"1 分钟"撞在一起——
# 这个模型结构性地无法表示月线，静默接受会产生错误数据，所以直接拒绝。
_SUPPORTED_INTERVALS_MS: dict[str, int] = {
    "1m": 60_000,
    "3m": 180_000,
    "5m": 300_000,
    "15m": 900_000,
    "30m": 1_800_000,
    "1h": 3_600_000,
    "2h": 7_200_000,
    "4h": 14_400_000,
    "6h": 21_600_000,
    "8h": 28_800_000,
    "12h": 43_200_000,
    "1d": 86_400_000,
    "3d": 259_200_000,
    "1w": 604_800_000,
}

# 比 CanonicalMarketSymbol 自己的校验更严格——那个类只要求非空、无空白，像 "BTC/USDT"（带斜杠）
# 能通过它但不是 Binance 现货交易对的合法格式。
_SYMBOL_PATTERN = re.compile(r"^[A-Z0-9]+$")


class BinancePublicRestError(RuntimeError):
    """本模块所有错误的基类——调用方可以只 except 这一个兜底，也可以按需捕获下面的具体子类。"""


class BinanceTransportError(BinancePublicRestError):
    """DNS/TLS/连接/超时——请求根本没能拿到一个 HTTP 响应。"""


class BinanceHttpError(BinancePublicRestError):
    """收到了非 2xx 响应。status_code 是实际的 HTTP 状态码；retry_after 有值时说明服务端给了
    退避提示（Retry-After 响应头），本模块自己不做自动重试。"""

    def __init__(self, message: str, *, status_code: int, retry_after: str | None = None) -> None:
        super().__init__(message)
        self.status_code = status_code
        self.retry_after = retry_after


class BinanceRateLimitedError(BinanceHttpError):
    """HTTP 429（短暂限流）或 418（IP 被封，更严重）——单独的子类，方便调用方专门识别限流场景
    而不用比较 status_code 数字。本模块不做自动重试退避：遇到限流让调用方自己决定何时重试，
    比藏一个自动重试循环更诚实、更不容易在使用者没注意的时候把请求打得更猛。"""


class BinanceResponseError(BinancePublicRestError):
    """响应格式不对：顶层不是 list、单行字段数/类型不对、响应体超过 _MAX_RESPONSE_BYTES、
    数值字段无法解析成 Decimal 等。"""


class BinanceKlineGapError(BinancePublicRestError):
    """数据连续性问题：页内相邻 K 线间隔不等于一个 interval、新页跟已累积数据不连续、游标未到
    终点（且尚未追上"现在"）时收到空页、或 close_time 与 open_time+interval-1 的内在约束不一致。"""


class BinanceIncompleteCoverageError(BinancePublicRestError):
    """仅当 strict_coverage=True 且实际覆盖到的时间早于请求的 end_time_utc 时抛出（通常是因为
    请求区间延伸到了"现在"附近，末尾的 K 线还没收盘、被过滤掉了）。"""


@dataclass(frozen=True, slots=True)
class FetchMeta:
    """这次拉取的 fetch-only 元信息。写进 fetch_meta.json，跟 manifest.json 交叉核对用——
    manual_ohlcv.ManualOhlcvImportManifest 是手工导入和网络拉取共用的通用形状，这里装的是那个
    通用形状装不下的 fetch-only 信息。csv_sha256 由调用方（CLI）在写完 CSV 之后用
    dataclasses.replace() 回填，fetch_binance_ohlcv() 本身不写任何文件。"""

    request_symbol: str
    request_interval: str
    request_start_utc: datetime
    request_end_utc: datetime
    coverage_end_utc: datetime  # 实际覆盖到的截止时间；因为过滤未收盘 bar，可能早于 request_end_utc
    retrieval_cutoff_utc: datetime  # 判定"已收盘"用的时间戳，一次调用内只采样一次
    dropped_unclosed_bar_count: int
    host: str
    page_count: int
    csv_sha256: str
    schema_version: int
    response_weight_headers: dict[str, str]  # 最后一次成功响应的 X-MBX-* 限流用量头，尽力而为


def _to_ms(dt: datetime) -> int:
    if dt.tzinfo is None:
        raise ValueError("datetime 必须带时区（UTC）")
    return int(dt.timestamp() * 1000)


def _from_ms(ms: int) -> datetime:
    return datetime.fromtimestamp(ms / 1000, tz=UTC)


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    """拒绝所有 3xx 跳转，不维护 allowlist——一个正确配置的 Binance 公开市场数据端点正常不会
    跳转，直接拒绝比维护 allowlist 更简单也够用。默认的 urlopen() 行为是自动跟随跳转（包括跳到
    不同 host），这里显式覆盖掉。"""

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # type: ignore[no-untyped-def]
        raise urllib.error.HTTPError(
            req.full_url, code, f"Redirect refused (would go to {newurl!r})", headers, fp
        )


def _build_opener() -> urllib.request.OpenerDirector:
    https_handler = urllib.request.HTTPSHandler(context=ssl.create_default_context())
    return urllib.request.build_opener(_NoRedirectHandler(), https_handler)


def _fetch_klines_page(
    opener: urllib.request.OpenerDirector,
    symbol: str,
    interval: str,
    start_time_ms: int,
    end_time_ms: int,
    limit: int,
) -> tuple[list[object], dict[str, str]]:
    """一次 HTTP 请求，返回 (Binance 原始 klines 数组, 限流用量 header 字典)。未做任何字段转换。"""
    query = urllib.parse.urlencode(
        {
            "symbol": symbol,
            "interval": interval,
            "startTime": start_time_ms,
            "endTime": end_time_ms,
            "limit": limit,
        }
    )
    url = f"{_KLINES_URL}?{query}"
    request = urllib.request.Request(url, headers={"User-Agent": _USER_AGENT})

    try:
        with opener.open(request, timeout=_REQUEST_TIMEOUT_SECONDS) as response:
            body = response.read(_MAX_RESPONSE_BYTES + 1)
            weight_headers = {
                k: v for k, v in response.headers.items() if k.upper().startswith("X-MBX")
            }
    except urllib.error.HTTPError as exc:
        try:
            body_excerpt = exc.read(_MAX_ERROR_BODY_BYTES).decode("utf-8", errors="replace")
        # AUDIT LINT-BACKLOG-059: this is a best-effort diagnostic read of the error
        # response body -- .decode(errors="replace") never raises, so the only real
        # failure modes are the underlying socket read itself (OSError, e.g. a dropped
        # connection) or an incomplete HTTP response (http.client.HTTPException, which is
        # not an OSError subclass). Narrowed from a blind `except Exception` so a genuine
        # bug elsewhere isn't silently swallowed here.
        except (OSError, http.client.HTTPException):
            body_excerpt = ""
        retry_after = exc.headers.get("Retry-After") if exc.headers else None
        message = (
            f"Binance klines 请求失败: HTTP {exc.code} {exc.reason}"
            + (f"，Retry-After={retry_after}" if retry_after else "")
            + (f"，响应片段: {body_excerpt[:200]!r}" if body_excerpt else "")
        )
        if exc.code in (429, 418):
            raise BinanceRateLimitedError(message, status_code=exc.code, retry_after=retry_after) from exc
        raise BinanceHttpError(message, status_code=exc.code, retry_after=retry_after) from exc
    except urllib.error.URLError as exc:
        raise BinanceTransportError(f"Binance klines 请求失败（传输层）: {exc.reason}") from exc
    except TimeoutError as exc:
        raise BinanceTransportError(f"Binance klines 请求超时: {exc}") from exc

    if len(body) > _MAX_RESPONSE_BYTES:
        raise BinanceResponseError(f"响应体超过 {_MAX_RESPONSE_BYTES} 字节上限")

    try:
        data = json.loads(body)
    except json.JSONDecodeError as exc:
        raise BinanceResponseError("Binance klines 响应不是合法 JSON") from exc
    if not isinstance(data, list):
        raise BinanceResponseError(f"Binance klines 响应格式非预期: {type(data).__name__}")
    if len(data) > limit:
        raise BinanceResponseError(f"响应行数 {len(data)} 超过请求的 limit={limit}")

    return data, weight_headers


def _validate_kline_row_shape(row: object, interval_ms: int) -> tuple[int, int]:
    """校验单行原始响应的 schema，早于任何 Decimal 转换/下标运算。返回 (open_time_ms, close_time_ms)。"""
    if not isinstance(row, list) or len(row) != _KLINE_ROW_FIELD_COUNT:
        raise BinanceResponseError(
            f"klines 行字段数非预期（应为 {_KLINE_ROW_FIELD_COUNT}）: {row!r}"
        )
    open_time, close_time = row[0], row[6]
    # bool 是 int 的子类，isinstance(True, int) 是 True——必须显式排除，否则 JSON 里的
    # true/false 会被当成合法时间戳整数放行。
    if isinstance(open_time, bool) or not isinstance(open_time, int):
        raise BinanceResponseError(f"klines open_time 不是合法整数: {open_time!r}")
    if isinstance(close_time, bool) or not isinstance(close_time, int):
        raise BinanceResponseError(f"klines close_time 不是合法整数: {close_time!r}")

    expected_close = open_time + interval_ms - 1
    if close_time != expected_close:
        raise BinanceKlineGapError(
            f"klines close_time ({close_time}) 与 open_time+interval-1 ({expected_close}) 不一致，"
            "疑似 schema 漂移或响应被篡改"
        )
    return open_time, close_time


def _kline_row_to_record(
    row: list[object],
    open_time_ms: int,
    market: ManualMarket,
    symbol: CanonicalMarketSymbol,
    timeframe: OhlcvTimeframe,
) -> NormalizedOhlcvRecord:
    _open_time, open_p, high_p, low_p, close_p, volume = row[:6]
    try:
        return NormalizedOhlcvRecord(
            market=market,
            symbol=symbol,
            timeframe=timeframe,
            event_time_utc=_from_ms(open_time_ms),
            open_price=Decimal(str(open_p)),
            high_price=Decimal(str(high_p)),
            low_price=Decimal(str(low_p)),
            close_price=Decimal(str(close_p)),
            volume=Decimal(str(volume)),
        )
    except (InvalidOperation, ValueError, TypeError) as exc:
        raise BinanceResponseError(f"klines 行数值字段无法解析为 Decimal: {row!r}") from exc


def fetch_binance_ohlcv(
    symbol: str,
    interval: str,
    start_time_utc: datetime,
    end_time_utc: datetime,
    *,
    market: ManualMarket = ManualMarket.CRYPTO_SPOT,
    max_requests: int = 50,
    strict_coverage: bool = False,
) -> tuple[list[NormalizedOhlcvRecord], FetchMeta]:
    """自动分页拉取 [start_time_utc, end_time_utc) 区间**已收盘**的历史 K 线。

    strict_coverage=False（默认）：如果因为过滤未收盘 bar 导致实际覆盖不到请求的
    end_time_utc，正常返回，但 meta.coverage_end_utc 会明确小于 end_time_utc——调用方
    （尤其 CLI）必须检查这个字段，不能假设拿到的就是完整区间。
    strict_coverage=True：覆盖不足直接 raise BinanceIncompleteCoverageError。

    Args:
        symbol: Binance 现货交易对，如 "BTCUSDT"（大小写不敏感，内部转大写；必须只含
            大写字母/数字，比 CanonicalMarketSymbol 自己的校验更严格）。
        interval: Binance K 线周期字符串，必须是 _SUPPORTED_INTERVALS_MS 里的一个
            （"1M" 月线不支持，见模块顶部注释）。
        start_time_utc/end_time_utc: 必须带时区（UTC），start < end。
        market: 落到 NormalizedOhlcvRecord.market 的值，默认 crypto_spot。
        max_requests: 分页请求次数软上限（调用方可调），不能超过
            _ABSOLUTE_MAX_REQUESTS_CEILING。
        strict_coverage: 见上。

    Returns:
        (records, meta)。records 形状跟 load_ohlcv_csv() 完全一致，按时间升序排列，已经过
        （本轮加固过的）validate_ohlcv_record()/validate_ohlcv_batch()。

    Raises:
        ValueError: 参数不合法（interval 不支持、symbol 格式不对、时间范围不合法、
            max_requests 越界等）。
        ManualOhlcvValidationError: 拉取到的数据未通过既有校验。
        BinancePublicRestError（及其子类）: 网络/HTTP/响应格式/数据连续性问题。
    """
    if interval not in _SUPPORTED_INTERVALS_MS:
        raise ValueError(
            f"不支持的 interval: {interval!r}（支持: {sorted(_SUPPORTED_INTERVALS_MS)}；"
            "月线 '1M' 因为跟 OhlcvTimeframe 的校验规则冲突，这个模块不支持）"
        )
    if start_time_utc.tzinfo is None or end_time_utc.tzinfo is None:
        raise ValueError("start_time_utc/end_time_utc 必须带时区（UTC）")
    if start_time_utc >= end_time_utc:
        raise ValueError("start_time_utc 必须早于 end_time_utc")
    if max_requests <= 0:
        raise ValueError(f"max_requests 必须为正数，当前值: {max_requests}")
    if max_requests > _ABSOLUTE_MAX_REQUESTS_CEILING:
        raise ValueError(
            f"max_requests={max_requests} 超过硬上限 {_ABSOLUTE_MAX_REQUESTS_CEILING}"
        )

    canonical_symbol = CanonicalMarketSymbol(symbol)
    if not _SYMBOL_PATTERN.fullmatch(canonical_symbol.value):
        raise ValueError(
            f"symbol {canonical_symbol.value!r} 不是合法的 Binance 现货符号格式"
            "（只允许大写字母/数字，例如 'BTCUSDT'）"
        )
    timeframe = OhlcvTimeframe(interval)
    interval_ms = _SUPPORTED_INTERVALS_MS[interval]

    retrieval_cutoff_ms = int(time.time() * 1000)  # 只采样一次，保证一次调用内行为确定
    start_ms = _to_ms(start_time_utc)
    end_ms = _to_ms(end_time_utc)

    opener = _build_opener()
    records: list[NormalizedOhlcvRecord] = []
    dropped_unclosed_bar_count = 0
    last_accumulated_open_time_ms: int | None = None
    last_weight_headers: dict[str, str] = {}
    cursor_ms = start_ms
    requests_made = 0

    while cursor_ms < end_ms:
        if requests_made >= max_requests:
            raise BinancePublicRestError(
                f"超过 max_requests={max_requests} 次分页请求仍未拉完整个区间——区间可能过大，"
                "考虑缩小 [start_time_utc, end_time_utc) 或调大 max_requests（不超过硬上限）。"
            )

        raw_rows, weight_headers = _fetch_klines_page(
            opener, canonical_symbol.value, interval, cursor_ms, end_ms - 1, _MAX_LIMIT_PER_REQUEST
        )
        requests_made += 1
        if weight_headers:
            last_weight_headers = weight_headers

        if not raw_rows:
            # 游标已经追上"现在"时收到空页是合法的（还没有更多数据可拉，不是缺口）；游标还在
            # 真实历史区间内时收到空页则是可疑的数据缺口，不能静默当成"已经拉完"。
            if cursor_ms < retrieval_cutoff_ms:
                raise BinanceKlineGapError(
                    f"游标 {cursor_ms} 仍早于当前时间时收到空页，疑似数据缺口"
                )
            break

        page_open_times: list[int] = []
        for row in raw_rows:
            open_time_ms, _close_time_ms = _validate_kline_row_shape(row, interval_ms)
            page_open_times.append(open_time_ms)

        # 页内连续性：相邻 open_time 必须恰好差一个 interval_ms。
        for prev_ot, next_ot in itertools.pairwise(page_open_times):
            if next_ot - prev_ot != interval_ms:
                raise BinanceKlineGapError(
                    f"页内 K 线不连续: open_time {prev_ot} 之后是 {next_ot}（期望间隔 {interval_ms}）"
                )

        # 跨页连续性：新页第一根必须紧接上一页最后一根。
        if last_accumulated_open_time_ms is not None:
            expected_first = last_accumulated_open_time_ms + interval_ms
            if page_open_times[0] != expected_first:
                raise BinanceKlineGapError(
                    f"分页之间不连续: 上一页最后一根 open_time={last_accumulated_open_time_ms}，"
                    f"新页第一根 open_time={page_open_times[0]}（期望 {expected_first}）"
                )

        for row, open_time_ms in zip(raw_rows, page_open_times):
            if open_time_ms >= end_ms:
                continue  # 防御性：请求已经用 endTime=end_ms-1，正常不该发生
            close_time_ms = open_time_ms + interval_ms - 1
            if close_time_ms >= retrieval_cutoff_ms:
                dropped_unclosed_bar_count += 1
                continue
            record = _kline_row_to_record(row, open_time_ms, market, canonical_symbol, timeframe)
            records.append(record)
            last_accumulated_open_time_ms = open_time_ms

        last_open_time_ms = page_open_times[-1]
        next_cursor_ms = last_open_time_ms + interval_ms
        if next_cursor_ms <= cursor_ms:
            raise BinanceKlineGapError("klines 分页游标未前进，疑似响应异常，停止拉取")
        cursor_ms = next_cursor_ms

        if len(raw_rows) < _MAX_LIMIT_PER_REQUEST:
            break  # 这一页不满，说明已经到了 Binance 当前能提供的最新数据

    coverage_end_utc = (
        _from_ms(last_accumulated_open_time_ms + interval_ms)
        if last_accumulated_open_time_ms is not None
        else start_time_utc
    )

    if coverage_end_utc < end_time_utc and strict_coverage:
        raise BinanceIncompleteCoverageError(
            f"实际覆盖到 {coverage_end_utc.isoformat()}，早于请求的 {end_time_utc.isoformat()}"
            f"（{dropped_unclosed_bar_count} 根未收盘 K 线被丢弃）"
        )

    if records:
        record_issues = tuple(issue for r in records for issue in validate_ohlcv_record(r))
        batch_issues = validate_ohlcv_batch(tuple(records))
        all_issues = record_issues + batch_issues
        if all_issues:
            raise ManualOhlcvValidationError(all_issues)

    meta = FetchMeta(
        request_symbol=canonical_symbol.value,
        request_interval=interval,
        request_start_utc=start_time_utc,
        request_end_utc=end_time_utc,
        coverage_end_utc=coverage_end_utc,
        retrieval_cutoff_utc=_from_ms(retrieval_cutoff_ms),
        dropped_unclosed_bar_count=dropped_unclosed_bar_count,
        host=urllib.parse.urlparse(_KLINES_URL).netloc,
        page_count=requests_made,
        csv_sha256="",  # 调用方（CLI）写完 CSV 之后用 dataclasses.replace() 回填
        schema_version=1,
        response_weight_headers=last_weight_headers,
    )
    return records, meta
