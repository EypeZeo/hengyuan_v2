// Real two-thread stress test for the reconcile/poll loop (order_tracker.hpp)
// wired through live_submit_orchestrator.hpp.
//
// WHY THIS FILE EXISTS
// ---------------------
// test_order_tracker.cpp and test_live_submit_orchestrator.cpp both exercise
// poll_once()/drain_reconcile_events()/orchestrate_submit() as plain,
// single-threaded functions -- real, but they cannot fall over on a missing
// memory_order edge because nothing ever actually crosses a thread boundary in
// those tests. order_tracker.hpp's whole reason to exist is the claim that
// InFlightRegistry/AuditRingSink (hot-thread-owned) and OrderTracker
// (reconcile-thread-owned) can be driven from two REAL concurrent threads,
// synchronized only by the two SpscRing<T,N> queues, with no atomics on either
// owned structure. This file is what actually tests that claim, in the same
// spirit as test_spsc_concurrency.cpp (see that file's header for the fuller
// explanation of what a green run can and cannot prove on its own -- the same
// caveats apply here: this proves the LOGICAL contract across the boundary;
// ThreadSanitizer (-DHY_SANITIZER=thread, see native/cmake/Sanitizers.cmake and
// tools/wsl_verify.sh) is what actually proves no missing acquire/release edge,
// and is required, not optional, before trusting this file's design.
//
// SIMULATED THREAD ROLES
// -----------------------
//   Hot thread:       repeatedly calls orchestrate_submit() -- which itself
//                      calls drain_reconcile_events() at its own top, exactly
//                      as a real trading hot loop would. Owns ctx_.in_flight
//                      and ctx_.audit exclusively for the whole run.
//   Reconcile thread:  repeatedly calls poll_once() -- owns its OrderTracker
//                      exclusively for the whole run, never touches
//                      ctx_.in_flight or ctx_.audit directly (see
//                      order_tracker.hpp's file header, the thread-ownership
//                      contract this test exists to falsify-or-confirm).
//
// Only 5 of this run's orders are left Accepted (never resolved, by design --
// open-order polling until fill/cancel is explicitly out of scope, see
// order_tracker.hpp's file header) so they permanently hold a slot each; the
// rest are forced Ambiguous (mock Timeout) so the registry churns through
// reconcile continuously, which is the actual condition under test.
//
// GTEST + THREADS DISCIPLINE: never ASSERT_* on a worker thread (see
// test_spsc_concurrency.cpp's header for why) -- each thread records into its
// own result container; all assertions run after both threads join.
//
// Governance: L1, no network/token/order. GTest only.

#include <gtest/gtest.h>
#include <hengyuan/live_submit_orchestrator.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <unistd.h>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace hy;

namespace {

// --- Mock SubmitPort (hot thread only touches this) ---

thread_local SubmitOutcome t_next_submit_outcome{SubmitOutcome::Accepted};

SubmitResponse mock_submit(
    const char* /*coid*/, std::uint32_t /*sym*/,
    OrderSide /*side*/, OrderType /*type*/,
    std::int64_t /*price*/, std::int64_t /*qty*/,
    const SymbolRules& /*rules_snapshot*/, void* /*ud*/) {
    if (t_next_submit_outcome == SubmitOutcome::Accepted) {
        return SubmitResponse{SubmitOutcome::Accepted, 42, 0};
    }
    return SubmitResponse{t_next_submit_outcome, 0, 0};
}

std::uint32_t mock_current_rules_version(void* /*ud*/) { return 1; }

// --- Mock QueryPort (reconcile thread only touches this) ---
// Always resolves Found -> Filled on the very first attempt: this test's
// point is the cross-thread handoff, not backoff/retry timing (already
// covered single-threaded by test_order_tracker.cpp).

QueryResult mock_query(const char* /*coid*/, void* /*ud*/) {
    return QueryResult{QueryOutcome::Found, OrderState::Filled,
                        /*exchange_order_id=*/777, /*filled_qty=*/10,
                        /*avg_price=*/5000};
}

struct SubmittedOrder {
    ClientOrderId coid;
    OrchestratorGate gate;
};

}  // namespace

TEST(ReconcileConcurrency, HotThreadAndReconcileThreadRaceCleanly) {
    // Bounded well under AuditRingSink's fixed kAuditRingCapacity (1024,
    // audit_trail.hpp) -- that capacity is a deliberate fail-closed production
    // limit (ADR-019 D10: audit unavailable -> no new orders), not something
    // this test should grow into. Each Accepted order writes ~3 records
    // (intent/submitted/accepted); each Ambiguous order writes ~4 (the same 3
    // plus one OrderReconciled once resolved). 200 iterations stays far below
    // the cap with margin, while still forcing many full churn cycles through
    // InFlightRegistry's 64-slot capacity -- the actual condition under test.
    constexpr int kIterations = 200;
    // First N orders come back Accepted rather than Timeout. Before audit
    // EXEC-INFLIGHT-003 these were "never resolved, stay in-flight forever" -- which
    // was the defect, not a property worth preserving. They now flow through the same
    // reconcile handoff as the Ambiguous ones and reach a terminal state, so this
    // prefix exercises the Accepted->reconcile path across the thread boundary.
    constexpr int kAcceptedPrefix = 5;

    AuditRingSink audit;
    KillSwitch kill_switch;
    DryRunEvidenceChain evidence;
    RequestWeightTracker rate_tracker;
    InFlightRegistry in_flight;
    SymbolRules rules{};
    AccountSnapshot account{};
    ToReconcileRing to_reconcile;
    ReconcileEventRing reconcile_events;

    audit.set_available(true);
    kill_switch.operator_reset();
    evidence.record(EvidencePath::SubmitSuccess, 1, 0xABCD, 1);
    evidence.record(EvidencePath::SubmitReject, 2, 0xABCD, 1);
    evidence.record(EvidencePath::SubmitAmbiguous, 3, 0xABCD, 1);
    evidence.record(EvidencePath::KillSwitch, 4, 0xABCD, 1);
    rate_tracker.reset(100000, 500);  // headroom well above kIterations

    std::strncpy(rules.symbol, "BTCUSDT", sizeof(rules.symbol) - 1);
    rules.is_trading = true;
    rules.min_price_ticks = 100;
    rules.max_price_ticks = 10000000;
    rules.tick_size_ticks = 100;
    rules.min_qty_ticks = 10;
    rules.max_qty_ticks = 1000000;
    rules.step_size_ticks = 10;
    rules.min_notional_ticks = 1000;
    // price_scale=0, qty_scale=8 (native=8=kBalanceScale) makes
    // rescale_notional_ceil() an identity transform, matching the fixture
    // fix applied in test_account_truth.cpp / test_live_submit_orchestrator.cpp
    // for the same PR 5a scale-mismatch change.
    rules.price_scale = 0;
    rules.qty_scale = 8;
    rules.rules_version = 1;

    std::strncpy(account.assets[0].asset, "USDT", 5);
    account.assets[0].free_ticks = 999999999;
    account.asset_count = 1;
    account.can_trade = true;
    account.timestamp_ms = 900;

    // Phase 3: a single shared real DurableAuditSink for the hot thread's
    // orchestrate_submit() calls -- constructed here, outside the hot loop,
    // and never touched by the reconcile thread, so the single-writer
    // constraint DurableAuditSink documents is unaffected by this test's
    // two-thread design (reconcile thread only calls poll_once(), which never
    // touches ctx.audit/ctx.durable_audit/ctx.in_flight).
    std::string durable_audit_path;
#ifdef _WIN32
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    durable_audit_path = std::string(tmp) + "hy_reconcile_concurrency_" +
                          std::to_string(GetCurrentProcessId()) + ".log";
#else
    durable_audit_path = "/tmp/hy_reconcile_concurrency_" + std::to_string(getpid()) + ".log";
#endif
    std::remove(durable_audit_path.c_str());
    std::remove((durable_audit_path + ".lock").c_str());
    std::remove((durable_audit_path + ".tip").c_str());
    std::remove((durable_audit_path + ".tip.tmp").c_str());

    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x60 + i);
    auto key_ring = std::make_unique<KeyRing>(kek);
    WrappedKeyRecord key_record{};
    std::vector<std::byte> key_material{std::byte{0x0A}, std::byte{0x0B}, std::byte{0x0C}};
    ASSERT_EQ(key_ring->add_key(1, key_material, key_record), KeyRingAddStatus::Ok);
    auto durable_audit_sink = std::make_unique<DurableAuditSink>(durable_audit_path, *key_ring, 1);
    ASSERT_TRUE(durable_audit_sink->is_open());
    DurableOrderAuditPort durable_audit_port = make_durable_order_audit_port(*durable_audit_sink);

    std::vector<SubmittedOrder> results;
    results.reserve(kIterations);
    std::atomic<bool> hot_done{false};
    std::atomic<bool> reconcile_ready{false};

    std::thread hot_thread([&] {
        // Wait for the reconcile thread to actually be executing before
        // submitting anything. Without this, OS thread-creation/scheduling
        // latency can let the hot thread burn through most or all of
        // kIterations solo before the reconcile thread's first instruction
        // ever runs, which starves InFlightRegistry's 64 slots (nothing is
        // draining them) and degenerates this into an accidentally-sequential
        // run instead of the genuinely concurrent one this file exists to be.
        while (!reconcile_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < kIterations; ++i) {
            OrchestratorContext ctx{};
            ctx.audit = &audit;
            ctx.kill_switch = &kill_switch;
            ctx.evidence = &evidence;
            ctx.signer_ready = true;
            ctx.depth_synced = true;
            ctx.rate_tracker = &rate_tracker;
            ctx.in_flight = &in_flight;
            ctx.symbol_rules = &rules;
            ctx.account = &account;
            ctx.side = OrderSide::Buy;
            ctx.order_type = OrderType::Limit;
            ctx.base_asset = "BTC";
            ctx.quote_asset = "USDT";
            ctx.now_ms = 1000 + i;
            ctx.exposure_limits.freshness_max_age_ms = 3'000'000;
            ctx.exposure_limits.single_order_notional_cap = 500000;
            ctx.exposure_limits.total_exposure_notional_cap = 100'000'000;
            ctx.price_ticks = 1000;
            ctx.qty_ticks = 10;
            ctx.symbol_id = 1;
            ctx.sequence = static_cast<std::uint32_t>(i);
            ctx.mode = ExecutionMode::Live;
            ctx.order_weight = 1;
            ctx.to_reconcile = &to_reconcile;
            ctx.reconcile_events = &reconcile_events;
            ctx.durable_audit = durable_audit_port;

            ctx.confirmation.confirmed = true;
            ctx.confirmation.symbol_id = ctx.symbol_id;
            ctx.confirmation.side = ctx.side;
            ctx.confirmation.type = ctx.order_type;
            ctx.confirmation.price_ticks = ctx.price_ticks;
            ctx.confirmation.qty_ticks = ctx.qty_ticks;
            ctx.confirmation.max_notional = 1000000;
            ctx.confirmation.valid_until_ms = ctx.now_ms + 60000;

            t_next_submit_outcome = (i < kAcceptedPrefix) ? SubmitOutcome::Accepted
                                                            : SubmitOutcome::Timeout;
            ctx.submit_port = {mock_submit, nullptr, mock_current_rules_version};

            auto r = orchestrate_submit(ctx);
            results.push_back(SubmittedOrder{r.order.client_order_id, r.gate});

            // Keeps ceding the core between iterations so a fast hot thread
            // doesn't simply outrun the reconcile thread's scheduling slice
            // for the whole loop -- pacing only, not a change to what's tested.
            std::this_thread::yield();
        }
        hot_done.store(true, std::memory_order_release);
    });

    std::thread reconcile_thread([&] {
        OrderTracker tracker;
        QueryPort query_port{mock_query, nullptr};
        ReconcilePollPolicy policy{};
        std::int64_t now_ms = 0;

        reconcile_ready.store(true, std::memory_order_release);

        // Keep polling until the hot thread is done AND every order it handed
        // off has been drained out of the tracker -- a couple of extra passes
        // after observing hot_done covers the "pushed just before done was
        // set" straggler case (poll_once's inbound drain is a full `while`
        // loop, so a single post-done pass is sufficient in practice; looping
        // a few extra times costs nothing and removes any doubt).
        int extra_passes_after_done = 0;
        while (true) {
            poll_once(tracker, to_reconcile, reconcile_events, query_port, policy, now_ms++);
            if (hot_done.load(std::memory_order_acquire)) {
                if (tracker.count() == 0) {
                    ++extra_passes_after_done;
                    if (extra_passes_after_done >= 3) break;
                } else {
                    extra_passes_after_done = 0;
                }
            }
        }
    });

    hot_thread.join();
    reconcile_thread.join();

    // Both threads have joined -- no concurrent access remains. Catch any
    // ReconcileEvent the reconcile thread pushed after the hot thread's last
    // own drain_reconcile_events() call (its own top-of-call drain only runs
    // while the hot thread is still making calls).
    drain_reconcile_events(in_flight, &audit, reconcile_events, /*now_ms=*/999999);

    ReconcileEvent leftover{};
    EXPECT_FALSE(reconcile_events.try_pop(leftover)) << "reconcile_events ring must be fully drained by now";

    ASSERT_EQ(results.size(), static_cast<std::size_t>(kIterations));

    int accepted_count = 0, ambiguous_count = 0, other_count = 0;
    for (int i = 0; i < kIterations; ++i) {
        const auto& r = results[static_cast<std::size_t>(i)];
        if (r.gate == OrchestratorGate::SubmitAccepted) {
            ++accepted_count;
            // AUDIT EXEC-INFLIGHT-003: this used to assert the slot was STILL HELD,
            // because nothing could ever release it. An Accepted order now reaches the
            // reconcile loop, gets polled, and (mock_query returns Filled) resolves to
            // a terminal state, so its slot must be released exactly like an
            // Ambiguous one's.
            EXPECT_FALSE(in_flight.is_in_flight(r.coid.view()))
                << "Accepted order must reach a terminal state and release its slot, i=" << i;
        } else if (r.gate == OrchestratorGate::SubmitAmbiguous) {
            ++ambiguous_count;
            EXPECT_FALSE(in_flight.is_in_flight(r.coid.view()))
                << "Ambiguous order must have been resolved and released by the reconcile "
                   "thread + drain by now, i=" << i;
        } else {
            // OrchestratorGate::DuplicateInFlight (registry momentarily at
            // capacity) is the only other reachable outcome here -- every
            // earlier gate is satisfied unconditionally by this fixture's
            // setup. Not asserted against: natural backpressure, not a bug.
            ++other_count;
            EXPECT_EQ(r.gate, OrchestratorGate::DuplicateInFlight)
                << "unexpected gate at i=" << i;
        }
    }

    EXPECT_EQ(accepted_count, kAcceptedPrefix);
    // The invariant the whole gate chain rests on (protocol §31: an accepted request
    // must eventually reach a terminal state): once both threads have joined and the
    // final drain has run, NOTHING is still holding a slot. Before audit
    // EXEC-INFLIGHT-003 this could only ever have been `accepted_count`, because
    // Accepted orders had no route to a terminal state at all.
    EXPECT_EQ(in_flight.count(), 0u)
        << "every submitted order must have reached a terminal state and released its slot";

    RecordProperty("ambiguous_resolved", ambiguous_count);
    RecordProperty("duplicate_or_capacity_exhausted", other_count);

    // Sanity: this run must actually have exercised the reconcile path, not
    // degenerated into "everything hit capacity exhaustion immediately." The
    // per-order invariants above (is_in_flight matching gate outcome) are the
    // real correctness check; this threshold only guards against a scheduling
    // fluke making the whole run accidentally-sequential and vacuous. Deliberately
    // loose (not "> kIterations/2"): OS thread scheduling is not something this
    // test controls precisely, and the startup barrier + per-iteration yield
    // above only make good interleaving LIKELY, not guaranteed on every run.
    EXPECT_GT(ambiguous_count, kIterations / 8)
        << "too few orders reached the reconcile path -- test may not be exercising the "
           "cross-thread handoff it exists to verify";

    // Release the exclusive file/lock handles before removing them -- both are
    // opened with zero sharing (durable_log_store.hpp), so deleting while
    // still open would silently no-op on Windows.
    durable_audit_sink.reset();
    key_ring.reset();
    std::remove(durable_audit_path.c_str());
    std::remove((durable_audit_path + ".lock").c_str());
    std::remove((durable_audit_path + ".tip").c_str());
    std::remove((durable_audit_path + ".tip.tmp").c_str());
}
