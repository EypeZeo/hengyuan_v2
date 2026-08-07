// SPDX-License-Identifier: proprietary
// binance_depth_snapshot_codec.hpp — Pure JSON parsing/validation for Binance depth snapshot
// responses (P2-MD-02 / Track C).
//
// Deliberately zero Boost.Asio/Beast/OpenSSL dependency: this is the fail-closed validation
// logic extracted out of binance_rest_snapshot.hpp so it can be unit-tested against synthetic
// JSON strings under the default HY_BUILD_TESTS build, without needing HY_BUILD_DEMO's
// Boost/OpenSSL dependency. binance_rest_snapshot.hpp (the REST transport layer) is the only
// caller; it maps DepthSnapshotParseError into its own FetchError -- that mapping intentionally
// lives there, not here, so this header never needs to know FetchError exists.
//
// Validation is fail-closed: any single malformed/inconsistent level aborts the whole parse.
// The previous per-level "silently skip and continue" behavior could hand back a "successful"
// snapshot that was quietly missing real depth -- exactly the kind of silent partial-success
// this module exists to rule out.

#pragma once

#include <hengyuan/binance_json_parser.hpp>
#include <hengyuan/depth_manager.hpp>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#endif
#include <simdjson.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstdint>
#include <string_view>
#include <variant>

namespace hy {

enum class DepthSnapshotParseError {
    InvalidLastUpdateId,  // missing, wrong type, or == 0
    MalformedLevel,       // a level isn't a valid [price_string, qty_string] pair, or either
                           // string fails to parse as a decimal
    NonPositiveQty,        // qty <= 0
    TooManyLevels,         // more levels than DepthSnapshot's fixed 1024-per-side capacity
    OrderingViolation,     // bids not strictly descending, asks not strictly ascending, or
                            // best_bid >= best_ask
    EmptySide,             // bids or asks is empty after parsing
};

namespace detail {

inline bool parse_side(simdjson::ondemand::array& levels, PriceLevel* out, std::size_t& count,
                        std::int64_t price_multiplier, std::int64_t qty_multiplier,
                        DepthSnapshotParseError& err) {
    for (auto level_result : levels) {
        if (count >= 1024) {
            err = DepthSnapshotParseError::TooManyLevels;
            return false;
        }
        simdjson::ondemand::array pair;
        if (level_result.get_array().get(pair) != simdjson::SUCCESS) {
            err = DepthSnapshotParseError::MalformedLevel;
            return false;
        }
        auto it = pair.begin();
        if (it == pair.end()) {
            err = DepthSnapshotParseError::MalformedLevel;
            return false;
        }
        std::string_view price_str;
        if ((*it).get_string().get(price_str) != simdjson::SUCCESS) {
            err = DepthSnapshotParseError::MalformedLevel;
            return false;
        }
        ++it;
        if (it == pair.end()) {
            err = DepthSnapshotParseError::MalformedLevel;
            return false;
        }
        std::string_view qty_str;
        if ((*it).get_string().get(qty_str) != simdjson::SUCCESS) {
            err = DepthSnapshotParseError::MalformedLevel;
            return false;
        }

        auto price = BinanceJsonParser::parse_decimal_to_fixed(price_str, price_multiplier);
        auto qty = BinanceJsonParser::parse_decimal_to_fixed(qty_str, qty_multiplier);
        if (!price || !qty) {
            err = DepthSnapshotParseError::MalformedLevel;
            return false;
        }
        if (*qty <= 0) {
            err = DepthSnapshotParseError::NonPositiveQty;
            return false;
        }

        out[count] = PriceLevel{*price, *qty};
        ++count;
    }
    return true;
}

}  // namespace detail

inline std::variant<DepthSnapshot, DepthSnapshotParseError> parse_depth_response(
    std::string_view body, std::int64_t price_multiplier, std::int64_t qty_multiplier) {
    auto padded = simdjson::padded_string(body);
    simdjson::ondemand::parser json_parser;
    simdjson::ondemand::document doc;
    if (json_parser.iterate(padded).get(doc)) {
        return DepthSnapshotParseError::MalformedLevel;
    }

    DepthSnapshot snap;

    std::uint64_t last_update_id = 0;
    if (doc["lastUpdateId"].get_uint64().get(last_update_id) || last_update_id == 0) {
        return DepthSnapshotParseError::InvalidLastUpdateId;
    }
    snap.last_update_id = last_update_id;

    DepthSnapshotParseError err{};

    simdjson::ondemand::array bids_arr;
    if (doc["bids"].get_array().get(bids_arr) != simdjson::SUCCESS) {
        return DepthSnapshotParseError::MalformedLevel;
    }
    if (!detail::parse_side(bids_arr, snap.bids, snap.bid_count, price_multiplier, qty_multiplier,
                             err)) {
        return err;
    }

    simdjson::ondemand::array asks_arr;
    if (doc["asks"].get_array().get(asks_arr) != simdjson::SUCCESS) {
        return DepthSnapshotParseError::MalformedLevel;
    }
    if (!detail::parse_side(asks_arr, snap.asks, snap.ask_count, price_multiplier, qty_multiplier,
                             err)) {
        return err;
    }

    if (snap.bid_count == 0 || snap.ask_count == 0) {
        return DepthSnapshotParseError::EmptySide;
    }

    // bids must be strictly descending, asks strictly ascending (Binance's own documented
    // ordering); best_bid must be < best_ask. Any violation means the response is either
    // malformed or internally inconsistent -- fail closed rather than hand back a book that
    // silently isn't actually sorted/crossed correctly.
    for (std::size_t i = 1; i < snap.bid_count; ++i) {
        if (snap.bids[i].price_ticks >= snap.bids[i - 1].price_ticks) {
            return DepthSnapshotParseError::OrderingViolation;
        }
    }
    for (std::size_t i = 1; i < snap.ask_count; ++i) {
        if (snap.asks[i].price_ticks <= snap.asks[i - 1].price_ticks) {
            return DepthSnapshotParseError::OrderingViolation;
        }
    }
    if (snap.bids[0].price_ticks >= snap.asks[0].price_ticks) {
        return DepthSnapshotParseError::OrderingViolation;
    }

    return snap;
}

}  // namespace hy
