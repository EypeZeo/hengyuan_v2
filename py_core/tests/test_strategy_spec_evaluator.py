"""Tests for py_core.strategy_spec.evaluator (批次 3, round 2).

The docs/STRATEGY_SPEC.md §7 worked example is reused (from test_strategy_spec_schema.py's
_DOC_EXAMPLE) as the primary end-to-end check: its warm-up is hand-computable
(max(sma_50=50, rsi_14+1=15) = 50, exactly what the doc itself states) and its signal is
generated against synthetic OHLCV data, giving a real integration check spanning parsing,
warm-up computation, and evaluation together, not just each in isolation.
"""

from __future__ import annotations

from datetime import UTC, datetime, timedelta
from decimal import Decimal

import numpy as np
import pandas as pd
import pytest

from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)
from py_core.strategy_spec.evaluator import (
    compute_effective_warmup,
    evaluate_spec,
    validate_market_binding,
)
from py_core.strategy_spec.schema import StrategySpecError, parse_spec
from py_core.tests.test_strategy_spec_schema import _DOC_EXAMPLE, _minimal_spec


def _ohlcv_df(n: int, *, seed: int = 1) -> pd.DataFrame:
    rng = np.random.default_rng(seed)
    prices = 100.0 * np.exp(np.cumsum(rng.normal(0.0, 0.01, n)))
    idx = pd.date_range("2024-01-01", periods=n, freq="h")
    return pd.DataFrame(
        {"open": prices, "high": prices * 1.001, "low": prices * 0.999, "close": prices, "volume": 1000.0}, index=idx
    )


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


# ---------------------------------------------------------------------------
# compute_effective_warmup(): hand-verified against the doc's own stated value
# ---------------------------------------------------------------------------


def test_doc_example_warmup_matches_doc_stated_value() -> None:
    spec = parse_spec(_DOC_EXAMPLE)
    # doc §7: "该 spec 的有效 warm-up = max(sma50=50, rsi14=15) = 50 根 bar"
    assert compute_effective_warmup(spec) == 50


def test_warmup_hand_verified_simple_chain() -> None:
    spec = parse_spec(_minimal_spec())  # fast=sma(close,5), slow=sma(close,10), entry=gt(fast,slow)
    assert compute_effective_warmup(spec) == 10  # max(5, 10) + 0


def test_warmup_accounts_for_lag_and_crosses_extra_bar() -> None:
    text = """
spec_version = 1
name = "warmup_test"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "s"
op = "sma"
input = "close"
window = 10

[[indicators]]
id = "lagged"
op = "lag"
input = "s"
n = 3

[[indicators]]
id = "crossed"
op = "crosses_above"
left = "s"
right = "lagged"

[signal]
node = "crossed"
mode = "boolean"
"""
    spec = parse_spec(text)
    # s: warmup 10. lagged: own(3) + max(s=10) = 13. crossed: own(1, "one more bar") + max(s=10, lagged=13) = 14.
    assert compute_effective_warmup(spec) == 14


# ---------------------------------------------------------------------------
# evaluate_spec(): end-to-end on the doc's own example
# ---------------------------------------------------------------------------


def test_doc_example_evaluates_with_correct_warmup_region_and_boolean_range() -> None:
    spec = parse_spec(_DOC_EXAMPLE)
    df = _ohlcv_df(200)
    warmup = compute_effective_warmup(spec)

    signal = evaluate_spec(spec, df)
    assert not signal.isna().any()
    assert set(signal.unique()) <= {0.0, 1.0}
    assert (signal.iloc[:warmup] == 0.0).all()
    assert signal.index.equals(df.index)


def test_boolean_mode_treats_any_nonzero_as_true() -> None:
    text = """
spec_version = 1
name = "boolean_truthiness"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "raw"
op = "sma"
input = "close"
window = 1

[signal]
node = "raw"
mode = "boolean"
"""
    spec = parse_spec(text)
    df = pd.DataFrame(
        {"open": [1.0, -2.0, 0.0], "high": [1.0, -2.0, 0.0], "low": [1.0, -2.0, 0.0], "close": [1.0, -2.0, 0.0], "volume": [1.0, 1.0, 1.0]},
        index=pd.date_range("2024-01-01", periods=3, freq="h"),
    )
    signal = evaluate_spec(spec, df)
    assert signal.tolist() == [1.0, 1.0, 0.0]  # sma(window=1) == raw close; nonzero->1.0, 0.0->0.0


def test_scaled_mode_clips_and_fills_nan() -> None:
    text = """
spec_version = 1
name = "scaled_test"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "raw"
op = "sma"
input = "close"
window = 2

[signal]
node = "raw"
mode = "scaled"
"""
    spec = parse_spec(text)
    df = pd.DataFrame(
        {"open": [1.0, 5.0, -10.0], "high": [1.0, 5.0, -10.0], "low": [1.0, 5.0, -10.0], "close": [1.0, 5.0, -10.0], "volume": [1.0, 1.0, 1.0]},
        index=pd.date_range("2024-01-01", periods=3, freq="h"),
    )
    signal = evaluate_spec(spec, df)
    assert signal.iloc[0] == pytest.approx(0.0)  # NaN (warm-up) -> 0.0
    assert signal.iloc[1] == pytest.approx(1.0)  # sma(1,5)=3.0 clipped to 1.0
    assert signal.iloc[2] == pytest.approx(-1.0)  # sma(5,-10)=-2.5 clipped to -1.0


def test_literal_operand_broadcasts_correctly() -> None:
    text = """
spec_version = 1
name = "literal_test"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "above_100"
op = "gt"
left = "close"
right = 100.0

[signal]
node = "above_100"
mode = "boolean"
"""
    spec = parse_spec(text)
    df = pd.DataFrame(
        {"open": [90.0, 100.0, 110.0], "high": [90.0, 100.0, 110.0], "low": [90.0, 100.0, 110.0], "close": [90.0, 100.0, 110.0], "volume": [1.0, 1.0, 1.0]},
        index=pd.date_range("2024-01-01", periods=3, freq="h"),
    )
    signal = evaluate_spec(spec, df)
    assert signal.tolist() == [0.0, 0.0, 1.0]


def test_crosses_above_with_literal_right_operand() -> None:
    text = """
spec_version = 1
name = "crosses_literal_test"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "crossed"
op = "crosses_above"
left = "close"
right = 100.0

[signal]
node = "crossed"
mode = "boolean"
"""
    spec = parse_spec(text)
    df = pd.DataFrame(
        {
            "open": [90.0, 95.0, 105.0, 95.0],
            "high": [90.0, 95.0, 105.0, 95.0],
            "low": [90.0, 95.0, 105.0, 95.0],
            "close": [90.0, 95.0, 105.0, 95.0],
            "volume": [1.0, 1.0, 1.0, 1.0],
        },
        index=pd.date_range("2024-01-01", periods=4, freq="h"),
    )
    signal = evaluate_spec(spec, df)
    assert signal.tolist() == [0.0, 0.0, 1.0, 0.0]  # crosses 100 on bar 2 only


# ---------------------------------------------------------------------------
# validate_market_binding()
# ---------------------------------------------------------------------------


def test_validate_market_binding_accepts_matching_records() -> None:
    spec = parse_spec(_minimal_spec())
    validate_market_binding(spec, _records(20))  # should not raise


def test_validate_market_binding_rejects_mismatched_symbol() -> None:
    spec = parse_spec(_minimal_spec())
    records = _records(5)
    bad = [
        NormalizedOhlcvRecord(
            market=r.market, symbol=CanonicalMarketSymbol("ETHUSDT"), timeframe=r.timeframe,
            event_time_utc=r.event_time_utc, open_price=r.open_price, high_price=r.high_price,
            low_price=r.low_price, close_price=r.close_price, volume=r.volume,
        )
        for r in records
    ]
    with pytest.raises(StrategySpecError, match="不一致"):
        validate_market_binding(spec, bad)


def test_validate_market_binding_rejects_mismatched_timeframe() -> None:
    spec = parse_spec(_minimal_spec())
    records = _records(5)
    bad = [
        NormalizedOhlcvRecord(
            market=r.market, symbol=r.symbol, timeframe=OhlcvTimeframe("15m"),
            event_time_utc=r.event_time_utc, open_price=r.open_price, high_price=r.high_price,
            low_price=r.low_price, close_price=r.close_price, volume=r.volume,
        )
        for r in records
    ]
    with pytest.raises(StrategySpecError, match="不一致"):
        validate_market_binding(spec, bad)
