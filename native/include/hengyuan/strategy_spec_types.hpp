// SPDX-License-Identifier: proprietary
// strategy_spec_types.hpp — Batch 5, 5a: pure-data types for docs/STRATEGY_SPEC.md's TOML
// format. No I/O here (that's strategy_spec_toml_parser.hpp) — mirrors execution_types.hpp's
// role as a plain-data header other files build on.
//
// This is the C++ mirror of py_core/strategy_spec/schema.py's dataclasses. Fixed-capacity
// std::array/char-buffer storage throughout (no std::string/std::vector on anything that
// survives past load time) — matches the existing convention at account_truth.hpp's
// SymbolRules/ParsedExchangeInfo (kMaxSymbols=64, checked defensively at load, never grown at
// runtime). Loading a spec is a one-time, cold-start operation (like fetch_exchange_info()'s
// own parse step) — the per-bar streaming evaluation path (strategy_spec_operators.hpp/
// strategy_spec_evaluator.hpp, Batch 5's 5b) is the part that must stay allocation-free, not
// this file's types.
//
// Zero pointers anywhere in this file: an indicator's reference to an earlier node is a plain
// array index (Operand::node_idx), resolved once at load time — the C++ equivalent of
// evaluator.py's _resolve_operand() dict lookup, done once instead of every bar. This also
// means these types stay valid across a copy/move of the owning SpecDag (no dangling
// references the way a pointer-based DAG would risk).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hy {

// --- Operators (docs/STRATEGY_SPEC.md §5; py_core/strategy_spec/schema.py's KNOWN_OPS) ---
enum class SpecOp : std::uint8_t {
    Sma = 0,
    Ema = 1,
    Stddev = 2,
    RollingMax = 3,
    RollingMin = 4,
    Roc = 5,
    Rsi = 6,
    Lag = 7,
    Add = 8,
    Sub = 9,
    Mul = 10,
    Div = 11,
    Gt = 12,
    Lt = 13,
    Ge = 14,
    Le = 15,
    And = 16,
    Or = 17,
    CrossesAbove = 18,
    CrossesBelow = 19,
    Not = 20,
    IfThenElse = 21,
};

// Raw OHLCV field selector (schema.py's RAW_FIELDS = {open, high, low, close, volume}).
enum class RawField : std::uint8_t {
    Open = 0,
    High = 1,
    Low = 2,
    Close = 3,
    Volume = 4,
};

enum class OperandKind : std::uint8_t {
    RawFieldRef = 0,
    NodeRef = 1,
    Literal = 2,
};

// Non-owning, index-based operand — no pointers, no strings. `node_idx` is only meaningful
// when kind==NodeRef and is always < the owning node's own index in SpecDag::nodes (forward
// reference only, enforced at load time — see strategy_spec_toml_parser.cpp). A Literal
// operand is legal on crosses_above/crosses_below's operands (confirmed against actual
// evaluator.py behavior, not operators.py's docstring — see Batch 5's plan for the verified
// spec<->implementation arbitration: evaluator.py's _resolve_series() silently broadcasts a
// bare literal into a constant series, and schema.py never restricts these two ops' operands
// to string-only).
struct Operand {
    OperandKind kind{OperandKind::Literal};
    std::uint8_t raw_field_idx{0};  // valid iff kind==RawFieldRef; see RawField
    std::uint16_t node_idx{0};      // valid iff kind==NodeRef
    double literal_value{0.0};      // valid iff kind==Literal
};

static_assert(std::is_standard_layout_v<Operand>);
static_assert(std::is_trivially_copyable_v<Operand>);
// This is the size the natural layout produces (the double member forces 8-byte alignment on
// the whole struct) -- not a forced/padded alignment for a performance reason. See Batch 5's
// plan for why an artificial alignas() was declined here.
static_assert(sizeof(Operand) == 16);

inline constexpr std::size_t kMaxNodeIdLen = 32;         // schema.py: ^[a-z0-9_]{1,32}$
inline constexpr std::size_t kMaxOperandsPerNode = 3;    // if_then_else's cond/then/otherwise

struct SpecNode {
    char id[kMaxNodeIdLen + 1]{};  // NUL-terminated
    SpecOp op{SpecOp::Sma};
    std::uint8_t operand_count{0};
    Operand operands[kMaxOperandsPerNode]{};
    // window/n for ops that have one (schema.py's IndicatorNode.param: int | None);
    // -1 means "this op has no param field".
    std::int32_t param{-1};
};

static_assert(std::is_standard_layout_v<SpecNode>);
static_assert(std::is_trivially_copyable_v<SpecNode>);

// Generous fixed capacity, checked defensively at load time -- same "宽松固定容量+加载期拒绝"
// discipline as account_truth.hpp's kMaxSymbols=64 (a different, unrelated constant; see that
// file's own comment about the two other same-named-but-unrelated kMaxSymbols constants
// elsewhere in this codebase -- this one is unrelated to all three of those too).
inline constexpr std::size_t kMaxIndicatorNodes = 64;

enum class SignalMode : std::uint8_t {
    Boolean = 0,
    Scaled = 1,
};

struct SpecDag {
    std::array<SpecNode, kMaxIndicatorNodes> nodes{};
    std::size_t node_count{0};
    std::uint16_t signal_node_idx{0};
    SignalMode signal_mode{SignalMode::Boolean};
    // Computed once at load time (compute_effective_warmup()'s C++ port, 5b) -- §6 requires
    // this be statically knowable without any data, so it lives here rather than being
    // recomputed by every caller.
    std::uint32_t effective_warmup{0};
};

// --- [market] (py_core/manual_ohlcv.py's ManualMarket) ---
enum class ManualMarket : std::uint8_t {
    AShare = 0,
    UsStock = 1,
    HkStock = 2,
    CryptoSpot = 3,
};

inline constexpr std::size_t kMaxSymbolLen = 31;
inline constexpr std::size_t kMaxTimeframeLen = 7;  // e.g. "9999999w"-scale headroom

struct MarketBinding {
    ManualMarket market{ManualMarket::CryptoSpot};
    char symbol[kMaxSymbolLen + 1]{};       // normalized uppercase, matching CanonicalMarketSymbol
    char timeframe[kMaxTimeframeLen + 1]{}; // normalized lowercase, ^[1-9][0-9]*[mhdw]$
};

// --- [validation] (py_core/strategy_spec/schema.py's ValidationRecord; docs/STRATEGY_SPEC.md
// §4.5's 8 fields, in this exact order). deflated_sharpe_ratio is diagnostic-only in the
// Python pipeline (validation_sweep.py) and is never persisted -- it does not appear here
// either, on purpose, not by omission. ---
inline constexpr std::size_t kMaxTimestampLen = 63;
inline constexpr std::size_t kMaxValidatedByLen = 63;
inline constexpr std::size_t kMaxDigestLen = 127;

struct ValidationRecord {
    // false = "no [validation] section" -- same "0/false is the fail-closed unknown state"
    // convention as SymbolRules::rules_version==0 (account_truth.hpp). The C++ loader
    // (unlike py_core's schema.py, which must also parse specs that don't have one yet, since
    // it's the same parser validation_sweep.py uses to PRODUCE this section) always rejects a
    // spec where this is false -- see strategy_spec_toml_parser.hpp's load contract.
    bool present{false};
    char validated_at[kMaxTimestampLen + 1]{};
    char validated_by[kMaxValidatedByLen + 1]{};
    char data_start_utc[kMaxTimestampLen + 1]{};
    char data_end_utc[kMaxTimestampLen + 1]{};
    char data_digest[kMaxDigestLen + 1]{};
    double oos_sharpe{0.0};
    double pbo{0.0};
    std::int64_t trials{0};
};

// Fail-closed load status, mirroring env_loader.hpp's EnvLoadStatus/EnvLoadResult pattern.
enum class SpecLoadStatus : std::uint8_t {
    Ok = 0,
    TomlParseError = 1,
    UnknownOperator = 2,
    ForwardReferenceViolation = 3,
    DuplicateNodeId = 4,
    MissingRequiredField = 5,
    InvalidOperatorParameter = 6,
    TooManyIndicatorNodes = 7,
    UnsupportedSpecVersion = 8,
    MissingMarketBinding = 9,
    MissingValidationSection = 10,
    IncompleteValidationSection = 11,
};

struct SpecLoadResult {
    SpecLoadStatus status{SpecLoadStatus::TomlParseError};
    // Only meaningful when status==Ok -- same contract as EnvLoadResult's parse_result.
    SpecDag dag{};
    MarketBinding market{};
    ValidationRecord validation{};

    bool ok() const noexcept { return status == SpecLoadStatus::Ok; }
};

}  // namespace hy
