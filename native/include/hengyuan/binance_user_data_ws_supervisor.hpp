// SPDX-License-Identifier: proprietary
// binance_user_data_ws_supervisor.hpp — TODO 1A.4 batch 2: the real reconnect state machine
// BinanceUserDataWsSession itself deliberately does not implement (see that file's own header
// comment: fail-stop, zero reconnect logic inside the session class).
//
// WHY THIS IS ENTIRELY HOT-THREAD-OWNED, NO NEW CROSS-THREAD PRIMITIVE: BinanceUserDataWsSession
// is enable_shared_from_this -- dropping this supervisor's shared_ptr reference to a dead
// session does NOT destroy the object early; it stays alive via its own self-capture until its
// own posted cancellation genuinely completes on the strand. That means "construct a brand-new
// session per retry" needs no per-attempt join() -- only stopped() (a plain, always-safe-to-poll
// relaxed atomic, unlike stats_snapshot()'s "join first" convention) is read here, from
// whichever thread calls poll(). This file assumes poll() is always called from the SAME thread
// (the hot/submit thread, matching live_submit_reconcile_harness_demo.cpp's own topology: one
// io_context/ssl::context for the process's whole lifetime, one dedicated I/O thread running
// ioc.run(), the hot thread ticking this supervisor every loop iteration) -- the supervisor's
// OWN state (consecutive_failures_, next_retry_at_ms_, session_ itself) is never touched from
// more than that one thread, so it needs no synchronization of its own. The one piece of state
// that genuinely crosses threads is BinanceUserDataWsSession::stopped()/is_connected() --
// already-existing/already-added atomics on that class, not anything new here.
//
// Never terminates on repeated failure: a user-data stream exists solely so this process can
// discover fills it didn't cause itself (a stale/missed listenKey renewal, a transient network
// partition) -- going permanently silent is strictly worse than continuing to retry at the
// capped backoff interval forever. Contrast with InFlightRegistry's own
// OrderState::EscalatedToOperator: that exists because an AMBIGUOUS ORDER is a bounded, must-be-
// resolved problem a human has to look at; a WS connection that can heal itself is not that kind
// of problem, and this class deliberately has no equivalent "give up" state.

#pragma once

#include <hengyuan/binance_listen_key_publisher.hpp>
#include <hengyuan/binance_user_data_ws_session.hpp>

#include <cstdint>
#include <memory>

namespace hy {

// TODO 1A.4 batch 2: these numbers have no spec/doc basis (neither accepted spec covers a user-
// data WS reconnect curve at all -- see this batch's own plan) and are deliberately NOT copied
// from ReconcilePollPolicy's own defaults -- that policy governs cheap, single REST queries;
// this one governs a full DNS+TCP+TLS+WS-handshake attempt against a rate-limit-sensitive
// exchange endpoint, which needs a slower, gentler curve. Revisit once real operational
// experience exists.
struct UserDataWsReconnectPolicy {
    std::int64_t initial_backoff_ms{1000};
    std::uint32_t backoff_multiplier{2};
    std::int64_t max_backoff_ms{60000};
};

// base * multiplier^consecutive_failures_already_seen, saturating at max_backoff_ms, using the
// EXACT saturating-check-before-multiply loop shape reconcile_backoff_delay_ms() (order_tracker.hpp)
// already established as safe on this codebase's own toolchain -- deliberately not
// initial_backoff_ms * (1 << consecutive_failures), which is undefined behavior for a large
// enough shift count. `attempt` here means "how many additional multiplications beyond the
// initial backoff have already happened" (0 = the very first retry, waits initial_backoff_ms
// itself), matching reconcile_backoff_delay_ms()'s own `attempt` semantics exactly. Same
// degenerate-config fail-closed-to-max policy: a config with initial_backoff_ms<=0 or
// max_backoff_ms<=0 must still delay (never retry immediately), so it saturates to
// max_backoff_ms (or 0 if that itself is non-positive) rather than looping.
inline std::int64_t ws_reconnect_backoff_delay_ms(const UserDataWsReconnectPolicy& policy,
                                                   std::uint32_t attempt) noexcept {
    if (policy.initial_backoff_ms <= 0 || policy.max_backoff_ms <= 0) {
        return policy.max_backoff_ms > 0 ? policy.max_backoff_ms : 0;
    }
    std::int64_t interval = policy.initial_backoff_ms;
    if (interval > policy.max_backoff_ms) return policy.max_backoff_ms;
    const std::int64_t multiplier =
        policy.backoff_multiplier == 0 ? 1 : static_cast<std::int64_t>(policy.backoff_multiplier);
    for (std::uint32_t i = 0; i < attempt; ++i) {
        if (interval > policy.max_backoff_ms / multiplier) {
            return policy.max_backoff_ms;
        }
        interval *= multiplier;
    }
    return interval > policy.max_backoff_ms ? policy.max_backoff_ms : interval;
}

struct UserDataWsSupervisorStats {
    std::uint32_t consecutive_failures{0};
    std::uint64_t total_attempts{0};
    std::int64_t last_attempt_ms{0};
    std::int64_t last_connected_ms{0};   // 0 = never observed connected
    std::uint32_t last_listen_key_seq{0};
};

// THREAD OWNERSHIP: see this file's own header comment -- poll()/shutdown()/stats() must all be
// called from the same single thread for the entire lifetime of this object (the hot/submit
// thread). Not copyable/movable (holds a shared_ptr to a session bound to references this
// object also holds; default-generated special members are fine since there's nothing here
// that specifically forbids move, but this class is not designed to be relocated mid-lifetime,
// so no special member functions are declared either way -- keep usage to one long-lived
// instance per process, matching io_context/ssl::context's own "constructed once, lives for the
// process" lifetime in live_submit_reconcile_harness_demo.cpp).
class UserDataWsSessionSupervisor {
public:
    UserDataWsSessionSupervisor(net::io_context& ioc, ssl::context& ssl_ctx,
                                 UserDataWsEventRing& ring, UserDataJsonParser& parser,
                                 const ListenKeyPublisher& listen_key_pub,
                                 UserDataWsSessionConfig config,
                                 UserDataWsReconnectPolicy policy = {})
        : ioc_(ioc)
        , ssl_ctx_(ssl_ctx)
        , ring_(ring)
        , parser_(parser)
        , listen_key_pub_(listen_key_pub)
        , config_(std::move(config))
        , policy_(policy) {}

    // Non-blocking; call once per hot-loop tick. See this file's own header comment for the
    // full state machine reasoning.
    void poll(std::int64_t now_ms) noexcept {
        if (shutdown_) return;

        if (session_) {
            if (!session_->stopped()) {
                // Still running -- either mid-handshake or genuinely healthy. is_connected()
                // is the only signal that distinguishes those two; only a real, observed
                // success resets the backoff counter (a session that's merely still trying to
                // connect must not reset it -- that would defeat the whole point of backing
                // off).
                if (session_->is_connected()) {
                    consecutive_failures_ = 0;
                    last_connected_ms_ = now_ms;
                }
                return;
            }
            // Fail-stop discipline (BinanceUserDataWsSession's own contract): ANY stop, whether
            // it ever connected first or not, is a failure to react to here -- there is no
            // "stopped because we asked it to" case reachable from poll() (shutdown() handles
            // that separately, and sets shutdown_ before dropping the reference).
            session_.reset();
            ++consecutive_failures_;
            next_retry_at_ms_ = now_ms + ws_reconnect_backoff_delay_ms(
                                             policy_, consecutive_failures_ - 1);
            return;
        }

        // No live session: either this is the very first poll() ever (consecutive_failures_
        // still 0, attempt immediately, no backoff), or a prior attempt just failed and
        // next_retry_at_ms_ says how long to still wait.
        if (consecutive_failures_ > 0 && now_ms < next_retry_at_ms_) return;

        // Unconditionally re-load the CURRENT ListenKeyPublisher snapshot -- the only correct
        // way to guarantee a rotated/renewed key gets picked up (see this batch's own plan for
        // why comparing seq against a remembered value is unnecessary: seq is observability-
        // only here). BinanceUserDataWsSession::start() re-loads it again internally regardless
        // -- this call is purely for last_listen_key_seq_'s own diagnostic value.
        const ListenKeySnapshot snap = listen_key_pub_.load();
        last_listen_key_seq_ = snap.seq;

        ++total_attempts_;
        last_attempt_ms_ = now_ms;
        session_ = std::make_shared<BinanceUserDataWsSession>(ioc_, ssl_ctx_, ring_, parser_,
                                                                listen_key_pub_, config_);
        session_->start();
    }

    // Idempotent; safe to call even if never poll()'d. Posts cancellation to the current
    // session (if any) then drops this object's own reference -- same "no join needed, the
    // object outlives the drop via shared_from_this()" reasoning as every retry inside poll().
    // A caller that needs a hard guarantee the I/O is fully quiesced before proceeding still
    // needs the harness's own stop()+join()-on-the-io_context's-thread sequence
    // (live_submit_reconcile_harness_demo.cpp) -- this method alone does not block.
    void shutdown() noexcept {
        shutdown_ = true;
        if (session_) {
            session_->stop();
            session_.reset();
        }
    }

    // Diagnostic-only accessor (e.g. for a demo/harness process to print the current attempt's
    // own message/error counters alongside this supervisor's retry-level stats() below) --
    // not used by poll()/shutdown() themselves. May be null (no live attempt in flight, e.g.
    // mid-backoff-wait). session_->stats_snapshot() is itself mutex-guarded and safe to call
    // from this thread regardless of what the session's own I/O thread is doing.
    const BinanceUserDataWsSession* current_session() const noexcept { return session_.get(); }

    UserDataWsSupervisorStats stats() const noexcept {
        UserDataWsSupervisorStats s{};
        s.consecutive_failures = consecutive_failures_;
        s.total_attempts = total_attempts_;
        s.last_attempt_ms = last_attempt_ms_;
        s.last_connected_ms = last_connected_ms_;
        s.last_listen_key_seq = last_listen_key_seq_;
        return s;
    }

private:
    net::io_context& ioc_;
    ssl::context& ssl_ctx_;
    UserDataWsEventRing& ring_;
    UserDataJsonParser& parser_;
    const ListenKeyPublisher& listen_key_pub_;
    UserDataWsSessionConfig config_;
    UserDataWsReconnectPolicy policy_;

    std::shared_ptr<BinanceUserDataWsSession> session_;
    bool shutdown_{false};
    std::uint32_t consecutive_failures_{0};
    std::uint64_t total_attempts_{0};
    std::int64_t last_attempt_ms_{0};
    std::int64_t last_connected_ms_{0};
    std::uint32_t last_listen_key_seq_{0};
    std::int64_t next_retry_at_ms_{0};
};

}  // namespace hy
