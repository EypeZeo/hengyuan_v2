// SPDX-License-Identifier: proprietary
// binance_rest_snapshot.hpp — Binance REST depth snapshot fetcher (P2-MD-02 / Track C).
//
// GET https://api.binance.com/api/v3/depth?symbol=BTCUSDT&limit=<one of the allowed values>
// Parses the JSON response into a DepthSnapshot for DepthManager.
//
// Governance: L4 (real Binance public REST API, no token/HMAC/Private API).
// Real network I/O, real TLS -- verified against the real endpoint via
// native/src/binance_connectivity_smoke.cpp (manual, not part of CI; see that file's own
// header comment for the exact command). VPS manual verification = L4.
//
// Internally implemented as a single-shot net::awaitable coroutine driven by a local
// io_context (net::co_spawn + ioc.run()); the public fetch_depth_snapshot() signature stays
// synchronous. Every network stage (resolve/connect/TLS handshake/write/read) has its own
// bounded deadline, so a stalled connection fails within a predictable ~30s worst case instead
// of hanging forever. TLS verification confirms both chain-of-trust and hostname match
// (see binance_tls.hpp). Response headers/body are capped and depth data is validated
// fail-closed (see binance_depth_snapshot_codec.hpp) before being handed back.
//
// Deliberately NOT point-in-time-safe: a closed REST snapshot only proves the returned values
// won't change again, not that a real historical strategy could have obtained this exact data
// with zero latency. Callers should not treat a successful fetch as a live-data-feed guarantee.

#pragma once

#include <hengyuan/binance_depth_snapshot_codec.hpp>
#include <hengyuan/binance_tls.hpp>
#include <hengyuan/depth_manager.hpp>
#include <hengyuan/snapshot_refresh_gate.hpp>  // SnapshotFetcher/SnapshotRequest (VERIF-TSAN-016)

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace hy {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

struct RestSnapshotConfig {
    std::string host = "api.binance.com";
    std::string port = "443";
    int limit = 1000;  // depth levels; must be one of Binance's documented values, see
                        // kAllowedLimits below -- also bounded by DepthSnapshot's 1024-per-side
                        // fixed capacity (depth_manager.hpp), so 5000 is deliberately excluded.

    // Test-only escape hatch: when non-empty, this CA is trusted in ADDITION to the system
    // trust store (native/tests/test_helpers/blackhole_acceptor.hpp's TlsBlackholeAcceptor
    // uses it so the wrong-SAN/read-timeout tests can drive the real fetch_depth_snapshot()
    // pipeline against a local TLS fixture instead of the real Binance endpoint). Empty by
    // default -- production callers never set this, and setting it does not weaken anything
    // for them since it only ever *adds* a trust anchor, never removes verify_peer/hostname
    // checking.
    std::string extra_trusted_ca_pem_path;

    // Test-only escape hatch: when non-empty, resolve/connect target THIS instead of `host`,
    // while SNI and hostname verification still use `host`. Needed because a synthetic
    // `.invalid` test hostname (RFC 2606) cannot be relied on to fail DNS resolution --
    // observed directly in this repo's own WSL2 environment, where an unresolvable hostname
    // resolved to a synthesized address instead of NXDOMAIN. Without this override, a test
    // using a fake hostname for both "where to connect" and "what to verify" would silently
    // connect to some unrelated real host instead of the intended local test fixture. Empty by
    // default -- production callers never set this.
    std::string connect_host_override;
};

// REST transport-layer error type -- distinct from DepthSnapshotParseError (which belongs to
// the Boost/SSL-free codec layer, see binance_depth_snapshot_codec.hpp). Each value is set by
// the coroutine stage that was executing when the failure happened (see fetch_depth_snapshot_
// coro's current_stage tracking below) -- never inferred after the fact from an exception's
// error category, since Beast's own timeout errors (beast::error::timeout) don't live in the
// SSL category even when the timeout fires during the TLS handshake stage.
enum class FetchError {
    None,
    InvalidConfig,
    Resolve,
    Connect,
    TlsHandshake,
    Write,
    Read,
    HttpStatus,
    JsonParse,
    MalformedPayload,
    EmptyBook,
};

namespace detail {

inline bool is_valid_rest_symbol(std::string_view symbol) {
    if (symbol.empty() || symbol.size() > 20) return false;
    for (char c : symbol) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

inline bool is_valid_rest_host(std::string_view host) {
    if (host.empty() || host.size() > 253) return false;
    for (char c : host) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

inline bool is_allowed_depth_limit(int limit) {
    static constexpr int kAllowedLimits[] = {5, 10, 20, 50, 100, 500, 1000};
    for (int allowed : kAllowedLimits) {
        if (limit == allowed) return true;
    }
    return false;
}

}  // namespace detail

// Pure, no-I/O config validation -- rejects a request before any network traffic is sent,
// rather than silently truncating/misconstructing a request from an invalid symbol/limit.
inline std::optional<FetchError> validate_rest_config(const std::string& symbol,
                                                        const RestSnapshotConfig& cfg) {
    if (!detail::is_valid_rest_symbol(symbol)) return FetchError::InvalidConfig;
    if (!detail::is_valid_rest_host(cfg.host)) return FetchError::InvalidConfig;
    if (cfg.port.empty()) return FetchError::InvalidConfig;
    if (!detail::is_allowed_depth_limit(cfg.limit)) return FetchError::InvalidConfig;
    return std::nullopt;
}

namespace detail {

inline FetchError map_parse_error(DepthSnapshotParseError err) {
    switch (err) {
        case DepthSnapshotParseError::InvalidLastUpdateId:
        case DepthSnapshotParseError::MalformedLevel:
            return FetchError::JsonParse;
        case DepthSnapshotParseError::NonPositiveQty:
        case DepthSnapshotParseError::TooManyLevels:
        case DepthSnapshotParseError::OrderingViolation:
            return FetchError::MalformedPayload;
        case DepthSnapshotParseError::EmptySide:
            return FetchError::EmptyBook;
    }
    return FetchError::MalformedPayload;
}

inline net::awaitable<std::variant<DepthSnapshot, FetchError>> fetch_depth_snapshot_coro(
    std::string symbol, std::int64_t price_multiplier, std::int64_t qty_multiplier,
    RestSnapshotConfig cfg) {
    using namespace boost::asio::experimental::awaitable_operators;

    FetchError current_stage = FetchError::Resolve;
    try {
        auto executor = co_await net::this_coro::executor;

        current_stage = FetchError::TlsHandshake;  // covers ssl_ctx setup below too
        ssl::context ssl_ctx(ssl::context::tlsv12_client);
        hy::configure_binance_ssl_context(ssl_ctx);
        if (!cfg.extra_trusted_ca_pem_path.empty()) {
            ssl_ctx.load_verify_file(cfg.extra_trusted_ca_pem_path);
        }

        tcp::resolver resolver(executor);
        beast::ssl_stream<beast::tcp_stream> stream(executor, ssl_ctx);

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
        if (!SSL_set_tlsext_host_name(stream.native_handle(), cfg.host.c_str())) {
            co_return FetchError::TlsHandshake;
        }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        hy::configure_binance_hostname_verification(stream, cfg.host);

        // Resolve, racing a 5s timer -- beast::tcp_stream::expires_after() does not cover
        // tcp::resolver (a separate object), so a dedicated race is needed for this one stage.
        // connect_host_override (test-only) lets the resolve/connect target differ from the
        // host used for SNI/hostname verification above -- see its own doc comment.
        current_stage = FetchError::Resolve;
        const std::string& connect_host =
            cfg.connect_host_override.empty() ? cfg.host : cfg.connect_host_override;
        net::steady_timer resolve_timer(executor);
        resolve_timer.expires_after(std::chrono::seconds(5));
        auto resolve_result =
            co_await (resolver.async_resolve(connect_host, cfg.port, net::use_awaitable) ||
                      resolve_timer.async_wait(net::use_awaitable));
        if (resolve_result.index() == 1) {
            co_return FetchError::Resolve;  // timer won the race
        }
        auto results = std::get<0>(resolve_result);

        current_stage = FetchError::Connect;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await beast::get_lowest_layer(stream).async_connect(results, net::use_awaitable);

        current_stage = FetchError::TlsHandshake;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);

        std::string target =
            "/api/v3/depth?symbol=" + symbol + "&limit=" + std::to_string(cfg.limit);
        http::request<http::empty_body> req{http::verb::get, target, 11};
        req.set(http::field::host, cfg.host);
        req.set(http::field::user_agent, "HengYuan/0.1");

        current_stage = FetchError::Write;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await http::async_write(stream, req, net::use_awaitable);

        current_stage = FetchError::Read;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.header_limit(static_cast<std::uint32_t>(8 * 1024));
        parser.body_limit(static_cast<std::uint64_t>(1 * 1024 * 1024));
        co_await http::async_read(stream, buffer, parser, net::use_awaitable);
        auto res = parser.release();

        // One-shot REST call -- close the socket directly rather than performing a graceful
        // TLS close_notify shutdown, which would need its own deadline to stay bounded.
        beast::get_lowest_layer(stream).close();

        if (res.result() != http::status::ok) {
            co_return FetchError::HttpStatus;
        }

        current_stage = FetchError::JsonParse;
        auto parsed = parse_depth_response(res.body(), price_multiplier, qty_multiplier);
        if (std::holds_alternative<DepthSnapshotParseError>(parsed)) {
            co_return map_parse_error(std::get<DepthSnapshotParseError>(parsed));
        }
        co_return std::get<DepthSnapshot>(parsed);

    } catch (const boost::system::system_error&) {
        co_return current_stage;
    }
}

}  // namespace detail

inline std::optional<DepthSnapshot> fetch_depth_snapshot(
    const std::string& symbol, std::int64_t price_multiplier, std::int64_t qty_multiplier,
    const RestSnapshotConfig& cfg, FetchError& out_error) {
    out_error = FetchError::None;

    if (auto invalid = validate_rest_config(symbol, cfg)) {
        out_error = *invalid;
        return std::nullopt;
    }

    net::io_context ioc;
    std::variant<DepthSnapshot, FetchError> outcome = FetchError::None;
    net::co_spawn(
        ioc, detail::fetch_depth_snapshot_coro(symbol, price_multiplier, qty_multiplier, cfg),
        [&outcome](std::exception_ptr eptr, std::variant<DepthSnapshot, FetchError> r) {
            if (eptr) {
                // The coroutine itself catches boost::system::system_error; anything that
                // escapes here (e.g. std::bad_alloc) is genuinely exceptional and should
                // propagate as a real exception rather than being silently turned into a
                // FetchError.
                std::rethrow_exception(eptr);
            }
            outcome = std::move(r);
        });
    ioc.run();

    if (std::holds_alternative<DepthSnapshot>(outcome)) {
        return std::get<DepthSnapshot>(outcome);
    }
    out_error = std::get<FetchError>(outcome);
    return std::nullopt;
}

inline std::optional<DepthSnapshot> fetch_depth_snapshot(
    const std::string& symbol, std::int64_t price_multiplier, std::int64_t qty_multiplier,
    const RestSnapshotConfig& cfg = {}) {
    FetchError unused_error;
    return fetch_depth_snapshot(symbol, price_multiplier, qty_multiplier, cfg, unused_error);
}

// Production SnapshotFetcher for SnapshotRefreshGate.
//
// AUDIT VERIF-TSAN-016: this adapter used to live in snapshot_refresh_gate.hpp as a
// constructor default argument, which forced that header -- and therefore every test
// of the gate's threaded state machine -- to depend on Boost.Asio/Beast and OpenSSL.
// The TSan CI job builds HY_BUILD_DEMO=OFF, so the one class in this codebase that
// spawns a std::thread and hands a mailbox across it was never tested under
// ThreadSanitizer. Moving the adapter next to the function it adapts leaves the gate
// Boost-free; callers who want the real network fetcher ask for it explicitly.
inline SnapshotFetcher make_default_snapshot_fetcher() {
    return [](const SnapshotRequest& req) -> std::optional<DepthSnapshot> {
        return fetch_depth_snapshot(req.symbol, req.price_multiplier, req.qty_multiplier);
    };
}

}  // namespace hy
