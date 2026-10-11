// SPDX-License-Identifier: proprietary
// live_submit_preflight_harness.cpp — 批次 6 6b-1: cold-start harness proving
// orchestrate_submit() can genuinely reach "just missing operator confirmation" against REAL
// testnet market data, with ZERO real network write side effect.
//
// Extends H6's live_submit_real_credentials_demo.cpp cold-start chain (credentials/
// SymbolRegistry/BinancePrivateRestClient/make_binance_submit_port/clock sync/exchangeInfo/
// listenKey/private user-data WS -- see that file's own header comment, all still accurate here)
// with three things H6 never had:
//   1. The public kline stream feeding a loaded StreamingEvaluator (批次 5) + HoldingStateTracker
//      (批次 6a-2) -- real bar-by-bar signal evaluation.
//   2. A real public depth stream ("<symbol>@depth@100ms") through DepthManager, so
//      ctx.depth_synced can genuinely become true (H6 never set this field at all -- confirmed via
//      grep before this batch's plan was written).
//   3. A real KillSwitch (Normal) + dry-run evidence that is EARNED at process start: the four
//      EvidencePath scenarios really run through orchestrate_submit() (VerifiedDryRunEvidence,
//      批次 6 6b-0b) and only a scenario that reached its expected terminal state is recorded.
//      (An earlier version of this file "replayed" the four paths by looping record() over the
//      enum -- nothing had run. DryRunEvidenceChain::live_ready() is purely in-memory --
//      dry_run_evidence.hpp's own reset()/no serialization -- so this must happen fresh every
//      process start, not "once, ever".)
//
// Items 1 and 2 are one PublicFeedPipeline (批次 6b-0f, public_feed_pipeline.hpp): each feed is
// supervised (reconnect with backoff, a planned rollover before Binance's 24 h connection limit), a
// kline gap is repaired from a REST backfill that re-warms the evaluator, the depth book is rebuilt
// from a fresh snapshot after every reconnect, and one verdict -- "is the feed trustworthy right now"
// -- is read off the real objects. This file used to wire all of that by hand in main(), which no
// test could reach; the wiring is now the pipeline's, exercised against real sessions
// (test_public_feed_pipeline.cpp), and main() is a caller. (Not hot_thread.hpp's HotThread: that pulls
// in intent_channel.hpp/sim_executor.hpp, whose execution_types.hpp defines its OWN
// hy::OrderSide/OrderType, colliding with account_truth.hpp's -- MSVC C2011 -- in the one translation
// unit that needs both, which is this one, via live_submit_orchestrator.hpp.)
//
// SAFETY PROPERTY THIS FILE EXISTS TO DEMONSTRATE: ctx.submit_port stays pointed at a MOCK
// SubmitFn for the entire life of this process -- never swapped to the real
// make_binance_submit_port() result (that real port IS constructed below, proving H5's
// composite adapters still assemble against a real, freshly-refreshed SymbolRegistry, exactly
// like H6's own diagnostic -- but it is never assigned to ctx.submit_port). ctx.confirmation is
// never bound anywhere in this file either. Combined, orchestrate_submit() can therefore reach
// at most OrchestratorGate::OperatorNotConfirmed and NEVER Gate 12 (SubmitPortInvalid) with a
// real port, NEVER an actual network POST. This is why 6b-1 needs no ADR-019 D2 "L5 审" before
// merge -- it produces zero real network writes, unlike 6b-2 (which swaps in the real port AND
// a real human confirmation gate, and DOES require that review before real execution).
//
// Testnet only -- same posture as H6: no --production flag, .env read from a fixed
// process-cwd-relative path per ADR-019 D4.
//
// Usage: run from the repository root (so ./.env resolves), after `chmod 600 .env`:
//   ./live_submit_preflight_harness <spec_toml_path> [duration_seconds=60] [interval=1h]
// Symbol is fixed to BTCUSDT/btcusdt throughout, matching H6's own single-symbol scope.
//
// Data-level staleness is the pipeline's too: Binance's pings keep the sessions' 90 s idle timeout
// satisfied, so a connection can stay up while the DATA stops -- a depth feed with no accepted event
// for stale_after_ms resets the book and replaces its connection, and a kline whose next bar is
// overdue on the exchange clock is repaired from REST (and its connection replaced if it was up when
// the bar was due).
//
// NOT covered here, deliberately (each is its own batch): the durable startup recovery and the
// target-position planner (this file still probes on HoldingStateTracker transitions); operator
// confirmation. An AI session compiles this file but never runs it with real credentials.

#include <hengyuan/binance_clock_sync.hpp>
#include <hengyuan/binance_klines_rest.hpp>
#include <hengyuan/binance_listen_key_keepalive.hpp>
#include <hengyuan/binance_rest_snapshot.hpp>
#include <hengyuan/binance_submit_adapter.hpp>
#include <hengyuan/binance_user_data_ws_supervisor.hpp>
#include <hengyuan/depth_manager.hpp>
#include <hengyuan/env_loader.hpp>
#include <hengyuan/feed_validity_gate.hpp>
#include <hengyuan/holding_state_tracker.hpp>
#include <hengyuan/live_submit_orchestrator.hpp>
#include <hengyuan/public_feed_pipeline.hpp>
#include <hengyuan/strategy_spec_evaluator.hpp>
#include <hengyuan/strategy_spec_toml_parser.hpp>
#include <hengyuan/verified_dry_run_evidence.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
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

std::atomic<bool> g_stop{false};
void signal_handler(int) { g_stop.store(true); }

// Same FNV-1a-over-__DATE__-__TIME__ build stamp as live_submit_evidence_harness.cpp -- "derived
// from something real" rather than a hardcoded magic constant, without a build-system change to
// expose an actual git hash.
std::uint32_t build_stamp() noexcept {
    std::uint32_t h = 2166136261u;
    for (const char* p = __DATE__ __TIME__; *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 16777619u;
    }
    return h;
}

const char* gate_name(OrchestratorGate g) noexcept {
    switch (g) {
        case OrchestratorGate::Passed: return "Passed";
        case OrchestratorGate::AuditUnavailable: return "AuditUnavailable";
        case OrchestratorGate::KillSwitchNotNormal: return "KillSwitchNotNormal";
        case OrchestratorGate::DryRunEvidenceIncomplete: return "DryRunEvidenceIncomplete";
        case OrchestratorGate::SignerNotReady: return "SignerNotReady";
        case OrchestratorGate::DepthNotSynced: return "DepthNotSynced";
        case OrchestratorGate::PreTradeFailed: return "PreTradeFailed";
        case OrchestratorGate::AccountTruthStale: return "AccountTruthStale";
        case OrchestratorGate::RateLimitExhausted: return "RateLimitExhausted";
        case OrchestratorGate::OperatorNotConfirmed: return "OperatorNotConfirmed";
        case OrchestratorGate::SubmitPortInvalid: return "SubmitPortInvalid";
        case OrchestratorGate::SubmitRejected: return "SubmitRejected";
        case OrchestratorGate::SubmitAmbiguous: return "SubmitAmbiguous";
        case OrchestratorGate::SubmitNetworkError: return "SubmitNetworkError";
        case OrchestratorGate::SubmitAccepted: return "SubmitAccepted";
        case OrchestratorGate::ConfirmationMismatch: return "ConfirmationMismatch";
        case OrchestratorGate::DuplicateInFlight: return "DuplicateInFlight";
        case OrchestratorGate::InFlightRegistryUnavailable: return "InFlightRegistryUnavailable";
        case OrchestratorGate::SubmitStaleRulesVersion: return "SubmitStaleRulesVersion";
        case OrchestratorGate::AuditWriteNotAcked: return "AuditWriteNotAcked";
        case OrchestratorGate::SubmitPartialFill: return "SubmitPartialFill";
        case OrchestratorGate::SubmitFilled: return "SubmitFilled";
    }
    return "?";
}

// --- Mock submit port: reachable-and-consistent, but call() is a defensive trap -----------
// See this file's own header comment for why .call() must be unreachable: ctx.confirmation is
// never bound anywhere in main(). current_rules_version_fn DOES need to return the real,
// live-fetched rules_version (set once after registry.refresh_from_exchange_info()) -- otherwise
// Gate "1" (SubmitStaleRulesVersion, live_submit_orchestrator.hpp) would fail BEFORE reaching
// confirmation, for a reason that has nothing to do with what 6b-1 is trying to prove.
std::atomic<std::uint32_t> g_mock_rules_version{0};

std::uint32_t mock_current_rules_version(void*) noexcept {
    return g_mock_rules_version.load(std::memory_order_relaxed);
}

SubmitResponse mock_submit_must_never_be_called(const char*, std::uint32_t, OrderSide, OrderType,
                                                 std::int64_t, std::int64_t, const SymbolRules&,
                                                 void*) noexcept {
    std::fprintf(stderr,
                 "FATAL-LOGIC: mock submit_port.call() was invoked in the 6b-1 preflight "
                 "harness -- this should be unreachable, since ctx.confirmation.confirmed is "
                 "never set anywhere in this file. Returning NetworkError defensively; this is "
                 "a bug in this harness if it is ever observed, not a real submit attempt.\n");
    return SubmitResponse{SubmitOutcome::NetworkError, 0, -1};
}

// --- DemoControlPlaneSink -- identical shape to live_submit_real_credentials_demo.cpp's own
// (see that file's header comment for why append_snapshot() is the only real override this
// class needs); duplicated rather than shared because both are anonymous-namespace, src/-local
// types and this file must not reach into another translation unit's internals.
class DemoControlPlaneSink final : public DurableControlPlaneSink {
public:
    AuditAppendResult append_snapshot(const SymbolRegistrySnapshotPayload&,
                                       std::span<const SymbolRules>) noexcept override {
        return AuditAppendResult{AuditAppendResult::Status::Acked, ++sequence_};
    }
    AuditAppendResult append_rate_freeze(const RateLimitFreezePayload&, FrameTimeKind) noexcept override { return {}; }
    AuditAppendResult append_compacted_freeze_snapshot(const RateLimitFreezePayload&, FrameTimeKind,
                                                        const CompactionFreezeSnapshotProof&) noexcept override { return {}; }
    AuditAppendResult append_compacted_wait_evidence(const CompactedFreezeWaitEvidencePayload&, FrameTimeKind,
                                                      const CompactionWaitEvidenceProof&) noexcept override { return {}; }
    AuditAppendResult append_weight_config(const EndpointWeightConfig&) noexcept override { return {}; }
    AuditAppendResult append_usage_snapshot(const RateLimitUsageSnapshotPayload&) noexcept override { return {}; }
    AuditAppendResult append_generation_bridge(const GenerationBridgePayload&) noexcept override { return {}; }
    AuditAppendResult append_operator_override(const OperatorOverridePayload&) noexcept override { return {}; }
    AuditAppendResult append_freeze_probe_attempt(const FreezeProbeAttemptPayload&, FrameTimeKind) noexcept override { return {}; }
    AuditAppendResult append_freeze_clear(const FreezeClearPayload&, FrameTimeKind) noexcept override { return {}; }
    AuditAppendResult append_freeze_wait_arm(const FreezeWaitArmPayload&, FrameTimeKind) noexcept override { return {}; }
    AuditAppendResult append_freeze_wait_satisfied(const FreezeWaitSatisfiedPayload&, FrameTimeKind) noexcept override { return {}; }
    AuditAppendResult append_freeze_epoch_watermark(std::uint32_t, FrameTimeKind) noexcept override { return {}; }
    AuditAppendResult append_seal_journal_apply(const SealJournalAppliedView&) noexcept override { return {}; }
    RecoveryScanStatus recover_control_plane(RateLimitFreezePayload&, bool&, bool&, std::uint8_t&,
                                              EndpointWeightConfig&, bool&, RateLimitUsageSnapshotPayload&,
                                              bool&, GenerationBridgePayload&, bool&, std::uint32_t&, bool&,
                                              std::array<FreezeProbeAttemptPayload, 8>&, std::size_t&,
                                              FreezeClearPayload&, bool&, FreezeWaitSatisfiedPayload&, bool&,
                                              FreezeWaitArmPayload&, bool&) noexcept override {
        return RecoveryScanStatus::IoError;  // fresh cold-start process, never a crash-recovery scan
    }
private:
    std::uint64_t sequence_{0};
};

void print_env_load_error(EnvLoadStatus status) {
    switch (status) {
        case EnvLoadStatus::FileNotFound:
            std::fprintf(stderr,
                         "ERROR: failed to load .env (FileNotFound). Run this harness from the "
                         "repository root so ./.env resolves.\n");
            break;
        case EnvLoadStatus::PermissionTooWide:
            std::fprintf(stderr, "ERROR: failed to load .env (PermissionTooWide). chmod 600 .env\n");
            break;
        default:
            std::fprintf(stderr, "ERROR: failed to load .env (status=%d).\n", static_cast<int>(status));
            break;
    }
}

std::string durable_audit_temp_path() {
#ifdef _WIN32
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + "hy_preflight_harness_" + std::to_string(GetCurrentProcessId()) + ".log";
#else
    return "/tmp/hy_preflight_harness_" + std::to_string(getpid()) + ".log";
#endif
}

// Every file a DurableAuditSink derives from its path: the log plus the key-rotation and store-identity sidecars,
// each with its lock file, tip anchor and the tip's temp file. (Only the first four used to be removed here, which
// left the sidecars behind on every run.)
void remove_durable_audit_files(const std::string& base_path) {
    for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".keyrotations", ".keyrotations.lock",
                               ".keyrotations.tip", ".keyrotations.tip.tmp", ".storeid", ".storeid.lock",
                               ".storeid.tip", ".storeid.tip.tmp"}) {
        std::remove((base_path + suffix).c_str());
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <spec_toml_path> [duration_seconds=60] [interval=1h]\n",
                     argv[0]);
        return 1;
    }
    const std::string spec_path = argv[1];
    int duration_s = argc >= 3 ? std::atoi(argv[2]) : 60;
    if (duration_s <= 0) duration_s = 60;
    // No maximum run time: the public feeds are supervised and roll their connections over before
    // Binance's 24 h connection limit on their own.
    const std::string interval = argc >= 4 ? argv[3] : "1h";
    int exit_code = 0;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const auto start_time = std::chrono::steady_clock::now();
    auto get_now_ms = [&start_time]() -> std::int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start_time)
            .count();
    };

    // --- Load spec + fail-closed market-binding check (批次 6a-2's own established pattern) --
    std::string toml_text;
    {
        std::ifstream f(spec_path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "FATAL: could not open spec file: %s\n", spec_path.c_str());
            return 1;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        toml_text = ss.str();
    }
    const SpecLoadResult load_result = load_strategy_spec(toml_text);
    if (!load_result.ok()) {
        std::fprintf(stderr, "FATAL: spec load failed (status=%d): %s\n",
                     static_cast<int>(load_result.status), spec_path.c_str());
        return 1;
    }
    if (!market_binding_matches(load_result.market, ManualMarket::CryptoSpot, "BTCUSDT", interval)) {
        std::fprintf(stderr,
                     "FATAL: spec [market] binding (symbol=%s, timeframe=%s) does not match "
                     "this harness's fixed BTCUSDT/%s subscription -- refusing to start.\n",
                     load_result.market.symbol, load_result.market.timeframe, interval.c_str());
        return 1;
    }
    StreamingEvaluator evaluator;
    if (!evaluator.init(load_result.dag)) {
        std::fprintf(stderr, "FATAL: StreamingEvaluator::init() failed (capacity exceeded).\n");
        return 1;
    }
    HoldingStateTracker tracker;

    // --- H6's cold-start chain, unchanged -----------------------------------------------
    SecureEnvLoader loader;
    EnvironmentBinding binding = EnvironmentBinding::testnet();

    // --- The four dry-run drills, for real (批次 6 6b-0b) ------------------------------------------
    // Before the process reads a credential or touches the network (and before any io thread or feed
    // exists): a failure here is a plain `return` with nothing to undo. The verified chain is bound to
    // this build and this environment, and is the ONLY evidence handle below (ctx.evidence takes its
    // read-only chain()).
    const std::uint32_t stamp = build_stamp();
    VerifiedDryRunEvidence verified_evidence;
    {
        const DryRunReport drills = verified_evidence.run_all(stamp, drill_startup_now_ms(get_now_ms()), binding.environment(),
                                                              durable_audit_temp_path() + "_drill");
        static constexpr const char* kDrillNames[kEvidencePathCount] = {"SubmitSuccess", "SubmitReject",
                                                                        "SubmitAmbiguous", "KillSwitch"};
        for (std::size_t i = 0; i < kEvidencePathCount; ++i) {
            const DrillOutcome& d = drills.drills[i];
            std::printf("Dry-run drill %-15s %s  gate=%s order=%s port_calls=%d%s%s\n", kDrillNames[i],
                        d.passed ? "PASSED" : "FAILED", gate_name(d.gate), order_state_name(d.order_state),
                        d.port_calls, d.passed ? "" : "  -- ", d.passed ? "" : d.failure);
        }
        if (!drills.all_passed() || !verified_evidence.ready_for(binding.environment(), stamp)) {
            std::fprintf(stderr,
                         "FATAL: the dry-run drills did not all reach their expected terminal state -- "
                         "refusing to start.\n");
            return 1;
        }
    }
    std::printf("Dry-run evidence earned: all_paths_exercised=%s consistent_build=%s live_ready=%s\n",
                verified_evidence.chain().all_paths_exercised() ? "true" : "false",
                verified_evidence.chain().consistent_build() ? "true" : "false",
                verified_evidence.chain().live_ready() ? "true" : "false");

    std::array<std::string_view, 2> allowed_keys{binding.api_key_env_key(), binding.secret_env_key()};
    EnvAllowlist allowlist{allowed_keys.data(), allowed_keys.size()};

    const auto load_env_result = loader.load(".env", allowlist);
    if (load_env_result.status != EnvLoadStatus::Ok) {
        print_env_load_error(load_env_result.status);
        return 1;
    }

    auto [err, creds] = BoundHmacCredentials::load_and_bind_credentials(binding, loader);
    if (err != QuerySigningError::Ok) {
        std::fprintf(stderr, "ERROR: failed to bind credentials (QuerySigningError=%d).\n",
                     static_cast<int>(err));
        return 1;
    }

    SymbolRegistry registry;
    BinancePrivateRestClient client(binding, std::move(creds));
    SubmitAdapterContext submit_ctx{&client, &registry};
    // Constructed to prove H5's composite adapters still assemble against a real, freshly
    // refreshed SymbolRegistry -- exactly like H6's own diagnostic. NEVER assigned to
    // ctx.submit_port below (see this file's header comment).
    SubmitPort real_submit_port_unused = make_binance_submit_port(submit_ctx);

    DemoControlPlaneSink cp_sink;
    ParsedExchangeInfo parsed{};
    std::array<char, kListenKeyBufferLen> listen_key_buf{};
    std::size_t listen_key_len = 0;

    try {
        if (client.sync_clock() != PrivateRestError::None) {
            std::fprintf(stderr, "ERROR: sync_clock() failed.\n");
            return 1;
        }
        std::array<std::string_view, 1> symbols{"BTCUSDT"};
        if (client.fetch_exchange_info(parsed, symbols) != PrivateRestError::None) {
            std::fprintf(stderr, "ERROR: fetch_exchange_info() failed.\n");
            return 1;
        }
        if (client.create_listen_key(listen_key_buf, listen_key_len) != PrivateRestError::None) {
            std::fprintf(stderr, "ERROR: create_listen_key() failed.\n");
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: cold-start network setup failed: %s\n", e.what());
        return 1;
    }

    if (!registry.refresh_from_exchange_info(cp_sink, parsed)) {
        std::fprintf(stderr, "ERROR: SymbolRegistry::refresh_from_exchange_info() failed.\n");
        return 1;
    }

    // Read-only diagnostic, same placement/purpose as H6's own (live_submit_real_credentials_
    // demo.cpp): proves H5's composite adapters genuinely reach a real, freshly refreshed
    // SymbolRegistry, and (with -Werror=unused-but-set-variable active on the GCC tier) keeps
    // the compiler from flagging real_submit_port_unused as constructed-but-never-read. Still
    // never assigned to ctx.submit_port anywhere below.
    std::printf("Real SubmitPort assembled (never used for ctx.submit_port): rules_version=%u, valid=%d\n",
                real_submit_port_unused.current_rules_version(),
                real_submit_port_unused.is_valid() ? 1 : 0);

    // The one real, freshly-fetched SymbolRules this whole harness validates against --
    // captured once, right after refresh_from_exchange_info(), same "one snapshot, never
    // re-read mid-run" discipline orchestrate_submit() itself applies to
    // ctx.pre_trade_rules_snapshot.
    const SymbolRules rules = registry.current_rules(0);
    g_mock_rules_version.store(rules.rules_version, std::memory_order_relaxed);

    // price_mult/qty_mult MUST be derived from the real rules.price_scale/qty_scale, not a
    // hardcoded 1e8 -- otherwise the depth book's ticks (BinanceJsonParser::register_symbol())
    // and the REST snapshot's ticks (SnapshotRequest) would silently disagree with what
    // validate_pre_trade() checks rules.min_price_ticks/tick_size_ticks against.
    std::int64_t price_mult = 1, qty_mult = 1;
    if (!pow10_i64(rules.price_scale, price_mult) || !pow10_i64(rules.qty_scale, qty_mult)) {
        std::fprintf(stderr, "FATAL: registry's price_scale/qty_scale out of pow10_i64's domain.\n");
        return 1;
    }

    ListenKeyPublisher listen_key_pub;
    std::printf("listenKey created: key_len=%zu\n", listen_key_len);
    listen_key_pub.publish(std::string_view(listen_key_buf.data(), listen_key_len), get_now_ms(),
                            get_now_ms() + 3600000);
    ListenKeyKeepaliveScheduler keepalive_scheduler(client, listen_key_pub);

    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);
    hy::configure_binance_ssl_context(ssl_ctx);

    UserDataJsonParser ws_parser;
    UserDataWsEventRing user_data_events;
    UserDataWsSessionConfig ws_cfg;
    ws_cfg.host = std::string(binding.ws_host());
    ws_cfg.port = std::string(binding.ws_port());
    UserDataWsSessionSupervisor ws_supervisor(ioc, ssl_ctx, user_data_events, ws_parser,
                                               listen_key_pub, ws_cfg);

    InFlightRegistry in_flight;
    AuditRingSink audit;
    audit.set_available(true);
    OrderTracker order_tracker;
    ToReconcileRing to_reconcile;
    ReconcileEventRing reconcile_events;
    PositionTruth position_truth;
    OrderFillContext fill_context;
    ReconcilePollPolicy poll_policy{};
    QueryPort query_port{&query_order_adapter, &client};

    // --- Both public feeds, supervised (批次 6b-0f) --------------------------------------------
    // The io_context needs a work guard: the sessions are created LATER, by the supervisors'
    // poll(), and run() returns at once when nothing is pending -- after which no session would
    // ever run. Released at shutdown, once every feed has been told to stop.
    auto ioc_guard = boost::asio::make_work_guard(ioc);

    // The exchange-corrected wall clock in epoch ms, for the two decisions that must be judged
    // against EXCHANGE time rather than this machine's: which klines a backfill may count as
    // closed (that runs on the backfill gate's worker thread, so this reads only thread-safe
    // state: a mutex-guarded offset snapshot and two clock reads) and when a planned rollover may
    // happen. It is the pessimistic bound, and 0 when the offset is stale or the wall clock has
    // jumped: the backfill treats a non-positive reading as "no usable clock" and FAILS (retried
    // after its cooldown) instead of falling back to the uncalibrated local clock; a rollover
    // just waits.
    auto exchange_now_ms = [&client]() -> std::int64_t {
        std::int64_t now = 0;
        return try_get_pessimistic_server_now_ms(client.clock_publisher(), fetch_clock_pair(), now) ? now : 0;
    };

    // Testnet REST: both public fetchers take this process's binding (the testnet one) -- neither config has a
    // host field any more, and each fetch issues an EndpointPermit for the binding's host before anything is
    // resolved, so a fetch cannot reach another market. They run on the gates' worker threads, so they capture
    // values only (an EnvironmentBinding is trivially copyable).
    auto testnet_depth_fetcher = [binding](const SnapshotRequest& req) -> std::optional<DepthSnapshot> {
        RestSnapshotConfig cfg;
        cfg.port = "443";
        return fetch_depth_snapshot(binding, req.symbol, req.price_multiplier, req.qty_multiplier, cfg);
    };
    PublicRestConfig klines_rest;
    klines_rest.port = "443";
    auto testnet_klines_fetcher = [binding, klines_rest, exchange_now_ms](const KlinesBackfillRequest& req) {
        return fetch_klines_backfill_outcome(binding, req, klines_rest, exchange_now_ms);
    };

    PublicFeedPipelineConfig feed_cfg;
    feed_cfg.ws_host = std::string(binding.ws_host());
    feed_cfg.ws_port = std::string(binding.ws_port());
    feed_cfg.symbol = "BTCUSDT";
    feed_cfg.interval = interval;
    feed_cfg.symbol_id = 0;
    // Derived above from the real rules: the same scale validate_pre_trade() checks against.
    feed_cfg.price_multiplier = price_mult;
    feed_cfg.qty_multiplier = qty_mult;
    // Real entropy for the two supervisors' reconnect jitter (the pipeline's defaults are fixed seeds).
    std::random_device entropy;
    auto jitter_seed = [&entropy]() {
        return (static_cast<std::uint64_t>(entropy()) << 32) | static_cast<std::uint64_t>(entropy());
    };
    feed_cfg.kline_policy = make_public_feed_policy(jitter_seed());
    feed_cfg.depth_policy = make_public_feed_policy(jitter_seed());

    constexpr std::size_t kDepthRingSize = 65536;
    PipelineInitError feed_init_error = PipelineInitError::None;
    auto pipeline = PublicFeedPipeline<kDepthRingSize>::create(
        ioc, ssl_ctx, evaluator, load_result.dag, std::move(feed_cfg), testnet_klines_fetcher,
        testnet_depth_fetcher, exchange_now_ms, feed_init_error);
    if (!pipeline) {
        std::fprintf(stderr, "FATAL: the public feed pipeline refused to start (%s).\n",
                     pipeline_init_error_name(feed_init_error));
        return 1;
    }

    // From here to the shutdown block at the end of main() there is deliberately no `return`: the
    // pipeline's sessions run on this thread, and it must be joined before the pipeline is destroyed
    // (public_feed_pipeline.hpp's lifetime contract).
    std::thread io_thread([&ioc]() { ioc.run(); });

    // --- Real KillSwitch (Normal) --------------------------------------------------------------
    KillSwitch kill_switch;
    kill_switch.operator_reset();

    SpotRateLimitTracker rate_limiter;
    rate_limiter.configure(/*weight*/ 6000, 500, /*raw*/ 60000, 5000, /*orders*/ 100, 10);

    // Real DurableAuditSink -- this harness never gets far enough to write an order-lifecycle
    // record worth persisting (confirmation is never bound), so an ephemeral per-run temp file
    // (cleaned at shutdown) is appropriate here, unlike a genuine production audit trail.
    //
    // DEMO-ONLY, and NOT a recovery path (外部复核 P0-06): a fresh temp log with a fixture KEK can
    // never contain a previous run's orders, so this process cannot recover anything. Anything that
    // may submit for real must run startup_recovery.hpp's Bootstrapping -> Recovering ->
    // Reconciling -> Ready sequence over a PERSISTENT log first (6b-0d builds and tests that; the
    // 6b-2 harness wires it, once the owner has decided how the audit key is provisioned).
    const std::string durable_audit_path = durable_audit_temp_path();
    remove_durable_audit_files(durable_audit_path);
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x20 + i);
    // unique_ptr, not stack values -- same "explicit reset() before removing the temp file,
    // never a manual destructor call on a stack object" discipline as
    // live_submit_evidence_harness.cpp's own Fixture (a stack DurableAuditSink would double-
    // destruct if this file ever needed to close it before main() returns, which shutdown does).
    auto key_ring = std::make_unique<KeyRing>(kek);
    WrappedKeyRecord key_rec{};
    key_ring->add_key(1, std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}},
                       key_rec);
    auto durable_audit_sink = std::make_unique<DurableAuditSink>(durable_audit_path, *key_ring, 1);

    AccountSnapshot account{};

    std::printf("=== Live Submit Preflight Harness (testnet) ===\n");
    std::printf("Real credentials, real clock sync, real exchangeInfo, real listenKey, real "
                "depth+kline WS.\n");
    std::printf("ctx.submit_port is a MOCK, never the real make_binance_submit_port() result -- "
                "no real network order write is possible from this process.\n");
    std::printf("Running for %d seconds (Ctrl+C to stop early).\n\n", duration_s);

    std::int64_t next_clock_sync_ms = 60'000;
    std::int64_t next_account_refresh_ms = 0;  // fetch immediately on the first loop iteration
    const std::int64_t loop_start_ms = get_now_ms();
    std::uint64_t poll_ticks = 0;
    std::uint64_t preflight_probes = 0;
    // Starts at the verdict of a pipeline that has not connected yet, so the first change is logged.
    SupervisedFeedInvalidReason last_feed_validity = SupervisedFeedInvalidReason::KlineDisconnected;

    while (!g_stop.load()) {
        const std::int64_t now_ms = get_now_ms();

        drain_user_data_events(in_flight, &audit, user_data_events, now_ms, &position_truth, &fill_context);

        if (now_ms >= next_clock_sync_ms) {
            try {
                if (client.sync_clock() == PrivateRestError::None) {
                    next_clock_sync_ms = now_ms + 60'000;
                } else {
                    std::fprintf(stderr, "WARN: periodic clock resync failed, retrying sooner\n");
                    next_clock_sync_ms = now_ms + 10'000;
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "WARN: periodic clock resync threw (%s)\n", e.what());
                next_clock_sync_ms = now_ms + 10'000;
            }
        }

        // Real account refresh, well inside the 30s freshness window validate_pre_trade()
        // checks -- a failed fetch just leaves `account` at its last-known (possibly stale)
        // state, which correctly fails PreTradeFailed rather than fabricating freshness.
        if (now_ms >= next_account_refresh_ms) {
            try {
                AccountSnapshot fetched{};
                if (client.fetch_account(fetched) == PrivateRestError::None) {
                    fetched.timestamp_ms = now_ms;
                    account = fetched;
                } else {
                    std::fprintf(stderr, "WARN: fetch_account() failed, keeping stale snapshot\n");
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "WARN: fetch_account() threw (%s)\n", e.what());
            }
            next_account_refresh_ms = now_ms + 10'000;
        }

        keepalive_scheduler.poll(now_ms);
        drain_user_data_events(in_flight, &audit, user_data_events, now_ms, &position_truth, &fill_context);
        poll_once(order_tracker, to_reconcile, reconcile_events, query_port, poll_policy, now_ms,
                  reconcile_wall_now_ms());
        drain_reconcile_events(in_flight, &audit, reconcile_events, now_ms, &position_truth, &fill_context);
        ws_supervisor.poll(now_ms);

        // What a live closed bar does. Called by the pipeline, in order, for every bar that reached the
        // evaluator (a backfill after a gap warms the evaluator without calling it).
        auto on_bar = [&](const KlineWsEvent& kev, double target_position) {
            const auto action = tracker.on_target_position(target_position);
            std::printf("[bar close=%" PRId64 "] signal=%.4f state=%s\n", kev.close_time_ms,
                        target_position, tracker.state() == HoldingState::Long ? "LONG" : "FLAT");

            if (action == SuggestedAction::None) return;

            // A suggested action derived from a bar is only ever probed while the feed is currently
            // trustworthy. This runs inside tick(), before that tick's depth work, so the depth side of
            // the verdict is as of the previous tick (one loop period, 50 ms).
            const SupervisedFeedInvalidReason feed_reason =
                evaluate_supervised_feed_validity(pipeline->supervised_inputs());
            if (feed_reason != SupervisedFeedInvalidReason::None) {
                std::printf("  [suppressed] suggested action not probed: feed invalid (%s)\n",
                            supervised_feed_invalid_reason_name(feed_reason));
                return;
            }

            ++preflight_probes;
            const auto tob = pipeline->depth_manager().book().top_of_book();
            if (!tob) {
                std::printf("  [preflight probe #%" PRIu64 "] no book yet -- skipping\n", preflight_probes);
                return;
            }
            const OrderSide side = (action == SuggestedAction::Open) ? OrderSide::Buy : OrderSide::Sell;
            const std::int64_t price_ticks = (side == OrderSide::Buy) ? tob->second : tob->first;
            // Small, fixed preflight-only quantity, rounded up to this symbol's real
            // min_qty_ticks/step_size_ticks -- NOT the final 6b-2 sizing policy (that batch's own
            // plan text: "数量来自一个固定的小额配置上限"), just enough for pre-trade validation
            // to see a genuinely valid order shape.
            std::int64_t qty_ticks = rules.min_qty_ticks;
            if (rules.step_size_ticks > 0) {
                qty_ticks = ((qty_ticks + rules.step_size_ticks - 1) / rules.step_size_ticks) *
                            rules.step_size_ticks;
            }

            OrchestratorContext ctx{};
            ctx.audit = &audit;
            ctx.kill_switch = &kill_switch;
            ctx.evidence = &verified_evidence.chain();
            ctx.signer_ready = true;
            ctx.depth_synced = (pipeline->depth_manager().state() == DepthState::Tracking);
            ctx.rate_limiter = &rate_limiter;
            ctx.in_flight = &in_flight;
            ctx.durable_audit = make_durable_order_audit_port(*durable_audit_sink);
            ctx.symbol_rules = &rules;
            ctx.account = &account;
            ctx.side = side;
            ctx.order_type = OrderType::Limit;
            ctx.base_asset = "BTC";
            ctx.quote_asset = "USDT";
            ctx.now_ms = now_ms;
            // Generous caps -- this harness never reaches a real submit (confirmation is never
            // bound), so these do not gate any real risk; they only need to not spuriously
            // reject the notional this probe computed.
            ctx.exposure_limits.freshness_max_age_ms = 30000;
            ctx.exposure_limits.single_order_notional_cap = 1'000'000'000;
            ctx.exposure_limits.total_exposure_notional_cap = 1'000'000'000;
            ctx.price_ticks = price_ticks;
            ctx.qty_ticks = qty_ticks;
            ctx.symbol_id = 0;
            ctx.sequence = static_cast<std::uint32_t>(preflight_probes);
            ctx.mode = ExecutionMode::Live;
            ctx.order_weight = 1;
            // ctx.confirmation is left at its default {confirmed=false} -- deliberately never
            // bound anywhere in this file (see header comment).
            ctx.submit_port = {mock_submit_must_never_be_called, nullptr, mock_current_rules_version};

            auto result = orchestrate_submit(ctx);
            std::printf("  [preflight probe #%" PRIu64 "] %s %s price=%" PRId64 " qty=%" PRId64
                        " -> gate=%s\n",
                        preflight_probes, action == SuggestedAction::Open ? "OPEN" : "CLOSE",
                        side == OrderSide::Buy ? "BUY" : "SELL", price_ticks, qty_ticks,
                        gate_name(result.gate));
            if (result.gate != OrchestratorGate::OperatorNotConfirmed) {
                std::fprintf(stderr,
                             "  WARN: expected gate=OperatorNotConfirmed, got %s -- some "
                             "prerequisite this harness assembles is not yet ready (this can be "
                             "normal early in the run, e.g. depth/account not synced yet).\n",
                             gate_name(result.gate));
            }
        };

        // Both public feeds, one call: the kline supervisor is polled, live bars reach on_bar, the
        // depth feed is drained and rebuilt as needed. `feed.validity` is the one verdict.
        const PublicFeedTickReport feed = pipeline->tick(now_ms, on_bar);

        // What the feeds did this tick, for whoever reads the log.
        if (feed.kline.gap_detected) {
            std::printf("Kline gap detected -- waiting for a backfill\n");
        }
        if (feed.kline.bar_overdue) {
            std::printf("Kline bar overdue -- backfilling from REST (connection replaced: %s)\n",
                        feed.kline.overdue_session_restarted ? "yes" : "no");
        }
        if (feed.kline.backfill_collected) {
            std::printf("Kline backfill: fetch=%s applied=%s bars=%zu warmup_complete=%s\n",
                        feed.kline.backfill_fetch_ok ? "ok" : "FAILED",
                        feed.kline.backfill_applied ? "yes" : "no", feed.kline.bars_applied,
                        feed.kline.warmup_complete_after_apply ? "yes" : "no");
        }
        if (feed.depth.generation_reset) {
            std::printf("Depth: new connection -- book reset, rebuilding from a snapshot\n");
        }
        if (feed.depth.stale_restart) {
            std::printf("Depth feed stale -- no depth event accepted for a while; book reset, connection replaced\n");
        }
        if (feed.depth.snapshot_applied) {
            std::printf("Depth snapshot: sync=%s\n", feed.depth.snapshot_apply_ok ? "OK" : "RESYNC_NEEDED");
        }
        if (feed.validity != last_feed_validity) {
            std::printf("Feed validity: %s -> %s\n", supervised_feed_invalid_reason_name(last_feed_validity),
                        supervised_feed_invalid_reason_name(feed.validity));
            last_feed_validity = feed.validity;
        }

        // A supervisor that has given up is not coming back -- nothing reconnects it -- so continuing
        // would only keep polling a dead feed. End the run with a non-zero exit code.
        if (feed.terminal()) {
            std::fprintf(stderr, "ALERT: a public feed gave up (%s) -- stopping. Restart to recover.\n",
                         supervised_feed_invalid_reason_name(feed.validity));
            exit_code = 2;
            break;
        }

        ++poll_ticks;
        if (duration_s > 0 && (now_ms - loop_start_ms) >= static_cast<std::int64_t>(duration_s) * 1000) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Shutdown order (public_feed_pipeline.hpp's lifetime contract): tell every feed to stop, release
    // the work guard so run() returns once their pending handlers have finished, join the io thread --
    // and only then may the pipeline go out of scope.
    pipeline->shutdown();
    ws_supervisor.shutdown();
    ioc_guard.reset();
    if (io_thread.joinable()) io_thread.join();

    const std::int64_t shutdown_now_ms = get_now_ms();
    drain_reconcile_events(in_flight, &audit, reconcile_events, shutdown_now_ms, &position_truth, &fill_context);
    drain_user_data_events(in_flight, &audit, user_data_events, shutdown_now_ms, &position_truth, &fill_context);

    try {
        client.close_listen_key(listen_key_pub.load().view());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "WARN: close_listen_key() threw (%s), ignoring on shutdown\n", e.what());
    }

    std::printf("\n=== Stats ===\n");
    std::printf("Reconcile-loop ticks: %" PRIu64 "  preflight probes: %" PRIu64
                "  final holding state: %s\n",
                poll_ticks, preflight_probes,
                tracker.state() == HoldingState::Long ? "LONG" : "FLAT");
    std::printf("Audit records written: %zu\n", audit.count());
    const DepthManager& depth_mgr = pipeline->depth_manager();
    const auto ds = depth_mgr.stats();
    std::printf("Depth: snapshots=%" PRIu64 " applied=%" PRIu64 " resyncs=%" PRIu64 " final_state=%s\n",
                ds.snapshots, ds.events_applied, ds.resyncs,
                depth_mgr.state() == DepthState::Tracking ? "Tracking"
                : depth_mgr.state() == DepthState::Syncing ? "Syncing" : "Buffering");
    const FeedSupervisorStats kline_sup = pipeline->kline_supervisor_stats();
    const KlineFeedDriverStats& kline_drv = pipeline->kline_driver_stats();
    std::printf("Kline feed: sessions=%" PRIu64 " failures=%" PRIu64 " rollovers=%" PRIu64
                " | backfills applied=%" PRIu64 " fetch_failed=%" PRIu64 " rejected=%" PRIu64
                " | resumes=%" PRIu64 " | overdue bars=%" PRIu64 " (connections replaced=%" PRIu64 ")\n",
                kline_sup.generation, kline_sup.total_failures, kline_sup.rollovers,
                kline_drv.backfills_applied, kline_drv.backfill_fetch_failures,
                kline_drv.backfills_rejected, kline_drv.resumes_posted, kline_drv.overdue_invalidations,
                kline_drv.overdue_session_restarts);
    const FeedSupervisorStats depth_sup = pipeline->depth_supervisor_stats();
    const DepthFeedDriverStats& depth_drv = pipeline->depth_driver_stats();
    std::printf("Depth feed: sessions=%" PRIu64 " failures=%" PRIu64 " rollovers=%" PRIu64
                " | generation_resets=%" PRIu64 " resyncs=%" PRIu64 " snapshots applied=%" PRIu64
                " rejected=%" PRIu64 " | stale restarts=%" PRIu64 "\n",
                depth_sup.generation, depth_sup.total_failures, depth_sup.rollovers,
                depth_drv.generation_resets, depth_drv.resyncs_requested, depth_drv.snapshots_applied,
                depth_drv.snapshots_rejected, depth_drv.stale_restarts);
    // The kline session of the last generation, if there is one (its counters are that connection's
    // own; the io thread is joined, so reading them is safe).
    if (const auto kline_session = pipeline->kline_session()) {
        const auto kline_stats = kline_session->stats_snapshot();
        std::printf("Kline (last session): closed_bars=%" PRIu64 " unclosed_skipped=%" PRIu64
                    " gap_detected=%" PRIu64 " ring_overflow_suspended=%" PRIu64 " suspended=%s\n",
                    kline_stats.parse_ok, kline_stats.unclosed_skipped, kline_stats.gap_detected_count,
                    kline_stats.ring_overflow_suspended, kline_session->is_suspended() ? "true" : "false");
    }

    durable_audit_sink.reset();  // close before removing the temp file
    key_ring.reset();
    remove_durable_audit_files(durable_audit_path);

    return exit_code;
}
