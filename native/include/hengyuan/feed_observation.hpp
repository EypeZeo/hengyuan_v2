// SPDX-License-Identifier: proprietary
// feed_observation.hpp — private-stream migration M3a: the one place that turns a PublicFeedSupervisor and its current session into the
// FeedObservation the ReconnectBarrier consumes (reconnect_barrier.hpp). Boost-free and generic over the session type, like the supervisor itself,
// so it is tested against fakes; BinanceWsApiUserDataSession satisfies ObservableFeedSession (pinned in test_binance_ws_api_user_data_session.cpp).
//
// What "connected" means here, and why it is stricter than the supervisor's own healthy():
//   * the supervisor is in FeedState::Connected -- it has polled since the session came up, so a session that connected a microsecond ago and has
//     not been seen by poll() yet is NOT connected for the barrier (fail-closed by one tick);
//   * AND the session itself says it is connected (for the WS API session: the subscription is confirmed by BOTH answers, M2b) and has not
//     stopped. The supervisor learns that a session stopped only at its next poll(); asking the session directly closes that gap.
//
// `steady_now_ms` must come from the same clock as the session's last_activity_steady_ms(): std::chrono::steady_clock, milliseconds since its
// epoch (BinanceWsApiUserDataSession::steady_now_ms()). The barrier compares the two to detect a silent connection.
//
// THREAD OWNERSHIP: the supervisor's -- the hot thread. The session accessors are the cross-thread-safe ones it already has.

#pragma once

#include <hengyuan/public_feed_supervisor.hpp>
#include <hengyuan/reconnect_barrier.hpp>

#include <concepts>
#include <cstdint>
#include <memory>

namespace hy {

template <typename S>
concept ObservableFeedSession = SupervisedFeedSession<S> && requires(const S& s) {
    { s.last_activity_steady_ms() } -> std::convertible_to<std::int64_t>;
    { s.ring_drops() } -> std::convertible_to<std::uint64_t>;
    { s.lost_events() } -> std::convertible_to<std::uint64_t>;
};

template <ObservableFeedSession S>
[[nodiscard]] inline FeedObservation make_feed_observation(const PublicFeedSupervisor<S>& supervisor, std::int64_t steady_now_ms) noexcept {
    FeedObservation o{};
    o.sampled_at_ms = steady_now_ms;
    o.generation = supervisor.generation();
    const std::shared_ptr<S> session = supervisor.current();  // null between generations
    if (session && supervisor.healthy() && !session->stopped() && session->is_connected()) {
        o.connected = true;
        o.last_activity_ms = static_cast<std::int64_t>(session->last_activity_steady_ms());
        o.ring_drops = static_cast<std::uint64_t>(session->ring_drops());
        o.lost_events = static_cast<std::uint64_t>(session->lost_events());
    }
    return o;
}

}  // namespace hy
