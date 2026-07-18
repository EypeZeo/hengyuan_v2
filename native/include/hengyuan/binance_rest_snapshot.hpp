// SPDX-License-Identifier: proprietary
// binance_rest_snapshot.hpp — Synchronous Binance REST depth snapshot fetcher.
//
// GET https://api.binance.com/api/v3/depth?symbol=BTCUSDT&limit=1000
// Parses the JSON response into a DepthSnapshot for DepthManager.
//
// Governance: L4 (real Binance public REST API, no token/HMAC/Private API).
// CI compiles but does not make real HTTP calls. VPS manual verification = L4.

#pragma once

#include <hengyuan/binance_json_parser.hpp>
#include <hengyuan/depth_manager.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#endif
#include <simdjson.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstdint>
#include <optional>
#include <string>

namespace hy {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

struct RestSnapshotConfig {
    std::string host = "api.binance.com";
    std::string port = "443";
    int limit = 1000;  // depth levels (max 5000)
};

// Synchronous (blocking) depth snapshot fetch. Call from a non-hot thread
// (e.g. main thread before the hot loop, or a dedicated snapshot thread).
// Returns nullopt on any error (DNS, connect, HTTP, parse).
inline std::optional<DepthSnapshot> fetch_depth_snapshot(
    const std::string& symbol,
    std::int64_t price_multiplier,
    std::int64_t qty_multiplier,
    const RestSnapshotConfig& cfg = {}) {

    try {
        net::io_context ioc;
        ssl::context ssl_ctx(ssl::context::tlsv12_client);
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(ssl::verify_peer);

        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
        if (!SSL_set_tlsext_host_name(stream.native_handle(), cfg.host.c_str())) {
            return std::nullopt;
        }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

        auto results = resolver.resolve(cfg.host, cfg.port);
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);

        std::string target = "/api/v3/depth?symbol=" + symbol +
                             "&limit=" + std::to_string(cfg.limit);

        http::request<http::empty_body> req{http::verb::get, target, 11};
        req.set(http::field::host, cfg.host);
        req.set(http::field::user_agent, "HengYuan/0.1");
        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        if (res.result() != http::status::ok) {
            return std::nullopt;
        }

        beast::error_code ec;
        stream.shutdown(ec);
        // Ignore shutdown errors (common with short-lived connections)

        // Parse JSON response
        const std::string& body = res.body();
        auto padded = simdjson::padded_string(body);
        simdjson::ondemand::parser json_parser;
        simdjson::ondemand::document doc;
        if (json_parser.iterate(padded).get(doc)) {
            return std::nullopt;
        }

        DepthSnapshot snap;

        uint64_t last_update_id = 0;
        if (doc["lastUpdateId"].get_uint64().get(last_update_id)) {
            return std::nullopt;
        }
        snap.last_update_id = last_update_id;

        // Parse bids
        simdjson::ondemand::array bids_arr;
        if (doc["bids"].get_array().get(bids_arr) == simdjson::SUCCESS) {
            for (auto level_result : bids_arr) {
                if (snap.bid_count >= 1024) break;
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

                auto price = BinanceJsonParser::parse_decimal_to_fixed(price_str, price_multiplier);
                auto qty = BinanceJsonParser::parse_decimal_to_fixed(qty_str, qty_multiplier);
                if (!price || !qty) continue;

                snap.bids[snap.bid_count] = {*price, *qty};
                ++snap.bid_count;
            }
        }

        // Parse asks
        simdjson::ondemand::array asks_arr;
        if (doc["asks"].get_array().get(asks_arr) == simdjson::SUCCESS) {
            for (auto level_result : asks_arr) {
                if (snap.ask_count >= 1024) break;
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

                auto price = BinanceJsonParser::parse_decimal_to_fixed(price_str, price_multiplier);
                auto qty = BinanceJsonParser::parse_decimal_to_fixed(qty_str, qty_multiplier);
                if (!price || !qty) continue;

                snap.asks[snap.ask_count] = {*price, *qty};
                ++snap.ask_count;
            }
        }

        if (snap.bid_count == 0 && snap.ask_count == 0) {
            return std::nullopt;
        }

        return snap;

    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace hy
