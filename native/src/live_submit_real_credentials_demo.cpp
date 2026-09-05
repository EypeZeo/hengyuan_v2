// SPDX-License-Identifier: proprietary
// live_submit_real_credentials_demo.cpp — Batch H, H6: cold-start harness wiring H1-H5's
// components together against a REAL (owner-supplied, testnet-only) API key/secret pair,
// closing the gap live_submit_reconcile_harness_demo.cpp's own header comment names
// explicitly ("no real testnet credentials exist in this environment... mock SubmitPort AND
// mock QueryPort").
//
// CLAUDE.md's "Live-trading direction & boundary" red line is unchanged by this file: no AI
// session that writes/runs this code ever loads or uses a real credential. What this harness
// demonstrates is that the wiring COMPILES and, when an owner supplies their own testnet
// .env, correctly walks the cold-start sequence and runs a real reconcile/WS-drain loop.
// The main loop never calls SubmitPort::call()/orchestrate_submit() -- SubmitPort is
// constructed (proving H5's composite adapters assemble correctly against a real
// SymbolRegistry) but never invoked. Real order submission is a later, separate, owner-
// triggered step, not in this file's scope.
//
// Testnet only -- CLI intentionally has no --production flag (owner decision, see the batch
// plan). .env is read from a fixed, compile-time path (".env", resolved against the process's
// current working directory) per ADR-019 D4: never sourced from argv.
//
// Usage: run from the repository root (so ./.env resolves), after `chmod 600 .env`:
//   ./live_submit_real_credentials_demo [duration_seconds]
//   Default: 30 seconds. Ctrl+C to stop early.

#include <hengyuan/binance_listen_key_keepalive.hpp>
#include <hengyuan/binance_submit_adapter.hpp>
#include <hengyuan/binance_user_data_ws_supervisor.hpp>
#include <hengyuan/env_loader.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace hy;

namespace {

std::atomic<bool> g_stop{false};
void signal_handler(int) { g_stop.store(true); }

// Minimal DurableControlPlaneSink so SymbolRegistry::refresh_from_exchange_info() has
// somewhere to durably (in this demo's case, trivially) publish to. Physically independent
// of native/tests/test_symbol_registry_fakes.hpp's FakeSymbolRegistrySink -- that header is
// test-only, and this src/ binary must not reach across into native/tests/. The real
// production sink (ControlPlaneLogSink) needs a KeyRing + disk path + exclusive lock file,
// which is out of scope for a cold-start wiring demo; only append_snapshot() is ever
// exercised here (once, at startup), so every other override is an unused stub, same shape
// as FakeSymbolRegistrySink's own stubs.
class DemoControlPlaneSink final : public DurableControlPlaneSink {
public:
    AuditAppendResult append_snapshot(const SymbolRegistrySnapshotPayload& snap,
                                       std::span<const SymbolRules> /*entries*/) noexcept override {
        (void)snap;
        return AuditAppendResult{AuditAppendResult::Status::Acked, ++sequence_};
    }

    AuditAppendResult append_rate_freeze(const RateLimitFreezePayload&,
                                          FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_compacted_freeze_snapshot(
        const RateLimitFreezePayload&, FrameTimeKind,
        const CompactionFreezeSnapshotProof&) noexcept override {
        return {};
    }
    AuditAppendResult append_compacted_wait_evidence(
        const CompactedFreezeWaitEvidencePayload&, FrameTimeKind,
        const CompactionWaitEvidenceProof&) noexcept override {
        return {};
    }
    AuditAppendResult append_weight_config(const EndpointWeightConfig&) noexcept override {
        return {};
    }
    AuditAppendResult append_usage_snapshot(
        const RateLimitUsageSnapshotPayload&) noexcept override {
        return {};
    }
    AuditAppendResult append_generation_bridge(const GenerationBridgePayload&) noexcept override {
        return {};
    }
    AuditAppendResult append_operator_override(const OperatorOverridePayload&) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_probe_attempt(const FreezeProbeAttemptPayload&,
                                                   FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_clear(const FreezeClearPayload&,
                                           FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_wait_arm(const FreezeWaitArmPayload&,
                                              FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_wait_satisfied(const FreezeWaitSatisfiedPayload&,
                                                    FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_epoch_watermark(std::uint32_t,
                                                     FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_seal_journal_apply(const SealJournalAppliedView&) noexcept override {
        return {};
    }
    RecoveryScanStatus recover_control_plane(
        RateLimitFreezePayload&, bool&, bool&, std::uint8_t&, EndpointWeightConfig&, bool&,
        RateLimitUsageSnapshotPayload&, bool&, GenerationBridgePayload&, bool&, std::uint32_t&,
        bool&, std::array<FreezeProbeAttemptPayload, 8>&, std::size_t&, FreezeClearPayload&,
        bool&, FreezeWaitSatisfiedPayload&, bool&, FreezeWaitArmPayload&,
        bool&) noexcept override {
        // Fresh cold-start process, never a crash-recovery scan.
        return RecoveryScanStatus::IoError;
    }

private:
    std::uint64_t sequence_{0};
};

void print_env_load_error(EnvLoadStatus status) {
    switch (status) {
        case EnvLoadStatus::FileNotFound:
            std::fprintf(stderr,
                         "ERROR: failed to load .env (status=FileNotFound). Please ensure "
                         ".env exists in the CURRENT WORKING DIRECTORY (not the build/ or IDE "
                         "debug directory) -- run this demo from the repository root.\n");
            break;
        case EnvLoadStatus::PermissionTooWide:
            std::fprintf(stderr,
                         "ERROR: failed to load .env (status=PermissionTooWide). Linux "
                         "requires no group/other permission bits: chmod 600 .env\n");
            break;
        default:
            std::fprintf(stderr, "ERROR: failed to load .env (status=%d).\n",
                         static_cast<int>(status));
            break;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    int duration_s = 30;
    if (argc > 1) duration_s = std::atoi(argv[1]);
    if (duration_s <= 0) duration_s = 30;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Single monotonic time source shared by EVERY get_now_ms() call below, from the initial
    // listenKey publish() through the hot loop -- see the batch plan's own P0-severity finding
    // on this file: mixing an absolute wall-clock timestamp into publish() while the hot loop
    // measures elapsed-since-start would permanently starve ListenKeyKeepaliveScheduler::poll()
    // (its next_attempt_at_ms_ would never again be <= now_ms).
    const auto start_time = std::chrono::steady_clock::now();
    auto get_now_ms = [&start_time]() -> std::int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start_time)
            .count();
    };

    SecureEnvLoader loader;
    EnvironmentBinding binding = EnvironmentBinding::testnet();
    std::array<std::string_view, 2> allowed_keys{binding.api_key_env_key(),
                                                  binding.secret_env_key()};
    EnvAllowlist allowlist{allowed_keys.data(), allowed_keys.size()};

    const auto load_result = loader.load(".env", allowlist);
    if (load_result.status != EnvLoadStatus::Ok) {
        print_env_load_error(load_result.status);
        return 1;
    }

    // load_and_bind_credentials() captures THIS thread's id as the only thread ever allowed
    // to sign a request -- main() must be, and remain for the rest of the process, the
    // hot/submit/reconcile thread. Only ioc.run() is handed to a background thread below.
    auto [err, creds] = BoundHmacCredentials::load_and_bind_credentials(binding, loader);
    if (err != QuerySigningError::Ok) {
        std::fprintf(stderr, "ERROR: failed to bind credentials (QuerySigningError=%d).\n",
                     static_cast<int>(err));
        return 1;
    }

    // H5's confirmed declaration order (SymbolRegistry outlives client outlives submit_ctx
    // outlives submit_port -- reverse-destruction keeps every pointer valid for as long as
    // submit_port itself exists).
    SymbolRegistry registry;
    BinancePrivateRestClient client(binding, std::move(creds));
    SubmitAdapterContext submit_ctx{&client, &registry};
    SubmitPort submit_port = make_binance_submit_port(submit_ctx);

    DemoControlPlaneSink cp_sink;
    ParsedExchangeInfo parsed{};
    std::array<char, kListenKeyBufferLen> listen_key_buf{};
    std::size_t listen_key_len = 0;

    // Every real network call in cold start shares one try/catch: sync_clock()/
    // fetch_exchange_info()/create_listen_key() are none of them noexcept (their internal
    // co_spawn completion handlers can genuinely rethrow on a broken connection/TLS failure),
    // and letting that escape main() would std::terminate() -- Aborted (core dumped) on Linux,
    // an unhandled-exception dialog on Windows -- instead of a clean diagnostic exit(1).
    try {
        const auto clock_err = client.sync_clock();
        if (clock_err != PrivateRestError::None) {
            std::fprintf(stderr, "ERROR: sync_clock() failed (PrivateRestError=%d).\n",
                         static_cast<int>(clock_err));
            return 1;
        }

        // Explicit, non-empty symbol filter -- fetch_exchange_info()'s own header comment
        // documents an empty filter as "local testing only", and kMaxSymbols=64 (this repo's
        // capacity guard) would reject Binance's real 200+/2000+-symbol unfiltered response.
        std::array<std::string_view, 1> symbols{"BTCUSDT"};
        const auto exchange_err = client.fetch_exchange_info(parsed, symbols);
        if (exchange_err != PrivateRestError::None) {
            std::fprintf(stderr, "ERROR: fetch_exchange_info() failed (PrivateRestError=%d).\n",
                         static_cast<int>(exchange_err));
            return 1;
        }

        const auto listen_key_err = client.create_listen_key(listen_key_buf, listen_key_len);
        if (listen_key_err != PrivateRestError::None) {
            std::fprintf(stderr, "ERROR: create_listen_key() failed (PrivateRestError=%d).\n",
                         static_cast<int>(listen_key_err));
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: cold-start network setup failed: %s\n", e.what());
        return 1;
    }

    if (!registry.refresh_from_exchange_info(cp_sink, parsed)) {
        std::fprintf(stderr, "ERROR: SymbolRegistry::refresh_from_exchange_info() failed "
                              "(durable append not acked).\n");
        return 1;
    }

    // submit_port is assembled but deliberately never .call()'d in this batch (see this
    // file's header comment) -- this read-only diagnostic both proves H5's composite adapters
    // genuinely reach a real SymbolRegistry, and (with /W4 /WX + -Wall -Wextra -Werror active
    // repo-wide) keeps the compiler from flagging submit_port as an unreferenced local.
    std::printf("SubmitPort assembled: rules_version=%u, valid=%d\n",
                submit_port.current_rules_version(), submit_port.is_valid() ? 1 : 0);

    ListenKeyPublisher listen_key_pub;
    // Never log the listenKey itself -- only its length. Same posture as never logging the
    // API key/secret.
    std::printf("listenKey created: key_len=%zu\n", listen_key_len);
    listen_key_pub.publish(std::string_view(listen_key_buf.data(), listen_key_len),
                            get_now_ms(), get_now_ms() + 3600000);

    ListenKeyKeepaliveScheduler keepalive_scheduler(client, listen_key_pub);

    net::io_context ioc;
    ssl::context ssl_ctx(ssl::context::tlsv12_client);
    // Only the SSL CONTEXT is configured here. configure_binance_hostname_verification() is a
    // per-stream template (SslStream&, not ssl::context&) -- BinanceUserDataWsSession already
    // calls it internally on its own stream during the WS handshake; calling it again here on
    // ssl_ctx would fail to compile (wrong parameter type) and would be redundant even if it
    // did.
    hy::configure_binance_ssl_context(ssl_ctx);

    UserDataJsonParser ws_parser;
    UserDataWsEventRing user_data_events;
    UserDataWsSessionConfig ws_cfg;
    // H4's ws_host()/ws_port() used for the first time to override UserDataWsSessionConfig's
    // production-default host/port -- without this, a testnet listenKey would be handed to a
    // WS session connecting to the PRODUCTION stream endpoint.
    ws_cfg.host = std::string(binding.ws_host());
    ws_cfg.port = std::string(binding.ws_port());
    UserDataWsSessionSupervisor ws_supervisor(ioc, ssl_ctx, user_data_events, ws_parser,
                                               listen_key_pub, ws_cfg);

    InFlightRegistry in_flight;
    AuditRingSink audit;
    audit.set_available(true);
    OrderTracker tracker;
    ToReconcileRing to_reconcile;
    ReconcileEventRing reconcile_events;
    PositionTruth position_truth;
    OrderFillContext fill_context;
    ReconcilePollPolicy poll_policy{};
    // Real QueryPort this time (unlike live_submit_reconcile_harness_demo.cpp's mock) -- this
    // is the first process to wire query_order_adapter() against a real, credentialed client.
    // in_flight stays empty for the life of this run (nothing here ever submits an order), so
    // poll_once() has nothing to query most ticks; the path is wired for real regardless, as
    // groundwork for a later batch that actually calls orchestrate_submit().
    QueryPort query_port{&query_order_adapter, &client};

    std::thread io_thread([&ioc]() { ioc.run(); });

    std::printf("=== Live Submit Real-Credentials Demo (testnet) ===\n");
    std::printf("Real credentials, real clock sync, real exchangeInfo, real listenKey, real "
                "WS session.\n");
    std::printf("SubmitPort is assembled but never invoked -- no order is ever submitted by "
                "this demo.\n");
    std::printf("Running for %d seconds (Ctrl+C to stop early).\n\n", duration_s);

    std::int64_t next_clock_sync_ms = 60'000;  // first periodic resync 60s after the
                                                // just-completed initial sync_clock() above
    const std::int64_t loop_start_ms = get_now_ms();
    std::uint64_t poll_ticks = 0;
    while (!g_stop.load()) {
        const std::int64_t now_ms = get_now_ms();

        drain_user_data_events(in_flight, &audit, user_data_events, now_ms, &position_truth,
                                &fill_context);

        // Independent short-period clock resync, deliberately decoupled from the 30-minute
        // listenKey keepalive cadence -- sharing one trigger period would let the 5-minute
        // clock-offset freshness TTL (kOffsetTtlMs, binance_clock_sync.hpp) expire long before
        // the next keepalive tick, fail-closing every subsequent signed call.
        if (now_ms >= next_clock_sync_ms) {
            try {
                if (client.sync_clock() == PrivateRestError::None) {
                    next_clock_sync_ms = now_ms + 60'000;
                } else {
                    std::fprintf(stderr, "WARN: periodic clock resync failed, retrying sooner\n");
                    next_clock_sync_ms = now_ms + 10'000;
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "WARN: periodic clock resync threw (%s), retrying sooner\n",
                             e.what());
                next_clock_sync_ms = now_ms + 10'000;
            }
        }

        keepalive_scheduler.poll(now_ms);

        drain_user_data_events(in_flight, &audit, user_data_events, now_ms, &position_truth,
                                &fill_context);
        poll_once(tracker, to_reconcile, reconcile_events, query_port, poll_policy, now_ms);
        drain_reconcile_events(in_flight, &audit, reconcile_events, now_ms, &position_truth,
                                &fill_context);
        ws_supervisor.poll(now_ms);
        ++poll_ticks;

        if (duration_s > 0 &&
            (now_ms - loop_start_ms) >= static_cast<std::int64_t>(duration_s) * 1000) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    bool have_session_stats = false;
    UserDataWsSessionStats session_stats{};
    if (const auto* session = ws_supervisor.current_session(); session != nullptr) {
        session_stats = session->stats_snapshot();
        have_session_stats = true;
    }

    ws_supervisor.shutdown();
    if (io_thread.joinable()) io_thread.join();

    const std::int64_t shutdown_now_ms = get_now_ms();
    drain_reconcile_events(in_flight, &audit, reconcile_events, shutdown_now_ms, &position_truth,
                            &fill_context);
    drain_user_data_events(in_flight, &audit, user_data_events, shutdown_now_ms, &position_truth,
                            &fill_context);

    // Best-effort clean release of the real listenKey -- close_listen_key() is also not
    // noexcept, so this is wrapped the same way the cold-start block above is: a network
    // failure here must never crash the process before its final stats get printed.
    try {
        client.close_listen_key(listen_key_pub.load().view());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "WARN: close_listen_key() threw (%s), ignoring on shutdown\n",
                     e.what());
    }

    auto sup_stats = ws_supervisor.stats();
    auto keepalive_stats = keepalive_scheduler.stats();
    std::printf("\n=== Stats ===\n");
    std::printf("Reconcile-loop ticks: %llu\n", static_cast<unsigned long long>(poll_ticks));
    std::printf("Audit records written: %zu\n", audit.count());
    std::printf("Keepalive: attempts=%llu successes=%llu failures=%llu "
                "recreation_attempts=%llu\n",
                static_cast<unsigned long long>(keepalive_stats.keepalive_attempts),
                static_cast<unsigned long long>(keepalive_stats.keepalive_successes),
                static_cast<unsigned long long>(keepalive_stats.keepalive_failures),
                static_cast<unsigned long long>(keepalive_stats.recreation_attempts));
    std::printf("WS supervisor: attempts=%llu consecutive_failures=%u last_listen_key_seq=%u\n",
                static_cast<unsigned long long>(sup_stats.total_attempts),
                sup_stats.consecutive_failures, sup_stats.last_listen_key_seq);
    if (have_session_stats) {
        std::printf("Last attempt: messages=%llu errors=%llu last_error_stage=%s\n",
                    static_cast<unsigned long long>(session_stats.messages_received),
                    static_cast<unsigned long long>(session_stats.errors),
                    session_stats.last_error_stage.empty() ? "(none)"
                                                             : session_stats.last_error_stage.c_str());
    } else {
        std::printf("No live WS attempt at shutdown (mid-backoff-wait).\n");
    }

    return 0;
}
