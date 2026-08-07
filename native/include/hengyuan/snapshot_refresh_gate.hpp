// SPDX-License-Identifier: proprietary
// snapshot_refresh_gate.hpp — Minimal, demo-only snapshot refresh gate (P2-MD-02 / Track C).
//
// NOT a general-purpose production SnapshotRefreshController: it deliberately does not parse
// HTTP status/Retry-After, does not do exponential backoff/jitter, and has no epoch/cancellation
// machinery. It exists to close two specific, verified bugs in binance_dry_run_demo.cpp:
//   1. The demo's hot loop used to call fetch_depth_snapshot() synchronously inline, blocking
//      SPSC-ring drainage for up to that call's own ~30s worst-case budget on every resync
//      (including the very first snapshot at startup, not just later resyncs).
//   2. A failed snapshot attempt (fetch failure OR a fetch that succeeded but whose result was
//      then rejected by DepthManager::apply_snapshot(), e.g. because of a gap or a buffer
//      overflow -- see depth_manager.hpp) was retried on literally the next hot-loop iteration,
//      with no cooldown at all.
//
// State machine: Idle -> InFlight -> Idle (success) or Idle -> InFlight -> Cooldown -> Idle
// (fetch failure or apply rejection). Single-flight is enforced structurally: a new worker is
// only ever started from Idle. The worker thread does exactly one thing (call the injected
// SnapshotFetcher) -- it never touches DepthManager/HotThread/the market-data ring, so the hot
// thread stays the sole owner of DepthManager::apply_snapshot() and there is no data race to
// reason about there.
//
// A full production SnapshotRefreshController (HTTP-status-aware backoff, Retry-After, epoch
// cancellation, deterministic fake-server fault injection, TLA+ modeling) is out of scope for
// this round -- see the P2-MD-02 plan's "用户决定 v2" notes.

// AUDIT VERIF-TSAN-016: this header used to include binance_rest_snapshot.hpp --
// solely to name fetch_depth_snapshot_default as a constructor default argument --
// which dragged in Boost.Asio/Beast and OpenSSL. That put every test of this class
// behind HY_BUILD_DEMO, and the TSan CI job builds with HY_BUILD_DEMO=OFF and a
// hand-picked target list. Net effect: SnapshotRefreshGate spawns a real
// std::thread and hands a mailbox across it, and that code had never once run under
// ThreadSanitizer.
//
// The gate's state machine needs nothing from the REST layer -- the fetcher is
// injected. The production default now lives with the thing it defaults to
// (binance_rest_snapshot.hpp's make_default_snapshot_fetcher()), so this header is
// Boost-free and its tests build under the plain HY_BUILD_TESTS configuration.

#pragma once

#include <hengyuan/depth_manager.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace hy {

// Value-semantic request parameters. The worker thread's lambda must capture this by value --
// poll() is non-blocking and returns immediately, so any reference into the caller's stack
// (e.g. a `const std::string&` parameter) would dangle by the time the worker thread actually
// runs fetch_depth_snapshot().
struct SnapshotRequest {
    std::string symbol;
    std::int64_t price_multiplier{0};
    std::int64_t qty_multiplier{0};
};

using SnapshotFetcher = std::function<std::optional<DepthSnapshot>(const SnapshotRequest&)>;

class SnapshotRefreshGate {
public:
    // No default argument any more (audit VERIF-TSAN-016): naming the production
    // fetcher here is what coupled this header to Boost/OpenSSL. Production callers
    // pass hy::make_default_snapshot_fetcher() (binance_rest_snapshot.hpp); tests
    // inject a fake so the state machine -- single-flight, mailbox reset, cooldown on
    // fetch-failure/apply-rejection -- is exercised deterministically with no network.
    explicit SnapshotRefreshGate(SnapshotFetcher fetcher)
        : fetcher_(std::move(fetcher)) {}

    ~SnapshotRefreshGate() {
        if (worker_.joinable()) worker_.join();
    }

    SnapshotRefreshGate(const SnapshotRefreshGate&) = delete;
    SnapshotRefreshGate& operator=(const SnapshotRefreshGate&) = delete;

    // Call once per hot-loop iteration. Non-blocking. resync_needed is typically
    // depth_mgr.needs_snapshot(). A non-nullopt return means a fresh snapshot is ready; the
    // caller (hot thread) should apply it via DepthManager::apply_snapshot() and then report
    // the outcome via notify_apply_result() -- the gate itself never touches DepthManager.
    std::optional<DepthSnapshot> poll(bool resync_needed, const SnapshotRequest& request) {
        switch (state_) {
            case State::Idle:
                return poll_idle(resync_needed, request);
            case State::InFlight:
                return poll_in_flight();
            case State::Cooldown:
                return poll_cooldown();
        }
        return std::nullopt;
    }

    // Call immediately after DepthManager::apply_snapshot() returns, with its result. A
    // rejected apply (gap or buffer overflow, see depth_manager.hpp) is treated the same as a
    // fetch failure: it forces the same fixed cooldown rather than allowing the next poll() to
    // immediately re-fetch.
    void notify_apply_result(bool applied) {
        if (applied) return;
        cooldown_until_ = std::chrono::steady_clock::now() + kCooldown;
        state_ = State::Cooldown;
    }

private:
    enum class State { Idle, InFlight, Cooldown };

    struct MailboxEntry {
        std::uint64_t generation{0};
        std::optional<DepthSnapshot> snapshot;
    };

    std::optional<DepthSnapshot> poll_idle(bool resync_needed, const SnapshotRequest& request) {
        if (!resync_needed) return std::nullopt;

        // Reset the mailbox BEFORE starting a new worker -- a stale mailbox_ready_==true left
        // over from the previous cycle would otherwise make the very next InFlight check
        // immediately (and incorrectly) join() a worker that just started, blocking the hot
        // thread for up to that fetch's full budget, and could hand back the previous cycle's
        // snapshot instead of the new one.
        mailbox_ready_.store(false, std::memory_order_relaxed);
        mailbox_.reset();

        SnapshotRequest req_copy = request;
        std::uint64_t gen = ++next_generation_;
        try {
            worker_ = std::thread([this, req_copy, gen] {
                auto snap = fetcher_(req_copy);
                mailbox_ = MailboxEntry{gen, std::move(snap)};
                mailbox_ready_.store(true, std::memory_order_release);
            });
            inflight_generation_ = gen;
            state_ = State::InFlight;
        } catch (const std::system_error&) {
            // std::thread construction failed (e.g. resource exhaustion). mailbox_ is already
            // reset above; state_ stays Idle so the next poll() retries as if nothing happened
            // -- no half-started InFlight state to get stuck in.
        }
        return std::nullopt;
    }

    std::optional<DepthSnapshot> poll_in_flight() {
        if (!mailbox_ready_.load(std::memory_order_acquire)) return std::nullopt;

        if (worker_.joinable()) worker_.join();  // already finished; returns promptly
        MailboxEntry entry = std::move(*mailbox_);
        mailbox_.reset();
        mailbox_ready_.store(false, std::memory_order_relaxed);

        if (entry.generation != inflight_generation_) {
            // Defense in depth: the state machine's own single-flight guarantee means this
            // should never trigger, but if it ever did, discarding a mismatched result is
            // safer than silently handing back data from an unrelated request.
            state_ = State::Idle;
            return std::nullopt;
        }

        if (entry.snapshot) {
            state_ = State::Idle;
            return entry.snapshot;
        }

        cooldown_until_ = std::chrono::steady_clock::now() + kCooldown;
        state_ = State::Cooldown;
        return std::nullopt;
    }

    std::optional<DepthSnapshot> poll_cooldown() {
        if (std::chrono::steady_clock::now() < cooldown_until_) return std::nullopt;
        state_ = State::Idle;
        return std::nullopt;
    }

    State state_{State::Idle};
    SnapshotFetcher fetcher_;
    std::thread worker_;

    std::optional<MailboxEntry> mailbox_;
    std::atomic<bool> mailbox_ready_{false};
    std::uint64_t next_generation_{0};     // hot-thread-only, no atomic needed
    std::uint64_t inflight_generation_{0}; // hot-thread-only, no atomic needed

    std::chrono::steady_clock::time_point cooldown_until_{};
    static constexpr std::chrono::seconds kCooldown{5};
};

}  // namespace hy
