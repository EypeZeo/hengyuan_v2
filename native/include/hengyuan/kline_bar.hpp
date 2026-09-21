// SPDX-License-Identifier: proprietary
// kline_bar.hpp — 批次 6 6b-0f: the Boost-free kline types shared by the live WS session
// (binance_kline_ws_session.hpp), the REST backfill codec (binance_klines_codec.hpp) and the
// feed supervisor, so logic that only needs a bar, the ring or the continuity guard can be built
// and unit-tested without dragging in Boost.Asio/Beast/OpenSSL.
//
// What is moved and what is new: KlineWsEvent, KlineWsEventRing, KlineBarGapGuard,
// KlineIngestResult and ingest_closed_bar() are moved out of binance_kline_ws_session.hpp with their
// code unchanged (only the comments were re-pointed at that header, which still owns the two design
// requirements -- in-place unclosed-bar filtering and bar-gap detection -- and external review
// P0-04). The interval helpers below are NEW: kline_interval_span_ms() is the span table the
// backfill codec needs, and is_valid_kline_interval() is the same 16-value allow-list the session
// used to keep as a private loop -- that session-side detail::is_valid_kline_interval() now
// delegates here, so there is one list, not two.

#pragma once

#include <hengyuan/spsc_ring.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace hy {

// Binance's documented kline interval set. "1M" (calendar month, capital M) is deliberately
// listed distinctly from "1m" (minute) -- this is Binance's own convention, not a typo.
// Returns the fixed span of one bar in ms -- close_time - open_time + 1, e.g. a 1m bar opening at
// 00:00:00.000 closes at 00:00:59.999 -- or 0 for an unknown interval AND for "1M", whose span
// varies with the calendar month (callers must skip a span check when this returns 0, and use
// is_valid_kline_interval() to tell "unknown" from "variable").
inline constexpr std::int64_t kline_interval_span_ms(std::string_view s) noexcept {
    if (s == "1s") return 1'000;
    if (s == "1m") return 60'000;
    if (s == "3m") return 3 * 60'000;
    if (s == "5m") return 5 * 60'000;
    if (s == "15m") return 15 * 60'000;
    if (s == "30m") return 30 * 60'000;
    if (s == "1h") return 60 * 60'000;
    if (s == "2h") return 2 * 60 * 60'000;
    if (s == "4h") return 4 * 60 * 60'000;
    if (s == "6h") return 6 * 60 * 60'000;
    if (s == "8h") return 8 * 60 * 60'000;
    if (s == "12h") return 12 * 60 * 60'000;
    if (s == "1d") return 24 * 60 * 60'000;
    if (s == "3d") return 3LL * 24 * 60 * 60'000;
    if (s == "1w") return 7LL * 24 * 60 * 60'000;
    return 0;  // "1M" (variable) or unknown
}

inline constexpr bool is_valid_kline_interval(std::string_view s) noexcept {
    return s == "1M" || kline_interval_span_ms(s) != 0;
}

// A closed candle only, double-valued -- deliberately NOT hy::BinanceMarketEvent's int64-ticks
// shape. This session has no SymbolRegistry access (same discipline as UserDataWsEvent, see
// binance_user_data_event.hpp's own header comment) and its one consumer,
// hy::strategy_spec::Bar (strategy_spec_operators.hpp), is itself all-double -- converting to
// fixed-point ticks here would just be undone one layer up for zero benefit. symbol_id is
// stamped by the session from KlineWsSessionConfig::symbol_id (the caller's own SymbolRegistry
// id for the single symbol this session subscribes to), never parsed from the JSON payload.
struct KlineWsEvent {
    std::int64_t open_time_ms{0};   // "k"."t"
    std::int64_t close_time_ms{0};  // "k"."T"
    double open{0.0};               // "k"."o"
    double high{0.0};               // "k"."h"
    double low{0.0};                // "k"."l"
    double close{0.0};              // "k"."c"
    double volume{0.0};             // "k"."v"
    std::uint32_t symbol_id{0};     // stamped by the session, not parsed -- see struct comment
    bool is_closed{false};          // always true for anything actually pushed to the ring (see
                                     // binance_kline_ws_session.hpp's header comment, requirement
                                     // 1) -- kept as an explicit field anyway so a consumer never
                                     // has to trust "if it's in the ring it must be closed" as an
                                     // unstated invariant.
};
static_assert(std::is_trivially_copyable_v<KlineWsEvent>,
              "KlineWsEvent crosses the SpscRing producer/consumer boundary");

// Small, closed-candles-only ring -- see binance_kline_ws_session.hpp's header comment,
// requirement 1, for why a large ring would just paper over a filtering bug instead of catching it.
using KlineWsEventRing = SpscRing<KlineWsEvent, 16>;

// Binance's own maximum `limit` for /api/v3/klines (verified against the official docs: "max: 1000,
// Default: 500"). Moved here from binance_klines_codec.hpp with KlineBackfill (6b-0f-3c): both are
// plain data the feed driver needs without the codec's simdjson dependency.
inline constexpr std::size_t kMaxBackfillBars = 1000;

// What a successful klines parse yields: closed bars only, oldest first, contiguous.
// Caller-owned (64 KB): keep one long-lived instance, not a stack local in a hot function.
struct KlineBackfill {
    std::array<KlineWsEvent, kMaxBackfillBars> bars{};
    std::size_t count{0};
    bool dropped_unclosed_tail{false};
};

// What a backfill fetcher is asked for. Value-semantic: SingleFlightFetchGate hands its worker thread
// a private copy. Lives here (not next to the fetcher) so the Boost-free feed driver and the
// Boost-dependent REST fetcher can share it without including each other.
struct KlinesBackfillRequest {
    std::string symbol;    // UPPERCASE, as the REST endpoint wants it (the WS stream name is lowercase)
    std::string interval;  // Binance's own spelling, e.g. "1h"
    std::uint32_t limit{0};
    std::uint32_t symbol_id{0};
};

// Bar-gap (跳空) continuity + suspend state machine, extracted out of BinanceKlineWsSession so
// it can be unit-tested directly against synthetic KlineWsEvent values (see
// test_binance_kline_ws_session.cpp's KlineBarGapGuard test group) -- no I/O, no Boost/Beast
// dependency of its own. Enforces binance_kline_ws_session.hpp's header comment, requirement 2:
// consecutive closed bars' boundaries must be exactly 1ms apart (Binance's own kline
// open_time/close_time convention, true for every interval), or the guard suspends and every event
// is rejected (including the gap-triggering one) until resume() is explicitly called.
class KlineBarGapGuard {
public:
    // Returns true if `event` is the accepted next bar in an unbroken sequence (caller should
    // push it); false if it was rejected -- either because this call is the one that just
    // detected a gap (is_suspended() flips true), or because the guard was already suspended.
    bool accept(const KlineWsEvent& event) noexcept {
        if (suspended_) return false;
        if (have_prev_close_ && event.open_time_ms != prev_close_time_ms_ + 1) {
            suspended_ = true;
            return false;
        }
        prev_close_time_ms_ = event.close_time_ms;
        have_prev_close_ = true;
        return true;
    }

    bool is_suspended() const noexcept { return suspended_; }

    // Suspends WITHOUT a continuity mismatch having been observed. Exists for one case only: a
    // bar this guard accepted (advancing prev_close_time_ms_) then failed to actually reach its
    // consumer -- ring full (see ingest_closed_bar() below). Without this, the guard's baseline
    // says "no gap" while the consumer is in fact one bar short, and every later bar still passes
    // the continuity check, silently corrupting SMA/EMA/RSI state (external review P0-04,
    // verified against the session's earlier handle_closed_bar()).
    void force_suspend() noexcept { suspended_ = true; }

    // Clears suspension and the continuity baseline -- the next accept() call is unconditionally
    // accepted and becomes the new baseline, with no check against the pre-gap value. Caller's
    // responsibility to only call this once it is actually safe to trust the next observed bar
    // (human confirmation, or StreamingEvaluator::reset() + REST backfill already done) -- see
    // binance_kline_ws_session.hpp's header comment, requirement 2.
    void resume() noexcept {
        have_prev_close_ = false;
        suspended_ = false;
    }

private:
    bool have_prev_close_{false};
    bool suspended_{false};
    std::int64_t prev_close_time_ms_{0};
};

enum class KlineIngestResult : std::uint8_t {
    Pushed = 0,           // accepted by the guard AND actually in the ring
    RingFull = 1,         // accepted by the guard but the ring was full -- guard now suspended
    GapDetected = 2,      // continuity mismatch -- this call suspended the guard, bar NOT pushed
    SuspendedEarlier = 3, // guard was already suspended -- bar discarded, nothing touched
};

// The one place a closed bar is admitted to the ring. Requirement 3 of the session's design (added
// by external review P0-04): "accepted by the continuity guard" and "delivered to the consumer"
// are two different facts, and only the second one is what keeps downstream indicator state
// correct. A full ring therefore suspends exactly like a gap does -- the consumer is now missing
// a bar, which is a gap the consumer itself cannot see from timestamps it never received.
// Free function (not a member) so it is unit-testable against a real ring with no session/network.
inline KlineIngestResult ingest_closed_bar(KlineBarGapGuard& guard, KlineWsEventRing& ring,
                                            const KlineWsEvent& event) noexcept {
    if (guard.is_suspended()) return KlineIngestResult::SuspendedEarlier;
    if (!guard.accept(event)) return KlineIngestResult::GapDetected;
    if (!ring.try_push(event)) {
        guard.force_suspend();
        return KlineIngestResult::RingFull;
    }
    return KlineIngestResult::Pushed;
}

}  // namespace hy
