// SPDX-License-Identifier: proprietary
// binance_ws_api_user_data_session.hpp — private-stream migration M2 (docs/PRIVATE_STREAM_MIGRATION_DESIGN.md §3, §4, §5.1): the
// Boost.Beast async WebSocket session for the spot WebSocket API's USER DATA stream
// (wss://<ws_api_host>:<port>/ws-api/v3, blueprint 5.7 P-24 to P-31). It replaces BinanceUserDataWsSession (binance_user_data_ws_session.hpp),
// whose /ws/<listenKey> path no longer exists on production spot (P-07) nor on the testnet (P-32); that class and its supervisor stay
// until the listenKey stack is retired (M5).
//
// Governance: L4 (real network I/O). This session NEVER touches credentials and never signs: the subscribe request is signed on the thread
// that owns BoundHmacCredentials (binance_ws_api_signing.hpp) and handed over through a one-shot mailbox (ws_api_subscribe_mailbox.hpp).
//
// What one session is: ONE connection and ONE subscription, i.e. one GENERATION (the number is assigned by the supervisor and stamped on
// every event the session delivers). Lifecycle, all on the session's strand:
//   resolve -> TCP connect -> TLS (SNI + hostname verification) -> WebSocket upgrade -> wait for the pre-signed subscribe frame ->
//   write it -> wait for the answer -> ask which subscriptions the connection holds (session.subscriptions) -> wait for that answer -> live.
// The read loop runs from the end of the upgrade on (a ping is answered only while a read is pending). The invariants the code enforces
// rather than hopes for:
//   1. The host comes from a WsEndpointPermit and nowhere else, and one WebSocket message is read at most permit.max_message_bytes(): the
//      permit exists only for a host on the policy's WebSocket-API allowlist. A connection that skipped the allowlist cannot be written
//      without the compiler noticing (tests/test_rest_freeze_guard.cpp pins the connection point).
//   2. The connection was counted against the local rate-limit budget: the constructor takes the RateLimitPermit of the connect, and the
//      subscribe frame is accepted only together with the RateLimitPermit of the subscribe request. A permit reserved for something cheaper
//      than the request is refused.
//   3. The subscribe frame is sent EXACTLY ONCE per connection and never replayed (audit E-06): the mailbox hands it out once, and a session
//      that ends is never revived — the supervisor builds a new one, whose factory signs again with a new timestamp.
//   4. A frame that waited so long since it was signed that the exchange would reject its timestamp (recvWindow, default 5000 ms) is NOT
//      sent: the session ends with "stale_subscribe_request" and the supervisor retries with a fresh signature, instead of the exchange
//      spending a request and a ban-relevant error on a doomed one.
//   5. NO event is accepted before the subscribe answer has been read, and the answer must be {"id":<ours>,"status":200,"result":
//      {"subscriptionId":N}}. Anything else — another status, no subscription id, another id, an event before the answer — ends the session
//      (fail-stop), and a rejected answer's status, error code and retryAfter (an absolute ms timestamp when the IP is rate limited or banned,
//      P-26) are recorded for whoever decides how long to wait.
//      Then the exchange is asked what it holds: session.subscriptions (no signature, so the session builds it itself, with the id after the
//      subscribe request's) and the stream counts as connected only when that answer is 200 and lists EXACTLY ONE subscription, the one just
//      confirmed (design 5.1; the exchange keeps one active subscription per connection and account, P-06/P-27). A list of none, of two, of
//      another number or with an entry that is not a subscription, another status or id, or no answer in time ends the session the same way.
//      Events that arrive between the two answers carry our subscription number and are delivered like any other; they only do not make
//      the stream "connected".
//   6. Every accepted event is stamped with this session's generation. An event that names a subscription that is not ours is dropped and
//      counted (serverShutdown names none: it is a connection event, and it is accepted whenever it arrives).
//   7. An executionReport the codec cannot read completely (no usable client order id, no cumulative quantity) is NOT delivered half-filled;
//      it is counted in lost_events(), which the reconnect barrier (M3) answers with a fresh reconcile sweep: a fill we could not read must
//      never be treated as a fill that did not happen.
//   8. serverShutdown and eventStreamTerminated are delivered to the ring FIRST and then end the generation (end_cause()), so the consumer
//      sees why; the supervisor starts the next one.
//   9. Any other failure (DNS, TCP, TLS, upgrade, read error or idle timeout, oversized message, write error, answer timeout) is fail-stop,
//      exactly like BinanceUserDataWsSession: no reconnect logic inside this class.
//
// is_connected() means "the subscription is confirmed by both answers" (the subscribe answer and the session.subscriptions listing), NOT
// "the TCP/WebSocket handshake is done" (that is handshake_done()): a connection that has not subscribed carries no events, and
// PublicFeedSupervisor's connect deadline then covers the whole path up to the verified stream. last_activity_steady_ms() is updated by
// every data frame AND every control frame (the server's 20 s ping is the only sign of life on a quiet account, P-30): the reconnect
// barrier's heartbeat check (M3, FI-020) reads it.
//
// Threads. All I/O handlers run on the session's strand on the ONE io_context thread (consecutive generations share the event ring and the
// codec, so they must share that thread — see public_feed_supervisor.hpp). Callable from any thread: stop(), stopped(), is_connected(),
// handshake_done(), subscription_id(), end_cause(), lost_events(), ring_drops(), pings_received(), last_activity_steady_ms(),
// stats_snapshot(). deposit_subscribe() is the producer side of the mailbox: call it from the credential-owning thread, before start() or
// after it. Shutdown contract identical to BinanceUserDataWsSession: stop() then join the thread driving the io_context; do not call
// io_context::stop() as part of that sequence.

#pragma once

#include <hengyuan/binance_tls.hpp>
#include <hengyuan/binance_user_data_event.hpp>  // UserDataWsEvent / UserDataWsEventRing
#include <hengyuan/binance_ws_api_codec.hpp>
#include <hengyuan/binance_ws_api_signing.hpp>
#include <hengyuan/spot_rate_limit_budget.hpp>
#include <hengyuan/transport_policy.hpp>
#include <hengyuan/ws_api_subscribe_mailbox.hpp>

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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace hy {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// The mailbox header is deliberately free of the signing header's dependencies, so the one number they share is checked here, where both are
// visible.
static_assert(kWsApiSubscribeFrameMaxBytes == kMaxWsApiFrameBytes, "a signed request must fit the mailbox exactly");

// One generation sends two requests on its connection: the subscribe request (id R, chosen by the factory) and, right after its answer,
// session.subscriptions (id R + 1, chosen here). The factory hands out request ids in steps of this many, so no two requests of one
// connection share an id and no id of one generation is reused by the next.
inline constexpr std::uint64_t kWsApiRequestIdsPerGeneration = 2;

struct WsApiUserDataSessionConfig {
    std::string port = "443";
    std::string target = "/ws-api/v3";  // EnvironmentBinding::ws_api_target()
    std::chrono::milliseconds resolve_timeout{10'000};
    std::chrono::milliseconds connect_timeout{10'000};  // TCP connect and TLS handshake each
    // After the upgrade: how long to wait for the mailbox to hold a frame. A producer that posts before start() makes this irrelevant; one that
    // posts later (just-in-time signing) must be done within it.
    std::chrono::milliseconds subscribe_wait{10'000};
    // After a request was written (the subscribe frame, then the session.subscriptions request): how long to wait for its answer. Each of
    // the two answers gets the whole time.
    std::chrono::milliseconds response_timeout{10'000};
    // A signed frame older than this (steady clock, since signing) is not sent. recvWindow is 5000 ms by default (TransportPolicy), and the
    // frame still has to cross the wire.
    std::chrono::milliseconds max_signed_age{4'000};
    // Beast's idle timeout. The server pings every 20 s and expects a pong within a minute (P-30); 90 s is also what the other sessions use.
    std::chrono::seconds idle_timeout{90};
};

// How a session ended. First cause wins; None while it is still running.
enum class WsApiSessionEnd : std::uint8_t {
    None = 0,
    Requested = 1,         // stop() — by the supervisor (a planned rollover, a connect deadline) or the owner
    Failed = 2,            // an error: see WsApiUserDataSessionStats::last_error_stage
    ServerShutdown = 3,    // the exchange announced it is closing this connection (P-29)
    StreamTerminated = 4,  // the exchange ended our subscription (eventStreamTerminated)
};

struct WsApiUserDataSessionStats {
    std::uint64_t messages_received{0};
    std::uint64_t bytes_received{0};
    std::uint64_t events_delivered{0};  // pushed into the ring
    std::uint64_t push_dropped{0};      // the ring was full
    std::uint64_t parse_failed{0};      // not a frame (malformed, unrecognized, no event type): counted, the session continues
    std::uint64_t unknown_events{0};    // a well-formed event of a type this build does not know
    std::uint64_t lost_events{0};       // executionReport the codec refused to return half-filled
    std::uint64_t foreign_subscription_events{0};
    std::uint64_t unexpected_responses{0};  // a response frame when none was awaited
    std::uint64_t server_shutdowns{0};
    std::uint64_t stream_terminations{0};
    std::uint64_t errors{0};
    // The subscribe answer, once there is one.
    std::int32_t subscribe_status{0};
    std::int64_t subscribe_error_code{0};
    // The session.subscriptions answer, once there is one: its status, its error code, and how many subscriptions it listed (at most
    // kMaxListedSubscriptions are counted).
    std::int32_t verify_status{0};
    std::int64_t verify_error_code{0};
    std::uint32_t subscriptions_listed{0};
    // Absolute timestamp (ms) from the exchange when EITHER answer was refused with a retryAfter; 0 = none.
    std::int64_t retry_after_ms{0};
    beast::error_code last_error_ec{};
    std::string last_error_stage;  // "resolve", "connect", "ssl_handshake", "ws_handshake", "read", "write", "subscribe_rejected", ...
};

class BinanceWsApiUserDataSession : public std::enable_shared_from_this<BinanceWsApiUserDataSession> {
    using StrandType = net::strand<net::io_context::executor_type>;

public:
    // connect_permit pays for the connection and verify_permit for the session.subscriptions request this session sends by itself once the
    // subscribe answer is in; the subscribe request's own permit travels with its frame (deposit_subscribe). start() refuses a permit that
    // was reserved for less than its request costs.
    BinanceWsApiUserDataSession(net::io_context& ioc, ssl::context& ssl_ctx, UserDataWsEventRing& ring, WsApiCodec& codec,
                                const WsEndpointPermit& permit, RateLimitPermit connect_permit, RateLimitPermit verify_permit,
                                std::uint64_t generation, WsApiUserDataSessionConfig config)
        : strand_(net::make_strand(ioc))
        , resolver_(strand_)
        , ws_(strand_, ssl_ctx)
        , resolve_timer_(strand_)
        , mailbox_timer_(strand_)
        , response_timer_(strand_)
        , ring_(ring)
        , codec_(codec)
        , host_(permit.host())
        , max_message_bytes_(permit.max_message_bytes())
        , connect_permit_(connect_permit)
        , verify_permit_(verify_permit)
        , generation_(generation)
        , config_(std::move(config)) {
        // One message never grows the buffer: the cap is known before the first byte (and Beast refuses a larger message while it streams in).
        const std::size_t reserve = max_message_bytes_ < kReserveCapBytes ? static_cast<std::size_t>(max_message_bytes_) : kReserveCapBytes;
        buffer_.reserve(reserve);
    }

    BinanceWsApiUserDataSession(const BinanceWsApiUserDataSession&) = delete;
    BinanceWsApiUserDataSession& operator=(const BinanceWsApiUserDataSession&) = delete;

    // Producer side of the mailbox. `request` is what build_subscribe_signature_request() returned; `permit` proves the subscribe request's
    // weight was reserved (a permit reserved for something cheaper, or one that is not a tracker reservation, is refused); `signed_at_steady_ms`
    // is the local steady-clock time at which signing finished. True exactly once per session.
    [[nodiscard]] bool deposit_subscribe(const WsApiSignedRequest& request, RateLimitPermit permit,
                                         std::int64_t signed_at_steady_ms = steady_now_ms()) noexcept {
        if (!permit_covers(permit, WsApiRequestKind::SubscribeSignature)) return false;
        WsApiSubscribeFrame frame{};
        const std::string_view text = request.frame();
        if (text.empty() || text.size() > frame.bytes.size()) return false;
        std::memcpy(frame.bytes.data(), text.data(), text.size());
        frame.length = static_cast<std::uint32_t>(text.size());
        frame.request_id = request.request_id();
        frame.signed_at_steady_ms = signed_at_steady_ms;
        return mailbox_.post(frame);
    }

    // Exactly-once: a second call is a silent no-op.
    void start() {
        if (started_.exchange(true, std::memory_order_relaxed)) return;
        if (host_.empty() || config_.port.empty() || config_.target.empty() || max_message_bytes_ == 0) {
            fail(beast::error_code{}, "invalid_config");
            return;
        }
        if (!permit_covers(connect_permit_, WsApiRequestKind::Connect)) {
            fail(beast::error_code{}, "connect_permit_too_small");
            return;
        }
        if (!permit_covers(verify_permit_, WsApiRequestKind::SessionSubscriptions)) {
            fail(beast::error_code{}, "verify_permit_too_small");
            return;
        }
        net::co_spawn(strand_, resolve_coro(this->shared_from_this()), net::detached);
    }

    // Posts cancellation onto the session's strand and returns immediately (see the header comment for the shutdown contract).
    void stop() {
        stop_requested_.store(true, std::memory_order_release);
        net::post(strand_, [self = this->shared_from_this()] { self->do_stop(); });
    }

    // True once the session has ended; end_cause() and stats_snapshot() then already say why (see claim_end()).
    [[nodiscard]] bool stopped() const noexcept { return stop_.load(std::memory_order_acquire); }
    // The subscription is confirmed by both answers (see the header comment). Events may already have been delivered by then: the ones that
    // arrived between the two answers.
    [[nodiscard]] bool is_connected() const noexcept { return subscribed_.load(std::memory_order_acquire); }
    // TCP, TLS and the WebSocket upgrade are done (the subscribe frame may not have been sent yet).
    [[nodiscard]] bool handshake_done() const noexcept { return handshake_done_.load(std::memory_order_relaxed); }
    // -1 until the subscribe answer was accepted (is_connected() additionally waits for the session.subscriptions listing).
    [[nodiscard]] std::int64_t subscription_id() const noexcept { return subscription_id_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] WsApiSessionEnd end_cause() const noexcept {
        return static_cast<WsApiSessionEnd>(end_cause_.load(std::memory_order_acquire));
    }

    // Monotonic counters for the reconnect barrier (M3): an executionReport we could not read completely, and an event the full ring refused.
    // Either one means "an order may have changed state without this process knowing": the answer is a reconcile sweep, not a shrug.
    [[nodiscard]] std::uint64_t lost_events() const noexcept { return lost_events_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t ring_drops() const noexcept { return ring_drops_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t pings_received() const noexcept { return pings_received_.load(std::memory_order_relaxed); }
    // Steady-clock ms of the last data or control frame; 0 = nothing yet.
    [[nodiscard]] std::int64_t last_activity_steady_ms() const noexcept { return last_activity_ms_.load(std::memory_order_relaxed); }

    // Safe from any thread (a time-final snapshot after the io thread has been joined, like the other sessions).
    [[nodiscard]] WsApiUserDataSessionStats stats_snapshot() const {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        return stats_;
    }

    [[nodiscard]] static std::int64_t steady_now_ms() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // A permit only covers a request if it is a real reservation (SpotRateLimitTracker::reserve_*) of at least that request's weight.
    [[nodiscard]] static bool permit_covers(const RateLimitPermit& p, WsApiRequestKind kind) noexcept {
        return p.basis() == RateLimitPermit::Basis::Reserved && p.weight() >= ws_api_weight(kind);
    }

private:
    static constexpr std::size_t kReserveCapBytes = 64 * 1024;
    static constexpr std::chrono::milliseconds kMailboxPollInterval{2};

    // Where the session is in its two-request conversation. Touched only on the strand.
    enum class Phase : std::uint8_t {
        Idle = 0,               // no request is out yet (connecting, or waiting for the subscribe frame)
        SubscribeSent = 1,      // the subscribe frame is out; its answer is awaited
        SubscribeAnswered = 2,  // accepted; the session.subscriptions request is not out yet (it waits for the subscribe write to complete)
        VerifySent = 3,         // the session.subscriptions request is out; its answer is awaited
        Live = 4,               // both answers accepted
    };

    // The subscribe answer has been accepted: from now on events of our subscription are legitimate.
    bool subscribe_answered() const noexcept {
        return phase_ == Phase::SubscribeAnswered || phase_ == Phase::VerifySent || phase_ == Phase::Live;
    }

    bool is_expected_stop(beast::error_code ec) const noexcept {
        if (stop_requested_.load(std::memory_order_acquire)) {
            (void)ec;
            return true;
        }
        return stop_.load(std::memory_order_relaxed) && !ec;
    }

    static net::awaitable<void> resolve_coro(std::shared_ptr<BinanceWsApiUserDataSession> self) {
        using namespace boost::asio::experimental::awaitable_operators;
        self->resolve_timer_.expires_after(self->config_.resolve_timeout);
        try {
            auto resolve_result = co_await (self->resolver_.async_resolve(self->host_, self->config_.port, net::use_awaitable) ||
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
        beast::get_lowest_layer(ws_).expires_after(config_.connect_timeout);
        beast::get_lowest_layer(ws_).async_connect(
            results, beast::bind_front_handler(&BinanceWsApiUserDataSession::on_connect, this->shared_from_this()));
    }

    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "connect");
        beast::get_lowest_layer(ws_).expires_after(config_.connect_timeout);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
        if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host_.c_str())) {
            return fail(beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()), "ssl_sni");
        }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        hy::configure_binance_hostname_verification(ws_.next_layer(), host_);
        ws_.next_layer().async_handshake(
            ssl::stream_base::client,
            beast::bind_front_handler(&BinanceWsApiUserDataSession::on_ssl_handshake, this->shared_from_this()));
    }

    void on_ssl_handshake(beast::error_code ec) {
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "ssl_handshake");
        beast::get_lowest_layer(ws_).expires_never();
        // The read cap comes out of the permit (invariant 1), not from a literal.
        hy::configure_ws_stream(ws_, config_.idle_timeout, static_cast<std::size_t>(max_message_bytes_));
        ws_.control_callback([this](websocket::frame_type kind, beast::string_view) noexcept { on_control(kind); });
        ws_.async_handshake(host_, config_.target,
                            beast::bind_front_handler(&BinanceWsApiUserDataSession::on_handshake, this->shared_from_this()));
    }

    void on_handshake(beast::error_code ec) {
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "ws_handshake");
        handshake_done_.store(true, std::memory_order_relaxed);
        touch_activity();
        mailbox_wait_started_ms_ = steady_now_ms();
        // Reading starts at once, before the subscribe frame is out: control frames (the server's ping) are only answered while a read is
        // pending, and a data frame arriving before we have subscribed is a protocol violation worth catching (invariant 5).
        do_read();
        poll_mailbox();
    }

    // Control frames are handled inside Beast's read operations; this only observes them.
    void on_control(websocket::frame_type kind) noexcept {
        touch_activity();
        if (kind == websocket::frame_type::ping) pings_received_.fetch_add(1, std::memory_order_relaxed);
    }

    void touch_activity() noexcept { last_activity_ms_.store(steady_now_ms(), std::memory_order_relaxed); }

    // --- the subscribe frame ---------------------------------------------------------------------------------

    void poll_mailbox() {
        if (stop_) return;
        WsApiSubscribeFrame frame{};
        if (mailbox_.take(frame)) {
            send_subscribe(frame);
            return;
        }
        if (steady_now_ms() - mailbox_wait_started_ms_ >= config_.subscribe_wait.count()) {
            fail(beast::error_code{}, "no_subscribe_request");
            return;
        }
        mailbox_timer_.expires_after(kMailboxPollInterval);
        mailbox_timer_.async_wait([self = this->shared_from_this()](beast::error_code ec) {
            if (ec) return;  // cancelled by a stop
            self->poll_mailbox();
        });
    }

    void send_subscribe(const WsApiSubscribeFrame& frame) {
        const std::int64_t age_ms = steady_now_ms() - frame.signed_at_steady_ms;
        if (age_ms > config_.max_signed_age.count()) {
            // Nothing is written: the exchange would answer with a timestamp error, and that is a request (and a rate-limit signal) wasted.
            return fail(beast::error_code{}, "stale_subscribe_request");
        }
        subscribe_frame_ = frame;
        request_id_ = frame.request_id;
        phase_ = Phase::SubscribeSent;
        ws_.text(true);
        ws_.async_write(net::buffer(subscribe_frame_.bytes.data(), subscribe_frame_.length),
                        beast::bind_front_handler(&BinanceWsApiUserDataSession::on_subscribe_written, this->shared_from_this()));
    }

    void on_subscribe_written(beast::error_code ec, std::size_t) {
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "write");
        subscribe_written_ = true;
        if (phase_ == Phase::SubscribeSent) {
            arm_response_timer();  // the answer is still to come
        } else if (phase_ == Phase::SubscribeAnswered) {
            // The answer overtook this completion handler. Beast allows one write at a time and counts a write as finished when its handler
            // has run, so the second request waited for exactly this moment.
            (void)send_verify();
        }
    }

    // --- the second request: what does the exchange say this connection holds? ---------------------------------------

    // On the strand. False = the session ended (the request could not be built).
    bool send_verify() {
        verify_request_id_ = request_id_ + 1;  // request_id_ came from a request the signing code accepted: far below the maximum
        auto [err, request] = build_session_subscriptions_request(verify_request_id_);
        if (err != WsApiSigningError::Ok) {
            fail(beast::error_code{}, "verify_request");
            return false;
        }
        verify_request_.emplace(request);  // the frame must outlive the async write that sends it
        const std::string_view text = verify_request_->frame();
        phase_ = Phase::VerifySent;
        ws_.text(true);
        ws_.async_write(net::buffer(text.data(), text.size()),
                        beast::bind_front_handler(&BinanceWsApiUserDataSession::on_verify_written, this->shared_from_this()));
        return true;
    }

    void on_verify_written(beast::error_code ec, std::size_t) {
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "write");
        if (phase_ == Phase::VerifySent) arm_response_timer();  // the answer is still to come
    }

    // The wait for the answer to the request that was just written. The handler remembers WHICH wait it is: a timer that expired a moment
    // before cancel() cannot be recalled, and its handler must not end the next wait.
    void arm_response_timer() {
        const Phase armed_for = phase_;
        response_timer_.expires_after(config_.response_timeout);
        response_timer_.async_wait([self = this->shared_from_this(), armed_for](beast::error_code wec) {
            if (wec) return;  // cancelled: the answer arrived, or a stop
            self->on_response_timeout(armed_for);
        });
    }

    void on_response_timeout(Phase armed_for) {
        if (stop_ || phase_ != armed_for) return;
        fail(beast::error_code{}, armed_for == Phase::SubscribeSent ? "subscribe_timeout" : "subscriptions_timeout");
    }

    // --- the read loop ---------------------------------------------------------------------------------------

    void do_read() {
        if (stop_) return;
        buffer_.clear();
        ws_.async_read(buffer_, beast::bind_front_handler(&BinanceWsApiUserDataSession::on_read, this->shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        if (is_expected_stop(ec)) return;
        if (ec || stop_) return fail(ec, "read");
        touch_activity();

        const auto data = buffer_.data();
        const std::string_view body(static_cast<const char*>(data.data()), data.size());
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.messages_received;
            stats_.bytes_received += bytes_transferred;
        }

        WsApiFrame frame{};
        const WsApiParseResult pr = codec_.parse(body, frame);
        switch (pr) {
            case WsApiParseResult::Ok:
                break;
            case WsApiParseResult::ExecutionReportIncomplete:
                lost_events_.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    ++stats_.lost_events;
                }
                do_read();
                return;
            case WsApiParseResult::FrameTooLarge:
                return fail(beast::error_code{}, "frame_too_large");  // cannot happen under the read cap; defensive
            default:  // malformed JSON, an unrecognized frame, an event without a type
                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    ++stats_.parse_failed;
                }
                do_read();
                return;
        }

        if (frame.kind == WsApiFrameKind::Response) {
            if (!handle_response(frame.response)) return;
        } else if (frame.kind == WsApiFrameKind::Event) {
            if (!handle_event(frame.event)) return;
        }
        do_read();
    }

    // False = the session ended (do not read again).
    bool handle_response(const WsApiResponse& r) {
        switch (phase_) {
            case Phase::SubscribeSent:
                return on_subscribe_answer(r);
            case Phase::VerifySent:
                // A repeat of the subscribe answer is a duplicate (counted and ignored, like any answer nobody awaits); any other id that is
                // not the awaited one ends the session in on_verify_answer().
                if (r.id_known && r.id == request_id_) return count_unexpected_response();
                return on_verify_answer(r);
            case Phase::Idle:
            case Phase::SubscribeAnswered:
            case Phase::Live:
                break;
        }
        return count_unexpected_response();
    }

    bool count_unexpected_response() {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.unexpected_responses;
        return true;
    }

    bool on_subscribe_answer(const WsApiResponse& r) {
        if (!r.id_known || r.id != request_id_) {
            fail(beast::error_code{}, "response_id_mismatch");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.subscribe_status = r.status;
            stats_.subscribe_error_code = r.error_code;
            if (r.retry_after_ms > 0) stats_.retry_after_ms = r.retry_after_ms;
        }
        if (r.status != 200 || !r.has_subscription_id || r.subscription_id < 0) {
            fail(beast::error_code{}, "subscribe_rejected");
            return false;
        }
        response_timer_.cancel();
        subscription_id_.store(r.subscription_id, std::memory_order_release);
        phase_ = Phase::SubscribeAnswered;
        // The second request goes out now, unless the subscribe write has not completed yet: then on_subscribe_written() sends it.
        return subscribe_written_ ? send_verify() : true;
    }

    // The answer to session.subscriptions: the stream is "connected" only if the exchange lists exactly the subscription it just confirmed.
    bool on_verify_answer(const WsApiResponse& r) {
        if (!r.id_known || r.id != verify_request_id_) {
            fail(beast::error_code{}, "response_id_mismatch");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.verify_status = r.status;
            stats_.verify_error_code = r.error_code;
            stats_.subscriptions_listed = r.subscription_count;
            if (r.retry_after_ms > 0) stats_.retry_after_ms = r.retry_after_ms;
        }
        if (!r.ok()) {
            fail(beast::error_code{}, "subscriptions_rejected");
            return false;
        }
        const bool exactly_ours = r.has_subscription_list && !r.subscription_list_truncated && !r.subscription_list_malformed &&
                                  r.subscription_count == 1 && r.subscription_ids[0] == subscription_id_.load(std::memory_order_relaxed);
        if (!exactly_ours) {
            fail(beast::error_code{}, "subscriptions_mismatch");
            return false;
        }
        response_timer_.cancel();
        phase_ = Phase::Live;
        subscribed_.store(true, std::memory_order_release);
        return true;
    }

    bool handle_event(UserDataWsEvent& ev) {
        // serverShutdown is a CONNECTION event: no subscription id, and valid at any moment, even before the answer (P-29).
        if (ev.kind != UserDataEventKind::ServerShutdown) {
            if (!subscribe_answered()) {
                fail(beast::error_code{}, "event_before_subscribe_response");
                return false;
            }
            if (ev.subscription_id != subscription_id_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.foreign_subscription_events;
                return true;
            }
        }
        if (ev.kind == UserDataEventKind::Unknown) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.unknown_events;
            return true;
        }

        ev.generation = generation_;
        const bool pushed = ring_.try_push(ev);
        if (!pushed) ring_drops_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            if (pushed) {
                ++stats_.events_delivered;
            } else {
                ++stats_.push_dropped;
            }
            if (ev.kind == UserDataEventKind::ServerShutdown) ++stats_.server_shutdowns;
            if (ev.kind == UserDataEventKind::EventStreamTerminated) ++stats_.stream_terminations;
        }
        if (ev.kind == UserDataEventKind::ServerShutdown) {
            end_with(WsApiSessionEnd::ServerShutdown);
            return false;
        }
        if (ev.kind == UserDataEventKind::EventStreamTerminated) {
            end_with(WsApiSessionEnd::StreamTerminated);
            return false;
        }
        return true;
    }

    // --- ending ----------------------------------------------------------------------------------------------

    // The first cause wins: true = this call set it, so this call alone decides how the session ended (a loser changes nothing, records
    // nothing). The cause is claimed BEFORE stop_ is raised, and whatever the winner records is written before stop_ too: a thread that sees
    // stopped() == true therefore also sees why (end_cause(), stats_snapshot()), and a failure that races a stop() cannot be recorded as one
    // while the cause says the other.
    bool claim_end(WsApiSessionEnd cause) noexcept {
        std::uint8_t expected = static_cast<std::uint8_t>(WsApiSessionEnd::None);
        return end_cause_.compare_exchange_strong(expected, static_cast<std::uint8_t>(cause), std::memory_order_acq_rel,
                                                  std::memory_order_acquire);
    }

    // Fail-stop: records the first failure only (the cancelled handlers of a session that is already ending must not add noise), then closes
    // the transport on the strand. Callable from any thread.
    void fail(beast::error_code ec, const char* what) {
        if (!claim_end(WsApiSessionEnd::Failed)) return;
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.errors;
            stats_.last_error_ec = ec;
            stats_.last_error_stage = what;
        }
        stop_.store(true, std::memory_order_release);
        net::post(strand_, [self = this->shared_from_this()] { self->close_transport(); });
    }

    // On the strand: the exchange ended this generation (not a failure).
    void end_with(WsApiSessionEnd cause) {
        if (!claim_end(cause)) return;
        stop_.store(true, std::memory_order_release);
        close_transport();
    }

    // On the strand, posted by stop().
    void do_stop() {
        (void)claim_end(WsApiSessionEnd::Requested);
        stop_.store(true, std::memory_order_release);
        close_transport();
    }

    // Cancelling here (not just setting a flag) bounds shutdown latency, and closing the transport is the backstop for an in-progress TLS
    // handshake that tcp_stream::cancel() alone cannot reliably interrupt (same reasoning as BinanceWsSession::do_stop()).
    void close_transport() {
        resolver_.cancel();
        resolve_timer_.cancel();
        mailbox_timer_.cancel();
        response_timer_.cancel();
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
    net::steady_timer mailbox_timer_;
    net::steady_timer response_timer_;
    beast::flat_buffer buffer_;
    UserDataWsEventRing& ring_;
    WsApiCodec& codec_;
    const std::string host_;  // a copy of permit.host(): the resolver and the SNI call need a NUL-terminated string
    const std::uint64_t max_message_bytes_;
    const RateLimitPermit connect_permit_;
    const RateLimitPermit verify_permit_;
    const std::uint64_t generation_;
    const WsApiUserDataSessionConfig config_;

    WsApiSubscribeMailbox mailbox_;

    // Touched only on the strand.
    WsApiSubscribeFrame subscribe_frame_{};  // must outlive the async write that sends it
    std::optional<WsApiUnsignedRequest> verify_request_;  // ditto
    std::uint64_t request_id_{0};
    std::uint64_t verify_request_id_{0};
    Phase phase_{Phase::Idle};
    bool subscribe_written_{false};  // the subscribe write's completion handler has run
    std::int64_t mailbox_wait_started_ms_{0};

    mutable std::mutex stats_mutex_;
    WsApiUserDataSessionStats stats_{};

    std::atomic<bool> stop_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> handshake_done_{false};
    std::atomic<bool> subscribed_{false};
    std::atomic<std::int64_t> subscription_id_{-1};
    std::atomic<std::uint8_t> end_cause_{static_cast<std::uint8_t>(WsApiSessionEnd::None)};
    std::atomic<std::uint64_t> lost_events_{0};
    std::atomic<std::uint64_t> ring_drops_{0};
    std::atomic<std::uint64_t> pings_received_{0};
    std::atomic<std::int64_t> last_activity_ms_{0};
};

}  // namespace hy
