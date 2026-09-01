// SPDX-License-Identifier: proprietary
// binance_listen_key_publisher.hpp — TODO 1A.4: cross-thread publisher for the current
// listenKey (Binance's private user-data-stream WS session identifier).
//
// Publish mechanism is mutex + by-value copy, deliberately mirroring
// binance_clock_sync.hpp::ClockOffsetPublisher verbatim, not a new pattern:
// - listenKey is CURRENT STATE, not a discrete event stream. The hot/credentials-owning
//   thread (binance_private_rest.hpp's create_listen_key()/keepalive_listen_key(), a signed
//   -- well, USER_STREAM-type, header-only -- REST call, see that file's own comment on why
//   these three endpoints never touch build_signed_query()) is the only thread allowed to
//   issue those calls (BoundHmacCredentials::copy_api_key() fail-closes on any other calling
//   thread, binance_environment.hpp:180). The WS I/O thread only ever needs "what is the
//   current key" -- forcing that through an SpscRing (order_tracker.hpp's own pattern)
//   would make the WS thread drain-and-discard stale entries just to reconstruct "current",
//   exactly the problem ClockOffsetPublisher's own header comment says the snapshot-publish
//   pattern exists to avoid.
// - Off the hot path: created once at startup, renewed roughly every ~30-60 minutes (real
//   Binance semantics -- see the 1A.4 plan's own caveat on which specifics still want a
//   final live-docs check), not a contention concern. Same reasoning ClockOffsetPublisher's
//   own header comment already used to justify mutex+copy over
//   std::atomic<std::shared_ptr<...>>.
//
// Governance: L4 (offline logic only, no network, no real credentials -- same tier as
// binance_clock_sync.hpp; the actual REST calls that feed this live in
// binance_private_rest.hpp, not here).

#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>

namespace hy {

// Deliberately NOT reusing binance_private_rest.hpp::kListenKeyLen -- that file pulls in
// Boost/Beast/OpenSSL/simdjson, and this file stays dependency-light on purpose, matching
// ClockOffsetPublisher's own "offline logic only" governance tier. Kept numerically equal
// (128) so a key that fits in binance_private_rest.hpp's parse buffer always fits here too;
// if the two ever need to diverge, that's a deliberate future decision, not an oversight.
inline constexpr std::size_t kListenKeyBufferLen = 128;

struct ListenKeySnapshot {
    char key[kListenKeyBufferLen]{};  // not null-terminated by contract -- see key_len
    std::size_t key_len{0};
    std::int64_t issued_at_ms{0};
    std::int64_t expires_at_ms{0};   // 0 = unknown/not yet established
    std::uint32_t seq{0};             // 0 = never published; publish() bumps by exactly 1

    std::string_view view() const noexcept { return std::string_view(key, key_len); }
};

class ListenKeyPublisher {
public:
    // false: publish failed -- seq already at UINT32_MAX (refuse to wrap, retain old
    // snapshot, same policy as ClockOffsetPublisher::publish()), or key_view is empty/too
    // long for kListenKeyBufferLen (refuse rather than truncate a credential-adjacent
    // token). Caller-supplied issued_at_ms/expires_at_ms are taken as given; seq is
    // always assigned here (= published_.seq + 1), any caller-supplied seq is ignored,
    // matching ClockOffsetPublisher::publish()'s own contract.
    bool publish(std::string_view key_view, std::int64_t issued_at_ms,
                 std::int64_t expires_at_ms) noexcept {
        if (key_view.empty() || key_view.size() >= kListenKeyBufferLen) return false;
        std::lock_guard<std::mutex> lk(mu_);
        if (published_.seq == UINT32_MAX) return false;

        ListenKeySnapshot snap{};
        std::memcpy(snap.key, key_view.data(), key_view.size());
        snap.key_len = key_view.size();
        snap.issued_at_ms = issued_at_ms;
        snap.expires_at_ms = expires_at_ms;
        snap.seq = published_.seq + 1;
        published_ = snap;
        return true;
    }

    // By-value copy taken under the lock -- never a pointer/reference into published_, same
    // by-value-snapshot pattern ClockOffsetPublisher::load()/SymbolRegistry::current_rules()
    // already use. seq == 0 means "never published" -- callers must check that before
    // trusting key_len/key.
    ListenKeySnapshot load() const noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        return published_;
    }

private:
    mutable std::mutex mu_;
    ListenKeySnapshot published_{};
};

}  // namespace hy
