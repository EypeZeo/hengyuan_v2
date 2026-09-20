// SPDX-License-Identifier: proprietary
// binance_kline_ws_session.hpp — 批次 6 6a-1: Boost.Beast async WebSocket session for
// Binance's PUBLIC kline/candlestick stream ("/ws/<symbol>@kline_<interval>").
//
// Governance: L2/L4, same class as BinanceWsSession (binance_ws_session.hpp) -- public WS,
// no token/HMAC/credentials of any kind, no SymbolRegistry access.
//
// Deliberately a SEPARATE class from BinanceWsSession, not that class genericized in place --
// the same precedent binance_user_data_ws_session.hpp's own header comment already established
// for the private user-data stream applies here nearly verbatim:
//   - BinanceWsSession is hard-wired to BinanceJsonParser (binance_json_parser.hpp), which only
//     recognizes trade/aggTrade/depthUpdate payload shapes -- no kline support.
//   - binance_ws_session.hpp's own detail::is_valid_stream_name() (confirmed via direct read,
//     批次 6 计划已核实发现 2) only allows lowercase-alnum stream-name segment characters --
//     Binance's real kline stream name "<symbol>@kline_<interval>" (e.g. "btcusdt@kline_1m")
//     has an underscore in its second segment and would be REJECTED outright by that existing
//     validator. This session builds its own target string and never goes through
//     validate_ws_config()/is_valid_stream_name() at all -- it does not include
//     binance_ws_session.hpp.
//
// Same fail-stop discipline as BinanceWsSession/BinanceUserDataWsSession, deliberately NOT
// relaxed here: any failure (resolve/connect/TLS/WS-handshake/read error, idle timeout, explicit
// stop()) permanently stops this session -- no reconnect state machine inside this class.
//
// Shutdown contract: IDENTICAL to BinanceWsSession's -- call stop() then join the thread
// driving this session's io_context. Do NOT call io_context::stop() as part of that sequence
// (see binance_user_data_ws_session.hpp's own header comment for why).
//
// Two safety-critical design requirements, both from 批次 6 计划's first external-review round
// (verified independently before adoption -- see the plan's "第一轮外部（Gemini）复核意见处理
// 记录" section), both enforced inside this session, never left to the consumer to get right:
//
//   1. In-place is_closed filtering. Binance's "@kline_<interval>" stream pushes an update on
//      roughly every underlying trade while a candle is still open (~250ms-1000ms cadence during
//      active periods), not just once at close. KlineJsonParser classifies every message but only
//      ever returns KlineParseResult::Ok (the "push this to the ring" result) when "k"."x" ==
//      true; an unclosed-candle update comes back as KlineParseResult::Unclosed and is discarded
//      in on_read() BEFORE any ring push is attempted -- never relying on a downstream consumer
//      to filter it back out. A consumer-side filter would let a high-frequency stream of
//      unclosed updates fill and overwrite this session's deliberately small ring before the one
//      closed-candle event a consumer actually needs ever gets read. Same discipline
//      UserDataJsonParser already applies ("classify first, only push what the consumer needs"),
//      applied here to a much higher-frequency stream where it is load-bearing, not incidental.
//
//   2. Bar-gap (跳空) detection + suspend. A WS reconnect does not replay missed candles -- if
//      this stream disconnects at 14:01 and reconnects at 14:03, the next closed 1m candle this
//      session observes would silently follow the pre-disconnect one with two bars missing.
//      StreamingEvaluator's SMA/EMA/RSI state (strategy_spec_evaluator.hpp, 批次 5) all assume an
//      unbroken, equally-spaced bar sequence -- feeding it a gapped sequence would silently
//      corrupt that state with no way for the evaluator itself to detect it. This session tracks
//      prev_close_time_ms_ (strand-only state, written only from on_read()) and requires
//      `event.open_time_ms == prev_close_time_ms_ + 1` for every closed bar after the first
//      (Binance's own kline open_time/close_time convention: consecutive candles' boundaries are
//      exactly 1ms apart, e.g. a 1m candle closing at 00:00:59.999 is followed by one opening at
//      00:01:00.000 -- this holds for every interval, so no per-interval duration table is
//      needed). A mismatch flips is_suspended() true and the GAP-TRIGGERING bar itself is also
//      NOT pushed; every event observed while suspended is parsed (for stats/observability) but
//      neither the gap baseline nor the ring is touched again until resume_after_gap() is
//      explicitly called -- by a human operator, or by code that has already called
//      StreamingEvaluator::reset() and backfilled history via REST (批次 5's existing API; see
//      this session's own resume_after_gap() comment).

#pragma once

#include <hengyuan/binance_tls.hpp>
#include <hengyuan/kline_bar.hpp>
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

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#endif
#include <simdjson.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <charconv>
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

// KlineWsEvent, KlineWsEventRing, KlineBarGapGuard, KlineIngestResult and ingest_closed_bar()
// live in kline_bar.hpp (Boost-free, 批次 6 6b-0f) -- moved there verbatim so the REST backfill
// codec and the feed supervisor can use them without this header's Boost/Beast/OpenSSL weight.

enum class KlineParseResult : std::uint8_t {
    Ok = 0,             // closed candle ("k"."x" == true), event fully populated -- push it
    Unclosed = 1,        // "k"."x" == false -- deliberately not an error; on_read() must not push
    MalformedJson = 2,
    MissingFields = 3,   // wrong/absent "e", absent "k", or a required field inside "k" missing
};

// Zero-steady-state-allocation, matching UserDataJsonParser/binance_json_parser.hpp's own AUDIT
// PERF-ALLOC-012 discipline exactly: a reused padded_buf_ that only grows, never a fresh
// simdjson::padded_string per message. `out` is left at its default (is_closed == false) on any
// non-Ok path -- never partially filled in a way a caller could mistake for a real closed bar.
class KlineJsonParser {
public:
    KlineParseResult parse(std::string_view body, KlineWsEvent& out) noexcept {
        out = KlineWsEvent{};

        const std::size_t doc_len = body.size();
        if (padded_buf_.size() < doc_len + simdjson::SIMDJSON_PADDING) {
            try {
                padded_buf_.resize(doc_len + simdjson::SIMDJSON_PADDING);
            } catch (...) {
                return KlineParseResult::MalformedJson;  // OOM on an attacker-sized message
            }
        }
        if (doc_len > 0) {
            std::memcpy(padded_buf_.data(), body.data(), doc_len);
        }
        std::memset(padded_buf_.data() + doc_len, 0, simdjson::SIMDJSON_PADDING);
        const simdjson::padded_string_view padded(padded_buf_.data(), doc_len, padded_buf_.size());

        simdjson::ondemand::document doc;
        if (parser_.iterate(padded).get(doc)) return KlineParseResult::MalformedJson;

        std::string_view event_type;
        if (doc["e"].get_string().get(event_type) != simdjson::SUCCESS || event_type != "kline") {
            return KlineParseResult::MissingFields;
        }

        simdjson::ondemand::object k;
        if (doc["k"].get_object().get(k) != simdjson::SUCCESS) {
            return KlineParseResult::MissingFields;
        }

        // Field access ordered to match Binance's own documented kline "k" object field order
        // (t,T,s,i,f,L,o,c,h,l,v,n,x,q,V,Q,B; s/i/f/L/n are skipped over, not read) -- a single
        // forward pass, avoiding the rewind-and-rescan cost simdjson's on-demand API pays for an
        // out-of-order lookup. This means OHLCV is parsed even for an update this call will end
        // up returning Unclosed for (x is read last) -- deliberate: it keeps this parser a single
        // forward scan instead of optimizing for the common "will discard" case at the cost of a
        // backward-jump on every message that DOES turn out closed.
        std::int64_t open_time_ms = 0;
        std::int64_t close_time_ms = 0;
        if (k["t"].get_int64().get(open_time_ms) != simdjson::SUCCESS) {
            return KlineParseResult::MissingFields;
        }
        if (k["T"].get_int64().get(close_time_ms) != simdjson::SUCCESS) {
            return KlineParseResult::MissingFields;
        }

        double open_v = 0.0, high_v = 0.0, low_v = 0.0, close_v = 0.0, volume_v = 0.0;
        if (!parse_decimal_field(k, "o", open_v)) return KlineParseResult::MissingFields;
        if (!parse_decimal_field(k, "h", high_v)) return KlineParseResult::MissingFields;
        if (!parse_decimal_field(k, "l", low_v)) return KlineParseResult::MissingFields;
        if (!parse_decimal_field(k, "c", close_v)) return KlineParseResult::MissingFields;
        if (!parse_decimal_field(k, "v", volume_v)) return KlineParseResult::MissingFields;

        bool is_closed = false;
        if (k["x"].get_bool().get(is_closed) != simdjson::SUCCESS) {
            return KlineParseResult::MissingFields;
        }

        out.open_time_ms = open_time_ms;
        out.close_time_ms = close_time_ms;
        out.open = open_v;
        out.high = high_v;
        out.low = low_v;
        out.close = close_v;
        out.volume = volume_v;

        if (!is_closed) return KlineParseResult::Unclosed;
        out.is_closed = true;
        return KlineParseResult::Ok;
    }

private:
    // Binance's OHLCV fields are decimal strings, not JSON numbers (same reason
    // binance_decimal.hpp's ticks parser exists for the order path) -- std::from_chars is
    // allocation-free and locale-independent, matching CLAUDE.md §1/§5's zero-heap-alloc /
    // no-implicit-conversion discipline even though this parser is not itself on the
    // sub-microsecond order-submission hot path.
    static bool parse_decimal_field(simdjson::ondemand::object& obj, std::string_view key,
                                     double& out) noexcept {
        std::string_view sv;
        if (obj[key].get_string().get(sv) != simdjson::SUCCESS || sv.empty()) return false;
        double v = 0.0;
        const auto res = std::from_chars(sv.data(), sv.data() + sv.size(), v);
        if (res.ec != std::errc{}) return false;
        out = v;
        return true;
    }

    simdjson::ondemand::parser parser_;
    std::string padded_buf_ = std::string(static_cast<std::size_t>(4 * 1024), '\0');
};

// Host/symbol/interval only -- target ("/ws/<symbol>@kline_<interval>") is built in start().
// symbol_id is this session's caller-supplied SymbolRegistry id (see KlineWsEvent's own comment
// for why it is stamped here, not parsed).
struct KlineWsSessionConfig {
    std::string host = "stream.binance.com";
    std::string port = "9443";
    std::string symbol;    // lowercase, e.g. "btcusdt" -- validated in validate_kline_ws_config()
    std::string interval;  // e.g. "1m" -- validated against Binance's fixed interval set
    std::uint32_t symbol_id{0};
};

namespace detail {

inline bool is_valid_kline_ws_host(std::string_view host) {
    if (host.empty() || host.size() > 253) return false;
    for (char c : host) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

inline bool is_valid_kline_symbol(std::string_view s) {
    if (s.empty() || s.size() > 20) return false;
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

// Delegates to kline_bar.hpp's Boost-free hy::is_valid_kline_interval() (the same allow-list, now
// shared with the REST backfill codec).
inline bool is_valid_kline_interval(std::string_view s) { return hy::is_valid_kline_interval(s); }

}  // namespace detail

// Pure, no-I/O config validation. Must be checked before start() ever issues a DNS lookup --
// same "a defined-but-never-called validator is not a real defense" discipline
// binance_ws_session.hpp's own validate_ws_config() comment already documents.
inline bool validate_kline_ws_config(const KlineWsSessionConfig& cfg) {
    if (!detail::is_valid_kline_ws_host(cfg.host)) return false;
    if (!detail::is_valid_kline_symbol(cfg.symbol)) return false;
    if (!detail::is_valid_kline_interval(cfg.interval)) return false;
    return true;
}

struct KlineWsSessionStats {
    std::uint64_t messages_received{0};
    std::uint64_t bytes_received{0};
    std::uint64_t parse_ok{0};
    std::uint64_t parse_failed{0};
    std::uint64_t unclosed_skipped{0};    // "k"."x"==false updates discarded in-thread, never
                                           // reaching the ring -- this file's header comment,
                                           // requirement 1
    std::uint64_t push_ok{0};
    std::uint64_t push_dropped{0};
    std::uint64_t gap_detected_count{0};  // bar-gap continuity check failures -- requirement 2
    std::uint64_t ring_overflow_suspended{0};  // closed bar lost to a full ring -> suspended
                                                // (ingest_closed_bar()); also counted in push_dropped
    std::uint64_t errors{0};
    beast::error_code last_error_ec{};
    std::string last_error_stage;  // e.g. "resolve", "connect", "ssl_handshake", "read"
};

class BinanceKlineWsSession : public std::enable_shared_from_this<BinanceKlineWsSession> {
    using StrandType = net::strand<net::io_context::executor_type>;

public:
    BinanceKlineWsSession(net::io_context& ioc, ssl::context& ssl_ctx, KlineWsEventRing& ring,
                          KlineJsonParser& parser, KlineWsSessionConfig config)
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
        if (!validate_kline_ws_config(config_)) {
            fail(beast::error_code{}, "invalid_config");
            return;
        }
        target_ = "/ws/" + config_.symbol + "@kline_" + config_.interval;
        net::co_spawn(strand_, resolve_coro(this->shared_from_this()), net::detached);
    }

    // Posts cancellation onto this session's strand and returns immediately -- see this file's
    // header comment for the required stop()+join() shutdown contract.
    void stop() {
        stop_requested_.store(true, std::memory_order_release);
        net::post(strand_, [self = this->shared_from_this()] { self->do_stop(); });
    }

    bool stopped() const { return stop_.load(std::memory_order_relaxed); }

    bool is_connected() const { return connected_.load(std::memory_order_relaxed); }

    // Safe to poll from any thread at any time -- see resume_after_gap()'s own comment for why
    // the state this flag gates is only ever mutated on the strand.
    bool is_suspended() const { return suspended_.load(std::memory_order_relaxed); }

    // Clears the gap-suspended state and resets the gap-continuity baseline so the NEXT closed
    // bar this session observes becomes the new baseline, without re-checking it against the
    // stale prev-gap value. Safe to call from any thread -- posts onto strand_ rather than
    // mutating prev_close_time_ms_/have_prev_close_ directly, since those are strand-only state
    // written exclusively from on_read() (same "post onto the strand, don't touch cross-thread
    // state directly" discipline as stop()).
    //
    // Caller's responsibility (this session has no way to enforce it, and deliberately does not
    // try to): only call this after either a human operator has confirmed it is safe to resume,
    // or after calling StreamingEvaluator::reset() (批次 5, strategy_spec_evaluator.hpp) and
    // backfilling the missed history via REST -- see this file's own header comment,
    // requirement 2.
    void resume_after_gap() {
        net::post(strand_, [self = this->shared_from_this()] {
            self->gap_guard_.resume();
            self->suspended_.store(false, std::memory_order_relaxed);
        });
    }

    // Safe to call from any thread once the driving io_context's thread has been joined
    // following stop() -- same contract as BinanceWsSession::stats_snapshot().
    KlineWsSessionStats stats_snapshot() const {
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

    static net::awaitable<void> resolve_coro(std::shared_ptr<BinanceKlineWsSession> self) {
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
            beast::bind_front_handler(&BinanceKlineWsSession::on_connect,
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
            beast::bind_front_handler(&BinanceKlineWsSession::on_ssl_handshake,
                                     this->shared_from_this()));
    }

    void on_ssl_handshake(beast::error_code ec) {
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "ssl_handshake");
        beast::get_lowest_layer(ws_).expires_never();
        // Same idle-timeout/max-message-size knobs as BinanceWsSession/BinanceUserDataWsSession's
        // own call sites -- fixed here too, not exposed via the config struct.
        hy::configure_ws_stream(ws_, std::chrono::seconds(90),
                                 static_cast<std::size_t>(4 * 1024 * 1024));
        ws_.async_handshake(config_.host, target_,
                            beast::bind_front_handler(&BinanceKlineWsSession::on_handshake,
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
                       beast::bind_front_handler(&BinanceKlineWsSession::on_read,
                                                this->shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "read");

        auto data = buffer_.data();
        std::string_view sv(static_cast<const char*>(data.data()), data.size());

        KlineWsEvent event{};
        const auto pr = parser_.parse(sv, event);

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.messages_received;
            stats_.bytes_received += bytes_transferred;
            switch (pr) {
                case KlineParseResult::Ok:
                    ++stats_.parse_ok;
                    break;
                case KlineParseResult::Unclosed:
                    ++stats_.unclosed_skipped;
                    break;
                default:
                    ++stats_.parse_failed;
                    break;
            }
        }

        if (pr == KlineParseResult::Ok) {
            event.symbol_id = config_.symbol_id;
            handle_closed_bar(event);
        }

        do_read();
    }

    // Runs on strand_ (on_read()'s own completion-handler contract) -- delegates the actual
    // gap-continuity decision to gap_guard_ (KlineBarGapGuard, unit-tested independently in
    // test_binance_kline_ws_session.cpp) and only handles the ring-push/stats side here. gap_guard_
    // is strand-only state: touched only from here and from resume_after_gap()'s strand-posted
    // lambda -- never from any other thread.
    void handle_closed_bar(const KlineWsEvent& event) {
        switch (ingest_closed_bar(gap_guard_, ring_, event)) {
            case KlineIngestResult::Pushed: {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.push_ok;
                break;
            }
            case KlineIngestResult::GapDetected: {
                // This call is the one that just detected the gap -- counted exactly once, not on
                // every further discard while suspended (SuspendedEarlier below). The
                // gap-triggering bar itself is also NOT pushed -- see header comment.
                suspended_.store(true, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.gap_detected_count;
                break;
            }
            case KlineIngestResult::RingFull: {
                // A closed bar was lost, and the guard has already suspended itself -- mirror
                // that into the cross-thread-visible flag so a consumer polling is_suspended()
                // sees it. push_dropped keeps its existing meaning (any push that failed).
                suspended_.store(true, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.push_dropped;
                ++stats_.ring_overflow_suspended;
                break;
            }
            case KlineIngestResult::SuspendedEarlier:
                break;  // discard silently; already counted when the suspension began
        }
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

    // Runs on strand_ (posted via stop()). Identical to BinanceUserDataWsSession::do_stop() --
    // see that function's own comment for why cancelling here (not just setting a flag) bounds
    // shutdown latency.
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
    KlineWsEventRing& ring_;
    KlineJsonParser& parser_;
    KlineWsSessionConfig config_;
    std::string target_;  // built in start() from config_.symbol/config_.interval
    mutable std::mutex stats_mutex_;
    KlineWsSessionStats stats_{};
    std::atomic<bool> stop_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> suspended_{false};
    // Strand-only (on_read()/handle_closed_bar()/resume_after_gap()'s posted lambda) -- see
    // handle_closed_bar()'s own comment.
    KlineBarGapGuard gap_guard_;
};

}  // namespace hy
