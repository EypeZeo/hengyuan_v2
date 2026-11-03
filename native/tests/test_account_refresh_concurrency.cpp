// Real two-thread test of the account refresh path (account_refresh.hpp, barrier_sweep.hpp SweepAccountStep, order_tracker.hpp, sweep_pump.hpp) -- the
// cross-thread claim that test_account_refresh.cpp deliberately does not make.
//
// THE CLAIM. The barrier may become Reconciled only on an account that was fetched AFTER every order of the sweep had its answer, and an account update
// the reconcile thread put on the ring BEFORE it pushed a sweep's marker is on the ring when the hot thread has popped that marker. The path has exactly
// one cross-thread thing that is new, an SpscRing<AccountUpdate, 4>, and the claim rests on two orderings:
//   (1) in the reconcile thread, the sweep's account step runs after the sweep's last query -- program order in one thread; the update it publishes is
//       tagged with the epoch, and the hot thread answers "refreshed by this sweep?" from the tags only (a periodic update, however new, is no answer);
//   (2) the update is pushed on the account ring before the marker is pushed on the event ring, and the hot thread asks only after it popped the marker:
//       release (marker push) / acquire (marker pop) carries the account ring's tail with it.
//
// THE SETUP. Reconcile thread: an OrderTracker, a SweepJob whose account step is a thin wrapper of a real AccountRefresher's (the wrapper only notes how
// many queries had been answered when the step began), and a SCRIPTED account port: every call notes how many
// queries had been answered by then, numbers the snapshot it returns (the number is in two assets, so a torn copy shows) and, in the first test, sometimes
// fails, refuses or dawdles -- while the periodic refresh keeps fetching on its own short period. Hot thread (the gtest thread): the readiness with a
// scripted feed, a real AccountHolder, the pump with the holder's probe, the drain with a checking sink; it admits orders, disrupts the feed (soft
// barriers, new generations) so that sweeps restart many times, and polls the holder every cycle.
//
// THE CHECKS, on the hot thread, at the instant the barrier becomes Reconciled for epoch E: the holder holds an update tagged with E or later; the held
// snapshot is whole (both assets carry the same number); and the fetch it came from was made after at least as many queries had been answered as when
// E's step began -- that is, after the last query of E's sweep. A periodic update from before the sweep's last answer held up as the sweep's account
// would show here: the first test makes the step's own fetch fail often enough for that to happen if the holder let it. In the second test, whose port
// never fails, also: no sweep's marker was ever popped without its account (reports_account_stale == 0) -- the observable consequence of ordering (2);
// a step that ran after the marker would show here as an occasional stale report.
//
// Per the discipline of test_reconcile_concurrency.cpp: no ASSERT_* on the worker thread; the worker only records into its own result struct, read after
// join. ThreadSanitizer (tools/wsl_verify.sh thread, the CI TSan and arm64 jobs) is what proves the absence of a missing acquire/release edge; a green
// run of this file alone proves the logical contract.
//
// Governance: L1, no network/token/order. GTest only.

#include <gtest/gtest.h>

#include <hengyuan/account_refresh.hpp>
#include <hengyuan/kill_switch.hpp>
#include <hengyuan/order_tracker.hpp>
#include <hengyuan/startup_recovery.hpp>
#include <hengyuan/submit_readiness.hpp>
#include <hengyuan/sweep_pump.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace hy;

namespace {

std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Xorshift {
    std::uint64_t s{0x9E3779B97F4A7C15ULL};
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

// --- the reconcile thread's exchange (orders) and account port (owned by that thread, except the arrays, see below) ----------------

struct Exchange {
    std::unordered_map<std::string, int> asked;
    std::uint64_t queries{0};  // answered queries, in order; read by the account port and the step wrapper (same thread)
};

// HY-<n>: n % 3 inconclusive answers first, then Filled -- sweeps stay open across a few ticks.
QueryResult scripted_query(const OrderExpectation& expected, void* user_data) {
    auto* x = static_cast<Exchange*>(user_data);
    const std::string coid(expected.client_order_id.view());
    ++x->queries;
    const int times = ++x->asked[coid];
    unsigned n = 0;
    std::sscanf(coid.c_str(), "HY-%u", &n);
    if (times <= static_cast<int>(n % 3)) return QueryResult{};
    return QueryResult{QueryOutcome::Found, OrderState::Filled, 777, expected.intended_qty_ticks, 5000};
}

constexpr std::size_t kMaxFetches = 200'000;
constexpr std::size_t kMaxEpochs = 200'000;

struct AccountScript {
    const Exchange* exchange{nullptr};
    // Written by the reconcile thread BEFORE the update (or the marker) is pushed (release), read by the hot thread only for facts it received through
    // the rings (acquire) -- ordered by the rings, atomic only to be explicit about the access.
    //   queries_at_call[id]      the queries answered when the id-th fetch was made
    //   queries_at_step[epoch]   the queries answered when the epoch's account step began (= after the epoch's last query)
    std::vector<std::atomic<std::uint64_t>> queries_at_call = std::vector<std::atomic<std::uint64_t>>(kMaxFetches);
    std::vector<std::atomic<std::uint64_t>> queries_at_step = std::vector<std::atomic<std::uint64_t>>(kMaxEpochs);
    std::uint64_t calls{0};  // reconcile thread only
    bool misbehave{false};
    Xorshift rng;
};

AccountFetchOutcome scripted_fetch(void* user_data, AccountSnapshot& out) noexcept {
    auto* s = static_cast<AccountScript*>(user_data);
    const std::uint64_t id = ++s->calls;
    if (id < kMaxFetches) s->queries_at_call[id].store(s->exchange->queries, std::memory_order_relaxed);
    if (s->misbehave) {
        const std::uint64_t r = s->rng.next() % 4U;
        if (r == 0U) return AccountFetchOutcome::Failed;
        if (r == 1U) return AccountFetchOutcome::NotSent;
        if (r == 2U) std::this_thread::sleep_for(std::chrono::milliseconds(1));  // a slow fetch: the sweep waits at its step, the loop is not hot
    }
    AccountSnapshotBuilder b;
    b.set_can_trade(true);
    (void)b.add_asset("USDT", static_cast<std::int64_t>(id));
    (void)b.add_asset("BTC", static_cast<std::int64_t>(id));
    return b.build(1'700'000'000'000, out) == AccountSnapshotError::None ? AccountFetchOutcome::Fetched : AccountFetchOutcome::Failed;
}

// The job's step: notes what the checks need, then runs the real refresher's.
struct StepWrap {
    AccountRefresher* refresher{nullptr};
    AccountScript* script{nullptr};
};
void wrapped_step(void* user_data, std::uint64_t epoch, std::int64_t sweep_started_ms, std::int64_t now_ms) noexcept {
    auto* w = static_cast<StepWrap*>(user_data);
    if (epoch < kMaxEpochs) {
        w->script->queries_at_step[epoch].store(w->script->exchange->queries, std::memory_order_relaxed);
    }
    const SweepAccountStep real = w->refresher->sweep_step();
    real.fn(real.user_data, epoch, sweep_started_ms, now_ms);
}

struct ReconcileResult {
    SweepJobStats job{};
    AccountRefresherStats refresher{};
    std::uint64_t ticks{0};
};

// --- the hot thread's scripted facts (owned by the hot thread) -----------------------------------------------------------------

struct Script {
    FeedObservation feed{};
    RecoveryObservation recovery{};
    bool clock_ready{true};
};
FeedObservation read_feed(void* p) noexcept { return static_cast<Script*>(p)->feed; }
RecoveryObservation read_recovery(void* p) noexcept { return static_cast<Script*>(p)->recovery; }
bool read_clock(void* p) noexcept { return static_cast<Script*>(p)->clock_ready; }

struct Durable {
    std::uint64_t frames{0};
};
AuditAppendResult durable_append(void* p, const AuditRecord&, std::int64_t) noexcept {
    auto* d = static_cast<Durable*>(p);
    AuditAppendResult r{};
    r.status = AuditAppendResult::Status::Acked;
    r.sequence = ++d->frames;
    return r;
}

constexpr std::uint32_t kSymbol = 1;

struct HotChecks {
    SubmitReadiness* readiness{nullptr};
    SweepPump* pump{nullptr};
    AccountHolder* holder{nullptr};
    AccountScript* script{nullptr};
    std::uint64_t reconciled_transitions{0};
    std::uint64_t markers{0};
    std::uint64_t violations{0};
    std::uint64_t unchecked_epochs{0};
    std::string first_violation;

    void violate(const std::string& what) {
        ++violations;
        if (first_violation.empty()) first_violation = what;
    }
};

void checking_sink(void* user_data, const SweepDone& done) noexcept {
    auto* h = static_cast<HotChecks*>(user_data);
    ++h->markers;
    const BarrierState before = h->readiness->barrier().state();
    h->pump->sink().deliver(done);
    if (before == BarrierState::Reconciled || h->readiness->barrier().state() != BarrierState::Reconciled) return;
    ++h->reconciled_transitions;
    if (done.epoch == 0 || done.epoch >= kMaxEpochs) {
        ++h->unchecked_epochs;
        return;
    }
    const std::string where = "epoch " + std::to_string(done.epoch) + ": ";
    if (!h->holder->has_snapshot() || h->holder->refreshed_epoch() < done.epoch) {
        h->violate(where + "Reconciled with no update tagged with this epoch or a later one");
        return;
    }
    const AccountSnapshot& view = h->holder->view();
    const AssetBalance* usdt = view.find("USDT");
    const AssetBalance* btc = view.find("BTC");
    if (usdt == nullptr || btc == nullptr || usdt->free_ticks() != btc->free_ticks()) {
        h->violate(where + "the held snapshot is torn");
        return;
    }
    const auto id = static_cast<std::uint64_t>(usdt->free_ticks());
    if (id == 0 || id >= kMaxFetches) {
        h->violate(where + "the held snapshot carries an impossible fetch number");
        return;
    }
    const std::uint64_t step_queries = h->script->queries_at_step[done.epoch].load(std::memory_order_relaxed);
    const std::uint64_t held_queries = h->script->queries_at_call[id].load(std::memory_order_relaxed);
    if (held_queries < step_queries) {
        h->violate(where + "Reconciled on fetch #" + std::to_string(id) + " made after " + std::to_string(held_queries) + " queries, but the sweep's last query was number " +
                   std::to_string(step_queries) + ": the account predates the sweep's answers");
    }
}

// Runs one scenario; `misbehave` scripts the account port to fail, refuse and dawdle.
struct Outcome {
    HotChecks checks;
    ReconcileResult reconcile;
    SweepPumpStats pump_stats{};
    BarrierStats barrier_stats{};
    AccountHolderStats holder_stats{};
    bool timed_out{false};
    std::uint64_t iterations{0};
    std::uint64_t forced_soft_barriers{0};
    std::uint64_t forced_new_generations{0};
};

void run_scenario(bool misbehave, std::uint64_t min_reconciled, Outcome& result) {
    constexpr std::uint32_t kOrders = 60;
    constexpr std::size_t kMaxOpen = 20;
    constexpr std::int64_t kBudgetMs = 40'000;

    ToReconcileRing to_reconcile;
    ReconcileEventRing events;
    SweepRequestRing requests;
    AccountUpdateRing account_ring;
    AccountScript account_script;
    account_script.misbehave = misbehave;
    std::atomic<bool> stop{false};

    std::thread reconcile_thread([&] {
        OrderTracker tracker;
        Exchange exchange;
        account_script.exchange = &exchange;
        const QueryPort query_port{&scripted_query, &exchange};
        AccountRefresher refresher{account_ring, AccountFetchPort{&scripted_fetch, &account_script}, AccountRefreshPolicy{3, 1}};
        StepWrap wrap{&refresher, &account_script};
        SweepJob job{requests, SweepAccountStep{&wrapped_step, &wrap}};
        ReconcilePollPolicy policy{};
        policy.base_interval_ms = 1;
        policy.max_interval_ms = 4;
        policy.live_poll_interval_ms = 5;
        policy.sweep_retry_interval_ms = 1;
        policy.max_queries_per_tick = 16;
        policy.quarantine.hard_cap_ms = 3'600'000;
        policy.quarantine.min_elapsed_ms = 3'600'000;
        std::uint64_t ticks = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const std::int64_t now = steady_ms();
            refresher.tick(now);
            poll_once(tracker, to_reconcile, events, query_port, policy, now, OrderRecord::kClockUnset, &job);
            ++ticks;
            std::this_thread::yield();
        }
        result.reconcile.job = job.stats();
        result.reconcile.refresher = refresher.stats();
        result.reconcile.ticks = ticks;
    });

    // --- the hot thread ---
    AuditRingSink audit;
    audit.set_available(true);
    InFlightRegistry in_flight;
    PositionTruth truth;
    OrderFillContext fill_context;
    Durable durable;
    ReconcileDurablePublisher publisher{DurableOrderAuditPort{&durable_append, nullptr, &durable}};
    KillSwitch kill_switch;
    StartupRecovery recovery{60'000};
    Script script;
    BarrierPolicy barrier_policy{};
    barrier_policy.resweep_after_ms = 1;
    barrier_policy.sweep_timeout_ms = 400;
    SubmitReadiness readiness{recovery, kill_switch, ReadinessProbe{&read_feed, &read_recovery, &read_clock, &script}, barrier_policy};
    AccountHolder holder{account_ring};
    SweepPump pump{readiness, requests, holder.sweep_probe()};

    HotChecks& checks = result.checks;
    checks.readiness = &readiness;
    checks.pump = &pump;
    checks.holder = &holder;
    checks.script = &account_script;
    const SweepSink sink{&checking_sink, &checks};

    script.feed.generation = 1;
    script.feed.connected = true;
    Xorshift rng;
    std::uint32_t admitted = 0;
    const std::int64_t started = steady_ms();

    while (true) {
        const std::int64_t now = steady_ms();
        script.feed.last_activity_ms = now;
        script.feed.sampled_at_ms = now;
        ++result.iterations;

        // an Ambiguous submit, as orchestrate_submit() does it
        if (admitted < kOrders && in_flight.count() < kMaxOpen && (rng.next() & 3U) == 0U) {
            char coid[32];
            std::snprintf(coid, sizeof(coid), "HY-%u", static_cast<unsigned>(admitted));
            const InFlightHandle h = in_flight.register_submit_handle(coid);
            const bool tracked = h.valid() && fill_context.track(coid, kSymbol, OrderSide::Buy, 0, 0, 0, /*intended_qty_ticks=*/100) &&
                                 fill_context.seed_shadow(coid, DurableShadow{OrderState::Ambiguous, false, 0});
            OrderRecord rec{};
            std::snprintf(rec.client_order_id.id, sizeof(rec.client_order_id.id), "%s", coid);
            rec.symbol_id = kSymbol;
            rec.side = OrderSide::Buy;
            rec.intended_price_ticks = 5000;
            rec.intended_qty_ticks = 100;
            rec.state = OrderState::Submitting;
            rec.transition_to(OrderState::Ambiguous);
            if (!tracked || !to_reconcile.try_push(ReconcileIngress{h, rec})) {
                // no ASSERT_* here: the reconcile thread is running and must be stopped and joined below
                ADD_FAILURE() << "could not admit " << coid << " (the registry, the fill table or the ingress ring is full)";
                result.timed_out = true;
                break;
            }
            ++admitted;
        }

        // disruptions of the scripted stream restart sweeps again and again: a lost event (soft barrier) and a new generation
        if (result.iterations % 150U == 0U || (rng.next() % 300U) == 0U) {
            ++script.feed.ring_drops;
            ++result.forced_soft_barriers;
        }
        if (result.iterations % 700U == 0U || (rng.next() % 1500U) == 0U) {
            ++script.feed.generation;
            script.feed.ring_drops = 0;
            script.feed.lost_events = 0;
            ++result.forced_new_generations;
        }

        holder.poll();
        pump.service();
        drain_reconcile_events(in_flight, &audit, events, now, &truth, &fill_context, &publisher, &sink);

        if (admitted == kOrders && in_flight.count() == 0 && checks.reconciled_transitions >= min_reconciled) break;
        if (now - started > kBudgetMs) {
            result.timed_out = true;
            break;
        }
        std::this_thread::yield();
    }

    stop.store(true, std::memory_order_release);
    reconcile_thread.join();
    result.pump_stats = pump.stats();
    result.barrier_stats = readiness.barrier().stats();
    result.holder_stats = holder.stats();
}

}  // namespace

TEST(AccountRefreshConcurrency, TheBarrierIsReconciledOnlyOnAnAccountFetchedAfterTheSweepsLastAnswer) {
    Outcome r;
    run_scenario(/*misbehave=*/true, /*min_reconciled=*/12, r);
    EXPECT_FALSE(r.timed_out) << "the run did not settle: reconciled transitions " << r.checks.reconciled_transitions << ", markers " << r.checks.markers;
    EXPECT_EQ(r.checks.violations, 0U) << r.checks.first_violation;
    EXPECT_EQ(r.checks.unchecked_epochs, 0U);

    // The run really exercised the protocol: many sweeps, restarted by the disruptions, steps that failed or were refused, periodic fetches in between.
    EXPECT_GE(r.checks.reconciled_transitions, 12U);
    EXPECT_GE(r.barrier_stats.soft_barriers, 1U);
    EXPECT_GE(r.barrier_stats.generations_seen, 2U);
    EXPECT_GE(r.reconcile.job.account_steps, 12U);
    EXPECT_GE(r.reconcile.refresher.failed + r.reconcile.refresher.not_sent, 1U) << "the scripted exchange refused or failed some fetches";
    EXPECT_GE(r.reconcile.refresher.delivered, 12U);
    EXPECT_EQ(r.holder_stats.rejected, 0U);
    EXPECT_EQ(r.holder_stats.stale, 0U);
    RecordProperty("iterations", static_cast<int>(r.iterations));
    RecordProperty("markers", static_cast<int>(r.checks.markers));
    RecordProperty("reconciled_transitions", static_cast<int>(r.checks.reconciled_transitions));
    RecordProperty("account_steps", static_cast<int>(r.reconcile.job.account_steps));
    RecordProperty("stale_reports", static_cast<int>(r.pump_stats.reports_account_stale));
    RecordProperty("reconcile_ticks", static_cast<int>(r.reconcile.ticks));
}

// The same traffic with a port that always delivers: now every marker must find its account on the ring when it is popped. A step that ran after the
// marker was made would let the hot thread pop the marker first and find no update for the epoch -- a stale report. (It would not be unsafe, the
// barrier just asks again; it is the observable proof that the order of the two pushes is the one the design relies on.)
TEST(AccountRefreshConcurrency, ASweepsAccountIsOnTheRingWhenItsMarkerIsPopped) {
    Outcome r;
    run_scenario(/*misbehave=*/false, /*min_reconciled=*/12, r);
    EXPECT_FALSE(r.timed_out) << "the run did not settle: reconciled transitions " << r.checks.reconciled_transitions << ", markers " << r.checks.markers;
    EXPECT_EQ(r.checks.violations, 0U) << r.checks.first_violation;
    EXPECT_EQ(r.checks.unchecked_epochs, 0U);
    EXPECT_GE(r.checks.reconciled_transitions, 12U);
    EXPECT_GE(r.reconcile.job.account_steps, 12U);
    EXPECT_EQ(r.reconcile.refresher.failed + r.reconcile.refresher.not_sent, 0U);
    // A dropped update (the hot thread was descheduled for four updates in a row) is the one honest reason for a stale report here, so a run that saw
    // one proves nothing about the order of the two pushes and makes no claim.
    if (r.reconcile.refresher.ring_full == 0U) {
        EXPECT_EQ(r.pump_stats.reports_account_stale, 0U) << "a marker was popped before the account update that was pushed ahead of it was visible";
    }
    RecordProperty("iterations", static_cast<int>(r.iterations));
    RecordProperty("markers", static_cast<int>(r.checks.markers));
    RecordProperty("reconciled_transitions", static_cast<int>(r.checks.reconciled_transitions));
}
