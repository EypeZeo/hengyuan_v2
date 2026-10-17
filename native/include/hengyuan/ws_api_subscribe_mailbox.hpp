// SPDX-License-Identifier: proprietary
// ws_api_subscribe_mailbox.hpp — private-stream migration M2 (docs/PRIVATE_STREAM_MIGRATION_DESIGN.md §3): the one-shot handoff of a
// PRE-SIGNED subscribe frame from the thread that owns the credentials to the WS API session on the I/O thread.
//
// Why a mailbox and not a function call: BoundHmacCredentials answers only on the thread that bound it (the hot/submit thread), while the
// session's handlers run on the one io_context thread. So the hot thread signs the subscribe request (timestamp from a fresh
// ClockReadyToken) and posts the finished bytes here; the session, once the WebSocket handshake is done, takes them out and sends them.
// The session never holds credentials and never signs — it can only send what a builder of binance_ws_api_signing.hpp produced.
//
// ONE-SHOT, per connection (audit E-06, blueprint 5.2: a signed request is never replayed). Each session owns its own mailbox, so a new
// generation always gets a fresh signature with a fresh timestamp; and inside one mailbox the frame can be posted once and taken once,
// whatever happens: a second post() and a second take() return false and change nothing, even when misused from more than one thread. That is
// what the claim states — "the same signed bytes cannot be sent twice" — and why both transitions are compare-exchanges, not plain stores.
//
// Order independence: the producer may post before the session starts, while it connects, or after the handshake; the consumer polls. Nothing
// depends on which comes first, so the handoff is a real cross-thread boundary (test_ws_api_mailbox_concurrency, under TSan; its negative
// control is tests/tsan_control_relaxed_mailbox.cpp, the same protocol with the release/acquire pair downgraded to relaxed, which TSan must
// report).
//
// Memory order: the producer writes the slot with plain stores and then publishes with a release store of Ready; the consumer claims the
// frame with an acquire compare-exchange on Ready, so the slot is fully visible before it is copied. The frame is never written after Ready.
//
// Layout: the state word is a coordinator flag touched by both threads and sits on its own cache line (CLAUDE.md, Engineering Mandate 2;
// hy::kCacheLine is the house value of std::hardware_destructive_interference_size, see spsc_ring.hpp); the slot sits on the next one.
//
// Boost-free and OpenSSL-free on purpose: the TSan job builds without HY_BUILD_DEMO, so the concurrency test of this boundary must not need
// either. The signed request itself (WsApiSignedRequest) is copied into a WsApiSubscribeFrame by the session header, which has both.
//
// Governance: L1 (no network, no secrets: the frame is already signed).

#pragma once

#include <hengyuan/spsc_ring.hpp>  // hy::kCacheLine

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hy {

// One request frame, as binance_ws_api_signing.hpp builds it. The session header static_asserts that this equals kMaxWsApiFrameBytes.
inline constexpr std::size_t kWsApiSubscribeFrameMaxBytes = 1536;

// A signed subscribe frame in flight. Plain bytes: the JSON text exactly as it will be written to the socket.
struct WsApiSubscribeFrame {
    std::array<char, kWsApiSubscribeFrameMaxBytes> bytes{};
    std::uint32_t length{0};
    std::uint64_t request_id{0};
    // Local steady-clock milliseconds at which the producer finished signing. The session refuses a frame that waited so long that the
    // exchange would reject its timestamp anyway (recvWindow, default 5000 ms), and says so instead of spending a request on it.
    std::int64_t signed_at_steady_ms{0};
};
static_assert(std::is_trivially_copyable_v<WsApiSubscribeFrame>);

class WsApiSubscribeMailbox {
public:
    WsApiSubscribeMailbox() = default;
    WsApiSubscribeMailbox(const WsApiSubscribeMailbox&) = delete;
    WsApiSubscribeMailbox& operator=(const WsApiSubscribeMailbox&) = delete;

    // Producer. True exactly once per mailbox. Refuses (false, nothing changed, the shot is NOT used) a frame whose length is zero or does not
    // fit; refuses (false) every post after the first, from any thread, including one that races the first.
    [[nodiscard]] bool post(const WsApiSubscribeFrame& frame) noexcept {
        if (frame.length == 0 || frame.length > frame.bytes.size()) return false;
        std::uint32_t expected = kEmpty;
        if (!state_.compare_exchange_strong(expected, kWriting, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return false;
        }
        slot_ = frame;
        state_.store(kReady, std::memory_order_release);
        return true;
    }

    // Consumer. True exactly once per mailbox, when a frame has been posted: copies it into `out`. False while nothing has been posted, and
    // forever after the one successful take — a second consumer, or a second call, never sees the frame again.
    [[nodiscard]] bool take(WsApiSubscribeFrame& out) noexcept {
        // The common poll finds the mailbox empty: look before writing, so a consumer that polls does not bounce the line with failed CASes.
        if (state_.load(std::memory_order_relaxed) != kReady) return false;
        std::uint32_t expected = kReady;
        if (!state_.compare_exchange_strong(expected, kTaking, std::memory_order_acquire, std::memory_order_relaxed)) {
            return false;
        }
        out = slot_;
        state_.store(kTaken, std::memory_order_release);
        return true;
    }

    // Any thread. A frame has been published (whether or not it has been taken yet).
    [[nodiscard]] bool posted() const noexcept { return state_.load(std::memory_order_acquire) >= kReady; }
    // Any thread. The frame has been taken (and copied out).
    [[nodiscard]] bool taken() const noexcept { return state_.load(std::memory_order_acquire) == kTaken; }

private:
    static constexpr std::uint32_t kEmpty = 0;
    static constexpr std::uint32_t kWriting = 1;  // a producer owns the slot
    static constexpr std::uint32_t kReady = 2;    // published; the slot is immutable from here on
    static constexpr std::uint32_t kTaking = 3;   // a consumer owns the right to read it
    static constexpr std::uint32_t kTaken = 4;

    alignas(kCacheLine) std::atomic<std::uint32_t> state_{kEmpty};
    alignas(kCacheLine) WsApiSubscribeFrame slot_{};
};

}  // namespace hy
