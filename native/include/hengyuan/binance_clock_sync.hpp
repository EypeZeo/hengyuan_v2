// SPDX-License-Identifier: proprietary
// binance_clock_sync.hpp — Binance L4 §2.2/§7.1.2 clock offset tracking.
//
// Offline pure-logic portion only: ClockOffsetSnapshot/ClockOffsetPublisher,
// checked arithmetic, freshness (TTL + wall-clock-jump) judgment, and the
// two server_now_ms_* derived functions. The real GET /api/v3/time network
// round trip that feeds compute_clock_offset() is out of scope here.
//
// Publish mechanism is mutex + by-value copy per spec §7.1.2's own
// canonical text (not std::atomic<std::shared_ptr<...>> — every load() on
// an atomic shared_ptr does a real atomic refcount inc/dec pair even for a
// pure read, and this structure is off the hot path: owner actor + an
// occasional resync thread, §9). Matches SymbolRegistry::current_rules()'s
// established by-value-snapshot pattern for a structurally similar problem
// (§5.3).
//
// Governance: L4 (offline logic only, no network, no real credentials).

#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>

namespace hy {

inline constexpr std::int64_t kClockSlopMs = 100;
inline constexpr std::int64_t kMaxUsableRttMs = 2000;
inline constexpr std::int64_t kOffsetTtlMs = 5 * 60 * 1000;  // §2.2 TTL
inline constexpr std::int64_t kMaxClockDriftMs = 1000;       // §2.2 wall-jump tolerance

// Reference out-param, matches spec §7.1.2's own signature. Overflow
// returns false; out is left untouched (caller must not read it then).
inline bool checked_add_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (b >= 0) {
        if (a > std::numeric_limits<std::int64_t>::max() - b) return false;
    } else {
        if (a < std::numeric_limits<std::int64_t>::min() - b) return false;
    }
    out = a + b;
    return true;
}

inline bool checked_sub_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (b >= 0) {
        if (a < std::numeric_limits<std::int64_t>::min() + b) return false;
    } else {
        if (a > std::numeric_limits<std::int64_t>::max() + b) return false;
    }
    out = a - b;
    return true;
}

// Field names/types match spec §7.1.2 literally — downstream
// FreezeTimeProbeProof::clock_snapshot_seq/clock_offset_ms already consume
// the seq/offset_ms field names.
struct ClockOffsetSnapshot {
    std::int64_t offset_ms{0};
    std::int64_t error_bound_ms{0};      // = rtt_ms/2 + kClockSlopMs
    std::int64_t system_at_fetch_ms{0};  // §2.2 wall-jump baseline
    std::int64_t steady_at_fetch_ms{0};  // sampled at the same instant as
                                          // system_at_fetch_ms
    std::uint32_t seq{0};                // 0 = never published; publish()
                                          // bumps by exactly 1 on success
};

static_assert(sizeof(ClockOffsetSnapshot) <= 64);  // 44B payload, single cache line

class ClockOffsetPublisher {
public:
    // false: publish failed (seq already at UINT32_MAX — refuse to wrap,
    // retain old snapshot; spec Round-25 P1). Caller-supplied snap.seq is
    // ignored; publish() assigns seq = published_.seq + 1 under the lock.
    bool publish(ClockOffsetSnapshot snap) noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        if (published_.seq == UINT32_MAX) return false;
        snap.seq = published_.seq + 1;
        published_ = snap;
        return true;
    }

    // By-value copy taken under the lock — never a pointer/reference into
    // published_, so the caller's copy is immune to a concurrent publish()
    // no matter how long it's held (same by-value-snapshot pattern
    // SymbolRegistry::current_rules() already uses, §5.3).
    ClockOffsetSnapshot load() const noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        return published_;
    }

private:
    mutable std::mutex mu_;
    ClockOffsetSnapshot published_{};
};

inline bool server_now_ms_pessimistic(const ClockOffsetSnapshot& s,
                                       std::int64_t local_utc_ms,
                                       std::int64_t& out) noexcept {
    std::int64_t mid = 0;
    if (!checked_add_i64(local_utc_ms, s.offset_ms, mid)) return false;
    return checked_sub_i64(mid, s.error_bound_ms, out);
}

inline bool server_now_ms_for_signing(const ClockOffsetSnapshot& s,
                                       std::int64_t local_utc_ms,
                                       std::int64_t& out) noexcept {
    return checked_add_i64(local_utc_ms, s.offset_ms, out);
}

// system_ms()/steady_ms() must always come from the SAME instant (never two
// independent reads) — otherwise the gap between two separate reads can
// misclassify a normal snapshot as "jumped" near a TTL/drift boundary.
//
// AUDIT L4-CLOCKPAIR-API-002: this used to be a public aggregate, and
// is_snapshot_fresh()/try_get_signing_timestamp_ms() used to take two loose
// std::int64_t parameters instead of one of these — which let a caller pass
// two values sourced from independent reads and silently defeat the
// wall-clock-jump check this whole file exists to enforce (fail-open, not
// fail-closed). Construction is now restricted to fetch_clock_pair() (real
// callers) and ClockPairSampleTestHooks (synthetic values for tests that
// need to construct a specific past fetch-time pair) — a caller can no
// longer assemble one from two independently-sourced timestamps.
class ClockPairSample {
public:
    std::int64_t system_ms() const noexcept { return system_ms_; }
    std::int64_t steady_ms() const noexcept { return steady_ms_; }

private:
    friend ClockPairSample fetch_clock_pair() noexcept;
    friend class ClockPairSampleTestHooks;   // unconditional; literal-
                                               // identical across every TU.

    ClockPairSample(std::int64_t system_ms, std::int64_t steady_ms) noexcept
        : system_ms_(system_ms), steady_ms_(steady_ms) {}

    std::int64_t system_ms_{0};
    std::int64_t steady_ms_{0};
};

inline ClockPairSample fetch_clock_pair() noexcept {
    return ClockPairSample(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count(),
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// §2.2 freshness judgment (TTL + wall-clock jump), pure logic, no network.
// Never published (seq == 0), TTL expired, or the wall/steady clocks have
// diverged by more than kMaxClockDriftMs since the last fetch (a real step
// change, not ordinary slew) — any of these three fails closed.
inline bool is_snapshot_fresh(const ClockOffsetSnapshot& s,
                               ClockPairSample now) noexcept {
    if (s.seq == 0) return false;

    std::int64_t steady_elapsed = 0;
    if (!checked_sub_i64(now.steady_ms(), s.steady_at_fetch_ms, steady_elapsed)) return false;
    if (steady_elapsed < 0 || steady_elapsed > kOffsetTtlMs) return false;

    std::int64_t system_delta = 0, steady_delta = 0, drift = 0;
    if (!checked_sub_i64(now.system_ms(), s.system_at_fetch_ms, system_delta)) return false;
    if (!checked_sub_i64(now.steady_ms(), s.steady_at_fetch_ms, steady_delta)) return false;
    if (!checked_sub_i64(system_delta, steady_delta, drift)) return false;
    // Negating INT64_MIN is signed-overflow UB — same discipline as
    // transport_policy.hpp::check_clock_skew's AUDIT TIME-SKEW-025: take
    // the magnitude of a signed difference via unsigned arithmetic.
    const std::uint64_t abs_drift = drift < 0
        ? (std::uint64_t{0} - static_cast<std::uint64_t>(drift))
        : static_cast<std::uint64_t>(drift);
    if (abs_drift > static_cast<std::uint64_t>(kMaxClockDriftMs)) return false;

    return true;
}

// Pure logic (in scope for this round): given the three timestamps a
// §3 GET /api/v3/time round trip already measured, compute
// offset_ms/error_bound_ms. The real network round trip itself is out of
// scope — the caller supplies the local send/recv timestamps and the
// server's reported time, and this function performs zero network I/O, so
// it's fully offline-testable by constructing those three timestamps
// directly. rtt > kMaxUsableRttMs is discarded; error_bound = rtt/2 +
// kClockSlopMs.
inline bool compute_clock_offset(std::int64_t local_send_ms,
                                  std::int64_t local_recv_ms,
                                  std::int64_t server_time_ms,
                                  ClockPairSample fetch_sample,
                                  ClockOffsetSnapshot& out) noexcept {
    std::int64_t rtt = 0;
    if (!checked_sub_i64(local_recv_ms, local_send_ms, rtt)) return false;
    if (rtt < 0 || rtt > kMaxUsableRttMs) return false;

    std::int64_t mid = 0;
    if (!checked_add_i64(local_send_ms, rtt / 2, mid)) return false;
    std::int64_t offset = 0;
    if (!checked_sub_i64(server_time_ms, mid, offset)) return false;
    std::int64_t error_bound = 0;
    if (!checked_add_i64(rtt / 2, kClockSlopMs, error_bound)) return false;

    out.offset_ms = offset;
    out.error_bound_ms = error_bound;
    out.system_at_fetch_ms = fetch_sample.system_ms();
    out.steady_at_fetch_ms = fetch_sample.steady_ms();
    // out.seq is left untouched — assigned by ClockOffsetPublisher::publish().
    return true;
}

// Sole call site (in scope for this round) for §2.2's "must be fresh to
// sign" rule. Does not cache the return value: spec §2.2's own text
// requires the timestamp to be generated as the last step before signing,
// never cached earlier — callers call this once per signature, use the
// value immediately, and discard it.
inline bool try_get_signing_timestamp_ms(const ClockOffsetPublisher& pub,
                                          ClockPairSample now,
                                          std::int64_t& out) noexcept {
    ClockOffsetSnapshot s = pub.load();
    if (!is_snapshot_fresh(s, now)) return false;
    return server_now_ms_for_signing(s, now.system_ms(), out);
}

// The exchange-corrected wall clock for a decision that must never run AHEAD of the exchange -- "has this
// kline closed yet" (binance_klines_rest.hpp counts a bar as closed only once a margin has elapsed past its
// close_time). Same freshness rule as signing: a stale or wall-jumped snapshot yields nothing, and there is
// no fall-back to the uncalibrated local clock. But it is the PESSIMISTIC estimate (local + offset -
// error_bound): it can only be behind the exchange, so a bar may be judged "not closed yet" a moment too
// long and is never judged closed too early. Not a signature timestamp, so it does not have to be generated
// as the last step before use; it is a bound.
inline bool try_get_pessimistic_server_now_ms(const ClockOffsetPublisher& pub,
                                               ClockPairSample now,
                                               std::int64_t& out) noexcept {
    const ClockOffsetSnapshot s = pub.load();
    if (!is_snapshot_fresh(s, now)) return false;
    return server_now_ms_pessimistic(s, now.system_ms(), out);
}

}  // namespace hy
