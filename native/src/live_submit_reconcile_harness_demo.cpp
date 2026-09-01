// SPDX-License-Identifier: proprietary
// live_submit_reconcile_harness_demo.cpp — TODO 1A.4 prerequisite: the first process in
// native/src/ that actually runs OrderTracker::poll_once()/drain_reconcile_events() in a
// continuous loop (previously referenced only from test_order_tracker.cpp/
// test_reconcile_concurrency.cpp), alongside a real BinanceUserDataWsSession on a second
// thread draining into the new drain_user_data_events().
//
// Governance: L4. Real Boost.Asio/Beast threading and shutdown sequence, mirroring
// binance_feed_demo.cpp's two-thread model (I/O thread runs ioc.run(), hot thread polls at a
// fixed cadence) -- but SIMULATED at the credential/order boundary, same discipline
// live_submit_evidence_harness.cpp already established: mock SubmitPort AND mock QueryPort
// (no real testnet credentials exist in this environment, and CLAUDE.md's own "Live-trading
// direction & boundary" section is explicit that no AI session handles a real credential).
//
// What IS real here, not simulated: the WS session's DNS resolve/TCP connect/TLS handshake/
// WS handshake/read-loop against stream.binance.com (a listenKey path is required for a real
// user-data-stream subscription -- since create_listen_key() needs real credentials this
// harness deliberately doesn't have, ListenKeyPublisher is seeded with a synthetic
// placeholder key below; Binance will very likely close the connection once it sees an
// invalid listenKey, which is fine -- this harness's job is to prove the THREADING/SHUTDOWN/
// DRAIN-LOOP wiring genuinely runs as a standalone process, not to complete a real private
// session). The two-thread model, the stop()+join()+final-drain shutdown sequence (explicitly
// NOT calling io_context::stop(), see binance_user_data_ws_session.hpp's own header comment
// for why), and the fact that poll_once()/drain_reconcile_events()/drain_user_data_events()
// are all actually being called in a loop by a running process for the first time -- all of
// that is real.
//
// Usage: ./live_submit_reconcile_harness_demo [duration_seconds]
//   Default: 10 seconds. Ctrl+C to stop early.
//
// SIMULATION AT THE CREDENTIAL/ORDER BOUNDARY. No real order. No real credential. No real
// listenKey (Binance will reject the synthetic placeholder). Real network/TLS/threading.

#include <hengyuan/binance_user_data_ws_session.hpp>
#include <hengyuan/live_submit_orchestrator.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

using namespace hy;

namespace {

std::atomic<bool> g_stop{false};
void signal_handler(int) { g_stop.store(true); }

// --- Mock QueryPort, same shape as the GTest suite's own mock_query() -- no real testnet
// credentials exist here. This harness exercises poll_once()/drain_reconcile_events()
// directly, not orchestrate_submit() (which needs a SubmitPort) -- see this file's own header
// comment: the prerequisite gap named in the TODO 1A.4 plan is specifically that the RECONCILE
// loop had never been run continuously, not the submit path (live_submit_evidence_harness.cpp
// already exercises that one, batch-style).

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
    UserDataWsEventRing user_data_events;
    ListenKeyPublisher listen_key_pub;
    QueryPort query_port{mock_query, nullptr};
    ReconcilePollPolicy poll_policy{};

    // Synthetic placeholder listenKey -- see this file's header comment for why a real one
    // can't exist here. Binance will very likely reject the WS handshake target once it
    // inspects this path; that's expected, not a bug in this harness.
    listen_key_pub.publish("HY-DEMO-PLACEHOLDER-NOT-A-REAL-LISTENKEY-0000000000", 0, 0);

    net::io_context ioc;
    ssl::context ssl_ctx(ssl::context::tlsv12_client);
    hy::configure_binance_ssl_context(ssl_ctx);

    UserDataJsonParser ws_parser;
    UserDataWsSessionConfig ws_cfg;  // defaults: stream.binance.com:9443

    auto ws_session = std::make_shared<BinanceUserDataWsSession>(
        ioc, ssl_ctx, user_data_events, ws_parser, listen_key_pub, ws_cfg);
    ws_session->start();

    // I/O thread -- identical model to binance_feed_demo.cpp's own io_thread.
    std::thread io_thread([&ioc]() { ioc.run(); });

    std::printf("=== Live Submit Reconcile Harness Demo ===\n");
    std::printf("SIMULATION AT THE CREDENTIAL/ORDER BOUNDARY. Mock SubmitPort/QueryPort.\n");
    std::printf("Real WS thread against stream.binance.com with a synthetic (invalid)\n");
    std::printf("listenKey -- expect the WS session to fail/stop quickly; that's fine,\n");
    std::printf("this harness's job is the threading/drain-loop wiring, not a real session.\n");
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
        poll_once(tracker, to_reconcile, reconcile_events, query_port, poll_policy, now_ms);
        drain_reconcile_events(in_flight, &audit, reconcile_events, now_ms);
        drain_user_data_events(in_flight, &audit, user_data_events, now_ms);
        ++poll_ticks;

        if (elapsed_ms.count() >= static_cast<std::int64_t>(duration_s) * 1000) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Shutdown -- deliberately NOT calling ioc.stop() here, same reasoning
    // binance_feed_demo.cpp's own shutdown comment and
    // binance_user_data_ws_session.hpp's header comment both give: stop() posts its
    // cancellation onto the session's own strand, and ioc.stop() could make ioc.run() return
    // before that posted work ever executes.
    ws_session->stop();
    if (io_thread.joinable()) io_thread.join();

    // Final drain, safe now that io_thread has been joined.
    poll_once(tracker, to_reconcile, reconcile_events, query_port, poll_policy, now_ms);
    drain_reconcile_events(in_flight, &audit, reconcile_events, now_ms);
    drain_user_data_events(in_flight, &audit, user_data_events, now_ms);

    auto ws_stats = ws_session->stats_snapshot();
    std::printf("\n=== Stats ===\n");
    std::printf("Reconcile-loop ticks: %llu\n", static_cast<unsigned long long>(poll_ticks));
    std::printf("Audit records written: %zu\n", audit.count());
    std::printf("WS messages: %llu  errors: %llu  last_error_stage=%s\n",
                static_cast<unsigned long long>(ws_stats.messages_received),
                static_cast<unsigned long long>(ws_stats.errors),
                ws_stats.last_error_stage.empty() ? "(none)" : ws_stats.last_error_stage.c_str());
    std::printf("WS ring push_ok=%llu push_dropped=%llu\n",
                static_cast<unsigned long long>(ws_stats.push_ok),
                static_cast<unsigned long long>(ws_stats.push_dropped));

    return 0;
}
