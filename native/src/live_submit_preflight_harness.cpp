// SPDX-License-Identifier: proprietary
// live_submit_preflight_harness.cpp — 批次 6 6b-1: cold-start harness proving
// orchestrate_submit() can genuinely reach "just missing operator confirmation" against REAL
// testnet market data, with ZERO real network write side effect.
//
// Extends H6's live_submit_real_credentials_demo.cpp cold-start chain (credentials/
// SymbolRegistry/BinancePrivateRestClient/make_binance_submit_port/clock sync/exchangeInfo/
// listenKey/private user-data WS -- see that file's own header comment, all still accurate here)
// with three things H6 never had:
//   1. 批次 6a-1's BinanceKlineWsSession, feeding a loaded StreamingEvaluator (批次 5) +
//      HoldingStateTracker (批次 6a-2) -- real bar-by-bar signal evaluation.
//   2. A real public depth stream ("<symbol>@depth@100ms") through DepthManager/
//      SnapshotRefreshGate (a manual drain loop, not hot_thread.hpp's HotThread -- see the
//      depth ring setup below for why), so ctx.depth_synced can genuinely become true (H6 never
//      set this field at all -- confirmed via grep before this batch's plan was written).
//   3. A real KillSwitch (Normal) + a DryRunEvidenceChain replayed with all 4 EvidencePath
//      scenarios at process start (DryRunEvidenceChain::live_ready() is purely in-memory --
//      dry_run_evidence.hpp's own reset()/no serialization -- so this must happen fresh every
//      process start, not "once, ever").
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

#include <hengyuan/binance_json_parser.hpp>
#include <hengyuan/binance_kline_ws_session.hpp>
#include <hengyuan/binance_listen_key_keepalive.hpp>
#include <hengyuan/binance_rest_snapshot.hpp>
#include <hengyuan/binance_submit_adapter.hpp>
#include <hengyuan/binance_user_data_ws_supervisor.hpp>
#include <hengyuan/binance_ws_session.hpp>
#include <hengyuan/env_loader.hpp>
#include <hengyuan/holding_state_tracker.hpp>
#include <hengyuan/depth_manager.hpp>
#include <hengyuan/input_validator.hpp>
#include <hengyuan/live_submit_orchestrator.hpp>
#include <hengyuan/strategy_spec_evaluator.hpp>
#include <hengyuan/strategy_spec_toml_parser.hpp>

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

void remove_durable_audit_files(const std::string& base_path) {
    std::remove(base_path.c_str());
    std::remove((base_path + ".lock").c_str());
    std::remove((base_path + ".tip").c_str());
    std::remove((base_path + ".tip.tmp").c_str());
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
    const std::string interval = argc >= 4 ? argv[3] : "1h";

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

    // --- 6a-1's kline stream + StreamingEvaluator (already loaded above) -------------------
    KlineWsSessionConfig kline_cfg;
    kline_cfg.host = std::string(binding.ws_host());
    kline_cfg.port = std::string(binding.ws_port());
    kline_cfg.symbol = "btcusdt";
    kline_cfg.interval = interval;
    kline_cfg.symbol_id = 0;
    KlineWsEventRing kline_ring;
    KlineJsonParser kline_parser;
    auto kline_session =
        std::make_shared<BinanceKlineWsSession>(ioc, ssl_ctx, kline_ring, kline_parser, kline_cfg);
    kline_session->start();

    // --- New: public depth stream -> DepthManager -> ctx.depth_synced (确认发现 1 的直接修复) --
    constexpr std::size_t kDepthRingSize = 65536;
    BinanceJsonParser depth_parser;
    if (!depth_parser.register_symbol("BTCUSDT", 0, price_mult, qty_mult)) {
        std::fprintf(stderr, "FATAL: could not register BTCUSDT with the depth parser.\n");
        return 1;
    }
    auto depth_ring = std::make_unique<SpscRing<BinanceMarketEvent, kDepthRingSize>>();
    DepthManager depth_mgr;
    // Manual depth-ring drain below (not hot_thread.hpp's HotThread) -- HotThread pulls in
    // intent_channel.hpp/sim_executor.hpp, whose execution_types.hpp defines its OWN
    // hy::OrderSide/hy::OrderType that collide (a real, pre-existing conflict between two
    // independently-evolved headers, confirmed by MSVC's C2011 "unsigned enum 类型重定义" the
    // first time this file tried to include both) with account_truth.hpp's OrderSide/OrderType,
    // which live_submit_orchestrator.hpp (this file's whole reason for existing) already needs.
    // This harness has no use for HotThread's multi-symbol-book/heartbeat/intent-channel
    // machinery anyway -- only the depth-event -> DepthManager path, which is a handful of
    // lines using InputValidator directly (see the main loop below).
    InputValidator depth_validator;

    WsSessionConfig depth_ws_cfg;
    depth_ws_cfg.host = std::string(binding.ws_host());
    depth_ws_cfg.port = std::string(binding.ws_port());
    depth_ws_cfg.subscribe_streams = {"btcusdt@depth@100ms"};
    auto depth_session = std::make_shared<BinanceWsSession<kDepthRingSize>>(
        ioc, ssl_ctx, *depth_ring, depth_parser, depth_ws_cfg);
    depth_session->start();

    // Testnet REST host override -- make_default_snapshot_fetcher() (binance_rest_snapshot.hpp)
    // is hardcoded to api.binance.com (production); this batch is testnet-only throughout, so a
    // dedicated fetcher pinned to binding.base_host() is used instead.
    const std::string rest_host(binding.base_host());
    auto testnet_depth_fetcher = [rest_host](const SnapshotRequest& req) -> std::optional<DepthSnapshot> {
        RestSnapshotConfig cfg;
        cfg.host = rest_host;
        cfg.port = "443";
        return fetch_depth_snapshot(req.symbol, req.price_multiplier, req.qty_multiplier, cfg);
    };
    SnapshotRefreshGate depth_gate(testnet_depth_fetcher);

    std::thread io_thread([&ioc]() { ioc.run(); });

    // --- Real KillSwitch (Normal) + DryRunEvidenceChain replay (确认发现 4 的直接修复) --------
    KillSwitch kill_switch;
    kill_switch.operator_reset();

    DryRunEvidenceChain evidence;
    const std::uint32_t stamp = build_stamp();
    for (int i = 0; i < static_cast<int>(kEvidencePathCount); ++i) {
        evidence.record(static_cast<EvidencePath>(i), get_now_ms(), stamp, /*suite_id=*/1);
    }
    std::printf("Dry-run evidence replayed: all_paths_exercised=%s consistent_build=%s live_ready=%s\n",
                evidence.all_paths_exercised() ? "true" : "false",
                evidence.consistent_build() ? "true" : "false",
                evidence.live_ready() ? "true" : "false");

    SpotRateLimitTracker rate_limiter;
    rate_limiter.configure(/*weight*/ 6000, 500, /*raw*/ 60000, 5000, /*orders*/ 100, 10);

    // Real DurableAuditSink -- this harness never gets far enough to write an order-lifecycle
    // record worth persisting (confirmation is never bound), so an ephemeral per-run temp file
    // (cleaned at shutdown) is appropriate here, unlike a genuine production audit trail.
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
        poll_once(order_tracker, to_reconcile, reconcile_events, query_port, poll_policy, now_ms);
        drain_reconcile_events(in_flight, &audit, reconcile_events, now_ms, &position_truth, &fill_context);
        ws_supervisor.poll(now_ms);

        // Manual depth-ring drain -- see depth_validator's own declaration comment for why this
        // isn't HotThread::run_once(). Same drain-bound discipline as HotThread's own
        // kMaxEventsPerRun (hot_thread.hpp): bounded per call so a burst cannot starve the rest
        // of this loop (clock resync, keepalive, kline draining) for an unbounded stretch.
        {
            BinanceMarketEvent ev{};
            std::size_t drained = 0;
            while (drained < 4096 && depth_ring->try_pop(ev)) {
                ++drained;
                const auto vr = depth_validator.validate(ev);
                if (vr == ValidationResult::ResyncRequired) {
                    depth_mgr.start_buffering();
                    continue;
                }
                if (vr == ValidationResult::RejectNegativePrice ||
                    vr == ValidationResult::RejectZeroPrice ||
                    vr == ValidationResult::RejectNegativeQty ||
                    vr == ValidationResult::RejectSeqRollback ||
                    vr == ValidationResult::DropDuplicate) {
                    continue;
                }
                if (ev.type == EventType::DepthDelta) {
                    depth_mgr.on_depth_event(ev, ev.aux_id, ev.event_id);
                }
            }
        }
        if (auto snap = depth_gate.poll(depth_mgr.needs_snapshot(), {"BTCUSDT", price_mult, qty_mult})) {
            const bool ok = depth_mgr.apply_snapshot(*snap);
            std::printf("Depth snapshot: lastUpdateId=%" PRIu64 " sync=%s\n", snap->last_update_id,
                        ok ? "OK" : "RESYNC_NEEDED");
            depth_gate.notify_apply_result(ok);
        }
        const bool depth_synced = (depth_mgr.state() == DepthState::Tracking);

        KlineWsEvent kev{};
        while (kline_ring.try_pop(kev)) {
            const Bar bar{kev.open, kev.high, kev.low, kev.close, kev.volume};
            const double target_position = evaluator.step(bar);
            const auto action = tracker.on_target_position(target_position);
            std::printf("[bar close=%" PRId64 "] signal=%.4f state=%s\n", kev.close_time_ms,
                        target_position, tracker.state() == HoldingState::Long ? "LONG" : "FLAT");

            if (action == SuggestedAction::None) continue;

            ++preflight_probes;
            const auto tob = depth_mgr.book().top_of_book();
            if (!tob) {
                std::printf("  [preflight probe #%" PRIu64 "] no book yet -- skipping\n", preflight_probes);
                continue;
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
            ctx.evidence = &evidence;
            ctx.signer_ready = true;
            ctx.depth_synced = depth_synced;
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
        }

        ++poll_ticks;
        if (duration_s > 0 && (now_ms - loop_start_ms) >= static_cast<std::int64_t>(duration_s) * 1000) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    depth_session->stop();
    kline_session->stop();
    ws_supervisor.shutdown();
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
    const auto ds = depth_mgr.stats();
    std::printf("Depth: snapshots=%" PRIu64 " applied=%" PRIu64 " resyncs=%" PRIu64 " final_state=%s\n",
                ds.snapshots, ds.events_applied, ds.resyncs,
                depth_mgr.state() == DepthState::Tracking ? "Tracking"
                : depth_mgr.state() == DepthState::Syncing ? "Syncing" : "Buffering");
    const auto kline_stats = kline_session->stats_snapshot();
    std::printf("Kline: closed_bars=%" PRIu64 " unclosed_skipped=%" PRIu64 " gap_detected=%" PRIu64
                " suspended=%s\n",
                kline_stats.parse_ok, kline_stats.unclosed_skipped, kline_stats.gap_detected_count,
                kline_session->is_suspended() ? "true" : "false");

    durable_audit_sink.reset();  // close before removing the temp file
    key_ring.reset();
    remove_durable_audit_files(durable_audit_path);

    return 0;
}
