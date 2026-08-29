"""Unit tests for py_core.market_data.binance_public_rest (P2-MD-02).

All tests patch py_core.market_data.binance_public_rest._build_opener() to return a fake
opener fed pre-programmed responses/exceptions -- NO real network requests are ever made.
This is a hard rule for this file, not a suggestion: a test here that actually reaches the
network is a bug in the test, not a slow-but-valid integration test.

All outputs are backtesting/research estimates only.
NOT financial advice. NOT trading authorization. NOT a live data feed guarantee.
"""

from __future__ import annotations

import io
import itertools
import urllib.error
from datetime import UTC, datetime, timedelta
from email.message import Message
from typing import Any, Self
from unittest import mock

import pytest

from py_core.manual_ohlcv import ManualOhlcvValidationError
from py_core.market_data.binance_public_rest import (
    BinanceHttpError,
    BinanceIncompleteCoverageError,
    BinanceKlineGapError,
    BinancePublicRestError,
    BinanceRateLimitedError,
    BinanceResponseError,
    BinanceTransportError,
    _NoRedirectHandler,
    fetch_binance_ohlcv,
)

_INTERVAL = "1d"
_INTERVAL_MS = 86_400_000
# Deliberately far in the past so every row in these tests is unambiguously "closed"
# relative to whenever the test suite actually runs (retrieval_cutoff_ms = real time.time()).
_BASE_MS = int(datetime(2020, 1, 1, tzinfo=UTC).timestamp() * 1000)


# ---------------------------------------------------------------------------
# Fakes
# ---------------------------------------------------------------------------


class _FakeResponse:
    def __init__(self, body: bytes, headers: dict[str, str] | None = None) -> None:
        self._body = body
        self.headers = headers or {}

    def read(self, n: int = -1) -> bytes:
        return self._body if n is None or n < 0 else self._body[:n]

    def __enter__(self) -> Self:
        return self

    def __exit__(self, *exc: object) -> bool:
        return False


class _FakeOpener:
    """Feeds a pre-programmed sequence of responses/exceptions to successive .open() calls."""

    def __init__(self, responses: list[Any]) -> None:
        self._responses = list(responses)
        self.calls: list[str] = []

    def open(self, request: Any, timeout: float | None = None) -> _FakeResponse:
        self.calls.append(request.full_url)
        if not self._responses:
            raise AssertionError("_FakeOpener ran out of programmed responses")
        item = self._responses.pop(0)
        if isinstance(item, Exception):
            raise item
        return item


def _http_error(code: int, *, msg: str = "err", retry_after: str | None = None, body: bytes = b"") -> urllib.error.HTTPError:
    headers = Message()
    if retry_after is not None:
        headers["Retry-After"] = retry_after
    return urllib.error.HTTPError("https://data-api.binance.vision/api/v3/klines", code, msg, headers, io.BytesIO(body))


def _row(
    open_time_ms: int,
    o: str = "100",
    h: str = "101",
    l: str = "99",
    c: str = "100.5",
    v: str = "10",
    *,
    interval_ms: int = _INTERVAL_MS,
) -> list[Any]:
    close_time_ms = open_time_ms + interval_ms - 1
    return [open_time_ms, o, h, l, c, v, close_time_ms, "0", 1, "0", "0", "0"]


def _consecutive_rows(start_open_time_ms: int, count: int, *, interval_ms: int = _INTERVAL_MS) -> list[list[Any]]:
    return [_row(start_open_time_ms + i * interval_ms, interval_ms=interval_ms) for i in range(count)]


def _response_json(rows: list[Any], headers: dict[str, str] | None = None) -> _FakeResponse:
    import json

    return _FakeResponse(json.dumps(rows).encode("utf-8"), headers)


def _patched_opener(responses: list[Any]):
    opener = _FakeOpener(responses)
    return mock.patch("py_core.market_data.binance_public_rest._build_opener", return_value=opener), opener


def _dt(day_offset: int) -> datetime:
    return datetime(2020, 1, 1, tzinfo=UTC) + timedelta(days=day_offset)


# ---------------------------------------------------------------------------
# Happy paths
# ---------------------------------------------------------------------------


def test_single_page_fetch_returns_normalized_records() -> None:
    rows = _consecutive_rows(_BASE_MS, 5)
    patcher, _opener = _patched_opener([_response_json(rows)])
    with patcher:
        records, meta = fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))

    assert len(records) == 5
    assert meta.page_count == 1
    assert meta.request_symbol == "BTCUSDT"
    assert meta.dropped_unclosed_bar_count == 0
    assert records[0].event_time_utc == _dt(0)


def test_pagination_advances_cursor_and_stitches_pages() -> None:
    page1 = _consecutive_rows(_BASE_MS, 1000)  # full page -> triggers a second request
    page2_start = _BASE_MS + 1000 * _INTERVAL_MS
    page2 = _consecutive_rows(page2_start, 10)
    patcher, opener = _patched_opener([_response_json(page1), _response_json(page2)])
    with patcher:
        records, meta = fetch_binance_ohlcv(
            "BTCUSDT", _INTERVAL, _dt(0), _dt(0) + timedelta(days=1010)
        )

    assert len(records) == 1010
    assert meta.page_count == 2
    assert len(opener.calls) == 2
    # strictly ascending, no duplicates, no gaps
    for prev, nxt in itertools.pairwise(records):
        assert nxt.event_time_utc - prev.event_time_utc == timedelta(days=1)


# ---------------------------------------------------------------------------
# Input validation (before any network call)
# ---------------------------------------------------------------------------


def test_rejects_unsupported_interval_1M() -> None:
    with pytest.raises(ValueError):
        fetch_binance_ohlcv("BTCUSDT", "1M", _dt(0), _dt(5))


def test_rejects_start_after_end() -> None:
    with pytest.raises(ValueError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(5), _dt(0))


def test_rejects_non_positive_max_requests() -> None:
    with pytest.raises(ValueError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5), max_requests=0)


def test_rejects_max_requests_above_ceiling() -> None:
    with pytest.raises(ValueError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5), max_requests=101)


def test_symbol_with_slash_rejected_even_though_canonical_symbol_allows_it() -> None:
    with pytest.raises(ValueError):
        fetch_binance_ohlcv("BTC/USDT", _INTERVAL, _dt(0), _dt(5))


# ---------------------------------------------------------------------------
# Pagination / cursor safety
# ---------------------------------------------------------------------------


def test_stale_cursor_raises_instead_of_looping() -> None:
    # A single row whose open_time is one interval BEFORE the requested start -- the
    # resulting next_cursor_ms would equal the current cursor, not advance past it.
    rows = [_row(_BASE_MS - _INTERVAL_MS)]
    patcher, _ = _patched_opener([_response_json(rows)])
    with patcher, pytest.raises(BinanceKlineGapError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))


def test_exceeds_max_requests_raises() -> None:
    full_page = _consecutive_rows(_BASE_MS, 1000)
    # Every page is full, so the loop keeps requesting more pages than max_requests allows.
    patcher, _ = _patched_opener([_response_json(full_page)] * 3)
    with patcher, pytest.raises(BinancePublicRestError):
        fetch_binance_ohlcv(
            "BTCUSDT", _INTERVAL, _dt(0), _dt(0) + timedelta(days=5000), max_requests=2
        )


def test_unexpected_empty_page_before_end_raises_gap_error() -> None:
    patcher, _ = _patched_opener([_response_json([])])
    with patcher, pytest.raises(BinanceKlineGapError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))


def test_gap_between_pages_raises_gap_error() -> None:
    page1 = _consecutive_rows(_BASE_MS, 1000)
    # page2 skips one bar instead of continuing immediately after page1's last row.
    page2_start = _BASE_MS + 1001 * _INTERVAL_MS
    page2 = _consecutive_rows(page2_start, 10)
    patcher, _ = _patched_opener([_response_json(page1), _response_json(page2)])
    with patcher, pytest.raises(BinanceKlineGapError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(0) + timedelta(days=1010))


def test_gap_within_page_raises_gap_error() -> None:
    rows = [_row(_BASE_MS), _row(_BASE_MS + 2 * _INTERVAL_MS)]  # skips one interval
    patcher, _ = _patched_opener([_response_json(rows)])
    with patcher, pytest.raises(BinanceKlineGapError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))


def test_duplicate_or_out_of_order_rows_raise() -> None:
    rows = [_row(_BASE_MS), _row(_BASE_MS)]  # duplicate open_time
    patcher, _ = _patched_opener([_response_json(rows)])
    with patcher, pytest.raises(BinanceKlineGapError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))


def test_end_exactly_on_kline_open_boundary() -> None:
    # end = day 3; the response includes a (spec-violating, but we shouldn't trust the
    # server blindly) row whose open_time is exactly at day 3 -- must be excluded from
    # the [start, end) half-open range.
    rows = _consecutive_rows(_BASE_MS, 3) + [_row(_BASE_MS + 3 * _INTERVAL_MS)]
    patcher, _ = _patched_opener([_response_json(rows)])
    with patcher:
        records, _meta = fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(3))
    assert len(records) == 3
    assert records[-1].event_time_utc == _dt(2)


# ---------------------------------------------------------------------------
# Row-shape / schema validation
# ---------------------------------------------------------------------------


def test_malformed_response_top_level_not_list_raises() -> None:
    import json

    patcher, _ = _patched_opener([_FakeResponse(json.dumps({"not": "a list"}).encode())])
    with patcher, pytest.raises(BinanceResponseError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))


def test_row_with_wrong_field_count_raises() -> None:
    bad_row = _row(_BASE_MS)[:6]  # only 6 fields instead of 12
    patcher, _ = _patched_opener([_response_json([bad_row])])
    with patcher, pytest.raises(BinanceResponseError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))


def test_row_not_a_list_raises() -> None:
    patcher, _ = _patched_opener([_response_json([{"not": "a row"}])])
    with patcher, pytest.raises(BinanceResponseError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))


def test_bool_timestamp_rejected() -> None:
    bad_row = _row(_BASE_MS)
    bad_row[0] = True  # open_time replaced with a bool
    patcher, _ = _patched_opener([_response_json([bad_row])])
    with patcher, pytest.raises(BinanceResponseError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))


def test_close_time_inconsistent_with_interval_raises() -> None:
    bad_row = _row(_BASE_MS)
    bad_row[6] = bad_row[6] + 1  # close_time off by one from the Binance-defined formula
    patcher, _ = _patched_opener([_response_json([bad_row])])
    with patcher, pytest.raises(BinanceKlineGapError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))


def test_response_too_large_raises() -> None:
    from py_core.market_data.binance_public_rest import _MAX_RESPONSE_BYTES

    oversized_body = b"[" + b"1" * (_MAX_RESPONSE_BYTES + 100) + b"]"
    patcher, _ = _patched_opener([_FakeResponse(oversized_body)])
    with patcher, pytest.raises(BinanceResponseError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))


def test_non_finite_decimal_field_raises_via_shared_validation() -> None:
    # "NaN" parses fine as a Decimal literal (no exception at construction), so this only
    # gets caught downstream by validate_ohlcv_record()'s is_finite() check -- verifying
    # the fetcher genuinely routes through that shared, hardened validation.
    bad_row = _row(_BASE_MS, c="NaN")
    patcher, _ = _patched_opener([_response_json([bad_row])])
    with patcher, pytest.raises(ManualOhlcvValidationError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))


def test_invalid_decimal_field_raises_binance_response_error() -> None:
    # A genuinely unparseable string fails at Decimal() construction time.
    bad_row = _row(_BASE_MS, c="not_a_number")
    patcher, _ = _patched_opener([_response_json([bad_row])])
    with patcher, pytest.raises(BinanceResponseError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))


def test_fetched_records_pass_existing_validation() -> None:
    rows = _consecutive_rows(_BASE_MS, 5)
    patcher, _ = _patched_opener([_response_json(rows)])
    with patcher:
        records, _meta = fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))

    from py_core.manual_ohlcv import validate_ohlcv_batch, validate_ohlcv_record

    for r in records:
        assert validate_ohlcv_record(r) == ()
    assert validate_ohlcv_batch(tuple(records)) == ()


# ---------------------------------------------------------------------------
# Unclosed-bar filtering / coverage
# ---------------------------------------------------------------------------


def test_unclosed_trailing_bar_is_dropped_and_coverage_end_reflects_it() -> None:
    now = datetime.now(UTC)
    # One bar starting "now" -- its close_time is necessarily in the future, so it must
    # be dropped as not-yet-closed.
    still_forming_open_ms = int(now.timestamp() * 1000)
    rows = [_row(still_forming_open_ms)]
    patcher, _ = _patched_opener([_response_json(rows)])
    with patcher:
        records, meta = fetch_binance_ohlcv(
            "BTCUSDT",
            _INTERVAL,
            now - timedelta(days=1),
            now + timedelta(days=1),
        )

    assert records == []
    assert meta.dropped_unclosed_bar_count == 1
    assert meta.coverage_end_utc == now - timedelta(days=1)


def test_strict_coverage_raises_incomplete_coverage_error() -> None:
    now = datetime.now(UTC)
    still_forming_open_ms = int(now.timestamp() * 1000)
    rows = [_row(still_forming_open_ms)]
    patcher, _ = _patched_opener([_response_json(rows)])
    with patcher, pytest.raises(BinanceIncompleteCoverageError):
        fetch_binance_ohlcv(
            "BTCUSDT",
            _INTERVAL,
            now - timedelta(days=1),
            now + timedelta(days=1),
            strict_coverage=True,
        )


# ---------------------------------------------------------------------------
# Network/HTTP error handling
# ---------------------------------------------------------------------------


def test_http_429_raises_without_retry() -> None:
    patcher, opener = _patched_opener([_http_error(429, retry_after="5")])
    with patcher, pytest.raises(BinanceRateLimitedError) as exc_info:
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))
    assert len(opener.calls) == 1  # no automatic retry
    assert exc_info.value.status_code == 429
    assert exc_info.value.retry_after == "5"
    assert "5" in str(exc_info.value)


def test_http_418_raises_without_retry() -> None:
    patcher, opener = _patched_opener([_http_error(418)])
    with patcher, pytest.raises(BinanceRateLimitedError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))
    assert len(opener.calls) == 1


def test_generic_http_error_raises_binance_http_error() -> None:
    patcher, _ = _patched_opener([_http_error(500)])
    with patcher, pytest.raises(BinanceHttpError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))


def test_transport_error_wrapped_as_binance_transport_error() -> None:
    patcher, _ = _patched_opener([urllib.error.URLError("name resolution failed")])
    with patcher, pytest.raises(BinanceTransportError):
        fetch_binance_ohlcv("BTCUSDT", _INTERVAL, _dt(0), _dt(5))


def test_redirect_is_rejected() -> None:
    handler = _NoRedirectHandler()
    with pytest.raises(urllib.error.HTTPError):
        handler.redirect_request(
            mock.Mock(full_url="https://data-api.binance.vision/api/v3/klines"),
            None,
            302,
            "Found",
            Message(),
            "https://evil.example/steal",
        )
