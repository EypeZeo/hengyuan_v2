"""Tests for py_core.strategies.spec_strategy (批次 3, round 2).

The key architectural property under test: a StrategySpec plugs into everything already
built around the Strategy ABC without any special-casing, INCLUDING via
`load_strategy()`'s trusted-namespace dispatch (no `allow_external=True` needed) -- proving
`SpecStrategy`'s placement inside `py_core.strategies.` rather than `py_core.strategy_spec`
was the right call, not just a documented intention.
"""

from __future__ import annotations

from datetime import UTC, datetime, timedelta
from decimal import Decimal

import numpy as np
import pytest

from py_core.backtests.models import BacktestConfig
from py_core.backtests.vectorized_engine import run_vectorized_backtest
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)
from py_core.strategies.base import load_strategy
from py_core.strategies.spec_strategy import SpecStrategy
from py_core.strategy_spec.schema import parse_spec

_SPEC_TEXT = """
spec_version = 1
name = "spec_strategy_test"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "fast"
op = "sma"
input = "close"
window = 5

[[indicators]]
id = "slow"
op = "sma"
input = "close"
window = 10

[[indicators]]
id = "entry"
op = "gt"
left = "fast"
right = "slow"

[signal]
node = "entry"
mode = "boolean"
"""


def _records(n: int, *, seed: int = 1) -> list[NormalizedOhlcvRecord]:
    rng = np.random.default_rng(seed)
    prices = 100.0 * np.exp(np.cumsum(rng.normal(0.0, 0.01, n)))
    t0 = datetime(2024, 1, 1, tzinfo=UTC)
    return [
        NormalizedOhlcvRecord(
            market=ManualMarket.CRYPTO_SPOT,
            symbol=CanonicalMarketSymbol("BTCUSDT"),
            timeframe=OhlcvTimeframe("1h"),
            event_time_utc=t0 + timedelta(hours=i),
            open_price=Decimal(str(round(float(p), 6))),
            high_price=Decimal(str(round(float(p) * 1.001, 6))),
            low_price=Decimal(str(round(float(p) * 0.999, 6))),
            close_price=Decimal(str(round(float(p), 6))),
            volume=Decimal(1000),
        )
        for i, p in enumerate(prices)
    ]


def test_spec_strategy_requires_exactly_one_of_spec_path_or_spec() -> None:
    with pytest.raises(ValueError, match="二者之一"):
        SpecStrategy()
    with pytest.raises(ValueError, match="二者之一"):
        SpecStrategy(spec_path="does_not_matter.toml", spec=parse_spec(_SPEC_TEXT))


def test_spec_strategy_from_parsed_spec_generates_valid_signal() -> None:
    spec = parse_spec(_SPEC_TEXT)
    strategy = SpecStrategy(spec=spec)
    records = _records(100)
    from py_core.backtests.vectorized_engine import records_to_dataframe

    df = records_to_dataframe(records)
    signal = strategy.generate_signals(df)
    assert signal.index.equals(df.index)
    assert not signal.isna().any()
    assert set(signal.unique()) <= {0.0, 1.0}


def test_spec_strategy_from_file_path(tmp_path) -> None:
    spec_path = tmp_path / "spec_strategy_test.toml"
    spec_path.write_text(_SPEC_TEXT, encoding="utf-8")

    strategy = SpecStrategy(spec_path=str(spec_path))
    assert strategy.spec.name == "spec_strategy_test"


def test_spec_strategy_loads_via_load_strategy_trusted_namespace(tmp_path) -> None:
    spec_path = tmp_path / "spec_strategy_test.toml"
    spec_path.write_text(_SPEC_TEXT, encoding="utf-8")

    # No allow_external=True -- proves SpecStrategy's placement inside py_core.strategies.
    # (not py_core.strategy_spec.) is load-bearing, not just documented intention.
    strategy = load_strategy("py_core.strategies.spec_strategy:SpecStrategy", spec_path=str(spec_path))
    assert strategy.spec.name == "spec_strategy_test"


def test_spec_strategy_runs_through_full_vectorized_backtest() -> None:
    strategy = SpecStrategy(spec=parse_spec(_SPEC_TEXT))
    records = _records(150)
    from py_core.backtests.vectorized_engine import records_to_dataframe

    df = records_to_dataframe(records)
    signal = strategy.generate_signals(df)

    config = BacktestConfig(initial_capital=10_000.0, fee_bps=1.0, slippage_bps=1.0)
    result = run_vectorized_backtest(config, records, signal)
    assert result.metrics is not None
    assert len(result.returns) == len(records)
