// SPDX-License-Identifier: proprietary
// strategy_spec_toml_parser.hpp — Batch 5, 5a: loads and strictly validates a
// docs/STRATEGY_SPEC.md TOML document into a SpecLoadResult.
//
// TOML subset only, hand-rolled (not toml++/tomlplusplus) -- the format actually used by
// STRATEGY_SPEC.md is narrow (bare integers/floats, double-quoted strings, three flat tables
// plus one flat array-of-tables; no inline tables, no native datetimes, no multi-line strings,
// no booleans). See Batch 5's plan for the full reasoning; a third-party general TOML library
// would add a new FetchContent dependency for ~95% unused surface area, which this repo's
// CMakeLists.txt deliberately keeps minimal (currently exactly two: simdjson, GoogleTest).
//
// Fail-closed contract (deliberately STRICTER than py_core/strategy_spec/schema.py's Python
// parser): this loader always requires a complete [validation] and [market] section. The
// Python side's ValidationRecord is `| None` because schema.py's own parser is also used by
// validation_sweep.py to PRODUCE the [validation] section before it exists -- this C++ loader
// has no such producer role, it only ever consumes an already-validated spec, so it is
// correct for it to reject anything less than complete (docs/STRATEGY_SPEC.md §4.5).

#pragma once

#include <hengyuan/strategy_spec_types.hpp>

#include <string_view>

namespace hy {

// Parses and strictly validates `toml_text`. Never throws; all failure modes are reported via
// SpecLoadResult::status. dag/market/validation are only meaningful when status==Ok.
SpecLoadResult load_strategy_spec(std::string_view toml_text) noexcept;

// Non-per-bar check: does a loaded spec's [market] binding match the market/symbol/timeframe
// of a live or backtest data feed being attached to it? Mirrors evaluator.py's
// validate_market_binding() -- called once when binding a spec to a feed, not on the
// per-bar path (docs/STRATEGY_SPEC.md §4.2).
bool market_binding_matches(const MarketBinding& binding, ManualMarket market,
                             std::string_view symbol, std::string_view timeframe) noexcept;

}  // namespace hy
