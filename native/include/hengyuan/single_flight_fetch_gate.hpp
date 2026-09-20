// SPDX-License-Identifier: proprietary
// single_flight_fetch_gate.hpp — 批次 6 6b-0f-3a: run ONE blocking fetch at a time on a worker
// thread, hand its outcome back to the hot thread through a mailbox, and never let the caller spin
// on a failing endpoint.
//
// WHY IT EXISTS: fetch_klines_backfill() (binance_klines_rest.hpp) blocks its caller for up to ~30s
// worst case, and the recovery path that needs it runs inside the very loop that has to keep
// draining the market-data and user-data rings. SnapshotRefreshGate solved the same problem for the
// depth snapshot, but is typed to DepthSnapshot, reads a fixed steady_clock cooldown (its tests wait
// out real seconds) and retries a failed thread spawn on the next tick. This is the same
// Idle -> InFlight -> (Cooldown) -> Idle state machine, generic over request/result, with an
// INJECTED millisecond clock so every path is testable without sleeping, and with the cooldown
// decided by the OUTCOME: the fetcher's result carries its own status, so a 429/418 (rate limit /
// IP ban) can impose a long back-off while an ordinary transient error imposes a short one.
//
// THREADING: poll()/impose_cooldown()/state()/stats() belong to ONE thread (the hot thread) for the
// object's lifetime. The worker thread does exactly one thing -- call the injected Fetcher with its
// private copy of the request and publish the result to the mailbox (release store; the hot thread
// acquires it and then join()s, which is a second, formal happens-before edge). It touches nothing
// else, so there is no other shared state to reason about. The Fetcher must not throw: if it does,
// the worker swallows the exception, the outcome counts as a failed fetch and the spawn-failure
// cooldown applies -- an escaping exception would std::terminate the process from a std::thread.
//
// Result may be large (a KlineBackfill is ~64 KB): it lives inside the gate's mailbox, so
// instantiate the gate on the heap or as a long-lived object, not as a stack local of a hot function.
// The destructor joins a still-running worker, so tearing the gate down mid-fetch blocks for at most
// the fetcher's own budget.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

namespace hy {

enum class FetchGateState : std::uint8_t {
    Idle = 0,      // nothing running; a poll(wanted=true) starts a fetch
    InFlight = 1,  // a worker is running (or has finished and is waiting to be collected)
    Cooldown = 2,  // no fetch until the cooldown elapses
};

struct FetchGateStats {
    std::uint64_t fetches_started{0};
    std::uint64_t results_delivered{0};
    std::uint64_t spawn_failures{0};      // std::thread could not be created
    std::uint64_t fetcher_exceptions{0};  // the Fetcher threw (contract violation, contained)
    std::uint64_t cooldowns_imposed{0};
};

inline constexpr const char* fetch_gate_state_name(FetchGateState s) noexcept {
    switch (s) {
        case FetchGateState::Idle: return "Idle";
        case FetchGateState::InFlight: return "InFlight";
        case FetchGateState::Cooldown: return "Cooldown";
    }
    return "?";
}

template <typename Request, typename Result>
class SingleFlightFetchGate {
public:
    // Runs on the worker thread with a private copy of the request. Must not throw.
    using Fetcher = std::function<Result(const Request&)>;
    // Runs on the hot thread when an outcome is collected: how many ms to wait before the next fetch
    // (<= 0 = none). Empty = never any cooldown.
    using CooldownFn = std::function<std::int64_t(const Result&)>;

    explicit SingleFlightFetchGate(Fetcher fetcher, CooldownFn cooldown = {},
                                   std::int64_t failure_cooldown_ms = 5000)
        : fetcher_(std::move(fetcher))
        , cooldown_(std::move(cooldown))
        , failure_cooldown_ms_(failure_cooldown_ms) {}

    ~SingleFlightFetchGate() {
        if (worker_.joinable()) worker_.join();
    }

    SingleFlightFetchGate(const SingleFlightFetchGate&) = delete;
    SingleFlightFetchGate& operator=(const SingleFlightFetchGate&) = delete;

    // Once per hot-loop tick. Non-blocking. `wanted` says whether a fetch is currently wanted; it only
    // matters when nothing is running. A non-empty return is a COMPLETED outcome (success or not --
    // the Result carries its own status), delivered exactly once; the caller judges it, and may call
    // impose_cooldown() if it rejects an outcome the transport considered fine.
    std::optional<Result> poll(bool wanted, const Request& request, std::int64_t now_ms) {
        if (state_ == FetchGateState::InFlight) return collect(now_ms);
        if (state_ == FetchGateState::Cooldown) {
            if (now_ms < cooldown_until_ms_) return std::nullopt;
            state_ = FetchGateState::Idle;
        }
        if (wanted) start_fetch(request, now_ms);
        return std::nullopt;
    }

    // "Do not fetch again for cooldown_ms": for an outcome the caller found unusable although the
    // fetch itself succeeded (e.g. a backfill too short to rebuild from). Extends an active cooldown,
    // never shortens it. Ignored while a fetch is in flight (there is nothing to hold off yet).
    void impose_cooldown(std::int64_t now_ms, std::int64_t cooldown_ms) noexcept {
        if (cooldown_ms <= 0 || state_ == FetchGateState::InFlight) return;
        const std::int64_t until = sat_add(now_ms, cooldown_ms);
        if (state_ != FetchGateState::Cooldown || until > cooldown_until_ms_) cooldown_until_ms_ = until;
        state_ = FetchGateState::Cooldown;
        ++stats_.cooldowns_imposed;
    }

    FetchGateState state() const noexcept { return state_; }
    bool in_flight() const noexcept { return state_ == FetchGateState::InFlight; }
    FetchGateStats stats() const noexcept { return stats_; }

private:
    static constexpr std::int64_t sat_add(std::int64_t a, std::int64_t b) noexcept {
        constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
        return (b > 0 && a > kMax - b) ? kMax : a + b;  // b > 0 here: only positive cooldowns reach it
    }

    void start_fetch(const Request& request, std::int64_t now_ms) {
        mailbox_.reset();
        mailbox_ready_.store(false, std::memory_order_relaxed);
        try {
            worker_ = std::thread([this, req = request] {
                try {
                    mailbox_.emplace(fetcher_(req));
                } catch (...) {
                    mailbox_.reset();  // contained: reported as a failed fetch by collect()
                }
                mailbox_ready_.store(true, std::memory_order_release);
            });
        } catch (const std::system_error&) {
            // The thread could not be created (resource exhaustion). Nothing is half-started; back off
            // instead of retrying on the very next tick.
            ++stats_.spawn_failures;
            impose_cooldown(now_ms, failure_cooldown_ms_);
            return;
        }
        ++stats_.fetches_started;
        state_ = FetchGateState::InFlight;
    }

    std::optional<Result> collect(std::int64_t now_ms) {
        if (!mailbox_ready_.load(std::memory_order_acquire)) return std::nullopt;

        if (worker_.joinable()) worker_.join();  // it has already published; returns promptly
        std::optional<Result> outcome = std::move(mailbox_);
        mailbox_.reset();
        mailbox_ready_.store(false, std::memory_order_relaxed);
        state_ = FetchGateState::Idle;

        if (!outcome) {
            ++stats_.fetcher_exceptions;
            impose_cooldown(now_ms, failure_cooldown_ms_);
            return std::nullopt;
        }
        ++stats_.results_delivered;
        const std::int64_t cooldown_ms = cooldown_ ? cooldown_(*outcome) : 0;
        impose_cooldown(now_ms, cooldown_ms);
        return outcome;
    }

    Fetcher fetcher_;
    CooldownFn cooldown_;
    const std::int64_t failure_cooldown_ms_;

    FetchGateState state_{FetchGateState::Idle};
    std::int64_t cooldown_until_ms_{0};
    FetchGateStats stats_{};
    std::thread worker_;

    std::optional<Result> mailbox_;
    std::atomic<bool> mailbox_ready_{false};
};

}  // namespace hy
