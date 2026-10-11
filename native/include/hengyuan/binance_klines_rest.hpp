// SPDX-License-Identifier: proprietary
// binance_klines_rest.hpp — 批次 6 6b-0f-1: Binance PUBLIC REST historical-kline fetcher
// (GET /api/v3/klines) -- the native transport for the backfill 6a-1's design has always assumed
// (外部复核 P0-05, verified: the only klines client in the repo was py_core's Python one).
//
// Governance: L4 (real Binance public REST, no token/HMAC/Private API), same class as
// binance_rest_snapshot.hpp. Structure deliberately mirrors that file: a pure codec
// (binance_klines_codec.hpp) does the fail-closed validation; this file is only the transport.
//
// THE ENVIRONMENT IS ALWAYS NAMED BY THE CALLER: there is no default host, and PublicRestConfig has no
// host field at all. Every fetch takes the process's EnvironmentBinding; a klines fetch that silently
// targeted production while the rest of the process is testnet would feed the indicators data from a
// different market than the one being traded. The binding's host must pass its own endpoint allowlist
// (an EndpointPermit is issued, and the coroutine connects to permit.host() and nothing else); a
// refusal is an InvalidConfig, not a fallback.
//
// Synchronous signature, driven by a local io_context (same as fetch_depth_snapshot()), with the
// same per-stage deadlines (resolve/connect/TLS/write 5s, read 10s) and the same hard caps on
// response headers (8 KiB) and body (1 MiB -- 1000 klines is ~130 KiB). It BLOCKS the calling
// thread for up to ~30s worst case, so it must run on a worker/recovery path, never inside a loop
// that has to keep draining rings -- exactly the constraint SnapshotRefreshGate exists to honour
// for depth snapshots.
//
// WHICH BARS ARE "CLOSED" (the one subtle thing in here): without an endTime Binance returns the
// current, still-forming candle as the last element, carrying PARTIAL OHLCV. Feeding a partial bar
// into the indicators as if it were final is silent, unrepairable state corruption, whereas
// dropping a bar that had in fact just closed is fully repairable (the recovery protocol re-joins
// against the live stream by close_time). So every rounding error here goes the same way:
//   * the clock is read BEFORE the request goes out, not after the response arrives -- a late read
//     would call a bar closed that was still forming when the server built the response;
//   * a bar counts as closed only once kKlinesClosedBarMarginMs has elapsed past its close_time,
//     absorbing exchange-clock-offset error and any late-settling final trade.
// A caller that needs N closed bars should therefore ask for N + 2 (forming candle + one bar that
// may sit inside the margin).

#pragma once

#include <hengyuan/binance_environment.hpp>
#include <hengyuan/binance_klines_codec.hpp>
#include <hengyuan/binance_rest_snapshot.hpp>  // detail::is_valid_rest_host / _symbol; net/beast/http/ssl/tcp aliases
#include <hengyuan/binance_tls.hpp>
#include <hengyuan/kline_bar.hpp>
#include <hengyuan/rest_test_seam.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace hy {

// No host in here: it is the bound environment's (EnvironmentBinding::base_host()), so a configuration
// cannot name one (audit P1-001 / SAFE-01).
struct PublicRestConfig {
    std::string port = "443";

    // Deliberately no connect-target override and no extra trust anchor in here (audit P1-001, fault
    // case FI-032), same as RestSnapshotConfig: a test passes a RestTestSeam (rest_test_seam.hpp) as the
    // separate, defaulted last argument of the fetch functions below.
};

enum class PublicRestError : std::uint8_t {
    None = 0,
    InvalidConfig = 1,
    Resolve = 2,
    Connect = 3,
    TlsHandshake = 4,
    Write = 5,
    Read = 6,
    HttpStatus = 7,
};

inline constexpr const char* public_rest_error_name(PublicRestError e) noexcept {
    switch (e) {
        case PublicRestError::None: return "None";
        case PublicRestError::InvalidConfig: return "InvalidConfig";
        case PublicRestError::Resolve: return "Resolve";
        case PublicRestError::Connect: return "Connect";
        case PublicRestError::TlsHandshake: return "TlsHandshake";
        case PublicRestError::Write: return "Write";
        case PublicRestError::Read: return "Read";
        case PublicRestError::HttpStatus: return "HttpStatus";
    }
    return "?";
}

// See "WHICH BARS ARE CLOSED" in the file header.
inline constexpr std::int64_t kKlinesClosedBarMarginMs = 1000;

struct PublicRestResponse {
    PublicRestError error{PublicRestError::None};
    // The status code whenever a response line was actually received (including non-200, where the
    // caller distinguishes 418/429 -- rate-limit/ban, back off hard -- from a plain 5xx); else 0.
    int http_status{0};
    std::string body;  // only populated when error == None
};

namespace detail {

inline PublicRestResponse public_rest_failure(PublicRestError error, int http_status = 0) {
    PublicRestResponse r;
    r.error = error;
    r.http_status = http_status;
    return r;
}

// Path + query, origin-form. Beast serialises the target verbatim, so a CR/LF/space in here would
// inject into our own request line -- reject every control character and space.
inline bool is_valid_public_rest_target(std::string_view target) {
    if (target.empty() || target.size() > 2048 || target.front() != '/') return false;
    for (char c : target) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20U || u == 0x7FU) return false;
    }
    return true;
}

inline net::awaitable<PublicRestResponse> fetch_public_body_coro(EndpointPermit permit, std::string target,
                                                                  PublicRestConfig cfg, RestTestSeam seam) {
    using namespace boost::asio::experimental::awaitable_operators;

    PublicRestError current_stage = PublicRestError::Resolve;
    try {
        // The host is the permit's and nobody else's: SNI, hostname verification, the Host header and
        // (unless a test seam redirects the TCP connection) the connect target all use it.
        const std::string host(permit.host());
        auto executor = co_await net::this_coro::executor;

        current_stage = PublicRestError::TlsHandshake;  // covers ssl_ctx setup below too
        ssl::context ssl_ctx(ssl::context::tlsv12_client);
        hy::configure_binance_ssl_context(ssl_ctx);
        if (!seam.extra_trusted_ca_pem_path().empty()) {
            ssl_ctx.load_verify_file(seam.extra_trusted_ca_pem_path());
        }

        tcp::resolver resolver(executor);
        beast::ssl_stream<beast::tcp_stream> stream(executor, ssl_ctx);

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            co_return public_rest_failure(PublicRestError::TlsHandshake);
        }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        hy::configure_binance_hostname_verification(stream, host);

        // Resolve, racing a 5s timer -- beast::tcp_stream::expires_after() does not cover
        // tcp::resolver, so this one stage needs its own race (same as the depth fetcher).
        current_stage = PublicRestError::Resolve;
        const std::string& connect_host =
            seam.connect_host_override().empty() ? host : seam.connect_host_override();
        net::steady_timer resolve_timer(executor);
        resolve_timer.expires_after(std::chrono::seconds(5));
        auto resolve_result =
            co_await (resolver.async_resolve(connect_host, cfg.port, net::use_awaitable) ||
                      resolve_timer.async_wait(net::use_awaitable));
        if (resolve_result.index() == 1) {  // timer won the race
            co_return public_rest_failure(PublicRestError::Resolve);
        }
        auto results = std::get<0>(resolve_result);

        current_stage = PublicRestError::Connect;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await beast::get_lowest_layer(stream).async_connect(results, net::use_awaitable);

        current_stage = PublicRestError::TlsHandshake;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);

        http::request<http::empty_body> req{http::verb::get, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "HengYuan/0.1");

        current_stage = PublicRestError::Write;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await http::async_write(stream, req, net::use_awaitable);

        current_stage = PublicRestError::Read;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.header_limit(static_cast<std::uint32_t>(8 * 1024));
        parser.body_limit(static_cast<std::uint64_t>(1 * 1024 * 1024));
        co_await http::async_read(stream, buffer, parser, net::use_awaitable);
        auto res = parser.release();

        // One-shot request -- close the socket directly rather than a graceful TLS close_notify
        // shutdown, which would need its own deadline to stay bounded.
        beast::get_lowest_layer(stream).close();

        const int status = static_cast<int>(res.result_int());
        if (res.result() != http::status::ok) {
            co_return public_rest_failure(PublicRestError::HttpStatus, status);
        }
        co_return PublicRestResponse{PublicRestError::None, status, std::move(res.body())};
    } catch (const boost::system::system_error&) {
        co_return public_rest_failure(current_stage);
    }
}

}  // namespace detail

// Fetches `target` (origin-form path + query) over TLS from the host of `binding`, provided that host
// passes the binding's endpoint allowlist (SAFE-01). Never throws for network or protocol failures; the
// failing stage is in `error`, and `body` is only set on success.
inline PublicRestResponse fetch_public_body(const EnvironmentBinding& binding, const std::string& target,
                                            const PublicRestConfig& cfg, const RestTestSeam& seam = {}) {
    // The permit proves allowlist membership, not that a corrupt allowlist did not list something that is no
    // hostname, so the host's syntax is checked as well.
    const auto permit = issue_endpoint_permit(binding.transport_policy(), binding.base_host());
    if (!permit || !detail::is_valid_rest_host(permit->host()) || cfg.port.empty() ||
        !detail::is_valid_public_rest_target(target)) {
        return detail::public_rest_failure(PublicRestError::InvalidConfig);
    }

    net::io_context ioc;
    bool completed = false;
    PublicRestResponse outcome;
    net::co_spawn(ioc, detail::fetch_public_body_coro(*permit, target, cfg, seam),
                  [&](std::exception_ptr eptr, PublicRestResponse r) {
                      // The coroutine catches boost::system::system_error itself; anything that
                      // escapes here (e.g. std::bad_alloc) is genuinely exceptional and propagates.
                      if (eptr) std::rethrow_exception(eptr);
                      outcome = std::move(r);
                      completed = true;
                  });
    ioc.run();

    // Every stage is deadline-bounded so the coroutine always completes; if that ever stops being
    // true, "no completion" must read as a failure, not as an empty success.
    if (!completed) return detail::public_rest_failure(PublicRestError::Read);
    return outcome;
}

// The request target for a klines fetch, or "" if symbol/interval/limit are not acceptable.
// `symbol` must be UPPERCASE (REST convention; the WS stream name is the lowercase one), `interval`
// one of Binance's documented set, `limit` in [1, kMaxBackfillBars].
inline std::string make_klines_target(std::string_view symbol, std::string_view interval, int limit) {
    if (!detail::is_valid_rest_symbol(symbol) || !is_valid_kline_interval(interval) || limit < 1 ||
        limit > static_cast<int>(kMaxBackfillBars)) {
        return {};
    }
    return "/api/v3/klines?symbol=" + std::string(symbol) + "&interval=" + std::string(interval) +
           "&limit=" + std::to_string(limit);
}

struct KlinesFetchResult {
    PublicRestError transport{PublicRestError::None};
    KlinesParseError parse{KlinesParseError::None};
    int http_status{0};  // see PublicRestResponse::http_status
    bool ok() const noexcept {
        return transport == PublicRestError::None && parse == KlinesParseError::None;
    }
};

// Fetch + parse. `now_ms` must return the exchange-time-corrected wall clock in epoch ms; it is
// called exactly once, BEFORE the request goes out (see "WHICH BARS ARE CLOSED" above), and a
// non-positive or implausibly small reading is an InvalidConfig rather than a silent "everything is
// forming". On any failure `out` must not be used.
inline KlinesFetchResult fetch_klines_backfill(const EnvironmentBinding& binding, std::string_view symbol,
                                                std::string_view interval, int limit, std::uint32_t symbol_id,
                                                const PublicRestConfig& cfg,
                                                const std::function<std::int64_t()>& now_ms,
                                                KlineBackfill& out, const RestTestSeam& seam = {}) {
    KlinesFetchResult result;
    out.count = 0;
    out.dropped_unclosed_tail = false;

    const std::string target = make_klines_target(symbol, interval, limit);
    if (target.empty() || !now_ms) {
        result.transport = PublicRestError::InvalidConfig;
        return result;
    }

    const std::int64_t now = now_ms();
    if (now <= kKlinesClosedBarMarginMs) {  // also keeps the subtraction below overflow-free
        result.transport = PublicRestError::InvalidConfig;
        return result;
    }
    const std::int64_t closed_before_ms = now - kKlinesClosedBarMarginMs;

    const PublicRestResponse response = fetch_public_body(binding, target, cfg, seam);
    result.transport = response.error;
    result.http_status = response.http_status;
    if (response.error != PublicRestError::None) return result;

    result.parse = parse_klines_response(response.body, closed_before_ms, symbol_id,
                                          kline_interval_span_ms(interval), out);
    return result;
}

// One backfill attempt as SingleFlightFetchGate carries it: the transport/parse status AND the bars
// in a single value. A KlineBackfill is ~64 KB and lives in the gate's mailbox, so the gate itself
// belongs on the heap or in a long-lived object.
struct KlinesBackfillOutcome {
    KlinesFetchResult status;
    KlineBackfill data;

    bool ok() const noexcept { return status.ok(); }
    std::span<const KlineWsEvent> bars() const noexcept { return {data.bars.data(), data.count}; }
};

// The Fetcher body for SingleFlightFetchGate<KlinesBackfillRequest, KlinesBackfillOutcome>. It runs on
// the gate's WORKER thread, so `binding` and `cfg` must be values (copied into the closure; a binding is
// trivially copyable) and `now_ms` may read only thread-safe state -- e.g. a ClockOffsetPublisher snapshot
// plus the wall clock -- never anything the hot thread mutates.
inline KlinesBackfillOutcome fetch_klines_backfill_outcome(const EnvironmentBinding& binding,
                                                            const KlinesBackfillRequest& request,
                                                            const PublicRestConfig& cfg,
                                                            const std::function<std::int64_t()>& now_ms,
                                                            const RestTestSeam& seam = {}) {
    KlinesBackfillOutcome outcome;
    outcome.status = fetch_klines_backfill(binding, request.symbol, request.interval,
                                            static_cast<int>(request.limit), request.symbol_id, cfg, now_ms,
                                            outcome.data, seam);
    return outcome;
}

// How long to leave the endpoint alone after `outcome` (the gate's CooldownFn). 0 on success. 429 (rate
// limit) and 418 (an IP ban) are the exchange telling us to stop; asking again too soon would escalate
// a ban. The numbers have no spec basis -- the response's Retry-After header is not parsed -- and are a
// starting point to revisit with operational experience.
inline constexpr std::int64_t kKlinesBackfillFailureCooldownMs = 5'000;
inline constexpr std::int64_t kKlinesBackfillRateLimitCooldownMs = 60'000;
inline constexpr std::int64_t kKlinesBackfillBanCooldownMs = 300'000;

inline std::int64_t klines_backfill_cooldown_ms(const KlinesBackfillOutcome& outcome) noexcept {
    if (outcome.ok()) return 0;
    if (outcome.status.http_status == 418) return kKlinesBackfillBanCooldownMs;
    if (outcome.status.http_status == 429) return kKlinesBackfillRateLimitCooldownMs;
    return kKlinesBackfillFailureCooldownMs;
}

}  // namespace hy
