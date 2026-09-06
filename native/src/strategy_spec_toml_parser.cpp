// SPDX-License-Identifier: proprietary
// strategy_spec_toml_parser.cpp — see strategy_spec_toml_parser.hpp for the contract.
//
// Single-pass line tokenizer + a two-phase parse (raw key=value accumulation per section,
// then a validation/assembly pass) -- mirrors py_core/strategy_spec/schema.py's own
// "tomllib.loads() then structural validation" split, just without a generic TOML value tree
// in between (this format's grammar is narrow enough that going straight from tokenized lines
// to typed fields is simpler than building then walking a generic tree).

#include <hengyuan/strategy_spec_toml_parser.hpp>

#include <charconv>
#include <cstring>
#include <optional>
#include <vector>

namespace hy {
namespace {

// --- op metadata (mirrors schema.py's per-op helper functions) ---

constexpr bool is_window_op(SpecOp op) noexcept {
    switch (op) {
        case SpecOp::Sma:
        case SpecOp::Ema:
        case SpecOp::Stddev:
        case SpecOp::RollingMax:
        case SpecOp::RollingMin:
        case SpecOp::Roc:
        case SpecOp::Rsi:
            return true;
        default:
            return false;
    }
}

std::uint8_t operand_arity(SpecOp op) noexcept {
    if (is_window_op(op) || op == SpecOp::Lag || op == SpecOp::Not) return 1;
    if (op == SpecOp::IfThenElse) return 3;
    return 2;  // binary value ops
}

// schema.py's _string_only_operand_fields(): only "input" (windowed/lag/not) and "cond"
// (ternary, slot 0) are forced to reference a raw field or node id. Binary value ops --
// including crosses_above/crosses_below -- allow a literal on either side; this is the
// verified spec<->implementation arbitration recorded in Batch 5's plan (evaluator.py's
// _resolve_series() silently broadcasts a bare literal into a constant series for these two
// ops, and schema.py never restricts their operands to string-only).
bool operand_must_be_reference(SpecOp op, std::uint8_t slot_idx) noexcept {
    if (is_window_op(op) || op == SpecOp::Lag || op == SpecOp::Not) return slot_idx == 0;
    if (op == SpecOp::IfThenElse) return slot_idx == 0;  // "cond"
    return false;
}

bool op_has_param(SpecOp op) noexcept { return is_window_op(op) || op == SpecOp::Lag; }

// Minimum legal window/n for a given op -- schema.py itself doesn't range-check this (that's
// operators.py's job at call time in Python); this loader does it at load time instead, per
// Batch 5's plan ("非法参数应该在 spec 被接受之前就报错"). stddev needs window>=2 for ddof=1
// (operators.py's own `if window < 2: raise ValueError`); every other windowed op and lag
// need >=1.
std::int32_t min_param_value(SpecOp op) noexcept { return op == SpecOp::Stddev ? 2 : 1; }

std::optional<SpecOp> op_from_name(std::string_view name) noexcept {
    if (name == "sma") return SpecOp::Sma;
    if (name == "ema") return SpecOp::Ema;
    if (name == "stddev") return SpecOp::Stddev;
    if (name == "rolling_max") return SpecOp::RollingMax;
    if (name == "rolling_min") return SpecOp::RollingMin;
    if (name == "roc") return SpecOp::Roc;
    if (name == "rsi") return SpecOp::Rsi;
    if (name == "lag") return SpecOp::Lag;
    if (name == "add") return SpecOp::Add;
    if (name == "sub") return SpecOp::Sub;
    if (name == "mul") return SpecOp::Mul;
    if (name == "div") return SpecOp::Div;
    if (name == "gt") return SpecOp::Gt;
    if (name == "lt") return SpecOp::Lt;
    if (name == "ge") return SpecOp::Ge;
    if (name == "le") return SpecOp::Le;
    if (name == "and") return SpecOp::And;
    if (name == "or") return SpecOp::Or;
    if (name == "crosses_above") return SpecOp::CrossesAbove;
    if (name == "crosses_below") return SpecOp::CrossesBelow;
    if (name == "not") return SpecOp::Not;
    if (name == "if_then_else") return SpecOp::IfThenElse;
    return std::nullopt;
}

// Field name for a given (op, slot) pair -- used only to match the correct TOML key while
// scanning a [[indicators]] entry's raw key=value lines (schema.py's _operand_field_names()).
std::string_view operand_field_name(SpecOp op, std::uint8_t slot_idx) noexcept {
    if (is_window_op(op) || op == SpecOp::Lag || op == SpecOp::Not) return "input";
    if (op == SpecOp::IfThenElse) {
        static constexpr std::string_view kNames[3] = {"cond", "then", "otherwise"};
        return kNames[slot_idx];
    }
    static constexpr std::string_view kNames[2] = {"left", "right"};
    return kNames[slot_idx];
}

std::optional<RawField> raw_field_from_name(std::string_view name) noexcept {
    if (name == "open") return RawField::Open;
    if (name == "high") return RawField::High;
    if (name == "low") return RawField::Low;
    if (name == "close") return RawField::Close;
    if (name == "volume") return RawField::Volume;
    return std::nullopt;
}

std::optional<ManualMarket> market_from_name(std::string_view name) noexcept {
    if (name == "a_share") return ManualMarket::AShare;
    if (name == "us_stock") return ManualMarket::UsStock;
    if (name == "hk_stock") return ManualMarket::HkStock;
    if (name == "crypto_spot") return ManualMarket::CryptoSpot;
    return std::nullopt;
}

bool is_lower_alnum_underscore(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

bool is_valid_id(std::string_view s) noexcept {
    if (s.empty() || s.size() > kMaxNodeIdLen) return false;
    for (char c : s) {
        if (!is_lower_alnum_underscore(c)) return false;
    }
    return true;
}

bool is_valid_name(std::string_view s) noexcept {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
        if (!is_lower_alnum_underscore(c)) return false;
    }
    return true;
}

// schema.py / manual_ohlcv.py's OhlcvTimeframe: normalized to lowercase, then
// ^[1-9][0-9]*[mhdw]$. `s` must already be lowercased by the caller.
bool is_valid_timeframe(std::string_view s) noexcept {
    if (s.size() < 2 || s.size() > kMaxTimeframeLen) return false;
    const char unit = s.back();
    if (unit != 'm' && unit != 'h' && unit != 'd' && unit != 'w') return false;
    std::string_view digits = s.substr(0, s.size() - 1);
    if (digits.empty() || digits[0] == '0') return false;
    for (char c : digits) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

char to_lower_ascii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// --- tokenizer ---

std::string_view trim(std::string_view s) noexcept {
    std::size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r')) ++start;
    std::size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) --end;
    return s.substr(start, end - start);
}

// Strips a trailing '#' comment, respecting double-quoted strings (a '#' inside quotes is
// literal text, not a comment marker).
std::string_view strip_comment(std::string_view line) noexcept {
    bool in_quotes = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') {
            in_quotes = !in_quotes;
        } else if (line[i] == '#' && !in_quotes) {
            return line.substr(0, i);
        }
    }
    return line;
}

enum class LineKind { Blank, Table, ArrayTable, KeyValue, Invalid };

struct ParsedLine {
    LineKind kind{LineKind::Invalid};
    std::string_view table_name;  // Table/ArrayTable
    std::string_view key;         // KeyValue
    std::string_view raw_value;   // KeyValue, still needs value parsing
};

ParsedLine classify_line(std::string_view line) noexcept {
    line = trim(strip_comment(line));
    if (line.empty()) return {LineKind::Blank, {}, {}, {}};

    if (line.size() >= 4 && line[0] == '[' && line[1] == '[' && line.back() == ']' &&
        line[line.size() - 2] == ']') {
        std::string_view name = trim(line.substr(2, line.size() - 4));
        if (name.empty()) return {LineKind::Invalid, {}, {}, {}};
        return {LineKind::ArrayTable, name, {}, {}};
    }
    if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
        std::string_view name = trim(line.substr(1, line.size() - 2));
        if (name.empty()) return {LineKind::Invalid, {}, {}, {}};
        return {LineKind::Table, name, {}, {}};
    }

    const auto eq = line.find('=');
    if (eq == std::string_view::npos) return {LineKind::Invalid, {}, {}, {}};
    std::string_view key = trim(line.substr(0, eq));
    std::string_view value = trim(line.substr(eq + 1));
    if (key.empty() || value.empty()) return {LineKind::Invalid, {}, {}, {}};
    for (char c : key) {
        if (!is_lower_alnum_underscore(c)) return {LineKind::Invalid, {}, {}, {}};
    }
    return {LineKind::KeyValue, {}, key, value};
}

enum class ValueKind { String, Integer, Float };

struct ParsedValue {
    ValueKind kind{ValueKind::String};
    std::string_view str_val;
    std::int64_t int_val{0};
    double float_val{0.0};
};

// Parses a single already-trimmed value token. Only double-quoted strings, bare integers, and
// bare decimal floats are recognized -- anything else (inline tables, arrays, booleans,
// multi-line/literal strings) is a hard parse error, matching this format's actual grammar
// (see this file's header comment).
bool parse_value(std::string_view token, ParsedValue& out) noexcept {
    if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
        // No escape-sequence support -- this format's string fields (ids, names, symbols,
        // timestamps, digests) never need one.
        out.kind = ValueKind::String;
        out.str_val = token.substr(1, token.size() - 2);
        return true;
    }

    std::int64_t int_result = 0;
    auto int_parse = std::from_chars(token.data(), token.data() + token.size(), int_result);
    if (int_parse.ec == std::errc{} && int_parse.ptr == token.data() + token.size()) {
        out.kind = ValueKind::Integer;
        out.int_val = int_result;
        return true;
    }

    double float_result = 0.0;
    auto float_parse = std::from_chars(token.data(), token.data() + token.size(), float_result);
    if (float_parse.ec == std::errc{} && float_parse.ptr == token.data() + token.size()) {
        out.kind = ValueKind::Float;
        out.float_val = float_result;
        return true;
    }

    return false;
}

// --- raw (pre-validation) accumulation for one [[indicators]] entry ---

struct RawOperand {
    bool present{false};
    ParsedValue value{};
};

struct RawIndicatorEntry {
    std::string_view id;
    bool has_id{false};
    std::string_view op_name;
    bool has_op{false};
    RawOperand operands[kMaxOperandsPerNode]{};
    bool has_param{false};
    ParsedValue param_value{};
};

template <std::size_t N>
bool copy_into_buffer(std::string_view src, char (&dst)[N]) noexcept {
    if (src.size() >= N) return false;  // must leave room for the NUL terminator
    std::memcpy(dst, src.data(), src.size());
    dst[src.size()] = '\0';
    return true;
}

}  // namespace

SpecLoadResult load_strategy_spec(std::string_view toml_text) noexcept {
    SpecLoadResult result{};

    // --- Phase 1: tokenize into lines, walk sections, accumulate raw fields. ---
    bool have_spec_version = false;
    std::int64_t spec_version = 0;
    std::string_view name_raw;
    bool have_name = false;

    bool have_market_table = false;
    std::string_view market_raw, symbol_raw, timeframe_raw;
    bool have_market_field = false, have_symbol_field = false, have_timeframe_field = false;

    bool have_signal_table = false;
    std::string_view signal_node_raw, signal_mode_raw;
    bool have_signal_node = false, have_signal_mode = false;

    bool have_validation_table = false;
    struct { std::string_view validated_at, validated_by, data_start_utc, data_end_utc, data_digest; } val_str{};
    struct { bool validated_at{false}, validated_by{false}, data_start_utc{false}, data_end_utc{false},
              data_digest{false}, oos_sharpe{false}, pbo{false}, trials{false}; } val_have{};
    double val_oos_sharpe = 0.0, val_pbo = 0.0;
    std::int64_t val_trials = 0;

    std::vector<RawIndicatorEntry> raw_nodes;

    enum class Section { TopLevel, Market, Signal, Validation, Indicator } section = Section::TopLevel;

    std::size_t pos = 0;
    while (pos <= toml_text.size()) {
        const std::size_t nl = toml_text.find('\n', pos);
        const std::string_view line =
            (nl == std::string_view::npos) ? toml_text.substr(pos) : toml_text.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? toml_text.size() + 1 : nl + 1;

        const ParsedLine pl = classify_line(line);
        switch (pl.kind) {
            case LineKind::Blank:
                continue;
            case LineKind::Invalid:
                result.status = SpecLoadStatus::TomlParseError;
                return result;
            case LineKind::Table: {
                if (pl.table_name == "market") {
                    section = Section::Market;
                    have_market_table = true;
                } else if (pl.table_name == "signal") {
                    section = Section::Signal;
                    have_signal_table = true;
                } else if (pl.table_name == "validation") {
                    section = Section::Validation;
                    have_validation_table = true;
                } else {
                    result.status = SpecLoadStatus::TomlParseError;
                    return result;
                }
                continue;
            }
            case LineKind::ArrayTable: {
                if (pl.table_name != "indicators") {
                    result.status = SpecLoadStatus::TomlParseError;
                    return result;
                }
                if (raw_nodes.size() >= kMaxIndicatorNodes) {
                    result.status = SpecLoadStatus::TooManyIndicatorNodes;
                    return result;
                }
                raw_nodes.emplace_back();
                section = Section::Indicator;
                continue;
            }
            case LineKind::KeyValue:
                break;  // handled below
        }

        ParsedValue val{};
        if (!parse_value(pl.raw_value, val)) {
            result.status = SpecLoadStatus::TomlParseError;
            return result;
        }

        switch (section) {
            case Section::TopLevel: {
                if (pl.key == "spec_version") {
                    if (val.kind != ValueKind::Integer) {
                        result.status = SpecLoadStatus::UnsupportedSpecVersion;
                        return result;
                    }
                    spec_version = val.int_val;
                    have_spec_version = true;
                } else if (pl.key == "name") {
                    if (val.kind != ValueKind::String) {
                        result.status = SpecLoadStatus::MissingRequiredField;
                        return result;
                    }
                    name_raw = val.str_val;
                    have_name = true;
                }
                // "description" and any other top-level key: accepted, not stored (5a
                // doesn't need description for anything the C++ side does).
                break;
            }
            case Section::Market: {
                if (pl.key == "market" && val.kind == ValueKind::String) {
                    market_raw = val.str_val;
                    have_market_field = true;
                } else if (pl.key == "symbol" && val.kind == ValueKind::String) {
                    symbol_raw = val.str_val;
                    have_symbol_field = true;
                } else if (pl.key == "timeframe" && val.kind == ValueKind::String) {
                    timeframe_raw = val.str_val;
                    have_timeframe_field = true;
                }
                break;
            }
            case Section::Signal: {
                if (pl.key == "node" && val.kind == ValueKind::String) {
                    signal_node_raw = val.str_val;
                    have_signal_node = true;
                } else if (pl.key == "mode" && val.kind == ValueKind::String) {
                    signal_mode_raw = val.str_val;
                    have_signal_mode = true;
                }
                break;
            }
            case Section::Validation: {
                if (pl.key == "validated_at" && val.kind == ValueKind::String) {
                    val_str.validated_at = val.str_val;
                    val_have.validated_at = true;
                } else if (pl.key == "validated_by" && val.kind == ValueKind::String) {
                    val_str.validated_by = val.str_val;
                    val_have.validated_by = true;
                } else if (pl.key == "data_start_utc" && val.kind == ValueKind::String) {
                    val_str.data_start_utc = val.str_val;
                    val_have.data_start_utc = true;
                } else if (pl.key == "data_end_utc" && val.kind == ValueKind::String) {
                    val_str.data_end_utc = val.str_val;
                    val_have.data_end_utc = true;
                } else if (pl.key == "data_digest" && val.kind == ValueKind::String) {
                    val_str.data_digest = val.str_val;
                    val_have.data_digest = true;
                } else if (pl.key == "oos_sharpe" &&
                           (val.kind == ValueKind::Float || val.kind == ValueKind::Integer)) {
                    val_oos_sharpe = val.kind == ValueKind::Float ? val.float_val
                                                                    : static_cast<double>(val.int_val);
                    val_have.oos_sharpe = true;
                } else if (pl.key == "pbo" &&
                           (val.kind == ValueKind::Float || val.kind == ValueKind::Integer)) {
                    val_pbo = val.kind == ValueKind::Float ? val.float_val : static_cast<double>(val.int_val);
                    val_have.pbo = true;
                } else if (pl.key == "trials" && val.kind == ValueKind::Integer) {
                    val_trials = val.int_val;
                    val_have.trials = true;
                }
                break;
            }
            case Section::Indicator: {
                RawIndicatorEntry& entry = raw_nodes.back();
                if (pl.key == "id" && val.kind == ValueKind::String) {
                    entry.id = val.str_val;
                    entry.has_id = true;
                } else if (pl.key == "op" && val.kind == ValueKind::String) {
                    entry.op_name = val.str_val;
                    entry.has_op = true;
                } else if (pl.key == "window" || pl.key == "n") {
                    if (val.kind != ValueKind::Integer) {
                        result.status = SpecLoadStatus::InvalidOperatorParameter;
                        return result;
                    }
                    entry.param_value = val;
                    entry.has_param = true;
                } else {
                    // Try each of the up-to-3 operand field name slots for this entry's op
                    // (input/left/right/cond/then/otherwise) -- op isn't known yet if "op ="
                    // hasn't been seen; defer matching to Phase 2 by storing every remaining
                    // key=value pair positionally isn't possible without knowing field names,
                    // so operand lines are matched by literal field name directly here.
                    static constexpr std::string_view kOperandFieldNames[] = {
                        "input", "left", "right", "cond", "then", "otherwise"};
                    bool matched = false;
                    for (std::string_view field_name : kOperandFieldNames) {
                        if (pl.key != field_name) continue;
                        // Slot index is resolved against the op once it's known (Phase 2);
                        // here we just need A slot to stash the raw value into. Since no op
                        // uses more than one of these names as more than one field, and no
                        // entry legitimately repeats a field name, slot 0 vs 1 vs 2 is
                        // re-derived from field_name identity in Phase 2 rather than from
                        // insertion order.
                        matched = true;
                        if (field_name == "input" || field_name == "cond") {
                            entry.operands[0] = {true, val};
                        } else if (field_name == "left" || field_name == "then") {
                            entry.operands[field_name == "left" ? 0 : 1] = {true, val};
                        } else if (field_name == "right" || field_name == "otherwise") {
                            entry.operands[field_name == "right" ? 1 : 2] = {true, val};
                        }
                        break;
                    }
                    if (!matched) {
                        result.status = SpecLoadStatus::TomlParseError;
                        return result;
                    }
                }
                break;
            }
        }
    }

    // --- Phase 2: validate + assemble ---

    if (!have_spec_version || spec_version != 1) {
        result.status = SpecLoadStatus::UnsupportedSpecVersion;
        return result;
    }
    if (!have_name || !is_valid_name(name_raw)) {
        result.status = SpecLoadStatus::MissingRequiredField;
        return result;
    }

    // [market]
    if (!have_market_table || !have_market_field || !have_symbol_field || !have_timeframe_field) {
        result.status = SpecLoadStatus::MissingMarketBinding;
        return result;
    }
    const auto market_enum = market_from_name(market_raw);
    if (!market_enum) {
        result.status = SpecLoadStatus::MissingMarketBinding;
        return result;
    }
    // CanonicalMarketSymbol normalizes to uppercase, non-empty, no whitespace.
    if (symbol_raw.empty()) {
        result.status = SpecLoadStatus::MissingMarketBinding;
        return result;
    }
    char symbol_buf[kMaxSymbolLen + 1]{};
    for (std::size_t i = 0; i < symbol_raw.size(); ++i) {
        if (symbol_raw[i] == ' ' || symbol_raw[i] == '\t') {
            result.status = SpecLoadStatus::MissingMarketBinding;
            return result;
        }
    }
    {
        char upper_buf[kMaxSymbolLen + 1]{};
        if (symbol_raw.size() >= sizeof(upper_buf)) {
            result.status = SpecLoadStatus::MissingMarketBinding;
            return result;
        }
        for (std::size_t i = 0; i < symbol_raw.size(); ++i) {
            char c = symbol_raw[i];
            upper_buf[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
        }
        upper_buf[symbol_raw.size()] = '\0';
        std::memcpy(symbol_buf, upper_buf, sizeof(upper_buf));
    }
    char timeframe_buf[kMaxTimeframeLen + 1]{};
    {
        if (timeframe_raw.size() >= sizeof(timeframe_buf)) {
            result.status = SpecLoadStatus::MissingMarketBinding;
            return result;
        }
        char lower_buf[kMaxTimeframeLen + 1]{};
        for (std::size_t i = 0; i < timeframe_raw.size(); ++i) lower_buf[i] = to_lower_ascii(timeframe_raw[i]);
        lower_buf[timeframe_raw.size()] = '\0';
        if (!is_valid_timeframe(std::string_view(lower_buf, timeframe_raw.size()))) {
            result.status = SpecLoadStatus::MissingMarketBinding;
            return result;
        }
        std::memcpy(timeframe_buf, lower_buf, sizeof(lower_buf));
    }

    // [[indicators]]
    if (raw_nodes.empty()) {
        result.status = SpecLoadStatus::MissingRequiredField;
        return result;
    }

    SpecDag dag{};
    for (std::size_t i = 0; i < raw_nodes.size(); ++i) {
        const RawIndicatorEntry& raw = raw_nodes[i];
        if (!raw.has_id || !is_valid_id(raw.id)) {
            result.status = SpecLoadStatus::MissingRequiredField;
            return result;
        }
        if (raw_field_from_name(raw.id)) {
            result.status = SpecLoadStatus::DuplicateNodeId;  // id collides with a raw field name
            return result;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (std::string_view(dag.nodes[j].id) == raw.id) {
                result.status = SpecLoadStatus::DuplicateNodeId;
                return result;
            }
        }
        if (!raw.has_op) {
            result.status = SpecLoadStatus::MissingRequiredField;
            return result;
        }
        const auto op = op_from_name(raw.op_name);
        if (!op) {
            result.status = SpecLoadStatus::UnknownOperator;
            return result;
        }

        SpecNode node{};
        if (!copy_into_buffer(raw.id, node.id)) {
            result.status = SpecLoadStatus::MissingRequiredField;
            return result;
        }
        node.op = *op;

        const std::uint8_t arity = operand_arity(*op);
        node.operand_count = arity;
        for (std::uint8_t slot = 0; slot < arity; ++slot) {
            const std::string_view field_name = operand_field_name(*op, slot);
            // Re-derive which raw slot this field name landed in during Phase 1 (see the
            // insertion logic above: input/cond -> 0, left/then -> the passed slot, etc).
            std::uint8_t raw_slot = slot;
            if (field_name == "then") raw_slot = 1;
            if (field_name == "otherwise") raw_slot = 2;
            if (field_name == "input" || field_name == "cond") raw_slot = 0;
            if (field_name == "left") raw_slot = 0;
            if (field_name == "right") raw_slot = 1;

            const RawOperand& raw_operand = raw.operands[raw_slot];
            if (!raw_operand.present) {
                result.status = SpecLoadStatus::MissingRequiredField;
                return result;
            }
            Operand operand{};
            if (raw_operand.value.kind == ValueKind::String) {
                std::string_view ref = raw_operand.value.str_val;
                if (const auto rf = raw_field_from_name(ref)) {
                    operand.kind = OperandKind::RawFieldRef;
                    operand.raw_field_idx = static_cast<std::uint8_t>(*rf);
                } else {
                    // Must resolve to an EARLIER node (forward-reference-only, §2.2).
                    std::optional<std::uint16_t> found;
                    for (std::size_t j = 0; j < i; ++j) {
                        if (std::string_view(dag.nodes[j].id) == ref) {
                            found = static_cast<std::uint16_t>(j);
                            break;
                        }
                    }
                    if (!found) {
                        result.status = SpecLoadStatus::ForwardReferenceViolation;
                        return result;
                    }
                    operand.kind = OperandKind::NodeRef;
                    operand.node_idx = *found;
                }
            } else {
                // Numeric literal.
                if (operand_must_be_reference(*op, slot)) {
                    result.status = SpecLoadStatus::InvalidOperatorParameter;
                    return result;
                }
                operand.kind = OperandKind::Literal;
                operand.literal_value = raw_operand.value.kind == ValueKind::Float
                                             ? raw_operand.value.float_val
                                             : static_cast<double>(raw_operand.value.int_val);
            }
            node.operands[slot] = operand;
        }

        if (op_has_param(*op)) {
            if (!raw.has_param) {
                result.status = SpecLoadStatus::MissingRequiredField;
                return result;
            }
            const std::int64_t param_val = raw.param_value.int_val;
            if (param_val < min_param_value(*op) || param_val > 0x7fffffff) {
                result.status = SpecLoadStatus::InvalidOperatorParameter;
                return result;
            }
            node.param = static_cast<std::int32_t>(param_val);
        } else {
            node.param = -1;
        }

        dag.nodes[i] = node;
    }
    dag.node_count = raw_nodes.size();

    // [signal]
    if (!have_signal_table || !have_signal_node || !have_signal_mode) {
        result.status = SpecLoadStatus::MissingRequiredField;
        return result;
    }
    std::optional<std::uint16_t> signal_idx;
    for (std::size_t j = 0; j < dag.node_count; ++j) {
        if (std::string_view(dag.nodes[j].id) == signal_node_raw) {
            signal_idx = static_cast<std::uint16_t>(j);
            break;
        }
    }
    if (!signal_idx) {
        result.status = SpecLoadStatus::MissingRequiredField;
        return result;
    }
    dag.signal_node_idx = *signal_idx;
    if (signal_mode_raw == "boolean") {
        dag.signal_mode = SignalMode::Boolean;
    } else if (signal_mode_raw == "scaled") {
        dag.signal_mode = SignalMode::Scaled;
    } else {
        result.status = SpecLoadStatus::MissingRequiredField;
        return result;
    }

    // [validation] -- must be complete, per this loader's stricter-than-Python contract.
    if (!have_validation_table) {
        result.status = SpecLoadStatus::MissingValidationSection;
        return result;
    }
    if (!val_have.validated_at || !val_have.validated_by || !val_have.data_start_utc ||
        !val_have.data_end_utc || !val_have.data_digest || !val_have.oos_sharpe || !val_have.pbo ||
        !val_have.trials) {
        result.status = SpecLoadStatus::IncompleteValidationSection;
        return result;
    }

    ValidationRecord validation{};
    validation.present = true;
    if (!copy_into_buffer(val_str.validated_at, validation.validated_at) ||
        !copy_into_buffer(val_str.validated_by, validation.validated_by) ||
        !copy_into_buffer(val_str.data_start_utc, validation.data_start_utc) ||
        !copy_into_buffer(val_str.data_end_utc, validation.data_end_utc) ||
        !copy_into_buffer(val_str.data_digest, validation.data_digest)) {
        result.status = SpecLoadStatus::IncompleteValidationSection;
        return result;
    }
    validation.oos_sharpe = val_oos_sharpe;
    validation.pbo = val_pbo;
    validation.trials = val_trials;

    MarketBinding market{};
    market.market = *market_enum;
    std::memcpy(market.symbol, symbol_buf, sizeof(symbol_buf));
    std::memcpy(market.timeframe, timeframe_buf, sizeof(timeframe_buf));

    result.status = SpecLoadStatus::Ok;
    result.dag = dag;
    result.market = market;
    result.validation = validation;
    return result;
}

bool market_binding_matches(const MarketBinding& binding, ManualMarket market, std::string_view symbol,
                             std::string_view timeframe) noexcept {
    if (binding.market != market) return false;
    if (std::string_view(binding.symbol) != symbol) return false;
    if (std::string_view(binding.timeframe) != timeframe) return false;
    return true;
}

}  // namespace hy
