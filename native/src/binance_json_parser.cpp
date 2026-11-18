// SPDX-License-Identifier: proprietary
#include <hengyuan/binance_json_parser.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace hy {

namespace {

std::int64_t scale_decimal_fraction(std::int64_t fraction_digits, std::int64_t divisor,
                                    std::int64_t multiplier) noexcept {
    if (multiplier % divisor == 0) return fraction_digits * (multiplier / divisor);
    if (divisor % multiplier == 0) return fraction_digits / (divisor / multiplier);

    // Work from the least significant decimal digit. Splitting the multiplier
    // by ten keeps the intermediate sum in range and the carry below multiplier.
    const auto scale = static_cast<std::uint64_t>(multiplier);
    const auto scale_quotient = scale / 10;
    const auto scale_remainder = scale % 10;
    std::uint64_t carry = 0;
    for (; divisor > 1; divisor /= 10) {
        const auto digit = static_cast<std::uint64_t>(fraction_digits % 10);
        carry = scale_quotient * digit + (scale_remainder * digit + carry) / 10;
        fraction_digits /= 10;
    }
    return static_cast<std::int64_t>(carry);
}

}  // namespace

BinanceJsonParser::BinanceJsonParser()
    : padded_buf_(kWsReadMessageMaxBytes + simdjson::SIMDJSON_PADDING, '\0') {
    // allocate() sizes everything an on-demand parser ever needs for a document of this length (string buffer, structural
    // indexes, depth stacks); iterate() only re-allocates for a LONGER one, and parse() refuses those before they get there.
    if (parser_.allocate(kWsReadMessageMaxBytes) != simdjson::SUCCESS) throw std::bad_alloc();
}

bool BinanceJsonParser::register_symbol(std::string_view binance_symbol,
                                         std::uint32_t symbol_id,
                                         std::int64_t price_mult,
                                         std::int64_t qty_mult) noexcept {
    if (symbol_count_ >= kMaxSymbols) return false;
    // Audit API-SYM-020: an id past the per-symbol table bound would silently be
    // served symbol 0's order book downstream. Reject at the only point that knows.
    if (symbol_id > kMaxSymbolId) return false;
    // Silently truncating a name would make two different symbols collide in
    // find_symbol(); reject instead. Empty names are rejected because find_symbol()
    // matches on length and every incoming "s" field is non-empty.
    if (binance_symbol.empty() || binance_symbol.size() > sizeof(SymbolEntry::name) - 1) {
        return false;
    }
    if (price_mult <= 0 || qty_mult <= 0) return false;  // parse_decimal_to_fixed rejects these anyway

    auto& entry = symbols_[symbol_count_];
    const std::size_t len = binance_symbol.size();
    std::memcpy(entry.name, binance_symbol.data(), len);
    entry.name[len] = '\0';
    entry.name_len = len;
    entry.config = {symbol_id, price_mult, qty_mult};
    ++symbol_count_;
    return true;
}

const SymbolConfig* BinanceJsonParser::find_symbol(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < symbol_count_; ++i) {
        if (symbols_[i].name_len == name.size() &&
            std::memcmp(symbols_[i].name, name.data(), name.size()) == 0) {
            return &symbols_[i].config;
        }
    }
    return nullptr;
}

std::optional<std::int64_t> BinanceJsonParser::parse_decimal_to_fixed(
    std::string_view s, std::int64_t multiplier) noexcept {
    if (s.empty()) return std::nullopt;
    if (multiplier <= 0) return std::nullopt;  // guards the later max/multiplier division

    bool negative = false;
    std::size_t pos = 0;
    if (s[0] == '-') {
        negative = true;
        pos = 1;
    }
    if (pos == s.size() || s.substr(pos) == ".") return std::nullopt;

    std::int64_t integer_part = 0;
    while (pos < s.size() && s[pos] != '.') {
        if (s[pos] < '0' || s[pos] > '9') return std::nullopt;
        std::int64_t digit = s[pos] - '0';
        if (integer_part > (std::numeric_limits<std::int64_t>::max() - digit) / 10)
            return std::nullopt;
        integer_part = integer_part * 10 + digit;
        ++pos;
    }

    std::int64_t frac_value = 0;
    std::int64_t frac_divisor = 1;
    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        while (pos < s.size()) {
            if (s[pos] < '0' || s[pos] > '9') return std::nullopt;
            std::int64_t digit = s[pos] - '0';
            // Same overflow-guard pattern as the integer part above -- a string with enough
            // fractional digits (roughly 19+) would otherwise overflow these into signed UB.
            if (frac_value > (std::numeric_limits<std::int64_t>::max() - digit) / 10)
                return std::nullopt;
            if (frac_divisor > std::numeric_limits<std::int64_t>::max() / 10) return std::nullopt;
            frac_value = frac_value * 10 + digit;
            frac_divisor *= 10;
            ++pos;
        }
    }

    if (integer_part > std::numeric_limits<std::int64_t>::max() / multiplier)
        return std::nullopt;
    const std::int64_t int_contrib = integer_part * multiplier;

    const std::int64_t frac_contrib =
        frac_value > 0 ? scale_decimal_fraction(frac_value, frac_divisor, multiplier) : 0;

    // Check the scaled fraction against the range left by the scaled integer.
    if (frac_contrib > std::numeric_limits<std::int64_t>::max() - int_contrib) {
        return std::nullopt;
    }
    std::int64_t result = int_contrib + frac_contrib;
    if (negative) result = -result;
    return result;
}

ParseResult BinanceJsonParser::parse(std::string_view json_bytes,
                                      std::uint64_t recv_ns,
                                      BinanceMarketEvent* out_events,
                                      std::size_t max_events,
                                      std::size_t& out_count) noexcept {
    out_count = 0;
    if (max_events == 0) return ParseResult::MalformedJson;

    // Fixed padded buffer, not a fresh simdjson::padded_string per message -- see
    // padded_buf_'s declaration for why (audit PERF-ALLOC-012, Appendix D L-38). A body
    // above the limit cannot come from a session (Beast refuses the message first, at
    // the same number); it is refused here, before the copy, rather than growing the
    // buffer: the steady state AND the first frame are memcpy + memset into storage
    // this object already owns.
    const std::size_t doc_len = json_bytes.size();
    if (doc_len > kWsReadMessageMaxBytes) [[unlikely]] {
        ++counters_.malformed;
        return ParseResult::MalformedJson;
    }
    if (doc_len > 0) {
        std::memcpy(padded_buf_.data(), json_bytes.data(), doc_len);
    }
    std::memset(padded_buf_.data() + doc_len, 0, simdjson::SIMDJSON_PADDING);
    const simdjson::padded_string_view padded(padded_buf_.data(), doc_len, padded_buf_.size());

    simdjson::ondemand::document doc;
    auto err = parser_.iterate(padded).get(doc);
    if (err) {
        ++counters_.malformed;
        return ParseResult::MalformedJson;
    }

    std::string_view event_type;
    auto e_err = doc["e"].get_string().get(event_type);
    if (e_err) {
        err = parser_.iterate(padded).get(doc);
        if (err) {
            ++counters_.malformed;
            return ParseResult::MalformedJson;
        }
        simdjson::ondemand::value id_field;
        if (doc["id"].get(id_field) == simdjson::SUCCESS) {
            ++counters_.ignored;
            return ParseResult::EventIgnored;
        }
        ++counters_.malformed;
        return ParseResult::MalformedJson;
    }

    std::string_view symbol;
    if (doc["s"].get_string().get(symbol)) {
        ++counters_.malformed;
        return ParseResult::MalformedJson;
    }

    const auto* sym_cfg = find_symbol(symbol);
    if (!sym_cfg) {
        ++counters_.unknown_symbol;
        return ParseResult::UnknownSymbol;
    }

    const bool is_trade = event_type == "trade";
    if (is_trade || event_type == "aggTrade") {
        std::uint64_t event_time = 0;
        std::uint64_t trade_id = 0;
        std::string_view price_str, qty_str;
        bool buyer_is_maker = false;
        if (doc["E"].get_uint64().get(event_time)
            || doc[is_trade ? "t" : "a"].get_uint64().get(trade_id)
            || doc["p"].get_string().get(price_str)
            || doc["q"].get_string().get(qty_str)
            || doc["m"].get_bool().get(buyer_is_maker)) {
            ++counters_.malformed;
            return ParseResult::MalformedJson;
        }

        const auto price = parse_decimal_to_fixed(price_str, sym_cfg->price_multiplier);
        if (!price) {
            ++counters_.price_overflow;
            return ParseResult::PriceOverflow;
        }

        const auto qty = parse_decimal_to_fixed(qty_str, sym_cfg->qty_multiplier);
        if (!qty) {
            ++counters_.qty_overflow;
            return ParseResult::QtyOverflow;
        }
        if (*price <= 0 || *qty <= 0) [[unlikely]] {
            ++counters_.malformed;
            return ParseResult::MalformedJson;
        }

        auto& out = out_events[0];
        std::memset(&out, 0, sizeof(out));
        out.event_id = trade_id;
        out.price_ticks = *price;
        out.qty_lots = *qty;
        out.ts_event_ms = event_time;
        out.ts_recv_ns = recv_ns;
        out.symbol_id = sym_cfg->symbol_id;
        out.type = is_trade ? EventType::Trade : EventType::AggTrade;
        out.side = buyer_is_maker ? Side::Sell : Side::Buy;
        out_count = 1;

    } else if (event_type == "depthUpdate") {
        std::uint64_t event_time = 0;
        std::uint64_t first_update_id = 0;
        std::uint64_t final_update_id = 0;
        if (doc["E"].get_uint64().get(event_time)
            || doc["U"].get_uint64().get(first_update_id)
            || doc["u"].get_uint64().get(final_update_id)
            || final_update_id < first_update_id) [[unlikely]] {
            ++counters_.malformed;
            return ParseResult::MalformedJson;
        }

        std::size_t n = 0;
        bool truncated = false;

        const auto parse_levels = [&](const char* key, Side side) {
            simdjson::ondemand::array levels;
            if (doc[key].get_array().get(levels) != simdjson::SUCCESS) return false;

            for (auto level_result : levels) {
                if (n >= max_events) {
                    truncated = true;
                    break;
                }
                simdjson::ondemand::array pair;
                if (level_result.get_array().get(pair) != simdjson::SUCCESS) return false;
                auto it = pair.begin();
                if (it == pair.end()) return false;
                std::string_view price_str;
                if ((*it).get_string().get(price_str) != simdjson::SUCCESS) return false;
                ++it;
                if (it == pair.end()) return false;
                std::string_view qty_str;
                if ((*it).get_string().get(qty_str) != simdjson::SUCCESS) return false;
                ++it;
                if (it != pair.end()) return false;

                const auto price = parse_decimal_to_fixed(price_str, sym_cfg->price_multiplier);
                const auto qty = parse_decimal_to_fixed(qty_str, sym_cfg->qty_multiplier);
                if (!price || !qty || *price <= 0 || *qty < 0) return false;
                // Only an actual zero may remove a level.
                if (*qty == 0 && qty_str.find_first_not_of("-0.") != std::string_view::npos) {
                    return false;
                }

                auto& ev = out_events[n];
                std::memset(&ev, 0, sizeof(ev));
                ev.event_id = final_update_id;
                ev.aux_id = first_update_id;
                ev.price_ticks = *price;
                ev.qty_lots = *qty;
                ev.ts_event_ms = event_time;
                ev.ts_recv_ns = recv_ns;
                ev.symbol_id = sym_cfg->symbol_id;
                ev.type = EventType::DepthDelta;
                ev.side = side;
                ++n;
            }
            return true;
        };

        if (!parse_levels("b", Side::Buy) || (!truncated && !parse_levels("a", Side::Sell))) {
            ++counters_.malformed;
            return ParseResult::MalformedJson;
        }

        // AUDIT MD-TRUNC-015: a depth delta is explicitly NOT conflatable
        // (binance_market_event.hpp). Handing back the levels that happened to fit
        // and returning Ok diverged the local book from the exchange permanently,
        // with no counter, no flag and no way for the caller to notice -- and the
        // bids loop runs first, so a large enough message could consume every slot
        // and leave the asks side entirely unrepresented.
        //
        // Discard the partial delta and emit a single synthetic resync marker
        // instead. event_flag::kResyncRequired is what InputValidator turns into
        // ValidationResult::ResyncRequired, which drives DepthManager back to
        // Buffering and a fresh snapshot -- the correct fail-closed response to
        // "this update could not be represented in full".
        if (truncated) {
            ++counters_.truncated_resync;
            auto& ev = out_events[0];
            std::memset(&ev, 0, sizeof(ev));
            ev.event_id = final_update_id;
            ev.aux_id = first_update_id;
            ev.ts_event_ms = event_time;
            ev.ts_recv_ns = recv_ns;
            ev.symbol_id = sym_cfg->symbol_id;
            ev.type = EventType::DepthDelta;
            ev.flags = event_flag::kResyncRequired;
            out_count = 1;
            return ParseResult::TruncatedResync;
        }

        out_count = n;
        if (n == 0) {
            ++counters_.malformed;
            return ParseResult::MalformedJson;
        }

    } else {
        ++counters_.unknown_event;
        return ParseResult::UnknownEventType;
    }

    ++counters_.parsed_ok;
    return ParseResult::Ok;
}

}  // namespace hy
