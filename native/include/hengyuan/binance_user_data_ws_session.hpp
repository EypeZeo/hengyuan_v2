// SPDX-License-Identifier: proprietary
// binance_user_data_ws_session.hpp — TODO 1A.4: Boost.Beast async WebSocket session for
// Binance's private spot user-data stream (/ws/<listenKey>).
//
// Governance: L4 (real network I/O; carries no HMAC/signature itself -- the WS connection is
// authenticated purely by the listenKey path, obtained via a separate, header-only,
// USER_STREAM-type REST call in binance_private_rest.hpp. This session never touches
// BoundHmacCredentials directly -- see binance_listen_key_publisher.hpp's own header comment
// for why that separation is load-bearing, not a convention).
//
// Deliberately a SEPARATE class from BinanceWsSession (binance_ws_session.hpp), not that class
// genericized in place:
//   - parser_ there is a concrete BinanceJsonParser& producing BinanceMarketEvent -- hard-typed,
//     not a template parameter. This session's narrow e/c/i parser (below) produces a different
//     event type (UserDataWsEvent) entirely.
//   - BinanceWsSession is a working, governed L2/L4 PUBLIC-data component already used in
//     production-adjacent demos; genericizing it in place to serve a feature that doesn't need
//     most of what it configures (subscribe_streams/is_valid_stream_name combined-stream-name
//     validation) risks destabilizing it for zero benefit to the public path.
//   - WsSessionConfig::target there is a fixed std::string set once at construction; this
//     session's target (/ws/<listenKey>) must be rebuilt from the CURRENT ListenKeyPublisher
//     snapshot at connect time, since the harness's supervisor (1A.4 plan's own reconnect
//     section) constructs a fresh session per retry specifically so a rotated/renewed listenKey
//     is picked up, not baked in at some earlier moment.
//
// Same fail-stop discipline as BinanceWsSession, deliberately NOT relaxed here: any failure
// (resolve/connect/TLS/WS-handshake/read error, idle timeout, explicit stop()) permanently
// stops this session -- no reconnect state machine inside this class. The harness/demo built
// around this session is the "supervisor" that constructs a brand-new instance on failure
// (fresh listenKey re-fetched first, since a stale key may itself be why the connection died),
// not this class healing itself. See binance_ws_session.hpp's own header comment for the full
// reasoning against building a half-finished reconnect state machine.
//
// Shutdown contract: IDENTICAL to BinanceWsSession's -- call stop() then join the thread
// driving this session's io_context. Do NOT call io_context::stop() as part of that sequence:
// stop() posts cancellation onto this session's strand, and io_context::stop() can make
// io_context::run() return before that posted work ever executes, leaving pending operations
// (and this object, kept alive via shared_from_this()) in limbo.
//
// JSON parsing scope (TODO 1A.4 batch 2 -- widened from the first slice's "e"/"E"/"c"/"i"
// only): now extracts the full executionReport field set (T/l/z/Z/L/S/X/x/f/q/p, alongside the
// original c/i/E) as raw decimal char[24] strings -- conversion to ticks needs a per-symbol
// scale this parser has no way to know (no SymbolRegistry access here), so it happens on the
// hot thread in drain_user_data_events() instead, via OrderFillContext (see that function's own
// header comment in binance_user_data_event.hpp). The double-apply hazard position_truth.hpp's
// own header comment warns about is guarded there too, via
// OrderFillContext::consume_delta() -- not solved in this parser, which stays a pure,
// side-effect-free field extractor.
//
// The event struct/ring this session pushes into, and the hot-thread drain function that
// consumes them, live in the lightweight binance_user_data_event.hpp, not here -- see that
// file's own header comment for why (avoiding a Boost.Beast/Asio/simdjson dependency on
// live_submit_orchestrator.hpp, which needs the drain function but not this session class).

#pragma once

#include <hengyuan/binance_listen_key_publisher.hpp>
#include <hengyuan/binance_tls.hpp>
#include <hengyuan/binance_user_data_event.hpp>  // UserDataWsEvent/Ring, drain_user_data_events()

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

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#endif
#include <simdjson.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>

namespace hy {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// --- Narrow JSON parser: "e"/"E"/"c"/"i" only, deliberately not full field-accurate parsing ---
// UserDataWsEvent/UserDataWsEventRing/drain_user_data_events() live in the lightweight
// binance_user_data_event.hpp (see this file's own header comment for why), not here.

enum class UserDataParseResult : std::uint8_t {
    Ok = 0,
    MalformedJson = 1,
    MissingEventType = 2,
};

// Zero-steady-state-allocation, matching binance_json_parser.hpp's own AUDIT PERF-ALLOC-012
// discipline exactly: a reused padded_buf_ that only grows (resize() is a no-op once the
// high-water mark is reached), never a fresh simdjson::padded_string per message. `out` is
// left at kind == Unknown/event_time_ms == 0 on any failure path -- never partially filled.
class UserDataJsonParser {
public:
    UserDataParseResult parse(std::string_view body, UserDataWsEvent& out) noexcept {
        out = UserDataWsEvent{};

        const std::size_t doc_len = body.size();
        if (padded_buf_.size() < doc_len + simdjson::SIMDJSON_PADDING) {
            try {
                padded_buf_.resize(doc_len + simdjson::SIMDJSON_PADDING);
            } catch (...) {
                return UserDataParseResult::MalformedJson;  // OOM on an attacker-sized message
            }
        }
        if (doc_len > 0) {
            std::memcpy(padded_buf_.data(), body.data(), doc_len);
        }
        std::memset(padded_buf_.data() + doc_len, 0, simdjson::SIMDJSON_PADDING);
        const simdjson::padded_string_view padded(padded_buf_.data(), doc_len, padded_buf_.size());

        simdjson::ondemand::document doc;
        if (parser_.iterate(padded).get(doc)) return UserDataParseResult::MalformedJson;

        std::string_view event_type;
        if (doc["e"].get_string().get(event_type) != simdjson::SUCCESS) {
            return UserDataParseResult::MissingEventType;
        }

        std::int64_t event_time_ms = 0;
        // "E" is present on every user-data-stream event type Binance documents; absence is
        // tolerated here (left at 0) rather than failing the whole parse over a field this
        // narrow slice doesn't act on -- only "e"/"c"/"i" are load-bearing for this batch.
        {
            std::int64_t v = 0;
            if (doc["E"].get_int64().get(v) == simdjson::SUCCESS) event_time_ms = v;
        }
        out.event_time_ms = event_time_ms;

        if (event_type == "executionReport") {
            out.kind = UserDataEventKind::ExecutionReport;

            // Field access ordered to roughly match Binance's own documented executionReport
            // JSON field order (e,E,s,c,S,o,f,q,p,P,F,g,C,x,X,r,i,l,z,L,n,N,T,t,I,w,m,M,O,Z,...)
            // -- simdjson's on-demand API supports out-of-order object field access, but forward
            // order avoids the extra rewind-and-rescan cost that would otherwise be paid on
            // every field. Every field here is best-effort/tolerant-of-absence, same discipline
            // already established for "E" above -- only "c" (coid) is load-bearing for
            // attribution; a missing/malformed decimal field leaves its char[24] buffer at its
            // default all-zero (empty string), which parse_decimal_to_ticks_with_scale()
            // (drain_user_data_events(), binance_user_data_event.hpp) already treats as a parse
            // failure via its own empty-string check, not a silently-wrong number.
            std::string_view coid_sv;
            if (doc["c"].get_string().get(coid_sv) == simdjson::SUCCESS && !coid_sv.empty() &&
                coid_sv.size() <= kClientOrderIdLen) {
                std::memcpy(out.coid.id, coid_sv.data(), coid_sv.size());
                out.coid.id[coid_sv.size()] = '\0';
            }

            std::string_view side_sv;
            if (doc["S"].get_string().get(side_sv) == simdjson::SUCCESS) {
                out.side_known = parse_binance_order_side(side_sv, out.side);
            }

            std::string_view tif_sv;
            if (doc["f"].get_string().get(tif_sv) == simdjson::SUCCESS) {
                out.tif_is_gtc = (tif_sv == "GTC");
            }

            std::string_view order_qty_sv;
            if (doc["q"].get_string().get(order_qty_sv) == simdjson::SUCCESS) {
                copy_raw_decimal_field(order_qty_sv, out.order_qty_raw);
            }
            std::string_view order_price_sv;
            if (doc["p"].get_string().get(order_price_sv) == simdjson::SUCCESS) {
                copy_raw_decimal_field(order_price_sv, out.order_price_raw);
            }

            std::string_view exec_type_sv;
            if (doc["x"].get_string().get(exec_type_sv) == simdjson::SUCCESS) {
                // Best-effort, audit-only (see ExecutionType's own header comment) -- an
                // unrecognized value leaves out.exec_type at its default Unknown, not a parse
                // failure for the whole event.
                (void)parse_binance_execution_type(exec_type_sv, out.exec_type);
            }

            std::string_view order_status_sv;
            if (doc["X"].get_string().get(order_status_sv) == simdjson::SUCCESS) {
                out.order_status_known = map_binance_order_status(order_status_sv, out.order_status);
            }

            std::int64_t order_id = 0;
            if (doc["i"].get_int64().get(order_id) == simdjson::SUCCESS) {
                out.exchange_order_id = order_id;
            }

            std::string_view last_qty_sv;
            if (doc["l"].get_string().get(last_qty_sv) == simdjson::SUCCESS) {
                copy_raw_decimal_field(last_qty_sv, out.last_qty_raw);
            }
            std::string_view cumulative_filled_qty_sv;
            if (doc["z"].get_string().get(cumulative_filled_qty_sv) == simdjson::SUCCESS) {
                copy_raw_decimal_field(cumulative_filled_qty_sv, out.cumulative_filled_qty_raw);
            }
            std::string_view last_price_sv;
            if (doc["L"].get_string().get(last_price_sv) == simdjson::SUCCESS) {
                copy_raw_decimal_field(last_price_sv, out.last_price_raw);
            }

            std::int64_t transaction_time_ms = 0;
            if (doc["T"].get_int64().get(transaction_time_ms) == simdjson::SUCCESS) {
                out.transaction_time_ms = transaction_time_ms;
            }

            std::string_view cumulative_quote_qty_sv;
            if (doc["Z"].get_string().get(cumulative_quote_qty_sv) == simdjson::SUCCESS) {
                copy_raw_decimal_field(cumulative_quote_qty_sv, out.cumulative_quote_qty_raw);
            }
        } else if (event_type == "outboundAccountPosition") {
            out.kind = UserDataEventKind::OutboundAccountPosition;
        } else if (event_type == "listenKeyExpired") {
            out.kind = UserDataEventKind::ListenKeyExpired;
        } else {
            out.kind = UserDataEventKind::Unknown;
        }
        return UserDataParseResult::Ok;
    }

private:
    // "Reject, don't truncate" (same discipline as binance_listen_key_publisher.hpp's own
    // publish()) -- an empty or overlong-for-char[24] decimal string is left as the field's
    // default all-zero/empty buffer rather than silently truncated into a shorter, wrong number.
    static void copy_raw_decimal_field(std::string_view sv, char (&out)[24]) noexcept {
        if (sv.empty() || sv.size() >= sizeof(out)) return;
        std::memcpy(out, sv.data(), sv.size());
    }

    simdjson::ondemand::parser parser_;
    std::string padded_buf_ = std::string(static_cast<std::size_t>(4 * 1024), '\0');
};

// --- Session config (host/port only -- target is derived from ListenKeyPublisher at connect
// time, see this file's own header comment) ---

struct UserDataWsSessionConfig {
    std::string host = "stream.binance.com";
    std::string port = "9443";
};

struct UserDataWsSessionStats {
    std::uint64_t messages_received{0};
    std::uint64_t bytes_received{0};
    std::uint64_t parse_ok{0};
    std::uint64_t parse_failed{0};
    std::uint64_t push_ok{0};
    std::uint64_t push_dropped{0};
    std::uint64_t errors{0};
    // TODO 1A.4 batch 2: listenKeyExpired is deliberately NOT wired to any immediate-reconnect
    // signal this batch (see binance_user_data_ws_supervisor.hpp's own header comment for why --
    // the normal fail-stop path already handles it, just not as fast as a dedicated channel
    // would). This counter is the cheap alternative: makes the event observable instead of
    // silently folded into the generic parse_ok count.
    std::uint64_t listen_key_expired_events{0};
    beast::error_code last_error_ec{};
    std::string last_error_stage;  // e.g. "resolve", "connect", "ssl_handshake", "read",
                                    // "no_listen_key" (this session's own addition -- see
                                    // resolve_coro())
};

class BinanceUserDataWsSession : public std::enable_shared_from_this<BinanceUserDataWsSession> {
    using StrandType = net::strand<net::io_context::executor_type>;

public:
    BinanceUserDataWsSession(net::io_context& ioc, ssl::context& ssl_ctx,
                              UserDataWsEventRing& ring, UserDataJsonParser& parser,
                              const ListenKeyPublisher& listen_key_pub,
                              UserDataWsSessionConfig config)
        : strand_(net::make_strand(ioc))
        , resolver_(strand_)
        , ws_(strand_, ssl_ctx)
        , resolve_timer_(strand_)
        , ring_(ring)
        , parser_(parser)
        , listen_key_pub_(listen_key_pub)
        , config_(std::move(config)) {}

    // Exactly-once: a second call is a silent no-op, not a second resolve attempt. Reads the
    // CURRENT ListenKeyPublisher snapshot synchronously, before ever issuing a DNS lookup --
    // an unpublished/never-set key fails closed immediately ("no_listen_key") rather than
    // resolving a host and handshaking a WS connection to a target with an empty/garbage path.
    void start() {
        if (started_.exchange(true, std::memory_order_relaxed)) return;
        if (config_.host.empty() || config_.port.empty()) {
            fail(beast::error_code{}, "invalid_config");
            return;
        }
        const ListenKeySnapshot snap = listen_key_pub_.load();
        if (snap.seq == 0 || snap.key_len == 0) {
            fail(beast::error_code{}, "no_listen_key");
            return;
        }
        target_ = "/ws/" + std::string(snap.view());
        net::co_spawn(strand_, resolve_coro(this->shared_from_this()), net::detached);
    }

    // Posts cancellation onto this session's strand and returns immediately -- see this file's
    // header comment for the required stop()+join() shutdown contract.
    void stop() {
        stop_requested_.store(true, std::memory_order_release);
        net::post(strand_, [self = this->shared_from_this()] { self->do_stop(); });
    }

    bool stopped() const { return stop_.load(std::memory_order_relaxed); }

    // TODO 1A.4 batch 2: the one new piece of cross-thread state this batch adds -- set once, on
    // the strand, right before the read loop starts (on_handshake()); polled from any thread
    // (the hot-thread-owned UserDataWsSessionSupervisor, binance_user_data_ws_supervisor.hpp).
    // A plain relaxed load, safe to call at any time, same as stopped() -- NOT the
    // "join first" convention stats_snapshot() carries (that one is about getting a
    // time-final snapshot, not about data-race safety). The supervisor needs this precisely to
    // distinguish "brand-new instance, DNS/TCP/TLS/WS-handshake still in flight" from "healthy,
    // reset the retry counter" -- see that file's own header comment.
    bool is_connected() const { return connected_.load(std::memory_order_relaxed); }

    // Safe to call from any thread once the driving io_context's thread has been joined
    // following stop() -- same contract as BinanceWsSession::stats_snapshot().
    UserDataWsSessionStats stats_snapshot() const {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        return stats_;
    }

private:
    bool is_expected_stop(beast::error_code ec) const noexcept {
        if (stop_requested_.load(std::memory_order_acquire)) {
            (void)ec;
            return true;
        }
        return stop_.load(std::memory_order_relaxed) && !ec;
    }

    static net::awaitable<void> resolve_coro(std::shared_ptr<BinanceUserDataWsSession> self) {
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
            beast::bind_front_handler(&BinanceUserDataWsSession::on_connect,
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
            beast::bind_front_handler(&BinanceUserDataWsSession::on_ssl_handshake,
                                     this->shared_from_this()));
    }

    void on_ssl_handshake(beast::error_code ec) {
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "ssl_handshake");
        beast::get_lowest_layer(ws_).expires_never();
        // Same idle-timeout/max-message-size/keep-alive-ping knobs as BinanceWsSession's own
        // call site -- these are fixed at the call site in that file too, not exposed via
        // either session's config struct.
        hy::configure_ws_stream(ws_, std::chrono::seconds(90),
                                 static_cast<std::size_t>(4 * 1024 * 1024));
        ws_.async_handshake(config_.host, target_,
                            beast::bind_front_handler(&BinanceUserDataWsSession::on_handshake,
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
                       beast::bind_front_handler(&BinanceUserDataWsSession::on_read,
                                                this->shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "read");

        auto data = buffer_.data();
        std::string_view sv(static_cast<const char*>(data.data()), data.size());

        UserDataWsEvent event{};
        const auto pr = parser_.parse(sv, event);

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.messages_received;
            stats_.bytes_received += bytes_transferred;
            if (pr == UserDataParseResult::Ok) {
                ++stats_.parse_ok;
                if (event.kind == UserDataEventKind::ListenKeyExpired) {
                    ++stats_.listen_key_expired_events;
                }
            } else {
                ++stats_.parse_failed;
            }
        }

        if (pr == UserDataParseResult::Ok) {
            // TODO 1A.4 follow-up (backpressure observability, external-review-verified gap):
            // mirrors BinanceWsSession::on_read()'s own push_ok/push_dropped accounting
            // (binance_ws_session.hpp) exactly, same field names -- a full-ring event must
            // never disappear silently, even in this narrow single-event-per-message slice.
            std::lock_guard<std::mutex> lock(stats_mutex_);
            if (ring_.try_push(event)) {
                ++stats_.push_ok;
            } else {
                ++stats_.push_dropped;
            }
        }

        do_read();
    }

    // fail-stop: any failure permanently stops the session (no reconnect attempt). Records
    // diagnostics under the same mutex stats_snapshot() reads through, rather than discarding
    // them -- identical discipline to BinanceWsSession::fail().
    void fail(beast::error_code ec, const char* what) {
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.errors;
            stats_.last_error_ec = ec;
            stats_.last_error_stage = what;
        }
        stop_.store(true, std::memory_order_relaxed);
    }

    // Runs on strand_ (posted via stop()). Identical to BinanceWsSession::do_stop() -- see that
    // function's own comment for why cancelling here (not just setting a flag) bounds shutdown
    // latency, and why closing the transport is the backstop for an in-progress TLS handshake
    // tcp_stream::cancel() alone cannot reliably interrupt.
    void do_stop() {
        stop_.store(true, std::memory_order_relaxed);
        resolver_.cancel();
        resolve_timer_.cancel();
        auto& socket = beast::get_lowest_layer(ws_).socket();
        beast::error_code ignored;
        socket.cancel(ignored);
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
    }

    StrandType strand_;
    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    net::steady_timer resolve_timer_;
    beast::flat_buffer buffer_;
    UserDataWsEventRing& ring_;
    UserDataJsonParser& parser_;
    const ListenKeyPublisher& listen_key_pub_;
    UserDataWsSessionConfig config_;
    std::string target_;  // built in start() from the ListenKeyPublisher snapshot at that moment
    mutable std::mutex stats_mutex_;
    UserDataWsSessionStats stats_{};
    std::atomic<bool> stop_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};
};

}  // namespace hy
