// SPDX-License-Identifier: proprietary
// account_refresh.hpp — private-stream migration M5a (docs/PRIVATE_STREAM_MIGRATION_DESIGN.md 5.2, 5.3): how a fresh ACCOUNT snapshot gets from the
// thread that may block on a REST call to the thread that decides whether an order may leave.
//
// Why it exists. The barrier is Reconciled only when, besides every in-flight order being answered, "the account snapshot was refreshed by this sweep"
// (design 5.2): balances must not predate the fills the sweep found. GET /api/v3/account is a blocking round trip (PrivateRestClient::fetch_account()
// runs its own io_context to completion), so it cannot run on the hot thread; the reconcile thread already blocks on REST for the order queries and is
// where it belongs. This header is the whole path, and it adds ONE cross-thread thing -- an SpscRing, the primitive the sweep transport already uses:
//
//   reconcile thread   AccountRefresher   fetches the account, puts an AccountUpdate on the ring: periodically (for trading) and once per sweep epoch
//                                         (for the barrier)
//   hot thread         AccountHolder      pops the ring, keeps the newest update, answers the three questions below
//
//   1. refreshed_by(epoch)       "was the account fetched BY the sweep of this epoch?"   -> SweepAccountProbe (sweep_pump.hpp's account argument)
//   2. fresh(now, limits)        "is the snapshot fresh enough to trade?"  -> RecoveryObservation::account_fresh (StartupRecovery's live
//                                prerequisite), by exactly the predicate validate_pre_trade() applies (check_freshness() with the same limits)
//   3. view()                    the snapshot itself                       -> OrchestratorContext::account
//
// "BY THE SWEEP" IS A TAG, NOT A CLOCK READING. The step the sweep runs (barrier_sweep.hpp, SweepAccountStep) executes on the reconcile thread inside
// poll_once(), once per epoch, AFTER every order has had its answer and BEFORE the SweepDone marker is made. What it fetches is therefore newer than
// everything the sweep found, by program order in one thread; the update it publishes carries the epoch (AccountUpdate::sweep_epoch), and the holder
// answers refreshed_by(epoch) from those tags alone. The first version of this path compared clock readings instead -- "was a snapshot fetched at or
// after the request?" -- and that is not enough: the periodic refresh can fetch a moment after the request was posted but before the sweep's last
// order was answered, and a fill that lands in between is then in the sweep and not in that account; with millisecond clocks two events in the same
// millisecond cannot be told apart at all. A periodic update (epoch 0) never counts for a sweep, and neither does an update of an earlier epoch.
//
// THE STAMP IS THE START OF THE FETCH. An AccountUpdate also carries the clock reading taken BEFORE the request left, not the time it was applied or
// the exchange's own clock. The answer reflects the exchange at some moment at or after that reading, so the stamp can only make a snapshot look older
// than it is, never newer. The reading is the steady clock the process uses for everything else (ctx.now_ms, the feed observations, the sweep pump's
// requests): ONE clock, in milliseconds since the steady clock's epoch. The holder re-stamps the snapshot with it (AccountSnapshotBuilder, as the
// preflight harness has always done with its own loop clock), so validate_pre_trade() judges its age in the clock it is given. The stamp decides
// freshness for TRADING only; it plays no part in the sweep question above.
//
// THE ORDER THE BARRIER RELIES ON. The update is pushed on the account ring first, the marker on the event ring second; the hot thread pops the marker
// (acquire) and only then asks refreshed_by(), which drains the account ring first (acquire): everything the reconcile thread did before it pushed the
// marker, the account update included, is visible. A step that delivered nothing -- the fetch was refused by the rate limiter, failed on the wire, or
// the ring was full -- does not hold the marker back; the hot thread then finds no update for the epoch, the barrier stays Sweeping and asks again under
// a new epoch. Fail-closed in every direction.
//
// WHAT IT DOES NOT DO. It does not fetch anything itself (the port is injected, so the tests script it and the production adapter lives with the
// process wiring), it creates no thread, and it decides nothing about orders: whether the account is good enough for a particular order is
// validate_pre_trade()'s job, as before.
//
// THREAD OWNERSHIP. AccountRefresher: the reconcile thread, which is the ring's single producer. AccountHolder: the hot/submit thread, the single
// consumer; nothing here is shared except the ring. Both outlive every call that uses them. No heap, no locks, no exceptions: the port must not throw
// (the production adapter catches whatever PrivateRestClient throws and reports Failed).

#pragma once

#include <hengyuan/account_truth.hpp>
#include <hengyuan/barrier_sweep.hpp>
#include <hengyuan/spsc_ring.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace hy {

// reconcile -> hot: one account snapshot, the clock reading its fetch started at, and which sweep (if any) fetched it.
struct AccountUpdate {
    std::int64_t fetch_started_ms{0};  // steady-clock ms, read before the request left; must be positive to be accepted
    std::uint64_t sweep_epoch{0};      // the epoch of the sweep whose account step fetched it; 0 = the periodic refresh (never counts for a sweep)
    AccountSnapshot snapshot{};        // whatever the port returned; the holder re-stamps it with fetch_started_ms
};

static_assert(std::is_trivially_copyable_v<AccountUpdate>, "AccountUpdate crosses the SpscRing producer/consumer boundary");

// Four slots: one update every ~10 s plus at most one per sweep, drained on every hot-thread cycle. If the hot thread has not looked for four updates
// the newest is dropped (counted) -- by then everything on the ring is stale anyway, and the next push after the hot thread resumes is the fresh one.
inline constexpr std::size_t kAccountUpdateRingCapacity = 4;
using AccountUpdateRing = SpscRing<AccountUpdate, kAccountUpdateRingCapacity>;

enum class AccountFetchOutcome : std::uint8_t {
    NotSent = 0,  // the request never left the process (rate limiter, stale clock, nothing wired): the zero value is not a success
    Fetched = 1,  // `out` holds the account as the exchange reported it
    Failed = 2,   // the request left (or was tried) and no usable answer came back; `out` is untouched
};

// The reconcile thread's way to read the account, injected as a function pointer like QueryPort and SubmitPort. `out` is assigned only on Fetched.
// The call may block (it is a REST round trip) and must not throw.
struct AccountFetchPort {
    using Fn = AccountFetchOutcome (*)(void* user_data, AccountSnapshot& out) noexcept;

    Fn fn{nullptr};
    void* user_data{nullptr};

    [[nodiscard]] AccountFetchOutcome call(AccountSnapshot& out) const noexcept {
        return fn != nullptr ? fn(user_data, out) : AccountFetchOutcome::NotSent;
    }
};

struct AccountRefreshPolicy {
    // Between periodic fetches after one that delivered. Well inside the 30 s window validate_pre_trade() allows (ExposureLimits::freshness_max_age_ms)
    // so that a single missed fetch does not take the account out of date. Each fetch costs weight 20 on the Reconciliation lane.
    std::int64_t period_ms{10'000};
    // After one that did not deliver: sooner than the period (the account should not stay stale for long), never in a tight loop.
    std::int64_t retry_ms{2'000};
};

struct AccountRefresherStats {
    std::uint64_t attempts{0};         // fetches the port was asked for
    std::uint64_t delivered{0};        // updates the ring took
    std::uint64_t not_sent{0};         // the port said NotSent
    std::uint64_t failed{0};           // the port said Failed
    std::uint64_t ring_full{0};        // the port delivered, the ring had no room: the update is dropped
    std::uint64_t bad_clock{0};        // a non-positive clock reading: not fetched (the holder would refuse the stamp)
    std::uint64_t sweep_steps{0};      // times the sweep's account step ran
    std::uint64_t sweep_delivered{0};  // ... of which the update reached the ring
};

class AccountRefresher {
public:
    AccountRefresher(AccountUpdateRing& out, AccountFetchPort port, AccountRefreshPolicy policy = {}) noexcept
        : out_(out),
          port_(port),
          period_ms_(policy.period_ms > 0 ? policy.period_ms : 1),
          retry_ms_(policy.retry_ms > 0 ? policy.retry_ms : 1) {}

    AccountRefresher(const AccountRefresher&) = delete;
    AccountRefresher& operator=(const AccountRefresher&) = delete;

    // The reconcile loop's periodic step, once per tick: the first call fetches at once, then every period (every retry interval while fetches do not
    // deliver). `now_ms` is the tick's steady-clock reading; it is the stamp, and a tick that began long before the fetch makes the stamp older than the
    // fetch really is -- the safe direction. The update carries epoch 0: it keeps the account fit for trading and says nothing to a sweep.
    void tick(std::int64_t now_ms) noexcept {
        if (now_ms < next_due_ms_) return;
        schedule_after(now_ms, fetch(now_ms, 0));
    }

    // What poll_once() calls through SweepJob, once per sweep epoch, after every order of the sweep was answered and before its marker. ALWAYS fetches:
    // an update from before the last answer is exactly what the sweep must not rest on, and the periodic one may be just that. The update is tagged with
    // the epoch.
    [[nodiscard]] SweepAccountStep sweep_step() noexcept { return SweepAccountStep{&AccountRefresher::on_sweep, this}; }

    [[nodiscard]] const AccountRefresherStats& stats() const noexcept { return stats_; }

private:
    static void on_sweep(void* self, std::uint64_t epoch, std::int64_t /*sweep_started_ms*/, std::int64_t now_ms) noexcept {
        static_cast<AccountRefresher*>(self)->sweep(epoch, now_ms);
    }

    void sweep(std::uint64_t epoch, std::int64_t now_ms) noexcept {
        ++stats_.sweep_steps;
        const bool delivered = fetch(now_ms, epoch);
        if (delivered) ++stats_.sweep_delivered;
        schedule_after(now_ms, delivered);
    }

    // True when the update is on the ring.
    bool fetch(std::int64_t now_ms, std::uint64_t sweep_epoch) noexcept {
        if (now_ms <= 0) {
            ++stats_.bad_clock;
            return false;
        }
        ++stats_.attempts;
        AccountUpdate update{};
        update.fetch_started_ms = now_ms;
        update.sweep_epoch = sweep_epoch;
        switch (port_.call(update.snapshot)) {
            case AccountFetchOutcome::Fetched:
                if (!out_.try_push(update)) {
                    ++stats_.ring_full;
                    return false;
                }
                ++stats_.delivered;
                return true;
            case AccountFetchOutcome::NotSent:
                ++stats_.not_sent;
                return false;
            case AccountFetchOutcome::Failed:
                ++stats_.failed;
                return false;
        }
        ++stats_.failed;  // an out-of-range outcome is no success
        return false;
    }

    void schedule_after(std::int64_t now_ms, bool delivered) noexcept {
        const std::int64_t delay = delivered ? period_ms_ : retry_ms_;
        constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
        next_due_ms_ = now_ms > kMax - delay ? kMax : now_ms + delay;
    }

    AccountUpdateRing& out_;
    AccountFetchPort port_;
    std::int64_t period_ms_;
    std::int64_t retry_ms_;
    std::int64_t next_due_ms_{std::numeric_limits<std::int64_t>::min()};  // due at once
    AccountRefresherStats stats_{};
};

struct AccountHolderStats {
    std::uint64_t applied{0};   // updates that became the held snapshot
    std::uint64_t stale{0};     // updates that began before the held one (cannot happen on a FIFO ring; refused all the same)
    std::uint64_t rejected{0};  // updates the snapshot builder refused (a non-positive stamp)
};

class AccountHolder {
public:
    explicit AccountHolder(AccountUpdateRing& in) noexcept : in_(in) {}

    AccountHolder(const AccountHolder&) = delete;
    AccountHolder& operator=(const AccountHolder&) = delete;

    // Drains the ring; the held snapshot is the newest update by the time its fetch began. The loop calls this every cycle; refreshed_by() calls it
    // again itself, so a marker that was just popped never meets a ring it has not read.
    void poll() noexcept {
        AccountUpdate update{};
        while (in_.try_pop(update)) apply(update);
    }

    // Was the account fetched by the sweep of `epoch`, or by a later sweep (whose step also ran after the answers of this one)? False before the
    // first sweep update, for epoch 0, and for an epoch whose step delivered nothing.
    [[nodiscard]] bool refreshed_by(std::uint64_t epoch) noexcept {
        poll();
        return epoch != 0 && refreshed_epoch_ >= epoch;
    }

    // The probe SweepPump takes. The holder must outlive the pump.
    [[nodiscard]] SweepAccountProbe sweep_probe() noexcept { return SweepAccountProbe{&AccountHolder::probe, this}; }

    // Is the held snapshot fresh enough to trade, in the sense validate_pre_trade() uses: the SAME predicate and the same limits, so there is one
    // definition of "fresh account". Reads what poll() last applied.
    [[nodiscard]] bool fresh(std::int64_t now_ms, const ExposureLimits& limits) const noexcept {
        return held_ && check_freshness(snapshot_.timestamp_ms(), now_ms, limits.freshness_max_age_ms, limits.max_future_skew_ms) == FreshnessStatus::Fresh;
    }

    // The held snapshot, always a valid object: before the first update it is a default snapshot ("never fetched", timestamp 0, cannot trade), which
    // validate_pre_trade() refuses. Its address is stable, so it can be wired into OrchestratorContext::account once.
    [[nodiscard]] const AccountSnapshot& view() const noexcept { return snapshot_; }
    [[nodiscard]] bool has_snapshot() const noexcept { return held_; }
    // The stamp of the held snapshot (0 = none yet).
    [[nodiscard]] std::int64_t fetch_started_ms() const noexcept { return held_ ? fetch_started_ms_ : 0; }
    // The highest sweep epoch an applied update was fetched for (0 = none yet).
    [[nodiscard]] std::uint64_t refreshed_epoch() const noexcept { return refreshed_epoch_; }

    [[nodiscard]] const AccountHolderStats& stats() const noexcept { return stats_; }

private:
    static bool probe(void* self, std::uint64_t epoch) noexcept { return static_cast<AccountHolder*>(self)->refreshed_by(epoch); }

    void apply(const AccountUpdate& update) noexcept {
        if (held_ && update.fetch_started_ms < fetch_started_ms_) {
            ++stats_.stale;
            return;
        }
        AccountSnapshotBuilder rebuilt(update.snapshot);
        AccountSnapshot restamped{};
        if (rebuilt.build(update.fetch_started_ms, restamped) != AccountSnapshotError::None) {
            ++stats_.rejected;
            return;
        }
        snapshot_ = restamped;
        fetch_started_ms_ = update.fetch_started_ms;
        held_ = true;
        if (update.sweep_epoch > refreshed_epoch_) refreshed_epoch_ = update.sweep_epoch;
        ++stats_.applied;
    }

    AccountUpdateRing& in_;
    AccountSnapshot snapshot_{};
    std::int64_t fetch_started_ms_{0};
    std::uint64_t refreshed_epoch_{0};
    bool held_{false};
    AccountHolderStats stats_{};
};

}  // namespace hy
