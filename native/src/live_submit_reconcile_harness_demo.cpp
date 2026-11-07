// SPDX-License-Identifier: proprietary
// live_submit_reconcile_harness_demo.cpp — the reconcile loop in a running process: nothing real at the credential/order boundary, and no network.
//
// TODO 1A.4 prerequisite: the first process in native/src/ that runs OrderTracker::poll_once() and drain_reconcile_events() in a continuous
// loop (previously referenced only from test_order_tracker.cpp / test_reconcile_concurrency.cpp). Until the private-stream migration retired
// the listenKey stack (M5b-2) it ran next to a real BinanceUserDataWsSession on a second thread, fed by a synthetic listenKey the exchange
// rejected on purpose -- which proved the threading, shutdown and reconnect wiring of THAT session. The session, its supervisor and the
// listenKey endpoints are gone (the exchange removed them on 2026-02-20), and with them the I/O thread, the TLS context and the user-data
// drain this file used to call. What is left is the reconcile loop itself, with a mock QueryPort (Inconclusive by default) and an in-flight
// table nothing ever fills: poll_once() has nothing to ask and drain_reconcile_events() nothing to drain.
//
// It adds nothing live_submit_preflight_harness.cpp (M5b-1) does not do against the real exchange: that harness is the first process to run
// this loop together with the private stream, the startup recovery and the reconnect barrier, and its Testnet joint run passed on 2026-11-07.
// This demo stays as the smallest runnable example of the loop, and because test_reconcile_durable_guard.cpp pins its drain call.
//
// Governance: L4. SIMULATED at the credential/order boundary, same discipline live_submit_evidence_harness.cpp established: mock QueryPort
// (no real testnet credentials exist in this environment, and CLAUDE.md's "Live-trading direction & boundary" section is explicit that no AI
// session handles a real credential). No real order. No real credential. No network.
//
// Usage: ./live_submit_reconcile_harness_demo [duration_seconds]
//   Default: 10 seconds. Ctrl+C to stop early.

#include <hengyuan/live_submit_orchestrator.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace hy;

namespace {

std::atomic<bool> g_stop{false};
void signal_handler(int) { g_stop.store(true); }

// --- Mock QueryPort, same shape as the GTest suite's own mock_query() -- no real testnet
// credentials exist here. This harness exercises poll_once()/drain_reconcile_events()
// directly, not orchestrate_submit() (which needs a SubmitPort): the prerequisite gap named in
// the TODO 1A.4 plan was that the RECONCILE loop had never been run continuously, not the
// submit path (live_submit_evidence_harness.cpp already exercises that one, batch-style).

QueryResult mock_query(const OrderExpectation& /*expected*/, void* /*ud*/) noexcept {
    // Inconclusive by default -- this harness's point is to prove the poll loop RUNS
    // continuously, not to exercise every reconciliation outcome (that's the GTest suite's
    // job). An empty in-flight table means poll_once() has nothing to query most ticks anyway.
    return {};
}

}  // namespace

int main(int argc, char* argv[]) {
    int duration_s = 10;
    if (argc > 1) duration_s = std::atoi(argv[1]);
    if (duration_s <= 0) duration_s = 10;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // --- Real components this harness genuinely exercises ---
    InFlightRegistry in_flight;
    AuditRingSink audit;
    audit.set_available(true);
    OrderTracker tracker;
    ToReconcileRing to_reconcile;
    ReconcileEventRing reconcile_events;
    QueryPort query_port{mock_query, nullptr};
    ReconcilePollPolicy poll_policy{};
    PositionTruth position_truth;
    OrderFillContext fill_context;

    std::printf("=== Live Submit Reconcile Harness Demo ===\n");
    std::printf("SIMULATION AT THE CREDENTIAL/ORDER BOUNDARY. Mock QueryPort, no network.\n");
    std::printf("The reconcile loop (poll_once / drain_reconcile_events) runs continuously;\n");
    std::printf("nothing is ever submitted, so there is nothing to reconcile.\n");
    std::printf("Running for %d seconds (Ctrl+C to stop early).\n\n", duration_s);

    std::int64_t now_ms = 0;
    auto start = std::chrono::steady_clock::now();
    std::uint64_t poll_ticks = 0;
    while (!g_stop.load()) {
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        now_ms = elapsed_ms.count();

        // This is the historical gap TODO 1A.4's own plan named explicitly: poll_once()/
        // drain_reconcile_events() had never been called from a running process anywhere in
        // native/src/ before this harness -- only from tests.
        poll_once(tracker, to_reconcile, reconcile_events, query_port, poll_policy, now_ms,
                  reconcile_wall_now_ms());
        // Memory-only on purpose (no ReconcileDurablePublisher): this demo has no DurableAuditSink and
        // submits nothing, so the ring is always empty. The day it gets a real submit path it must pass a
        // publisher here -- test_reconcile_durable_guard pins this call site so that cannot happen silently.
        drain_reconcile_events(in_flight, &audit, reconcile_events, now_ms, &position_truth,
                                &fill_context);
        ++poll_ticks;

        if (elapsed_ms.count() >= static_cast<std::int64_t>(duration_s) * 1000) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::printf("\n=== Stats ===\n");
    std::printf("Reconcile-loop ticks: %llu\n", static_cast<unsigned long long>(poll_ticks));
    std::printf("Audit records written: %zu\n", audit.count());

    return 0;
}
