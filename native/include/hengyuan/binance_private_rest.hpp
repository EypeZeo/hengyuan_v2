// SPDX-License-Identifier: proprietary
// binance_private_rest.hpp — Binance L4 read-only private REST client (§3/§4 of
// docs/BINANCE_PRIVATE_REST_L4_SPEC.md rev 72).
//
// Two GET endpoints, built on the already-shipped L4 signing foundation
// (binance_signer.hpp/binance_environment.hpp/binance_query_signing.hpp/
// binance_clock_sync.hpp — all landed separately, this file is their first real
// network consumer):
//
//   - GET /api/v3/time (§3, public, unauthenticated): closes the loop
//     binance_clock_sync.hpp's own header comment describes as "out of scope" —
//     that file has offline-only ClockOffsetSnapshot/compute_clock_offset()/
//     ClockOffsetPublisher machinery that nothing has ever actually driven with a
//     real round trip until this file's sync_clock().
//   - GET /api/v3/account (§4, signed, USER_DATA): backs account_truth.hpp's
//     AccountSnapshot. Fails closed on every one of §4.1-4.4's schema/precision/
//     capacity conditions — the fetch either produces a complete, correctly-scaled
//     snapshot or it changes nothing.
//
// Explicit non-goals for this first slice (see docs/SPEC_INVARIANTS.md's L4 entries
// and the spec's own §5/§6/§7/§9/§10 sections): GET /api/v3/exchangeInfo (§5, symbol
// registry), GET /api/v3/order (§6, reconciliation), rate-limit header accounting
// (§7), the single-owner actor/scheduler thread model (§9), and a concrete
// durable_control_plane.hpp-backed persistence implementation (§10, whose ABI
// surface already exists per docs/SPEC_INVARIANTS.md but has no durability
// requirement from §3/§4 specifically — §5.3.1's registry snapshot is the first
// place the spec actually mandates a durable ACK-before-publish write). None of
// those are implemented here. This client also does not itself enforce the
// single-owner-thread discipline BoundHmacCredentials/§9 require — same contract
// BoundHmacCredentials already has (construct + every sign()/copy_api_key() call
// from one thread), the caller's responsibility, not this class's.
//
// Structurally mirrors binance_rest_snapshot.hpp's coroutine skeleton (resolve ->
// connect -> TLS handshake -> write -> read, each with its own bounded deadline) —
// per spec §8, reusing that PLUMBING is correct, but the SECURITY posture is
// independently specified here: header_limit()+body_limit() both configured before
// reading (not just the latter), no Accept-Encoding is ever sent (never negotiates
// compression), the connection is always force-closed after use (single-shot calls,
// like the existing depth-snapshot fetcher), and every coroutine catches `...`
// (broader than binance_rest_snapshot.hpp's own `boost::system::system_error`-only
// catch — spec §8 explicitly requires converting ANY exception, not just Boost's).
//
// Governance: L4 (real network I/O + real HMAC signing via already-bound
// credentials; no order submission — every request here is a GET).

#pragma once

#include <hengyuan/account_truth.hpp>
#include <hengyuan/binance_clock_sync.hpp>
#include <hengyuan/binance_environment.hpp>
#include <hengyuan/binance_query_signing.hpp>
#include <hengyuan/binance_tls.hpp>

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
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

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace hy {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// host is NOT part of this config — it comes from the bound EnvironmentBinding, so there is
// exactly one source of truth for "which environment am I talking to" (the same reasoning
// EnvironmentBinding's own header comment gives for hardcoding host/credential-env-var-name
// per factory: no parameter a caller could pass to accidentally cross environments).
struct PrivateRestConfig {
    std::string port = "443";
    // Binance's own recvWindow business parameter (§2.1) — how long, from `timestamp`, the
    // server accepts this request as still fresh. Not related to this client's own network
    // timeouts below (which bound how long WE wait for the network).
    std::int64_t recv_window_ms = 5000;

    // Test-only escape hatches, matching RestSnapshotConfig's own (binance_rest_snapshot.hpp)
    // — empty by default, production callers never set either.
    std::string extra_trusted_ca_pem_path;
    std::string connect_host_override;
};

enum class PrivateRestError : std::uint8_t {
    None = 0,
    InvalidConfig = 1,
    Resolve = 2,
    Connect = 3,
    TlsHandshake = 4,
    Write = 5,
    Read = 6,
    HttpStatus = 7,
    JsonParse = 8,
    MalformedResponse = 9,   // §4.1 schema failure or §4.2 conversion failure
    CapacityExceeded = 10,   // §4.3: more nonzero-balance assets than kMaxAssets
    ClockNotFresh = 11,      // §2.2: no fresh clock-offset snapshot -- fail closed, never signs
                              // with uncalibrated local system time
    SigningFailed = 12,      // build_canonical_query()/build_signed_query()/copy_api_key()
};

// §4.2: lossless decimal-string -> fixed-point ticks at account_truth.hpp's kBalanceScale (8).
// Deliberately NOT binance_json_parser.hpp::parse_decimal_to_fixed() reused as-is: that
// function silently TRUNCATES a fractional part longer than its target scale (verified by
// reading its implementation) -- correct for its own WS/L1 hot-path use (streaming price/qty,
// governed by a different spec section with its own truncation-is-fine precedent), but exactly
// the "never truncate a balance silently and proceed with a smaller number than the account
// actually holds" failure §4.2 explicitly forbids. This is a fresh, deliberately stricter
// parser: any of a non-digit character, more than 8 fractional digits, a negative sign
// (balances are never negative), an empty string, or std::int64_t overflow at scale 8 all
// REJECT (return false, `out_ticks` untouched) rather than truncating or clamping.
inline bool parse_balance_decimal_to_ticks(std::string_view s, std::int64_t& out_ticks) noexcept {
    if (s.empty()) return false;
    if (s[0] == '-') return false;  // balances are never negative

    std::size_t pos = 0;
    std::int64_t integer_part = 0;
    while (pos < s.size() && s[pos] != '.') {
        const char c = s[pos];
        if (c < '0' || c > '9') return false;
        const int digit = c - '0';
        if (integer_part > (std::numeric_limits<std::int64_t>::max() - digit) / 10) return false;
        integer_part = integer_part * 10 + digit;
        ++pos;
    }

    std::int64_t frac_part = 0;
    int frac_digits = 0;
    if (pos < s.size()) {
        // Unreachable given the loop above (it only stops early on '.'), kept as an explicit
        // precondition rather than an assumption.
        if (s[pos] != '.') return false;
        ++pos;
        if (pos == s.size()) return false;  // trailing '.' with no digits after it ("5.")
        while (pos < s.size()) {
            const char c = s[pos];
            if (c < '0' || c > '9') return false;
            // The reject-not-truncate case §4.2 exists for.
            if (frac_digits >= kBalanceScale) return false;
            frac_part = frac_part * 10 + (c - '0');
            ++frac_digits;
            ++pos;
        }
    }

    // Pad fewer-than-kBalanceScale fractional digits up to kBalanceScale (e.g. "1.5" -> frac
    // 5 at 1 digit becomes 50000000 at 8 digits) so the final combine below is a single
    // fixed-width scale throughout.
    std::int64_t scale = 1;
    for (int i = 0; i < kBalanceScale; ++i) scale *= 10;  // 10^8 -- fits trivially in int64
    std::int64_t frac_scale = 1;
    for (int i = 0; i < frac_digits; ++i) frac_scale *= 10;
    const std::int64_t pad = scale / frac_scale;
    if (frac_part > std::numeric_limits<std::int64_t>::max() / pad) return false;
    frac_part *= pad;

    if (integer_part > (std::numeric_limits<std::int64_t>::max() - frac_part) / scale) {
        return false;
    }
    out_ticks = integer_part * scale + frac_part;
    return true;
}

// §3: `{"serverTime": <ms>}`. A negative value is rejected outright (Binance never returns
// one; a negative "server time" would corrupt every downstream RTT/offset computation that
// assumes a monotonically-sane epoch millisecond count).
inline bool parse_server_time_response(std::string_view body,
                                        std::int64_t& out_server_time_ms) {
    auto padded = simdjson::padded_string(body);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) return false;

    std::int64_t v = 0;
    if (doc["serverTime"].get_int64().get(v) != simdjson::SUCCESS) return false;
    if (v < 0) return false;
    out_server_time_ms = v;
    return true;
}

// §4.1-§4.3: schema validation + lossless conversion + capacity, collapsed to the single
// §4.4 outcome contract -- `out` is left completely UNCHANGED on any failure path (this
// function builds into a local AccountSnapshot and only assigns to `out` once, at the very
// end, on total success). `timestamp_ms` is NOT set here -- the caller (fetch_account())
// stamps it with the same server-calibrated fresh_ts_ms already computed for this request's
// signature, so the snapshot's timestamp and its own HTTP request's timestamp are the same
// value, not two independent clock reads.
inline PrivateRestError parse_account_response(std::string_view body, AccountSnapshot& out) {
    auto padded = simdjson::padded_string(body);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) return PrivateRestError::JsonParse;

    bool can_trade = false;
    if (doc["canTrade"].get_bool().get(can_trade) != simdjson::SUCCESS) {
        return PrivateRestError::MalformedResponse;
    }

    simdjson::ondemand::array balances;
    if (doc["balances"].get_array().get(balances) != simdjson::SUCCESS) {
        return PrivateRestError::MalformedResponse;
    }

    AccountSnapshot built{};
    built.can_trade = can_trade;

    for (auto entry_result : balances) {
        simdjson::ondemand::object entry;
        if (entry_result.get_object().get(entry) != simdjson::SUCCESS) {
            return PrivateRestError::MalformedResponse;
        }

        std::string_view asset_sv, free_sv, locked_sv;
        if (entry["asset"].get_string().get(asset_sv) != simdjson::SUCCESS) {
            return PrivateRestError::MalformedResponse;
        }
        if (entry["free"].get_string().get(free_sv) != simdjson::SUCCESS) {
            return PrivateRestError::MalformedResponse;
        }
        if (entry["locked"].get_string().get(locked_sv) != simdjson::SUCCESS) {
            return PrivateRestError::MalformedResponse;
        }

        std::int64_t free_ticks = 0, locked_ticks = 0;
        if (!parse_balance_decimal_to_ticks(free_sv, free_ticks)) {
            return PrivateRestError::MalformedResponse;
        }
        if (!parse_balance_decimal_to_ticks(locked_sv, locked_ticks)) {
            return PrivateRestError::MalformedResponse;
        }

        // §4.3: zero-balance assets don't consume a slot. Comparing the PARSED value (not the
        // raw string) so this is correct regardless of which zero spelling Binance sends
        // ("0", "0.0", "0.00000000", ...), not just the one example spelling the spec text
        // happens to use.
        if (free_ticks == 0 && locked_ticks == 0) continue;

        // Truncating an oversized asset symbol into AssetBalance::asset's fixed buffer would
        // misattribute this balance to an ambiguous/wrong asset -- reject, don't truncate,
        // same philosophy as every other check in this function.
        if (asset_sv.empty() || asset_sv.size() >= kAssetNameLen) {
            return PrivateRestError::MalformedResponse;
        }

        // §4.3: more nonzero-balance assets than kMaxAssets -> reject the whole fetch, never
        // silently truncate to the first kMaxAssets entries.
        if (built.asset_count >= kMaxAssets) {
            return PrivateRestError::CapacityExceeded;
        }

        AssetBalance& slot = built.assets[built.asset_count];
        std::memcpy(slot.asset, asset_sv.data(), asset_sv.size());
        slot.asset[asset_sv.size()] = '\0';
        slot.free_ticks = free_ticks;
        slot.locked_ticks = locked_ticks;
        ++built.asset_count;
    }

    out = built;
    return PrivateRestError::None;
}

namespace detail {

// Everything compute_clock_offset() needs from one real §3 round trip.
struct ServerTimeFetchResult {
    std::int64_t local_send_ms{0};
    std::int64_t local_recv_ms{0};
    std::int64_t server_time_ms{0};
    ClockPairSample fetch_sample{fetch_clock_pair()};  // placeholder; always overwritten
};

inline net::awaitable<std::variant<ServerTimeFetchResult, PrivateRestError>>
fetch_server_time_coro(std::string host, PrivateRestConfig cfg) {
    using namespace boost::asio::experimental::awaitable_operators;

    PrivateRestError current_stage = PrivateRestError::Resolve;
    try {
        auto executor = co_await net::this_coro::executor;

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
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            co_return PrivateRestError::TlsHandshake;
        }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        hy::configure_binance_hostname_verification(stream, host);

        const std::string& connect_host =
            cfg.connect_host_override.empty() ? host : cfg.connect_host_override;
        net::steady_timer resolve_timer(executor);
        resolve_timer.expires_after(std::chrono::seconds(5));
        auto resolve_result =
            co_await (resolver.async_resolve(connect_host, cfg.port, net::use_awaitable) ||
                      resolve_timer.async_wait(net::use_awaitable));
        if (resolve_result.index() == 1) {
            co_return PrivateRestError::Resolve;  // timer won the race
        }
        auto results = std::get<0>(resolve_result);

        current_stage = PrivateRestError::Connect;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await beast::get_lowest_layer(stream).async_connect(results, net::use_awaitable);

        current_stage = PrivateRestError::TlsHandshake;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);

        http::request<http::empty_body> req{http::verb::get, "/api/v3/time", 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "HengYuan/0.1");

        // Sampled immediately before the write -- "local_send_ms" for
        // compute_clock_offset()'s RTT calculation (spec §2.2).
        ClockPairSample t0 = fetch_clock_pair();

        current_stage = PrivateRestError::Write;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await http::async_write(stream, req, net::use_awaitable);

        current_stage = PrivateRestError::Read;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.header_limit(static_cast<std::uint32_t>(8 * 1024));
        parser.body_limit(static_cast<std::uint64_t>(64 * 1024));
        co_await http::async_read(stream, buffer, parser, net::use_awaitable);
        auto res = parser.release();

        // Sampled immediately after the read completes -- "local_recv_ms", and also the
        // joint (system, steady) instant this snapshot's later TTL/wall-clock-jump judgment
        // (is_snapshot_fresh()) is anchored to.
        ClockPairSample t1 = fetch_clock_pair();

        beast::get_lowest_layer(stream).close();

        if (res.result() != http::status::ok) {
            co_return PrivateRestError::HttpStatus;
        }

        current_stage = PrivateRestError::JsonParse;
        std::int64_t server_time_ms = 0;
        if (!parse_server_time_response(res.body(), server_time_ms)) {
            co_return PrivateRestError::JsonParse;
        }

        ServerTimeFetchResult out{};
        out.local_send_ms = t0.system_ms();
        out.local_recv_ms = t1.system_ms();
        out.server_time_ms = server_time_ms;
        out.fetch_sample = t1;
        co_return out;

    } catch (...) {
        // Spec §8: any exception, of any type, caught and converted -- never propagated.
        co_return current_stage;
    }
}

inline net::awaitable<std::variant<std::string, PrivateRestError>> fetch_signed_body_coro(
    std::string host, std::string target, std::string api_key, PrivateRestConfig cfg) {
    using namespace boost::asio::experimental::awaitable_operators;

    PrivateRestError current_stage = PrivateRestError::Resolve;
    try {
        auto executor = co_await net::this_coro::executor;

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
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            co_return PrivateRestError::TlsHandshake;
        }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        hy::configure_binance_hostname_verification(stream, host);

        const std::string& connect_host =
            cfg.connect_host_override.empty() ? host : cfg.connect_host_override;
        net::steady_timer resolve_timer(executor);
        resolve_timer.expires_after(std::chrono::seconds(5));
        auto resolve_result =
            co_await (resolver.async_resolve(connect_host, cfg.port, net::use_awaitable) ||
                      resolve_timer.async_wait(net::use_awaitable));
        if (resolve_result.index() == 1) {
            co_return PrivateRestError::Resolve;
        }
        auto results = std::get<0>(resolve_result);

        current_stage = PrivateRestError::Connect;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await beast::get_lowest_layer(stream).async_connect(results, net::use_awaitable);

        current_stage = PrivateRestError::TlsHandshake;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);

        http::request<http::empty_body> req{http::verb::get, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "HengYuan/0.1");
        // Never negotiates compression (spec §8): no Accept-Encoding header is ever sent, so
        // there is no decompression-bomb surface to bound in the first place.
        req.set("X-MBX-APIKEY", api_key);

        current_stage = PrivateRestError::Write;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await http::async_write(stream, req, net::use_awaitable);

        current_stage = PrivateRestError::Read;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.header_limit(static_cast<std::uint32_t>(8 * 1024));
        // §4.3's 32-asset cap keeps a real response tiny; 256 KiB is generous headroom, not a
        // reflection of an expected size.
        parser.body_limit(static_cast<std::uint64_t>(256 * 1024));
        co_await http::async_read(stream, buffer, parser, net::use_awaitable);
        auto res = parser.release();

        beast::get_lowest_layer(stream).close();

        if (res.result() != http::status::ok) {
            co_return PrivateRestError::HttpStatus;
        }
        co_return res.body();

    } catch (...) {
        co_return current_stage;
    }
}

}  // namespace detail

// Owns one bound credential set + one clock-offset publisher. Not internally thread-safe: the
// same single-owning-thread discipline BoundHmacCredentials itself already requires (construct
// + every sign()/copy_api_key() call from one thread, spec §9) applies transitively to this
// whole class's public methods -- this class does not add its own synchronization on top of
// that, nor does it implement the full actor/scheduler owner-thread model spec §9 describes
// (explicit non-goal for this slice; see this file's header comment).
class BinancePrivateRestClient {
public:
    BinancePrivateRestClient(EnvironmentBinding binding,
                              std::unique_ptr<BoundHmacCredentials> creds) noexcept
        : binding_(binding), creds_(std::move(creds)) {}

    // Call once, before the first sync_clock()/fetch_account(), on the same thread that will
    // make those calls. Spec §8: verifies the TLS trust store this process would actually use
    // resolves to something non-empty -- never proceeds on a silently-empty trust store, which
    // would otherwise make every handshake fail closed for a reason indistinguishable from "the
    // server's certificate is bad."
    bool init() const noexcept {
        try {
            ssl::context probe(ssl::context::tlsv12_client);
            hy::configure_binance_ssl_context(probe);
            return hy::binance_tls_trust_store_populated(probe);
        } catch (...) {
            return false;
        }
    }

    // §3: unauthenticated. On success, computes the offset (RTT > kMaxUsableRttMs is silently
    // discarded -- compute_clock_offset()'s own contract, not a bug here) and publishes it via
    // clock_publisher(), closing the previously-offline-only clock-sync loop. Never signs
    // anything and never touches creds_.
    PrivateRestError sync_clock(const PrivateRestConfig& cfg = {}) {
        net::io_context ioc;
        std::variant<detail::ServerTimeFetchResult, PrivateRestError> outcome =
            PrivateRestError::None;
        net::co_spawn(
            ioc, detail::fetch_server_time_coro(std::string(binding_.base_host()), cfg),
            [&outcome](std::exception_ptr eptr,
                       std::variant<detail::ServerTimeFetchResult, PrivateRestError> r) {
                if (eptr) {
                    // The coroutine itself catches everything (spec §8); anything that still
                    // escapes here (e.g. std::bad_alloc from Asio/Beast setup) is genuinely
                    // exceptional, matching binance_rest_snapshot.hpp's own precedent.
                    std::rethrow_exception(eptr);
                }
                outcome = std::move(r);
            });
        ioc.run();

        if (std::holds_alternative<PrivateRestError>(outcome)) {
            return std::get<PrivateRestError>(outcome);
        }
        const auto& r = std::get<detail::ServerTimeFetchResult>(outcome);
        ClockOffsetSnapshot snap{};
        if (!compute_clock_offset(r.local_send_ms, r.local_recv_ms, r.server_time_ms,
                                   r.fetch_sample, snap)) {
            // RTT over kMaxUsableRttMs, or checked-arithmetic overflow on pathological inputs.
            return PrivateRestError::JsonParse;
        }
        clock_pub_.publish(snap);
        return PrivateRestError::None;
    }

    // §4: signed. Fails closed on a stale/never-synced clock -- try_get_signing_timestamp_ms()
    // is the sole call site for that judgment, and this function never falls back to
    // uncalibrated local system time on its own failure (spec §2.2's core invariant). `out` is
    // left COMPLETELY UNCHANGED on every failure path (§4.4) -- this function only ever
    // assigns to `out` once, on total success, exactly mirroring parse_account_response()'s own
    // build-then-assign discipline.
    PrivateRestError fetch_account(AccountSnapshot& out, const PrivateRestConfig& cfg = {}) {
        std::int64_t fresh_ts_ms = 0;
        if (!try_get_signing_timestamp_ms(clock_pub_, fetch_clock_pair(), fresh_ts_ms)) {
            return PrivateRestError::ClockNotFresh;
        }

        char recv_window_buf[24];
        const int n = std::snprintf(recv_window_buf, sizeof(recv_window_buf), "%lld",
                                     static_cast<long long>(cfg.recv_window_ms));
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(recv_window_buf)) {
            return PrivateRestError::InvalidConfig;
        }
        const std::pair<std::string_view, std::string_view> params[] = {
            {"recvWindow", std::string_view(recv_window_buf, static_cast<std::size_t>(n))},
        };
        auto [qerr, unsigned_query] = build_canonical_query(params);
        if (qerr != QuerySigningError::Ok) return PrivateRestError::SigningFailed;

        auto [serr, signed_query] = build_signed_query(*creds_, unsigned_query, fresh_ts_ms);
        if (serr != QuerySigningError::Ok) return PrivateRestError::SigningFailed;

        std::array<char, kApiKeyLen> key_buf{};
        std::size_t key_len = 0;
        if (!creds_->copy_api_key(key_buf, key_len) || key_len == 0) {
            return PrivateRestError::SigningFailed;
        }

        const std::string target = "/api/v3/account?" + std::string(signed_query.wire_bytes());

        net::io_context ioc;
        std::variant<std::string, PrivateRestError> outcome = PrivateRestError::None;
        net::co_spawn(
            ioc,
            detail::fetch_signed_body_coro(std::string(binding_.base_host()), target,
                                            std::string(key_buf.data(), key_len), cfg),
            [&outcome](std::exception_ptr eptr, std::variant<std::string, PrivateRestError> r) {
                if (eptr) std::rethrow_exception(eptr);
                outcome = std::move(r);
            });
        ioc.run();

        if (std::holds_alternative<PrivateRestError>(outcome)) {
            return std::get<PrivateRestError>(outcome);
        }

        AccountSnapshot parsed{};
        const PrivateRestError perr =
            parse_account_response(std::get<std::string>(outcome), parsed);
        if (perr != PrivateRestError::None) return perr;
        parsed.timestamp_ms = fresh_ts_ms;
        out = parsed;
        return PrivateRestError::None;
    }

    const ClockOffsetPublisher& clock_publisher() const noexcept { return clock_pub_; }

private:
    EnvironmentBinding binding_;
    std::unique_ptr<BoundHmacCredentials> creds_;
    ClockOffsetPublisher clock_pub_;
};

}  // namespace hy
