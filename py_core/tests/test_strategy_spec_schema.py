"""Tests for py_core.strategy_spec.schema (批次 3, round 2).

The full example from docs/STRATEGY_SPEC.md §7 is used verbatim as the "known-good" spec
throughout -- if the doc's own worked example stopped parsing, that would mean this parser
and the document it's implementing have already diverged, which is exactly the kind of thing
a test suite for a spec implementation should catch first.
"""

from __future__ import annotations

import pytest

from py_core.manual_ohlcv import CanonicalMarketSymbol, ManualMarket, OhlcvTimeframe
from py_core.strategy_spec.schema import StrategySpecError, parse_spec

_DOC_EXAMPLE = """
spec_version = 1
name = "sma_crossover_btc_1h"
description = "20/50 SMA 金叉做多，用 RSI 过滤超买"

[market]
market    = "crypto_spot"
symbol    = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "fast"
op = "sma"
input = "close"
window = 20

[[indicators]]
id = "slow"
op = "sma"
input = "close"
window = 50

[[indicators]]
id = "rsi14"
op = "rsi"
input = "close"
window = 14

[[indicators]]
id = "not_overbought"
op = "lt"
left = "rsi14"
right = 70.0

[[indicators]]
id = "trend_up"
op = "gt"
left = "fast"
right = "slow"

[[indicators]]
id = "entry"
op = "and"
left = "trend_up"
right = "not_overbought"

[signal]
node = "entry"
mode = "boolean"

[validation]
validated_at   = "2026-09-15T08:30:00Z"
validated_by   = "walk_forward+cpcv"
data_start_utc = "2021-01-01T00:00:00Z"
data_end_utc   = "2026-06-30T00:00:00Z"
data_digest    = "sha256:0000000000000000000000000000000000000000000000000000000000000000"
oos_sharpe     = 1.34
pbo            = 0.21
trials         = 480
"""


def _minimal_spec(*, extra_indicator: str = "", signal_node: str = "entry") -> str:
    return f"""
spec_version = 1
name = "minimal_test"

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
{extra_indicator}

[signal]
node = "{signal_node}"
mode = "boolean"
"""


# ---------------------------------------------------------------------------
# The doc's own worked example: full round-trip
# ---------------------------------------------------------------------------


def test_doc_example_parses_correctly() -> None:
    spec = parse_spec(_DOC_EXAMPLE)
    assert spec.spec_version == 1
    assert spec.name == "sma_crossover_btc_1h"
    assert spec.description == "20/50 SMA 金叉做多，用 RSI 过滤超买"
    assert spec.market.market == ManualMarket.CRYPTO_SPOT
    assert spec.market.symbol == CanonicalMarketSymbol("BTCUSDT")
    assert spec.market.timeframe == OhlcvTimeframe("1h")
    assert [n.id for n in spec.indicators] == ["fast", "slow", "rsi14", "not_overbought", "trend_up", "entry"]
    assert spec.signal.node == "entry"
    assert spec.signal.mode == "boolean"
    assert spec.validation is not None
    assert spec.validation.oos_sharpe == pytest.approx(1.34)
    assert spec.validation.trials == 480


def test_doc_example_operand_and_param_shapes() -> None:
    spec = parse_spec(_DOC_EXAMPLE)
    by_id = {n.id: n for n in spec.indicators}
    assert by_id["fast"].operands == {"input": "close"}
    assert by_id["fast"].param == 20
    assert by_id["not_overbought"].operands == {"left": "rsi14", "right": 70.0}
    assert by_id["entry"].operands == {"left": "trend_up", "right": "not_overbought"}


def test_validation_section_is_optional() -> None:
    without_validation = _DOC_EXAMPLE.rsplit("[validation]", 1)[0]
    spec = parse_spec(without_validation)
    assert spec.validation is None


def test_description_is_optional() -> None:
    spec = parse_spec(_minimal_spec())
    assert spec.description is None


# ---------------------------------------------------------------------------
# Top-level field validation
# ---------------------------------------------------------------------------


def test_rejects_wrong_spec_version() -> None:
    with pytest.raises(StrategySpecError, match="spec_version"):
        parse_spec(_minimal_spec().replace("spec_version = 1", "spec_version = 2"))


def test_rejects_bad_name() -> None:
    with pytest.raises(StrategySpecError, match="name"):
        parse_spec(_minimal_spec().replace('name = "minimal_test"', 'name = "Not Valid!"'))


# ---------------------------------------------------------------------------
# [market] validation
# ---------------------------------------------------------------------------


def test_rejects_unknown_market() -> None:
    with pytest.raises(StrategySpecError, match="market"):
        parse_spec(_minimal_spec().replace('market = "crypto_spot"', 'market = "not_a_real_market"'))


def test_rejects_bad_timeframe() -> None:
    with pytest.raises(StrategySpecError, match="timeframe"):
        parse_spec(_minimal_spec().replace('timeframe = "1h"', 'timeframe = "notatimeframe"'))


def test_rejects_missing_market_table() -> None:
    text = _minimal_spec()
    text_no_market = "\n".join(
        line for line in text.splitlines() if not line.startswith(("[market]", "market =", "symbol =", "timeframe ="))
    )
    with pytest.raises(StrategySpecError, match="market"):
        parse_spec(text_no_market)


# ---------------------------------------------------------------------------
# [[indicators]] structural validation
# ---------------------------------------------------------------------------


def test_rejects_unknown_operator() -> None:
    text = _minimal_spec().replace('op = "gt"\nleft = "fast"\nright = "slow"', 'op = "banana"\nleft = "fast"\nright = "slow"')
    with pytest.raises(StrategySpecError, match="op"):
        parse_spec(text)


def test_rejects_duplicate_id() -> None:
    extra = '\n[[indicators]]\nid = "fast"\nop = "sma"\ninput = "close"\nwindow = 3\n'
    with pytest.raises(StrategySpecError, match="重复"):
        parse_spec(_minimal_spec(extra_indicator=extra))


def test_rejects_id_colliding_with_raw_field() -> None:
    extra = '\n[[indicators]]\nid = "close"\nop = "sma"\ninput = "close"\nwindow = 3\n'
    with pytest.raises(StrategySpecError, match="原始字段"):
        parse_spec(_minimal_spec(extra_indicator=extra))


def test_rejects_forward_reference_to_later_node() -> None:
    text = """
spec_version = 1
name = "bad_forward_ref"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "a"
op = "gt"
left = "b"
right = "close"

[[indicators]]
id = "b"
op = "sma"
input = "close"
window = 5

[signal]
node = "a"
mode = "boolean"
"""
    with pytest.raises(StrategySpecError, match="向前引用"):
        parse_spec(text)


def test_rejects_reference_to_undeclared_id() -> None:
    text = _minimal_spec().replace('right = "slow"', 'right = "nonexistent"')
    with pytest.raises(StrategySpecError, match="向前引用"):
        parse_spec(text)


def test_rejects_missing_required_operand_field() -> None:
    text = _minimal_spec().replace('left = "fast"\nright = "slow"', 'left = "fast"')
    with pytest.raises(StrategySpecError, match="right"):
        parse_spec(text)


def test_rejects_missing_window_param() -> None:
    text = _minimal_spec().replace('op = "sma"\ninput = "close"\nwindow = 5', 'op = "sma"\ninput = "close"')
    with pytest.raises(StrategySpecError, match="window"):
        parse_spec(text)


def test_rejects_non_integer_window() -> None:
    text = _minimal_spec().replace("window = 5", 'window = "five"')
    with pytest.raises(StrategySpecError, match="window"):
        parse_spec(text)


def test_rejects_literal_in_string_only_field() -> None:
    text = _minimal_spec().replace('input = "close"\nwindow = 5\n\n[[indicators]]\nid = "slow"', "input = 3.0\nwindow = 5\n\n[[indicators]]\nid = \"slow\"")
    with pytest.raises(StrategySpecError, match="不能是字面量"):
        parse_spec(text)


def test_rejects_bad_id_pattern() -> None:
    text = _minimal_spec().replace('id = "fast"', 'id = "FAST-not-valid"')
    with pytest.raises(StrategySpecError, match="id"):
        parse_spec(text)


def test_accepts_numeric_literal_operand() -> None:
    text = _minimal_spec(extra_indicator='')
    text = text.replace('right = "slow"', "right = 42.0")
    spec = parse_spec(text)
    by_id = {n.id: n for n in spec.indicators}
    assert by_id["entry"].operands["right"] == 42.0


# ---------------------------------------------------------------------------
# [signal] validation
# ---------------------------------------------------------------------------


def test_rejects_signal_referencing_unknown_node() -> None:
    with pytest.raises(StrategySpecError, match="signal"):
        parse_spec(_minimal_spec(signal_node="does_not_exist"))


def test_rejects_bad_signal_mode() -> None:
    text = _minimal_spec().replace('mode = "boolean"', 'mode = "not_a_mode"')
    with pytest.raises(StrategySpecError, match="mode"):
        parse_spec(text)


def test_accepts_scaled_mode() -> None:
    text = _minimal_spec().replace('mode = "boolean"', 'mode = "scaled"')
    spec = parse_spec(text)
    assert spec.signal.mode == "scaled"


# ---------------------------------------------------------------------------
# [validation] structural validation (when present)
# ---------------------------------------------------------------------------


def test_rejects_incomplete_validation_section() -> None:
    incomplete = _DOC_EXAMPLE.split("[validation]")[0] + '[validation]\nvalidated_at = "2026-09-15T08:30:00Z"\n'
    with pytest.raises(StrategySpecError, match="validation"):
        parse_spec(incomplete)
