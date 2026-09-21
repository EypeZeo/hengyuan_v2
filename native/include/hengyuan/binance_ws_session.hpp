// SPDX-License-Identifier: proprietary
// binance_ws_session.hpp — Boost.Beast async WebSocket session for Binance public WS
// (P2-MD-02 / Track C).
// Governance: L2/L4. Public WS only, no token/HMAC/Private API.
// Real network I/O, real TLS -- verified against the real endpoint via
// native/src/binance_connectivity_smoke.cpp (manual, not part of CI). VPS manual verification
// = L4.
//
// This is a fail-stop session, not a reconnecting client: any failure (resolve/connect/TLS
// handshake/WS handshake/read error, idle timeout, or an explicit stop() call) permanently
// stops the session -- there is no automatic reconnect, and none of its configuration/stats
// claim otherwise. A caller that wants to retry must construct a new BinanceWsSession instance.
// A full reconnect state machine (epoch-based lifecycle, backoff, generation-guarded stale
// completions) is deliberately out of scope for this round -- see the P2-MD-02 plan's
// "用户决定 v2"/P0-4b notes for why, and what a future reconnect-capable version would need.
//
// Shutdown contract: call stop() then join the thread driving this session's io_context.
// Do NOT call io_context::stop() as part of that sequence -- stop() posts its cancellation
// work onto this session's strand, and io_context::stop() can make io_context::run() return
// before that posted work ever executes, leaving pending operations (and this object, kept
// alive via shared_from_this()) in limbo. Once cancellation has actually run and all pending
// operations have unwound (which stop()+join() guarantees, bounded by this class's own
// per-stage timeouts), stats_snapshot() is safe to call from any thread.

#pragma once

#include <hengyuan/binance_json_parser.hpp>
#include <hengyuan/binance_market_event.hpp>
#include <hengyuan/binance_tls.hpp>
#include <hengyuan/spsc_ring.hpp>

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace hy {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

struct WsSessionConfig {
    std::string host = "stream.binance.com";
    std::string port = "9443";
    std::string target = "/ws";
    std::vector<std::string> subscribe_streams;  // e.g. {"btcusdt@trade", "ethusdt@trade"}
};

namespace detail {

inline bool is_valid_ws_host(std::string_view host) {
    if (host.empty() || host.size() > 253) return false;
    for (char c : host) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// Binance stream names look like "btcusdt@trade" or "btcusdt@depth@100ms" -- 2 or 3
// '@'-separated lowercase-alnum segments, none empty.
inline bool is_valid_stream_name(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    auto is_seg_char = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    int at_count = 0;
    std::size_t seg_start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '@') {
            if (i == seg_start) return false;  // empty segment (leading/trailing/double '@')
            for (std::size_t j = seg_start; j < i; ++j) {
                if (!is_seg_char(s[j])) return false;
            }
            if (i < s.size()) ++at_count;
            seg_start = i + 1;
        }
    }
    return at_count == 1 || at_count == 2;
}

}  // namespace detail

// Pure, no-I/O config validation. Must be checked before start() ever issues a DNS lookup --
// a defined-but-never-called validator is not a real defense (see this module's own history:
// the earlier draft of this class defined an equivalent check but never actually wired it in).
inline bool validate_ws_config(const WsSessionConfig& cfg) {
    if (!detail::is_valid_ws_host(cfg.host)) return false;
    if (cfg.target.empty() || cfg.target[0] != '/') return false;
    std::size_t total_len = cfg.target.size();
    for (const auto& s : cfg.subscribe_streams) {
        if (!detail::is_valid_stream_name(s)) return false;
        total_len += s.size() + 1;
    }
    if (total_len > 2048) return false;
    return true;
}

struct WsSessionStats {
    std::uint64_t messages_received{0};
    std::uint64_t bytes_received{0};
    std::uint64_t parse_ok{0};
    std::uint64_t parse_failed{0};
    std::uint64_t parse_truncated_resync{0};  // audit MD-TRUNC-015
    std::uint64_t push_ok{0};
    std::uint64_t push_dropped{0};
    std::uint64_t errors{0};
    beast::error_code last_error_ec{};
    std::string last_error_stage;  // e.g. "resolve", "connect", "ssl_handshake", "read"
};

template <std::size_t RingSize = 65536>
class BinanceWsSession : public std::enable_shared_from_this<BinanceWsSession<RingSize>> {
    // Binance depth@100ms typically sends ≤40 levels (20 bids + 20 asks).
    // 512 gives generous headroom without stack pressure (~32KB).
    static constexpr std::size_t kMaxEventsPerMessage = 512;
    using StrandType = net::strand<net::io_context::executor_type>;

public:
    BinanceWsSession(net::io_context& ioc, ssl::context& ssl_ctx,
                     SpscRing<BinanceMarketEvent, RingSize>& ring,
                     BinanceJsonParser& parser,
                     WsSessionConfig config)
        : strand_(net::make_strand(ioc))
        , resolver_(strand_)
        , ws_(strand_, ssl_ctx)
        , resolve_timer_(strand_)
        , ring_(ring)
        , parser_(parser)
        , config_(std::move(config)) {}

    // Exactly-once: a second call is a silent no-op, not a second resolve attempt.
    void start() {
        if (started_.exchange(true, std::memory_order_relaxed)) return;
        if (!validate_ws_config(config_)) {
            fail(beast::error_code{}, "invalid_config");
            return;
        }
        net::co_spawn(strand_, resolve_coro(this->shared_from_this()), net::detached);
    }

    // Posts cancellation onto this session's strand and returns immediately -- see this file's
    // header comment for the required stop()+join() shutdown contract.
    void stop() {
        // Publish the caller's intent before queuing do_stop(). A completion can be dispatched
        // between these two operations; it must still recognise a requested stop as clean rather
        // than accounting operation_aborted as a network fault.
        stop_requested_.store(true, std::memory_order_release);
        net::post(strand_, [self = this->shared_from_this()] { self->do_stop(); });
    }

    bool stopped() const { return stop_.load(std::memory_order_relaxed); }

    // True once the WebSocket handshake has completed and the read loop has started -- NOT merely
    // when TCP/TLS came up. A plain relaxed flag, same as BinanceKlineWsSession::is_connected() and
    // BinanceUserDataWsSession::is_connected(): it is how PublicFeedSupervisor (public_feed_supervisor.hpp)
    // tells "still connecting" from "healthy" from any thread. Never cleared again -- a session that
    // later fails reports stopped(), and a replacement is a NEW session object.
    bool is_connected() const { return connected_.load(std::memory_order_relaxed); }

    // Safe to call from any thread once the driving io_context's thread has been joined
    // following stop() -- see this file's header comment. The internal mutex also makes this
    // safe to call while I/O is still in flight (it just won't reflect writes made after the
    // copy is taken), but that usage is not the intended/tested contract.
    WsSessionStats stats_snapshot() const {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        return stats_;
    }

private:
    bool is_expected_stop(beast::error_code ec) const noexcept {
        // Once stop() has published the caller's intent, every later completion belongs to that
        // cancellation episode. Depending on the Boost/OpenSSL combination, force-closing a
        // pending TLS handshake may report operation_aborted, stream_truncated, eof, or a
        // platform socket-close error; none represents a new session fault after the caller has
        // already chosen the terminal stop transition. A real error that reaches a handler
        // before stop() publishes its release-store still takes the normal fail-stop path.
        if (stop_requested_.load(std::memory_order_acquire)) {
            (void)ec;
            return true;
        }
        return stop_.load(std::memory_order_relaxed) && !ec;
    }

    static net::awaitable<void> resolve_coro(std::shared_ptr<BinanceWsSession> self) {
        using namespace boost::asio::experimental::awaitable_operators;
        self->resolve_timer_.expires_after(std::chrono::seconds(10));
        try {
            auto resolve_result = co_await (
                self->resolver_.async_resolve(self->config_.host, self->config_.port,
                                               net::use_awaitable) ||
                self->resolve_timer_.async_wait(net::use_awaitable));
            if (resolve_result.index() == 1) {
                self->on_resolve(beast::error_code(net::error::timed_out), {});
                co_return;
            }
            self->on_resolve(beast::error_code{}, std::get<0>(resolve_result));
        } catch (const boost::system::system_error& e) {
            self->on_resolve(e.code(), {});
        }
    }

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "resolve");
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(ws_).async_connect(
            results,
            beast::bind_front_handler(&BinanceWsSession::on_connect,
                                     this->shared_from_this()));
    }

    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "connect");
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(10));
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
        if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(),
                                      config_.host.c_str())) {
            return fail(beast::error_code(static_cast<int>(::ERR_get_error()),
                                          net::error::get_ssl_category()), "ssl_sni");
        }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        hy::configure_binance_hostname_verification(ws_.next_layer(), config_.host);
        ws_.next_layer().async_handshake(
            ssl::stream_base::client,
            beast::bind_front_handler(&BinanceWsSession::on_ssl_handshake,
                                     this->shared_from_this()));
    }

    void on_ssl_handshake(beast::error_code ec) {
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "ssl_handshake");
        // The WS-level timeout option (below) takes over -- disarm the tcp_stream-level timer
        // so the two mechanisms don't fight each other.
        beast::get_lowest_layer(ws_).expires_never();
        hy::configure_ws_stream(ws_, std::chrono::seconds(90),
                                 static_cast<std::size_t>(4 * 1024 * 1024));
        std::string target = config_.target;
        if (!config_.subscribe_streams.empty()) {
            target += "/";
            for (std::size_t i = 0; i < config_.subscribe_streams.size(); ++i) {
                if (i > 0) target += "/";
                target += config_.subscribe_streams[i];
            }
        }
        ws_.async_handshake(config_.host, target,
                            beast::bind_front_handler(&BinanceWsSession::on_handshake,
                                                     this->shared_from_this()));
    }

    void on_handshake(beast::error_code ec) {
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "ws_handshake");
        connected_.store(true, std::memory_order_relaxed);
        do_read();
    }

    void do_read() {
        if (stop_) return;
        buffer_.clear();
        ws_.async_read(buffer_,
                       beast::bind_front_handler(&BinanceWsSession::on_read,
                                                this->shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        // AUDIT WS-STAT-026: `if (ec || stop_) fail(ec, ...)` recorded a spurious
        // errors++ with an empty error_code on every clean stop(), so a normal
        // shutdown was indistinguishable from a real read failure in
        // stats_snapshot(). A requested stop is not an error.
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "read");

        auto data = buffer_.data();
        std::string_view sv(static_cast<const char*>(data.data()), data.size());
        std::uint64_t recv_ns = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());

        BinanceMarketEvent events[kMaxEventsPerMessage];
        std::size_t count = 0;
        auto pr = parser_.parse(sv, recv_ns, events, kMaxEventsPerMessage, count);

        // TruncatedResync is neither a success nor a parse failure: the message was
        // understood, but it carried more depth levels than one batch can hold, so
        // out_events[0] is a resync marker rather than the delta (audit
        // MD-TRUNC-015). It MUST still be pushed -- that marker is what drives
        // DepthManager back to Buffering -- and it is counted separately so the
        // condition is visible instead of hiding inside parse_ok.
        const bool has_events = (pr == ParseResult::Ok || pr == ParseResult::TruncatedResync);
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.messages_received;
            stats_.bytes_received += bytes_transferred;
            if (pr == ParseResult::Ok) {
                ++stats_.parse_ok;
            } else if (pr == ParseResult::TruncatedResync) {
                ++stats_.parse_truncated_resync;
            } else if (pr != ParseResult::EventIgnored) {
                ++stats_.parse_failed;
            }
        }

        if (has_events) {
            std::uint64_t local_push_ok = 0;
            std::uint64_t local_push_dropped = 0;
            for (std::size_t i = 0; i < count; ++i) {
                if (ring_.try_push(events[i])) {
                    ++local_push_ok;
                } else {
                    ++local_push_dropped;
                }
            }
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.push_ok += local_push_ok;
            stats_.push_dropped += local_push_dropped;
        }

        do_read();
    }

    // fail-stop: any failure permanently stops the session (no reconnect attempt). Records
    // diagnostics under the same mutex stats_snapshot() reads through, rather than discarding
    // them.
    void fail(beast::error_code ec, const char* what) {
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.errors;
            stats_.last_error_ec = ec;
            stats_.last_error_stage = what;
        }
        stop_.store(true, std::memory_order_relaxed);
    }

    // Runs on strand_ (posted via stop()). Cancelling here -- rather than just setting a flag
    // and waiting for the next I/O completion to notice -- means shutdown is bounded by
    // however fast Asio dispatches the resulting operation_aborted completions, not by
    // whichever per-stage timeout happened to be running.
    void do_stop() {
        stop_.store(true, std::memory_order_relaxed);
        resolver_.cancel();
        resolve_timer_.cancel();
        auto& socket = beast::get_lowest_layer(ws_).socket();
        beast::error_code ignored;
        socket.cancel(ignored);
        // tcp_stream::cancel() alone does not reliably interrupt an in-progress OpenSSL
        // handshake on every supported Boost/OpenSSL combination. Closing the transport is the
        // bounded shutdown backstop: it completes the SSL/WebSocket operation with
        // operation_aborted instead of leaving join() to wait for the stage deadline.
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
    }

    StrandType strand_;
    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    net::steady_timer resolve_timer_;
    beast::flat_buffer buffer_;
    SpscRing<BinanceMarketEvent, RingSize>& ring_;
    BinanceJsonParser& parser_;
    WsSessionConfig config_;
    mutable std::mutex stats_mutex_;
    WsSessionStats stats_{};
    std::atomic<bool> stop_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};
};

}  // namespace hy
