"""Tests for py_core.strategy_spec.validation_sweep (批次 3, round 3).

run_validation_sweep()'s own integration test caught a real bug during design (not just a
test failure after the fact): SpecStrategy's constructor kwarg was originally named `spec`,
which collides with load_strategy()'s own first positional parameter of the same name the
moment a spec gets threaded through a param_grid -- see spec_strategy.py's docstring for the
fix (renamed to `parsed_spec`). The test below exercises exactly that call path end-to-end
(sweep -> load_strategy() dispatch inside walk-forward/PBO -> SpecStrategy construction) so a
regression here would fail loudly again, not silently.
"""

from __future__ import annotations

import re
from datetime import UTC, datetime, timedelta
from decimal import Decimal

import numpy as np
import pytest

from py_core.backtests.models import BacktestConfig
from py_core.manual_ohlcv import (
    CanonicalMarketSymbol,
    ManualMarket,
    NormalizedOhlcvRecord,
    OhlcvTimeframe,
)
from py_core.strategy_spec.schema import parse_spec
from py_core.strategy_spec.validation_sweep import (
    compute_data_digest,
    generate_spec_variants,
    render_validation_toml_block,
    run_validation_sweep,
    write_validation_section,
)
from py_core.validation.splits import walk_forward_splits

_BASE_SPEC_TEXT = """
spec_version = 1
name = "sweep_test"

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
window = 20

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


# ---------------------------------------------------------------------------
# generate_spec_variants()
# ---------------------------------------------------------------------------


def test_generate_spec_variants_hand_verified_cartesian_product() -> None:
    spec = parse_spec(_BASE_SPEC_TEXT)
    sweep = {("fast", "param"): [5, 10], ("slow", "param"): [20, 30]}
    variants = generate_spec_variants(spec, sweep)

    assert len(variants) == 4
    seen = set()
    for v in variants:
        fast = next(n for n in v.indicators if n.id == "fast")
        slow = next(n for n in v.indicators if n.id == "slow")
        seen.add((fast.param, slow.param))
    assert seen == {(5, 20), (5, 30), (10, 20), (10, 30)}


def test_generate_spec_variants_does_not_mutate_base_spec() -> None:
    spec = parse_spec(_BASE_SPEC_TEXT)
    original_fast_param = next(n for n in spec.indicators if n.id == "fast").param
    generate_spec_variants(spec, {("fast", "param"): [99]})
    assert next(n for n in spec.indicators if n.id == "fast").param == original_fast_param


def test_generate_spec_variants_can_sweep_a_literal_operand_field() -> None:
    spec = parse_spec(_BASE_SPEC_TEXT)
    variants = generate_spec_variants(spec, {("entry", "right"): [1.0, 2.0]})
    right_values = {next(n for n in v.indicators if n.id == "entry").operands["right"] for v in variants}
    assert right_values == {1.0, 2.0}


def test_generate_spec_variants_rejects_empty_sweep() -> None:
    with pytest.raises(ValueError, match="sweep"):
        generate_spec_variants(parse_spec(_BASE_SPEC_TEXT), {})


def test_generate_spec_variants_rejects_unknown_node_id() -> None:
    with pytest.raises(ValueError, match="不存在"):
        generate_spec_variants(parse_spec(_BASE_SPEC_TEXT), {("nonexistent", "param"): [1]})


def test_generate_spec_variants_rejects_param_sweep_on_node_without_param() -> None:
    with pytest.raises(ValueError, match="没有 param"):
        generate_spec_variants(parse_spec(_BASE_SPEC_TEXT), {("entry", "param"): [1]})


def test_generate_spec_variants_rejects_unknown_operand_field() -> None:
    with pytest.raises(ValueError, match="没有字段"):
        generate_spec_variants(parse_spec(_BASE_SPEC_TEXT), {("fast", "left"): [1.0]})


# ---------------------------------------------------------------------------
# compute_data_digest()
# ---------------------------------------------------------------------------


def test_compute_data_digest_is_deterministic_and_correctly_formatted() -> None:
    records = _records(20)
    digest1 = compute_data_digest(records)
    digest2 = compute_data_digest(_records(20))
    assert digest1 == digest2
    assert re.fullmatch(r"sha256:[0-9a-f]{64}", digest1)


def test_compute_data_digest_changes_with_different_data() -> None:
    digest1 = compute_data_digest(_records(20, seed=1))
    digest2 = compute_data_digest(_records(20, seed=2))
    assert digest1 != digest2


# ---------------------------------------------------------------------------
# render_validation_toml_block() / write_validation_section()
# ---------------------------------------------------------------------------


def _sample_validation():
    spec_with_validation_text = _BASE_SPEC_TEXT + """
[validation]
validated_at   = "2026-01-01T00:00:00Z"
validated_by   = "walk_forward+cpcv+pbo"
data_start_utc = "2024-01-01T00:00:00Z"
data_end_utc   = "2024-01-13T00:00:00Z"
data_digest    = "sha256:abc123"
oos_sharpe     = 0.5
pbo            = 0.3
trials         = 4
"""
    return parse_spec(spec_with_validation_text).validation


def test_write_validation_section_appends_when_absent() -> None:
    validation = _sample_validation()
    new_text = write_validation_section(_BASE_SPEC_TEXT, validation)
    assert "[validation]" not in _BASE_SPEC_TEXT
    assert "[validation]" in new_text
    reparsed = parse_spec(new_text)
    assert reparsed.validation == validation
    assert reparsed.name == "sweep_test"  # rest of the document survives untouched


def test_write_validation_section_replaces_when_present() -> None:
    original_with_validation = _BASE_SPEC_TEXT + '\n[validation]\nvalidated_at="old"\nvalidated_by="old"\ndata_start_utc="old"\ndata_end_utc="old"\ndata_digest="old"\noos_sharpe=0.0\npbo=1.0\ntrials=1\n'
    new_validation = _sample_validation()
    new_text = write_validation_section(original_with_validation, new_validation)

    assert new_text.count("[validation]") == 1
    reparsed = parse_spec(new_text)
    assert reparsed.validation == new_validation
    assert reparsed.market.symbol.value == "BTCUSDT"  # rest of the document survives untouched


def test_render_validation_toml_block_round_trips_through_parse() -> None:
    validation = _sample_validation()
    block = render_validation_toml_block(validation)
    full_text = _BASE_SPEC_TEXT + "\n" + block
    assert parse_spec(full_text).validation == validation


# ---------------------------------------------------------------------------
# run_validation_sweep(): full pipeline integration
# ---------------------------------------------------------------------------


def test_run_validation_sweep_produces_a_populated_validation_record() -> None:
    spec = parse_spec(_BASE_SPEC_TEXT)
    sweep = {("fast", "param"): [5, 10], ("slow", "param"): [20, 30]}
    records = _records(300)
    config = BacktestConfig(initial_capital=100_000.0, fee_bps=5.0, slippage_bps=2.0)
    splits = walk_forward_splits(300, train_window=100, test_window=50)

    report = run_validation_sweep(
        config, records, spec, sweep, walk_forward_splits=splits, cpcv_n_groups=4, purge_bars=1
    )

    assert report.trials == 4
    assert 0.0 <= report.pbo <= 1.0
    assert 0.0 <= report.deflated_sharpe_ratio <= 1.0

    validation = report.winning_spec.validation
    assert validation is not None
    assert validation.trials == 4
    assert validation.oos_sharpe == pytest.approx(report.oos_sharpe)
    assert validation.pbo == pytest.approx(report.pbo)
    assert validation.data_start_utc == records[0].event_time_utc.strftime("%Y-%m-%dT%H:%M:%SZ")
    assert validation.data_end_utc == records[-1].event_time_utc.strftime("%Y-%m-%dT%H:%M:%SZ")
    assert validation.data_digest == compute_data_digest(records)
    assert validation.validated_by == "walk_forward+cpcv+pbo"


def test_run_validation_sweep_result_writes_back_into_valid_toml() -> None:
    spec = parse_spec(_BASE_SPEC_TEXT)
    sweep = {("fast", "param"): [5, 10], ("slow", "param"): [20, 30]}
    records = _records(300)
    config = BacktestConfig(initial_capital=100_000.0, fee_bps=5.0, slippage_bps=2.0)
    splits = walk_forward_splits(300, train_window=100, test_window=50)

    report = run_validation_sweep(
        config, records, spec, sweep, walk_forward_splits=splits, cpcv_n_groups=4, purge_bars=1
    )

    new_text = write_validation_section(_BASE_SPEC_TEXT, report.winning_spec.validation)
    reparsed = parse_spec(new_text)
    assert reparsed.validation == report.winning_spec.validation

    # the winning spec's own indicator params must be one of the swept combinations
    fast_param = next(n for n in report.winning_spec.indicators if n.id == "fast").param
    slow_param = next(n for n in report.winning_spec.indicators if n.id == "slow").param
    assert fast_param in (5, 10)
    assert slow_param in (20, 30)
