// Real two-thread test of the barrier sweep (barrier_sweep.hpp, sweep_pump.hpp, order_tracker.hpp), the cross-thread claim that
// test_order_tracker_sweep.cpp deliberately does not make.
//
// THE CLAIM. The barrier may declare "Reconciled" only when every order that was in flight when its sweep request went out has been asked about
// AFTER the request, and every resolution that produced has been made durable and published on the hot thread. The transport has no shared state
// beyond what already existed: two SpscRings that carry the orders and the resolutions (the sweep's marker rides the second one, FIFO), plus one
// tiny SpscRing that carries the request. So the claim rests on exactly two orderings, both across a thread boundary:
//   (1) the orders the hot thread sent before it posted the request are in the ingress ring by the time the reconcile thread, having taken the
//       request, drains it -- which is why poll_once() takes the request FIRST;
//   (2) a marker is popped by the hot thread only after every resolution pushed before it -- which is the ring's FIFO order.
// This file runs the whole thing for real and checks (1) and (2) at the only moment that matters: the instant the barrier becomes Reconciled.
//
// THE SETUP. Hot thread (this one, the gtest thread): owns the in-flight registry, the audit ring, PositionTruth, the fill table, the durable
// publisher (a counting fake sink), the SubmitReadiness with a scripted feed, the SweepPump. It admits orders as an Ambiguous submit would (registry
// slot, fill baseline, ingress push), disrupts the scripted feed (dropped events -> soft barriers; new generations), refreshes a scripted account
// snapshot, services the pump and drains the reconcile ring with the pump's sink. Reconcile thread: owns the OrderTracker and the SweepJob and runs
// poll_once() against a scripted exchange that answers each order inconclusively or "never sent" a few times before it says Filled -- so sweeps
// stay open across many ticks, restart, and race with new arrivals.
//
// THE CHECK (inside the sink, on the hot thread, at the Sweeping -> Reconciled transition for epoch E): every order admitted before E's request was
// posted is gone from the in-flight registry (resolved, made durable, published, released), and PositionTruth holds exactly the fills of the
// orders released. A violation can only be a missing ordering above.
//
// Per the discipline of test_reconcile_concurrency.cpp: no ASSERT_* on the worker thread; the worker only records into its own result struct, read
// after join. ThreadSanitizer (tools/wsl_verify.sh thread, the CI TSan and arm64 jobs) is what proves the absence of a missing acquire/release edge;
// a green run of this file alone proves the logical contract.
//
// Governance: L1, no network/token/order. GTest only.

#include <gtest/gtest.h>

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

// --- the reconcile thread's exchange (owned by that thread) -----------------------------------------------------------------

struct Exchange {
    std::unordered_map<std::string, int> asked;  // times each coid was asked
    std::uint64_t queries{0};
};

// HY-<n>: n % 3 inconclusive answers first (and, for n % 7 == 0, the first one "never sent"), then Filled.
QueryResult scripted_query(const OrderExpectation& expected, void* user_data) {
    auto* x = static_cast<Exchange*>(user_data);
    const std::string coid(expected.client_order_id.view());
    ++x->queries;
    const int times = ++x->asked[coid];
    unsigned n = 0;
    std::sscanf(coid.c_str(), "HY-%u", &n);
    if (n % 7 == 0 && times == 1) return QueryResult::not_sent();
    if (times <= static_cast<int>(n % 3)) return QueryResult{};  // inconclusive
    return QueryResult{QueryOutcome::Found, OrderState::Filled, 777, expected.intended_qty_ticks, 5000};
}

// The second test's exchange: every query is answered at once.
QueryResult scripted_query_now(const OrderExpectation& expected, void* user_data) {
    auto* x = static_cast<Exchange*>(user_data);
    ++x->queries;
    return QueryResult{QueryOutcome::Found, OrderState::Filled, 777, expected.intended_qty_ticks, 5000};
}

struct ReconcileResult {
    SweepJobStats job{};
    std::uint64_t queries{0};
    std::size_t tracked_left{0};
    std::uint32_t unknown_fate{0};
    std::uint64_t ticks{0};
};

// --- the hot thread's scripted facts (owned by the hot thread) ---------------------------------------------------------------

struct Script {
    FeedObservation feed{};
    RecoveryObservation recovery{};
    bool clock_ready{true};
};
FeedObservation read_feed(void* p) noexcept { return static_cast<Script*>(p)->feed; }
RecoveryObservation read_recovery(void* p) noexcept { return static_cast<Script*>(p)->recovery; }
bool read_clock(void* p) noexcept { return static_cast<Script*>(p)->clock_ready; }

struct Account {
    std::int64_t refreshed_at_ms{0};
};
bool account_fresh(void* p, std::int64_t since_ms) noexcept { return static_cast<Account*>(p)->refreshed_at_ms >= since_ms; }

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

struct Admitted {
    ClientOrderId coid;
};

constexpr std::uint32_t kSymbol = 1;

// What the sink needs: the pump to forward to, the registry to inspect, and the bookkeeping of what each request covered.
struct HotChecks {
    SubmitReadiness* readiness{nullptr};
    SweepPump* pump{nullptr};
    const InFlightRegistry* in_flight{nullptr};
    const PositionTruth* truth{nullptr};
    const std::vector<Admitted>* admitted{nullptr};
    std::unordered_map<std::uint64_t, std::size_t> bound;  // epoch -> orders admitted when its request was posted
    std::uint64_t reconciled_transitions{0};
    std::uint64_t markers{0};
    std::uint64_t violations{0};
    std::uint64_t unbounded_epochs{0};
    std::string first_violation;
};

void checking_sink(void* user_data, const SweepDone& done) noexcept {
    auto* h = static_cast<HotChecks*>(user_data);
    ++h->markers;
    const BarrierState before = h->readiness->barrier().state();
    h->pump->sink().deliver(done);
    if (before == BarrierState::Reconciled || h->readiness->barrier().state() != BarrierState::Reconciled) return;
    ++h->reconciled_transitions;
    const auto it = h->bound.find(done.epoch);
    if (it == h->bound.end()) {
        ++h->unbounded_epochs;  // a Reconciled transition for an epoch this test never saw requested
        return;
    }
    for (std::size_t i = 0; i < it->second; ++i) {
        if (h->in_flight->is_in_flight((*h->admitted)[i].coid.view())) {
            ++h->violations;
            if (h->first_violation.empty()) {
                h->first_violation = std::string("epoch ") + std::to_string(done.epoch) + ": " + std::string((*h->admitted)[i].coid.view()) +
                                     " (admitted #" + std::to_string(i) + " of " + std::to_string(it->second) + ") is still in flight at Reconciled";
            }
        }
    }
}

}  // namespace

TEST(SweepConcurrency, TheBarrierIsReconciledOnlyOnceEveryOrderThatPrecededTheRequestIsResolvedAndPublished) {
    constexpr std::uint32_t kOrders = 300;
    constexpr std::size_t kMaxOpen = 40;  // well under the registry's 64 and the ingress ring's 64: a push never fails
    constexpr std::int64_t kBudgetMs = 40'000;

    ToReconcileRing to_reconcile;
    ReconcileEventRing events;
    SweepRequestRing requests;
    std::atomic<bool> stop{false};

    ReconcileResult reconcile_result;
    std::thread reconcile_thread([&] {
        OrderTracker tracker;
        SweepJob job{requests};
        Exchange exchange;
        const QueryPort port{&scripted_query, &exchange};
        ReconcilePollPolicy policy{};
        policy.base_interval_ms = 1;
        policy.max_interval_ms = 4;
        policy.live_poll_interval_ms = 5;
        policy.sweep_retry_interval_ms = 1;
        policy.max_queries_per_tick = 16;
        policy.quarantine.hard_cap_ms = 3'600'000;  // nothing is escalated in this test: the scripted exchange always gets there
        policy.quarantine.min_elapsed_ms = 3'600'000;
        std::uint64_t ticks = 0;
        while (!stop.load(std::memory_order_acquire)) {
            poll_once(tracker, to_reconcile, events, port, policy, steady_ms(), OrderRecord::kClockUnset, &job);
            ++ticks;
            std::this_thread::yield();
        }
        reconcile_result.job = job.stats();
        reconcile_result.queries = exchange.queries;
        reconcile_result.tracked_left = tracker.count();
        reconcile_result.unknown_fate = tracker.count_unknown_fate();
        reconcile_result.ticks = ticks;
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
    Account account;
    SweepPump pump{readiness, requests, SweepAccountProbe{&account_fresh, &account}};

    std::vector<Admitted> admitted;
    admitted.reserve(kOrders);
    HotChecks checks;
    checks.readiness = &readiness;
    checks.pump = &pump;
    checks.in_flight = &in_flight;
    checks.truth = &truth;
    checks.admitted = &admitted;
    const SweepSink sink{&checking_sink, &checks};

    script.feed.generation = 1;
    script.feed.connected = true;
    Xorshift rng;
    std::uint64_t last_requested = 0;
    std::uint64_t released = 0;
    std::uint64_t iterations = 0;
    std::uint64_t forced_soft_barriers = 0;
    std::uint64_t forced_new_generations = 0;
    std::uint64_t pushed_orders = 0;
    const std::int64_t started = steady_ms();
    bool timed_out = false;

    while (true) {
        const std::int64_t now = steady_ms();
        script.feed.last_activity_ms = now;
        script.feed.sampled_at_ms = now;
        ++iterations;

        // 1. an Ambiguous submit: registry slot, fill baseline, ingress push -- in this order, exactly as orchestrate_submit() does
        if (admitted.size() < kOrders && in_flight.count() < kMaxOpen && (rng.next() & 1U) == 0U) {
            char coid[32];
            std::snprintf(coid, sizeof(coid), "HY-%u", static_cast<unsigned>(admitted.size()));
            const InFlightHandle h = in_flight.register_submit_handle(coid);
            ASSERT_TRUE(h.valid());
            ASSERT_TRUE(fill_context.track(coid, kSymbol, OrderSide::Buy, 0, 0, 0));
            OrderRecord rec{};
            std::snprintf(rec.client_order_id.id, sizeof(rec.client_order_id.id), "%s", coid);
            rec.symbol_id = kSymbol;
            rec.side = OrderSide::Buy;
            rec.intended_price_ticks = 5000;
            rec.intended_qty_ticks = 100;
            rec.state = OrderState::Submitting;
            rec.transition_to(OrderState::Ambiguous);
            ASSERT_TRUE(to_reconcile.try_push(ReconcileIngress{h, rec})) << "the ingress ring must never be the thing that fails here";
            ++pushed_orders;
            Admitted a{};
            a.coid = rec.client_order_id;
            admitted.push_back(a);
        }

        // 2. disruptions of the scripted stream: a lost event (soft barrier), a new generation. Some at fixed points so every run covers both.
        const bool force_soft = iterations == 300 || iterations == 1500 || (rng.next() % 400U) == 0U;
        const bool force_gen = iterations == 800 || iterations == 2500 || (rng.next() % 1500U) == 0U;
        if (force_soft) {
            ++script.feed.ring_drops;
            ++forced_soft_barriers;
        }
        if (force_gen) {
            ++script.feed.generation;
            script.feed.ring_drops = 0;
            script.feed.lost_events = 0;
            ++forced_new_generations;
        }

        // 3. the account snapshot is refreshed most of the time, not always: some sweeps complete with a stale account and are asked again
        if ((rng.next() % 4U) != 0U) account.refreshed_at_ms = now;

        // 4. the pump, then the drain with the checking sink (which forwards to the pump's)
        pump.service();
        if (pump.requested_epoch() != last_requested) {
            last_requested = pump.requested_epoch();
            checks.bound[last_requested] = admitted.size();
        }
        const std::size_t open_before = in_flight.count();
        drain_reconcile_events(in_flight, &audit, events, now, &truth, &fill_context, &publisher, &sink);
        released += open_before > in_flight.count() ? open_before - in_flight.count() : 0;

        // 5. what PositionTruth holds is exactly the fills of the orders released (a resolution is applied with its slot release, on this thread)
        // (checked on every iteration: a slip between the two would show here)
        if (truth.net_qty_ticks(kSymbol) != static_cast<std::int64_t>(100 * (admitted.size() - in_flight.count()))) {
            ++checks.violations;
            if (checks.first_violation.empty()) checks.first_violation = "PositionTruth disagrees with the orders released";
        }

        if (admitted.size() == kOrders && in_flight.count() == 0 && readiness.barrier().state() == BarrierState::Reconciled &&
            readiness.barrier().stats().sweeps_completed >= 3 && forced_new_generations >= 1 && forced_soft_barriers >= 1) {
            break;
        }
        if (now - started > kBudgetMs) {
            timed_out = true;
            break;
        }
        std::this_thread::yield();
    }

    stop.store(true, std::memory_order_release);
    reconcile_thread.join();

    EXPECT_FALSE(timed_out) << "the run did not settle within " << kBudgetMs << " ms: admitted " << admitted.size() << ", in flight " << in_flight.count()
                            << ", barrier " << barrier_state_name(readiness.barrier().state());
    EXPECT_EQ(checks.violations, 0U) << checks.first_violation;
    EXPECT_EQ(checks.unbounded_epochs, 0U);
    EXPECT_EQ(pushed_orders, kOrders);
    EXPECT_EQ(truth.net_qty_ticks(kSymbol), static_cast<std::int64_t>(100 * kOrders)) << "every order was resolved and its fill applied exactly once";
    EXPECT_EQ(in_flight.count(), 0U);
    EXPECT_EQ(reconcile_result.tracked_left, 0U);
    EXPECT_EQ(reconcile_result.unknown_fate, 0U);
    EXPECT_FALSE(publisher.blocked());
    EXPECT_EQ(durable.frames, kOrders) << "one durable frame per resolution and none for any marker";

    // The run really exercised the protocol: several sweeps completed, were restarted by the disruptions, and the account sometimes held one back.
    const BarrierStats& bs = readiness.barrier().stats();
    EXPECT_GE(bs.sweeps_completed, 3U);
    EXPECT_GE(bs.soft_barriers, 1U) << "a lost event must have restarted a sweep inside the run";
    EXPECT_GE(bs.generations_seen, 2U);
    EXPECT_GE(checks.reconciled_transitions, 3U);
    EXPECT_GE(checks.markers, checks.reconciled_transitions);
    EXPECT_GE(pump.stats().requests_sent, 3U);
    EXPECT_GE(reconcile_result.job.sweeps_completed, 3U);
    EXPECT_EQ(pump.stats().markers_received, checks.markers);
    RecordProperty("iterations", static_cast<int>(iterations));
    RecordProperty("markers", static_cast<int>(checks.markers));
    RecordProperty("reconciled_transitions", static_cast<int>(checks.reconciled_transitions));
    RecordProperty("sweeps_started", static_cast<int>(reconcile_result.job.sweeps_started));
    RecordProperty("reconcile_ticks", static_cast<int>(reconcile_result.ticks));
    (void)released;
}

// A second, narrower run aimed at the request/ingress ordering (1): every iteration admits an order and immediately posts a request, so the
// request very often becomes visible together with, or right behind, the order it must cover. The exchange answers at once, so a sweep that
// missed an order shows up as that order still in flight at Reconciled.
TEST(SweepConcurrency, ARequestPostedRightAfterAnOrderAlwaysCoversThatOrder) {
    constexpr std::uint32_t kRounds = 400;

    ToReconcileRing to_reconcile;
    ReconcileEventRing events;
    SweepRequestRing requests;
    std::atomic<bool> stop{false};

    std::thread reconcile_thread([&] {
        OrderTracker tracker;
        SweepJob job{requests};
        Exchange exchange;
        QueryPort port{&scripted_query_now, &exchange};
        ReconcilePollPolicy policy{};
        policy.max_queries_per_tick = 64;
        while (!stop.load(std::memory_order_acquire)) {
            poll_once(tracker, to_reconcile, events, port, policy, steady_ms(), OrderRecord::kClockUnset, &job);
            std::this_thread::yield();
        }
    });

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
    Account account;
    SweepPump pump{readiness, requests, SweepAccountProbe{&account_fresh, &account}};
    std::vector<Admitted> admitted;
    admitted.reserve(kRounds);
    HotChecks checks;
    checks.readiness = &readiness;
    checks.pump = &pump;
    checks.in_flight = &in_flight;
    checks.truth = &truth;
    checks.admitted = &admitted;
    const SweepSink sink{&checking_sink, &checks};

    script.feed.generation = 1;
    script.feed.connected = true;
    std::uint64_t last_requested = 0;
    const std::int64_t started = steady_ms();
    bool timed_out = false;
    std::uint32_t round = 0;

    while (round < kRounds || in_flight.count() != 0) {
        const std::int64_t now = steady_ms();
        script.feed.last_activity_ms = now;
        script.feed.sampled_at_ms = now;
        account.refreshed_at_ms = now;

        if (round < kRounds && in_flight.count() < 30) {
            char coid[32];
            std::snprintf(coid, sizeof(coid), "HY-%u", round);
            const InFlightHandle h = in_flight.register_submit_handle(coid);
            ASSERT_TRUE(h.valid());
            ASSERT_TRUE(fill_context.track(coid, kSymbol, OrderSide::Buy, 0, 0, 0));
            OrderRecord rec{};
            std::snprintf(rec.client_order_id.id, sizeof(rec.client_order_id.id), "%s", coid);
            rec.symbol_id = kSymbol;
            rec.side = OrderSide::Buy;
            rec.intended_price_ticks = 5000;
            rec.intended_qty_ticks = 100;
            rec.state = OrderState::Submitting;
            rec.transition_to(OrderState::Ambiguous);
            ASSERT_TRUE(to_reconcile.try_push(ReconcileIngress{h, rec}));
            Admitted a{};
            a.coid = rec.client_order_id;
            admitted.push_back(a);
            ++round;
            // a trouble event right after the order: the barrier restarts its sweep and the NEW request follows the order into the rings at once
            ++script.feed.ring_drops;
        }

        pump.service();
        if (pump.requested_epoch() != last_requested) {
            last_requested = pump.requested_epoch();
            checks.bound[last_requested] = admitted.size();
        }
        drain_reconcile_events(in_flight, &audit, events, now, &truth, &fill_context, &publisher, &sink);

        if (now - started > 40'000) {
            timed_out = true;
            break;
        }
        std::this_thread::yield();
    }

    stop.store(true, std::memory_order_release);
    reconcile_thread.join();

    EXPECT_FALSE(timed_out);
    EXPECT_EQ(checks.violations, 0U) << checks.first_violation;
    EXPECT_EQ(checks.unbounded_epochs, 0U);
    EXPECT_EQ(in_flight.count(), 0U);
    EXPECT_EQ(truth.net_qty_ticks(kSymbol), static_cast<std::int64_t>(100 * kRounds));
    RecordProperty("reconciled_transitions", static_cast<int>(checks.reconciled_transitions));
}
