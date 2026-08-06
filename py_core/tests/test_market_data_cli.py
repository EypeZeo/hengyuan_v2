"""Unit tests for py_core.market_data.cli (P2-MD-02).

fetch_binance_ohlcv() itself is mocked throughout -- these tests exercise CLI-layer concerns
(date parsing, atomic publish, coverage-shortfall warnings, manifest/fetch_meta/CSV
cross-consistency), not the network fetch logic (covered separately in
test_binance_public_rest.py).
"""

from __future__ import annotations

import hashlib
import json
from datetime import UTC, datetime, timedelta
from decimal import Decimal
from pathlib import Path
from typing import Any
from unittest import mock

import pytest
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)

from py_core.market_data.binance_public_rest import FetchMeta
from py_core.market_data.cli import (
    _parse_utc_date,
    _write_ohlcv_csv,
    build_parser,
    cmd_fetch,
    publish_fetch_output,
)

_BASE_DT = datetime(2024, 1, 1, tzinfo=UTC)


def _rec(day: int, close_p: float = 100.0) -> NormalizedOhlcvRecord:
    return NormalizedOhlcvRecord(
        market=ManualMarket.CRYPTO_SPOT,
        symbol=CanonicalMarketSymbol("BTCUSDT"),
        timeframe=OhlcvTimeframe("1d"),
        event_time_utc=_BASE_DT + timedelta(days=day),
        open_price=Decimal(str(close_p)),
        high_price=Decimal(str(close_p)),
        low_price=Decimal(str(close_p)),
        close_price=Decimal(str(close_p)),
        volume=Decimal("10"),
    )


def _meta(**overrides: Any) -> FetchMeta:
    defaults: dict[str, Any] = dict(
        request_symbol="BTCUSDT",
        request_interval="1d",
        request_start_utc=_BASE_DT,
        request_end_utc=_BASE_DT + timedelta(days=5),
        coverage_end_utc=_BASE_DT + timedelta(days=5),
        retrieval_cutoff_utc=_BASE_DT + timedelta(days=10),
        dropped_unclosed_bar_count=0,
        host="data-api.binance.vision",
        page_count=1,
        csv_sha256="",
        schema_version=1,
        response_weight_headers={},
    )
    defaults.update(overrides)
    return FetchMeta(**defaults)


# ---------------------------------------------------------------------------
# _parse_utc_date()
# ---------------------------------------------------------------------------


def test_parse_utc_date_valid() -> None:
    dt = _parse_utc_date("2024-01-15")
    assert dt == datetime(2024, 1, 15, tzinfo=UTC)


def test_parse_utc_date_rejects_non_iso_format() -> None:
    with pytest.raises(ValueError):
        _parse_utc_date("01/15/2024")
    with pytest.raises(ValueError):
        _parse_utc_date("2024-01-15T00:00:00")
    with pytest.raises(ValueError):
        _parse_utc_date("not a date")


# ---------------------------------------------------------------------------
# _write_ohlcv_csv() format matches backtests/cli.py::load_ohlcv_csv() reader
# ---------------------------------------------------------------------------


def test_write_ohlcv_csv_matches_backtest_cli_reader(tmp_path: Path) -> None:
    from py_core.backtests.cli import load_ohlcv_csv

    records = [_rec(i, 100.0 + i) for i in range(3)]
    csv_path = tmp_path / "ohlcv.csv"
    _write_ohlcv_csv(csv_path, records)

    loaded = load_ohlcv_csv(csv_path, market=ManualMarket.CRYPTO_SPOT, symbol="BTCUSDT", timeframe="1d")
    assert len(loaded) == 3
    assert loaded[0].close_price == Decimal("100")
    assert loaded[0].event_time_utc == records[0].event_time_utc


# ---------------------------------------------------------------------------
# publish_fetch_output(): atomic publish
# ---------------------------------------------------------------------------


def test_publish_fetch_output_writes_three_files(tmp_path: Path) -> None:
    records = [_rec(i) for i in range(3)]
    meta = _meta()
    output_dir = tmp_path / "out"
    publish_fetch_output(records, meta, output_dir, symbol="BTCUSDT", interval="1d")

    assert (output_dir / "ohlcv.csv").exists()
    assert (output_dir / "manifest.json").exists()
    assert (output_dir / "fetch_meta.json").exists()


def test_publish_fetch_output_refuses_to_overwrite_existing_nonempty_dir(tmp_path: Path) -> None:
    records = [_rec(0)]
    meta = _meta()
    output_dir = tmp_path / "out"
    publish_fetch_output(records, meta, output_dir, symbol="BTCUSDT", interval="1d")
    with pytest.raises(FileExistsError):
        publish_fetch_output(records, meta, output_dir, symbol="BTCUSDT", interval="1d")


def test_publish_fetch_output_refuses_to_overwrite_existing_empty_dir(tmp_path: Path) -> None:
    records = [_rec(0)]
    meta = _meta()
    output_dir = tmp_path / "out"
    output_dir.mkdir(parents=True)
    assert list(output_dir.iterdir()) == []
    with pytest.raises(FileExistsError):
        publish_fetch_output(records, meta, output_dir, symbol="BTCUSDT", interval="1d")


def test_publish_fetch_output_leaves_no_partial_output_on_failure(tmp_path: Path) -> None:
    records = [_rec(0)]
    meta = _meta()
    output_dir = tmp_path / "out"

    real_dumps = json.dumps
    call_count = {"n": 0}

    def flaky_dumps(*args: Any, **kwargs: Any) -> str:
        call_count["n"] += 1
        if call_count["n"] == 1:
            raise RuntimeError("boom")
        return real_dumps(*args, **kwargs)

    with mock.patch("py_core.market_data.cli.json.dumps", side_effect=flaky_dumps):
        with pytest.raises(RuntimeError):
            publish_fetch_output(records, meta, output_dir, symbol="BTCUSDT", interval="1d")

    assert not output_dir.exists()
    assert list(tmp_path.glob(".tmp-*")) == []


def test_publish_fetch_output_manifest_fetch_meta_csv_cross_consistency(tmp_path: Path) -> None:
    records = [_rec(i) for i in range(4)]
    meta = _meta()
    output_dir = tmp_path / "out"
    publish_fetch_output(records, meta, output_dir, symbol="BTCUSDT", interval="1d")

    csv_bytes = (output_dir / "ohlcv.csv").read_bytes()
    actual_sha256 = hashlib.sha256(csv_bytes).hexdigest()

    manifest = json.loads((output_dir / "manifest.json").read_text(encoding="utf-8"))
    fetch_meta_dict = json.loads((output_dir / "fetch_meta.json").read_text(encoding="utf-8"))

    assert manifest["file_sha256"] == actual_sha256
    assert fetch_meta_dict["csv_sha256"] == actual_sha256
    assert manifest["record_count"] == 4
    assert manifest["symbol"] == "BTCUSDT"
    assert manifest["timeframe"] == "1d"
    assert manifest["point_in_time_safe"] is False
    assert manifest["input_format"] == "binance_public_rest"
    assert manifest["first_event_time_utc"] is not None
    assert manifest["last_event_time_utc"] is not None


def test_publish_fetch_output_empty_records_manifest_null_timestamps(tmp_path: Path) -> None:
    meta = _meta(coverage_end_utc=_BASE_DT, dropped_unclosed_bar_count=1)
    output_dir = tmp_path / "out"
    publish_fetch_output([], meta, output_dir, symbol="BTCUSDT", interval="1d")

    manifest = json.loads((output_dir / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["record_count"] == 0
    assert manifest["first_event_time_utc"] is None
    assert manifest["last_event_time_utc"] is None


# ---------------------------------------------------------------------------
# build_parser() / argument parsing
# ---------------------------------------------------------------------------


def test_build_parser_fetch_requires_all_mandatory_args() -> None:
    parser = build_parser()
    with pytest.raises(SystemExit):
        parser.parse_args(["fetch", "--symbol", "BTCUSDT"])


def test_build_parser_fetch_parses_defaults() -> None:
    parser = build_parser()
    args = parser.parse_args(
        [
            "fetch",
            "--symbol",
            "BTCUSDT",
            "--interval",
            "1d",
            "--start",
            "2024-01-01",
            "--end",
            "2024-02-01",
            "--output-dir",
            "out",
        ]
    )
    assert args.symbol == "BTCUSDT"
    assert args.max_requests == 50
    assert args.strict_coverage is False
    assert args.market == "crypto_spot"


def test_build_parser_fetch_strict_coverage_flag() -> None:
    parser = build_parser()
    args = parser.parse_args(
        [
            "fetch",
            "--symbol",
            "BTCUSDT",
            "--interval",
            "1d",
            "--start",
            "2024-01-01",
            "--end",
            "2024-02-01",
            "--output-dir",
            "out",
            "--strict-coverage",
        ]
    )
    assert args.strict_coverage is True


# ---------------------------------------------------------------------------
# cmd_fetch(): end-to-end CLI flow with fetch_binance_ohlcv() mocked
# ---------------------------------------------------------------------------


def _args(tmp_path: Path, **overrides: Any) -> Any:
    parser = build_parser()
    argv = [
        "fetch",
        "--symbol",
        "BTCUSDT",
        "--interval",
        "1d",
        "--start",
        "2024-01-01",
        "--end",
        "2024-01-06",
        "--output-dir",
        str(tmp_path / "out"),
    ]
    for k, v in overrides.items():
        argv.extend([f"--{k.replace('_', '-')}", str(v)])
    return parser.parse_args(argv)


def test_cmd_fetch_success_writes_output(tmp_path: Path) -> None:
    records = [_rec(i) for i in range(5)]
    meta = _meta()
    args = _args(tmp_path)
    with mock.patch(
        "py_core.market_data.cli.fetch_binance_ohlcv", return_value=(records, meta)
    ) as fake_fetch:
        rc = cmd_fetch(args)
    assert rc == 0
    assert fake_fetch.call_count == 1
    assert (tmp_path / "out" / "ohlcv.csv").exists()


def test_cmd_fetch_rejects_bad_date(tmp_path: Path, capsys: Any) -> None:
    args = _args(tmp_path)
    args.start = "not-a-date"
    rc = cmd_fetch(args)
    assert rc == 1
    assert "ERROR" in capsys.readouterr().err


def test_cmd_fetch_prints_warning_when_coverage_falls_short(tmp_path: Path, capsys: Any) -> None:
    records = [_rec(i) for i in range(3)]
    meta = _meta(
        request_end_utc=_BASE_DT + timedelta(days=6),
        coverage_end_utc=_BASE_DT + timedelta(days=3),
        dropped_unclosed_bar_count=2,
    )
    args = _args(tmp_path)
    with mock.patch("py_core.market_data.cli.fetch_binance_ohlcv", return_value=(records, meta)):
        rc = cmd_fetch(args)
    assert rc == 0
    err = capsys.readouterr().err
    assert "WARNING" in err


def test_cmd_fetch_output_dir_already_exists_returns_error(tmp_path: Path, capsys: Any) -> None:
    records = [_rec(0)]
    meta = _meta()
    (tmp_path / "out").mkdir(parents=True)
    args = _args(tmp_path)
    with mock.patch("py_core.market_data.cli.fetch_binance_ohlcv", return_value=(records, meta)):
        rc = cmd_fetch(args)
    assert rc == 1
    assert "ERROR" in capsys.readouterr().err
