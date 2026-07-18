// SPDX-License-Identifier: proprietary
#include <hengyuan/binance_json_parser.hpp>
#include <algorithm>
#include <cstring>
#include <limits>

namespace hy {

void BinanceJsonParser::register_symbol(std::string_view binance_symbol,
                                         std::uint32_t symbol_id,
                                         std::int64_t price_mult,
                                         std::int64_t qty_mult) noexcept {
    if (symbol_count_ >= kMaxSymbols) return;
    auto& entry = symbols_[symbol_count_];
    std::size_t len = (std::min)(binance_symbol.size(), sizeof(entry.name) - 1);
    std::memcpy(entry.name, binance_symbol.data(), len);
    entry.name[len] = '\0';
    entry.name_len = len;
    entry.config = {symbol_id, price_mult, qty_mult};
    ++symbol_count_;
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

    bool negative = false;
    std::size_t pos = 0;
    if (s[0] == '-') {
        negative = true;
        pos = 1;
    }

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
            frac_value = frac_value * 10 + (s[pos] - '0');
            frac_divisor *= 10;
            ++pos;
        }
    }

    // result = (integer_part + frac_value/frac_divisor) * multiplier
    // To avoid double: integer_part * multiplier + frac_value * (multiplier / frac_divisor)
    if (integer_part > std::numeric_limits<std::int64_t>::max() / multiplier)
        return std::nullopt;
    std::int64_t int_contrib = integer_part * multiplier;

    std::int64_t frac_contrib = 0;
    if (frac_divisor > 0 && frac_value > 0) {
        if (multiplier >= frac_divisor) {
            frac_contrib = frac_value * (multiplier / frac_divisor);
        } else {
            frac_contrib = frac_value / (frac_divisor / multiplier);
        }
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

    auto padded = simdjson::padded_string(json_bytes);
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

    uint64_t event_time = 0;
    [[maybe_unused]] auto e1 = doc["E"].get_uint64().get(event_time);

    if (event_type == "trade") {
        auto& out = out_events[0];
        std::memset(&out, 0, sizeof(out));
        out.symbol_id = sym_cfg->symbol_id;
        out.ts_recv_ns = recv_ns;
        out.ts_event_ms = event_time;
        out.type = EventType::Trade;

        uint64_t trade_id = 0;
        [[maybe_unused]] auto e2 = doc["t"].get_uint64().get(trade_id);
        out.event_id = trade_id;

        std::string_view price_str, qty_str;
        if (doc["p"].get_string().get(price_str) || doc["q"].get_string().get(qty_str)) {
            ++counters_.malformed;
            return ParseResult::MalformedJson;
        }

        auto price = parse_decimal_to_fixed(price_str, sym_cfg->price_multiplier);
        if (!price) { ++counters_.price_overflow; return ParseResult::PriceOverflow; }
        out.price_ticks = *price;

        auto qty = parse_decimal_to_fixed(qty_str, sym_cfg->qty_multiplier);
        if (!qty) { ++counters_.qty_overflow; return ParseResult::QtyOverflow; }
        out.qty_lots = *qty;

        bool buyer_is_maker = false;
        [[maybe_unused]] auto e3 = doc["m"].get_bool().get(buyer_is_maker);
        out.side = buyer_is_maker ? Side::Sell : Side::Buy;

        out_count = 1;

    } else if (event_type == "aggTrade") {
        auto& out = out_events[0];
        std::memset(&out, 0, sizeof(out));
        out.symbol_id = sym_cfg->symbol_id;
        out.ts_recv_ns = recv_ns;
        out.ts_event_ms = event_time;
        out.type = EventType::AggTrade;

        uint64_t agg_id = 0;
        [[maybe_unused]] auto e4 = doc["a"].get_uint64().get(agg_id);
        out.event_id = agg_id;

        std::string_view price_str, qty_str;
        if (doc["p"].get_string().get(price_str) || doc["q"].get_string().get(qty_str)) {
            ++counters_.malformed;
            return ParseResult::MalformedJson;
        }

        auto price = parse_decimal_to_fixed(price_str, sym_cfg->price_multiplier);
        if (!price) { ++counters_.price_overflow; return ParseResult::PriceOverflow; }
        out.price_ticks = *price;

        auto qty = parse_decimal_to_fixed(qty_str, sym_cfg->qty_multiplier);
        if (!qty) { ++counters_.qty_overflow; return ParseResult::QtyOverflow; }
        out.qty_lots = *qty;

        bool buyer_is_maker = false;
        [[maybe_unused]] auto e5 = doc["m"].get_bool().get(buyer_is_maker);
        out.side = buyer_is_maker ? Side::Sell : Side::Buy;

        out_count = 1;

    } else if (event_type == "depthUpdate") {
        uint64_t first_update_id = 0;
        [[maybe_unused]] auto e6a = doc["U"].get_uint64().get(first_update_id);
        uint64_t final_update_id = 0;
        [[maybe_unused]] auto e6b = doc["u"].get_uint64().get(final_update_id);

        std::size_t n = 0;

        // Parse bids ("b" array): each element is ["price", "qty"]
        simdjson::ondemand::array bids_arr;
        if (doc["b"].get_array().get(bids_arr) == simdjson::SUCCESS) {
            for (auto level_result : bids_arr) {
                if (n >= max_events) break;
                simdjson::ondemand::array pair;
                if (level_result.get_array().get(pair) != simdjson::SUCCESS) continue;
                auto it = pair.begin();
                if (it == pair.end()) continue;
                std::string_view price_str;
                if ((*it).get_string().get(price_str) != simdjson::SUCCESS) continue;
                ++it;
                if (it == pair.end()) continue;
                std::string_view qty_str;
                if ((*it).get_string().get(qty_str) != simdjson::SUCCESS) continue;

                auto price = parse_decimal_to_fixed(price_str, sym_cfg->price_multiplier);
                auto qty = parse_decimal_to_fixed(qty_str, sym_cfg->qty_multiplier);
                if (!price || !qty) continue;

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
                ev.side = Side::Buy;  // bid side
                ++n;
            }
        }

        // Parse asks ("a" array)
        simdjson::ondemand::array asks_arr;
        if (doc["a"].get_array().get(asks_arr) == simdjson::SUCCESS) {
            for (auto level_result : asks_arr) {
                if (n >= max_events) break;
                simdjson::ondemand::array pair;
                if (level_result.get_array().get(pair) != simdjson::SUCCESS) continue;
                auto it = pair.begin();
                if (it == pair.end()) continue;
                std::string_view price_str;
                if ((*it).get_string().get(price_str) != simdjson::SUCCESS) continue;
                ++it;
                if (it == pair.end()) continue;
                std::string_view qty_str;
                if ((*it).get_string().get(qty_str) != simdjson::SUCCESS) continue;

                auto price = parse_decimal_to_fixed(price_str, sym_cfg->price_multiplier);
                auto qty = parse_decimal_to_fixed(qty_str, sym_cfg->qty_multiplier);
                if (!price || !qty) continue;

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
                ev.side = Side::Sell;  // ask side
                ++n;
            }
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
