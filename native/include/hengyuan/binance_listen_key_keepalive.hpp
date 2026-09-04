// SPDX-License-Identifier: proprietary
// binance_listen_key_keepalive.hpp — Batch H, H3: proactive listenKey keepalive/expiry
// scheduler, closing the gap left since TODO 1A.4: ListenKeySnapshot::expires_at_ms
// (binance_listen_key_publisher.hpp) has always been written but never read by anything --
// binance_user_data_ws_session.hpp::start()/binance_user_data_ws_supervisor.hpp only ever
// wait passively for Binance's own server-side kick once a listenKey truly expires.
//
// A sibling class to UserDataWsSessionSupervisor, deliberately NOT folded into its poll() --
// two reasons: (1) thread-ownership incompatibility -- keepalive_listen_key()/
// create_listen_key() need BoundHmacCredentials::copy_api_key(), which fail-closes off the
// credential-owning thread (binance_environment.hpp:180); UserDataWsSessionSupervisor::poll()
// is deliberately credential-unaware and has no other reason to hold a
// BinancePrivateRestClient reference. (2) disjoint timing signal -- keepalive timing is
// driven purely by ListenKeySnapshot.expires_at_ms vs now_ms, independent of whatever the WS
// connection's own state happens to be.
//
// THREAD OWNERSHIP (load-bearing, mirrors order_tracker.hpp's/spot_rate_limit_budget.hpp's own
// wording): poll() must be called from the exact thread that owns the BoundHmacCredentials
// bound to `client` -- the same hot/submit/reconcile thread every other credentialed call in
// this batch already shares. No internal synchronization here; this class is not
// thread-safe and isn't meant to be.
//
// ARCHITECTURAL BOUNDARY this class must accept, not fight (verified against
// binance_user_data_ws_supervisor.hpp:115-127 before this file was written): republishing a
// renewed/recreated key does NOT actively force an already-connected WS session to switch --
// UserDataWsSessionSupervisor::poll() only re-loads the ListenKeyPublisher snapshot when it is
// about to start a brand-new session (no live session, or the prior one just stopped); a
// currently-healthy session is left alone and never told to reconnect. The effect of this
// scheduler's own publish() calls is therefore passive/eventual: the supervisor has no
// reconnect()/refresh() API, only an irreversible shutdown(). This scheduler's job stops at
// "keep the current key alive before it expires, and have a fresh one waiting if it doesn't" --
// not at forcing any particular WS reconnect timing.

#pragma once

#include <hengyuan/binance_listen_key_publisher.hpp>
#include <hengyuan/binance_private_rest.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace hy {

struct ListenKeyKeepalivePolicy {
    // No Binance-documented number backs this repo's choice of interval (same "no spec/doc
    // basis" posture UserDataWsReconnectPolicy's own header comment already takes for its
    // backoff numbers) -- 30 minutes is half of the real ~60-minute listenKey expiry window,
    // a conservative safety margin, not a transcribed constant.
    std::int64_t keepalive_interval_ms{30 * 60 * 1000};
    std::int64_t retry_backoff_ms{5000};
    // How long a freshly (re)published key is considered valid -- used to compute the
    // expires_at_ms passed to ListenKeyPublisher::publish() after every successful keepalive
    // or recreation. Parameterized (not hardcoded at the publish() call site) specifically so
    // tests can shrink it to make the "expired -> fall back to create_listen_key()" path
    // reachable in milliseconds instead of a real hour.
    std::int64_t listen_key_ttl_ms{60 * 60 * 1000};

    // Defensive fallback accessors, mirroring PrivateRestConfig::effective_*_timeout_ms()
    // (Batch H, H2) -- poll() reads exclusively through these, never the raw fields directly,
    // so a <= 0 misconfiguration can't collapse the retry/keepalive cadence to 0ms and spin
    // the hot loop into a busy-poll.
    std::int64_t effective_interval_ms() const noexcept {
        return keepalive_interval_ms > 0 ? keepalive_interval_ms : 30 * 60 * 1000;
    }
    std::int64_t effective_backoff_ms() const noexcept {
        return retry_backoff_ms > 0 ? retry_backoff_ms : 5000;
    }
    std::int64_t effective_ttl_ms() const noexcept {
        return listen_key_ttl_ms > 0 ? listen_key_ttl_ms : 60 * 60 * 1000;
    }
};

// Mirrors UserDataWsSupervisorStats's own shape (binance_user_data_ws_supervisor.hpp) -- same
// "hot loop needs to be able to tell which of several outcomes just happened" motivation:
// without this, a test (or H6's own diagnostic printing) can't distinguish "did a normal
// keepalive fire", "did we fall back to recreating the key", or "are we still backed off".
struct ListenKeyKeepaliveStats {
    std::uint64_t keepalive_attempts{0};
    std::uint64_t keepalive_successes{0};
    std::uint64_t keepalive_failures{0};
    std::uint64_t recreation_attempts{0};
    std::uint64_t recreation_successes{0};
    std::uint64_t recreation_failures{0};
    std::int64_t last_attempt_ms{0};
    std::int64_t last_success_ms{0};
    std::uint32_t consecutive_failures{0};
    PrivateRestError last_error{PrivateRestError::None};
};

class ListenKeyKeepaliveScheduler {
public:
    ListenKeyKeepaliveScheduler(BinancePrivateRestClient& client,
                                 ListenKeyPublisher& listen_key_pub,
                                 ListenKeyKeepalivePolicy policy = {}) noexcept
        : client_(client), listen_key_pub_(listen_key_pub), policy_(policy) {}

    // Non-owning reference members + mutable runtime state (stats_/next_attempt_at_ms_/
    // last_seen_seq_) -- explicitly non-copyable/non-movable to rule out a second instance
    // ever aliasing the same client_/listen_key_pub_ with its own independent state (which
    // would produce duplicate keepalive traffic and two disagreeing seq-tracking histories).
    ListenKeyKeepaliveScheduler(const ListenKeyKeepaliveScheduler&) = delete;
    ListenKeyKeepaliveScheduler& operator=(const ListenKeyKeepaliveScheduler&) = delete;
    ListenKeyKeepaliveScheduler(ListenKeyKeepaliveScheduler&&) = delete;
    ListenKeyKeepaliveScheduler& operator=(ListenKeyKeepaliveScheduler&&) = delete;

    // THREAD OWNERSHIP: see this file's own header comment. Non-blocking in the sense every
    // other poll()-style component in this codebase means it (at most one REST round trip per
    // call, synchronous -- see binance_private_rest.hpp's own header comment on why these
    // calls each block for a full network round trip); call once per hot-loop tick.
    void poll(std::int64_t now_ms) noexcept;

    ListenKeyKeepaliveStats stats() const noexcept { return stats_; }

private:
    BinancePrivateRestClient& client_;
    ListenKeyPublisher& listen_key_pub_;
    ListenKeyKeepalivePolicy policy_;
    ListenKeyKeepaliveStats stats_{};
    std::int64_t next_attempt_at_ms_{0};
    // Named after (and same diagnostic-field convention as) UserDataWsSessionSupervisor's own
    // last_listen_key_seq_, but this one is load-bearing, not diagnostic-only: it is the sole
    // mechanism that tells poll() whether the seq it just observed is one THIS scheduler's own
    // publish() call produced, or a genuinely external rotation it has never seen before. Get
    // this wrong (e.g. never resync it after this scheduler's own successful publish()) and
    // every self-triggered seq bump gets misclassified as an external rotation, which recomputes
    // next_attempt_at_ms_ from the ORIGINAL snap.issued_at_ms every tick -- pinning the
    // "next attempt" time in the past forever and firing a real REST call on every single tick
    // from that point on. See this file's own poll() implementation comment for the full trace.
    std::uint32_t last_seen_seq_{0};
};

inline void ListenKeyKeepaliveScheduler::poll(std::int64_t now_ms) noexcept {
    const auto snap = listen_key_pub_.load();
    if (snap.seq == 0 || snap.key_len == 0) return;  // never published yet -- silent no-op

    // External-rotation detection. Must ONLY ever fire for a seq this scheduler did not itself
    // just produce -- every branch below that calls listen_key_pub_.publish() re-syncs
    // last_seen_seq_ immediately afterward specifically so this check never misfires on its
    // own writes (see this class's own last_seen_seq_ field comment for the full failure mode
    // if that resync is ever dropped).
    if (snap.seq != last_seen_seq_) {
        last_seen_seq_ = snap.seq;
        next_attempt_at_ms_ =
            std::max(now_ms, snap.issued_at_ms + policy_.effective_interval_ms());
        stats_.consecutive_failures = 0;
        return;  // don't act the same tick a new key was first observed
    }

    if (now_ms < next_attempt_at_ms_) return;  // not due yet

    if (now_ms >= snap.expires_at_ms) {
        // --- Recreation path: the current key is unsalvageable, get a fresh one. ---
        ++stats_.recreation_attempts;
        stats_.last_attempt_ms = now_ms;
        std::array<char, kListenKeyBufferLen> new_buf{};  // stack buffer, zero heap allocation
        std::size_t new_len = 0;
        PrivateRestError err = PrivateRestError::None;
        try {
            err = client_.create_listen_key(new_buf, new_len);
        } catch (...) {
            // create_listen_key() is NOT noexcept (its internal net::co_spawn completion
            // handler can genuinely rethrow -- confirmed by direct inspection, unlike
            // query_order()/submit_order() which cross a fixed C-ABI boundary and are
            // noexcept). poll() itself IS noexcept, matching every other hot-loop poll()
            // in this codebase, so any exception must be folded here -- never allowed to
            // escape and call std::terminate().
            err = PrivateRestError::Read;
        }
        if (err == PrivateRestError::None && new_len > 0) {
            ++stats_.recreation_successes;
            stats_.consecutive_failures = 0;
            stats_.last_error = PrivateRestError::None;
            stats_.last_success_ms = now_ms;
            // issued_at_ms MUST be now_ms (this key's real creation time), never the old
            // snapshot's -- publishing a stale hour-old issued_at_ms on a brand-new key would
            // corrupt every downstream observer of that field, not just this scheduler.
            listen_key_pub_.publish(std::string_view(new_buf.data(), new_len), now_ms,
                                     now_ms + policy_.effective_ttl_ms());
            // Re-sync immediately -- this seq bump was this scheduler's own doing, not an
            // external rotation; see last_seen_seq_'s own field comment for why skipping this
            // is a P0-severity self-triggered infinite-retry bug.
            last_seen_seq_ = listen_key_pub_.load().seq;
            next_attempt_at_ms_ = now_ms + policy_.effective_interval_ms();
        } else {
            ++stats_.recreation_failures;
            ++stats_.consecutive_failures;
            stats_.last_error = err;
            next_attempt_at_ms_ = now_ms + policy_.effective_backoff_ms();
        }
    } else {
        // --- Ordinary keepalive path. ---
        ++stats_.keepalive_attempts;
        stats_.last_attempt_ms = now_ms;
        PrivateRestError err = PrivateRestError::None;
        try {
            err = client_.keepalive_listen_key(snap.view());
        } catch (...) {
            err = PrivateRestError::Read;  // see the recreation path's own comment above
        }
        if (err == PrivateRestError::None) {
            ++stats_.keepalive_successes;
            stats_.consecutive_failures = 0;
            stats_.last_error = PrivateRestError::None;
            stats_.last_success_ms = now_ms;
            // A keepalive PUT does not change the key's own issued_at_ms -- only extends how
            // long it stays valid.
            listen_key_pub_.publish(snap.view(), snap.issued_at_ms,
                                     now_ms + policy_.effective_ttl_ms());
            last_seen_seq_ = listen_key_pub_.load().seq;  // re-sync -- see above
            next_attempt_at_ms_ = now_ms + policy_.effective_interval_ms();
        } else {
            ++stats_.keepalive_failures;
            ++stats_.consecutive_failures;
            stats_.last_error = err;
            next_attempt_at_ms_ = now_ms + policy_.effective_backoff_ms();
        }
    }
}

}  // namespace hy
