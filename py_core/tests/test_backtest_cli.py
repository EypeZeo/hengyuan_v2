"""Unit tests for py_core.backtests.cli (P2-BT-01 / P2-STRAT-01).

All outputs are backtesting estimates only.
NOT financial advice. NOT trading authorization.
"""

from __future__ import annotations

import json
from datetime import UTC, datetime, timedelta
from decimal import Decimal
from typing import Any
from unittest import mock

import pandas as pd
import pytest

from py_core.backtests.cli import (
    _parse_strategy_params,
    ohlcv_fingerprint,
    save_results,
    strategy_source_fingerprint,
    validate_run_id,
)
from py_core.backtests.models import BacktestConfig, BacktestResult
from py_core.backtests.vectorized_engine import run_vectorized_backtest
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)

_BASE_DT = datetime(2024, 1, 1, tzinfo=UTC)


def _rec(day: int, close_p: float, volume: float = 1000.0) -> NormalizedOhlcvRecord:
    return NormalizedOhlcvRecord(
        market=ManualMarket.CRYPTO_SPOT,
        symbol=CanonicalMarketSymbol("BTC/USDT"),
        timeframe=OhlcvTimeframe("1d"),
        event_time_utc=_BASE_DT + timedelta(days=day),
        open_price=Decimal(str(close_p)),
        high_price=Decimal(str(close_p)),
        low_price=Decimal(str(close_p)),
        close_price=Decimal(str(close_p)),
        volume=Decimal(str(volume)),
    )


def _make_result() -> BacktestResult:
    records = [_rec(i, 100.0 + i) for i in range(5)]
    index = pd.DatetimeIndex([r.event_time_utc for r in records])
    signal: pd.Series[Any] = pd.Series([1.0] * 5, index=index)
    cfg = BacktestConfig(initial_capital=100_000.0, fee_bps=0.0, slippage_bps=0.0)
    return run_vectorized_backtest(cfg, records, signal)


# ---------------------------------------------------------------------------
# _parse_strategy_params()
# ---------------------------------------------------------------------------


def test_parse_strategy_params_valid_json_object() -> None:
    assert _parse_strategy_params('{"fast_window": 5, "slow_window": 20}') == {
        "fast_window": 5,
        "slow_window": 20,
    }


def test_parse_strategy_params_none_or_empty_returns_empty_dict() -> None:
    assert _parse_strategy_params(None) == {}
    assert _parse_strategy_params("") == {}


def test_parse_strategy_params_rejects_invalid_json() -> None:
    with pytest.raises(ValueError):
        _parse_strategy_params("{not valid json")


def test_parse_strategy_params_rejects_non_object_json() -> None:
    # Valid JSON but wrong top-level type -> TypeError (ruff TRY004: a type check
    # failure, not a value check failure -- distinct from the invalid-JSON case above).
    with pytest.raises(TypeError):
        _parse_strategy_params("[1, 2, 3]")
    with pytest.raises(TypeError):
        _parse_strategy_params("5")
    with pytest.raises(TypeError):
        _parse_strategy_params('"a string"')


# ---------------------------------------------------------------------------
# validate_run_id()
# ---------------------------------------------------------------------------


def test_validate_run_id_accepts_normal_id() -> None:
    validate_run_id("abc123")  # 不抛错
    validate_run_id("2026-01-01_run")


def test_validate_run_id_rejects_empty() -> None:
    with pytest.raises(ValueError):
        validate_run_id("")


def test_validate_run_id_rejects_path_separators_and_dotdot() -> None:
    with pytest.raises(ValueError):
        validate_run_id("../escape")
    with pytest.raises(ValueError):
        validate_run_id("a/b")
    with pytest.raises(ValueError):
        validate_run_id("a\\b")
    with pytest.raises(ValueError):
        validate_run_id("..")


# ---------------------------------------------------------------------------
# ohlcv_fingerprint() / strategy_source_fingerprint()
# ---------------------------------------------------------------------------


def test_ohlcv_fingerprint_uses_file_sha256(tmp_path: Any) -> None:
    csv_path = tmp_path / "ohlcv.csv"
    csv_path.write_text("timestamp_utc,open,high,low,close,volume\n", encoding="utf-8")
    records = [_rec(0, 100.0)]
    info = ohlcv_fingerprint(csv_path, records)
    assert info["kind"] == "file_sha256"
    assert "sha256" in info


def test_strategy_source_fingerprint_none_for_none_spec() -> None:
    assert strategy_source_fingerprint(None) is None


def test_strategy_source_fingerprint_locates_real_module() -> None:
    info = strategy_source_fingerprint("py_core.strategies.sma_crossover:SmaCrossoverStrategy")
    assert info is not None
    assert info["source_available"] is True
    assert "sha256" in info


def test_strategy_source_fingerprint_unavailable_for_unknown_module() -> None:
    info = strategy_source_fingerprint("this.module.does.not.exist:Foo")
    assert info is not None
    assert info["source_available"] is False


# ---------------------------------------------------------------------------
# save_results(): 原子发布
# ---------------------------------------------------------------------------


def test_save_results_writes_run_manifest_with_strategy_provenance(tmp_path: Any) -> None:
    result = _make_result()
    output_dir = tmp_path / "run1"
    save_results(
        result,
        output_dir,
        strategy_spec="py_core.strategies.sma_crossover:SmaCrossoverStrategy",
        strategy_params={"fast_window": 5, "slow_window": 20},
        signals_path=None,
        ohlcv_fingerprint_info={"kind": "record_summary", "record_count": 5},
        strategy_fingerprint_info={"spec": "x", "source_available": True, "sha256": "abc"},
    )
    manifest = json.loads((output_dir / "run_manifest.json").read_text(encoding="utf-8"))
    assert manifest["strategy_spec"] == "py_core.strategies.sma_crossover:SmaCrossoverStrategy"
    assert manifest["strategy_params"] == {"fast_window": 5, "slow_window": 20}
    assert manifest["ohlcv_fingerprint"] == {"kind": "record_summary", "record_count": 5}
    assert "python_version" in manifest
    assert "pandas_version" in manifest
    # 既有 artifact 文件仍然存在，没有因为新增 manifest 而丢失
    assert (output_dir / "metrics.json").exists()
    assert (output_dir / "equity_curve.jsonl").exists()


def test_save_results_refuses_to_overwrite_existing_nonempty_run_dir(tmp_path: Any) -> None:
    result = _make_result()
    output_dir = tmp_path / "run1"
    save_results(result, output_dir)
    with pytest.raises(FileExistsError):
        save_results(result, output_dir)


def test_save_results_refuses_to_overwrite_existing_empty_run_dir(tmp_path: Any) -> None:
    # 回归用例：之前"已存在且非空才拒绝"的写法在 POSIX 上对着一个已存在的空目录会被
    # rename 静默替换掉——跟 Windows 上 rename 对已存在空目录直接报错不一致，是真实的
    # 平台相关 TOCTOU，不只是理论风险。现在不管目标目录是否为空，已存在就必须拒绝。
    result = _make_result()
    output_dir = tmp_path / "run1"
    output_dir.mkdir(parents=True)
    assert list(output_dir.iterdir()) == []
    with pytest.raises(FileExistsError):
        save_results(result, output_dir)


def test_save_results_leaves_no_partial_output_on_failure(tmp_path: Any) -> None:
    result = _make_result()
    output_dir = tmp_path / "run1"

    real_dumps = json.dumps
    call_count = {"n": 0}

    def flaky_dumps(*args: Any, **kwargs: Any) -> str:
        call_count["n"] += 1
        if call_count["n"] == 3:
            raise RuntimeError("boom")
        return real_dumps(*args, **kwargs)

    with (
        mock.patch("py_core.backtests.cli.json.dumps", side_effect=flaky_dumps),
        pytest.raises(RuntimeError),
    ):
        save_results(result, output_dir)

    assert not output_dir.exists()
    assert list(tmp_path.glob(".tmp-*")) == []
