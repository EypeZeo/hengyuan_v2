// SPDX-License-Identifier: proprietary
// live_submit_preflight_harness.cpp — 批次 6 6b-1, on the private-stream pipeline since M5b-1: cold-start harness
// proving orchestrate_submit() can genuinely reach "just missing operator confirmation" against REAL testnet market
// data AND a REAL private user-data stream, with ZERO real network write side effect.
//
// The cold-start chain (credentials / SymbolRegistry / BinancePrivateRestClient / make_binance_submit_port / clock
// sync / exchangeInfo / the account's uid as the EXCHANGE reports it) is followed by four things:
//   1. The public kline stream feeding a loaded StreamingEvaluator (批次 5) + HoldingStateTracker
//      (批次 6a-2) -- real bar-by-bar signal evaluation.
//   2. A real public depth stream ("<symbol>@depth@100ms") through DepthManager, so
//      ctx.depth_synced can genuinely become true.
//   3. A real KillSwitch (Normal) + dry-run evidence that is EARNED at process start: the four
//      EvidencePath scenarios really run through orchestrate_submit() (VerifiedDryRunEvidence,
//      批次 6 6b-0b) and only a scenario that reached its expected terminal state is recorded.
//      (An earlier version of this file "replayed" the four paths by looping record() over the
//      enum -- nothing had run. DryRunEvidenceChain::live_ready() is purely in-memory --
//      dry_run_evidence.hpp's own reset()/no serialization -- so this must happen fresh every
//      process start, not "once, ever".)
//   4. The private stream, for real (M5b-1; it replaces the retired listenKey stream and the synthetic readiness this
//      file used before): a signed WebSocket API subscription (userDataStream.subscribe.signature) verified with
//      session.subscriptions, supervised and rolled over like the public feeds; the reconnect barrier and its sweep;
//      StartupRecovery over this run's scratch store; the account kept fresh through GET /api/v3/account (periodically,
//      and once per sweep, tagged with the sweep's epoch); and the store bound to the exchange's uid before anything can
//      be ready. That is one PrivateStreamPipeline (private_stream_pipeline.hpp). Gate 2b of every probe therefore asks a
//      readiness built over live objects, and "gate=OperatorNotConfirmed" means the whole chain passed -- the stream,
//      the sweep, the account and the recovery included -- not that a script said so.
//
// ONE LOOP THREAD. main() ticks the pipeline's two halves -- hot_tick() (stream, recovery, drains) and reconcile_tick()
// (periodic account fetch, the sweep's queries) -- from the same loop. That is not a shortcut: the REST adapters sign,
// and BoundHmacCredentials answers only on the thread that bound it (F11), and the rate-limit tracker belongs to that
// thread too. The REST client, the stream's session factory and the order path share this thread and ONE tracker; a
// separate reconcile thread is a design of its own (blueprint L-53).
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
// composite adapters still assemble against a real, freshly-refreshed SymbolRegistry -- but it
// is never assigned to ctx.submit_port). ctx.confirmation is never bound anywhere in this file
// either: fill_preflight_order() (preflight_probe.hpp), the one place this harness puts an
// order into a context, resets it to "not confirmed" and installs the mock port it is handed.
// Combined, orchestrate_submit() can therefore reach at most OrchestratorGate::OperatorNotConfirmed
// and NEVER Gate 12 (SubmitPortInvalid) with a real port, NEVER an actual network POST. This is
// why 6b-1 needs no ADR-019 D2 "L5 审" before merge -- it produces zero real network writes,
// unlike 6b-2 (which swaps in the real port AND a real human confirmation gate, and DOES require
// that review before real execution). The private stream adds reads and a subscription only: the
// signed WebSocket API request is userDataStream.subscribe.signature, and the REST calls are
// GETs (clock, exchangeInfo, account; the sweep queries only the orders this run's scratch store
// holds, and a fresh scratch store holds none).
//
// Testnet only: no --production flag, .env read from a fixed process-cwd-relative path per
// ADR-019 D4 (docs/PRIVATE_STREAM_JOINT_RUN.md is the owner's runbook for a Testnet run).
//
// Usage: run from the repository root (so ./.env resolves), after `chmod 600 .env`:
//   ./live_submit_preflight_harness <spec_toml_path> [duration_seconds=60] [interval=1h]
// Symbol is fixed to BTCUSDT/btcusdt throughout (a single-symbol scope).
//
// Data-level staleness is the pipeline's too: Binance's pings keep the sessions' 90 s idle timeout
// satisfied, so a connection can stay up while the DATA stops -- a depth feed with no accepted event
// for stale_after_ms resets the book and replaces its connection, and a kline whose next bar is
// overdue on the exchange clock is repaired from REST (and its connection replaced if it was up when
// the bar was due).
//
// A probe does not have to wait for the strategy: a 1h strategy flips rarely, and a run that never probes proves nothing about the order path. Once
// the private stream is reconciled, startup recovery is Ready and the depth book tracks, the harness asks the chain ONCE with an OPEN-shaped order
// ("[startup probe]" in the log) and expects the same OperatorNotConfirmed. Every probe, from either source, writes an Intent frame to this run's
// scratch store before the confirmation gate (Gate 10b) -- that is the real durable path, and the store is deleted at exit.
//
// NOT covered here, deliberately (each is its own batch): recovery over a PERSISTENT store (this one is a scratch store with a fixed test key: there
// is never anything to recover, and the audit key's provisioning is the owner's decision) and the target-position planner (this file still probes
// on HoldingStateTracker transitions); operator confirmation. An AI session compiles this file but never runs it with real credentials: the
// Testnet joint run is the owner's, with Testnet credentials.

#include <hengyuan/binance_clock_sync.hpp>
#include <hengyuan/binance_klines_rest.hpp>
#include <hengyuan/binance_rest_snapshot.hpp>
#include <hengyuan/binance_submit_adapter.hpp>
#include <hengyuan/binance_ws_api_session_factory.hpp>
#include <hengyuan/depth_manager.hpp>
#include <hengyuan/env_loader.hpp>
#include <hengyuan/feed_validity_gate.hpp>
#include <hengyuan/holding_state_tracker.hpp>
#include <hengyuan/live_submit_orchestrator.hpp>
#include <hengyuan/preflight_probe.hpp>
#include <hengyuan/private_stream_pipeline.hpp>
#include <hengyuan/public_feed_pipeline.hpp>
#include <hengyuan/public_feed_policy.hpp>
#include <hengyuan/rest_account_fetcher.hpp>
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
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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

// The symbol this harness trades is fixed (BTCUSDT), so the assets it needs balances of are fixed too: the base and quote asset the order path
// checks (Buy: quote, Sell: base). They are declared to the REST client as the account's asset scope (account_truth.hpp::AccountAssetScope, L4 spec
// 4.3) and handed to every probe's environment, from these two constants only. The Spot Testnet funds an account in EVERY asset (517 non-zero balances,
// a few with non-ASCII names, on the first joint run): without the declaration the account answer is refused whole -- the snapshot holds 32 assets.
constexpr std::string_view kProbeBaseAsset = "BTC";
constexpr std::string_view kProbeQuoteAsset = "USDT";

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
        case OrchestratorGate::NotReady: return "NotReady";
        case OrchestratorGate::FinalAuthorizationRefused: return "FinalAuthorizationRefused";
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

// --- DemoControlPlaneSink -- append_snapshot() is the only real override this class needs:
// SymbolRegistry::refresh_from_exchange_info() journals its snapshot there, and a fresh cold-start
// process reaches no other append. An anonymous-namespace, src/-local type on purpose (this file
// must not reach into another translation unit's internals).
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

// Every file a DurableAuditSink derives from its path: the log plus the key-rotation and store-identity sidecars and
// the store binding the pipeline writes beside the log (bind_store()), each with its lock file, tip anchor and the
// tip's temp file. (Only the first four used to be removed here, which left the sidecars behind on every run.)
void remove_durable_audit_files(const std::string& base_path) {
    for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".keyrotations", ".keyrotations.lock",
                               ".keyrotations.tip", ".keyrotations.tip.tmp", ".storeid", ".storeid.lock",
                               ".storeid.tip", ".storeid.tip.tmp", ".binding", ".binding.lock", ".binding.tip",
                               ".binding.tip.tmp"}) {
        std::remove((base_path + suffix).c_str());
    }
}

// Removes the scratch store's files when main() is left by any road, the early returns included. It is declared
// BEFORE the objects that hold the store open, so it runs after they are gone (an open file cannot be removed on
// Windows).
class ScratchStoreCleanup {
public:
    explicit ScratchStoreCleanup(std::string base_path) : base_path_(std::move(base_path)) {}
    ScratchStoreCleanup(const ScratchStoreCleanup&) = delete;
    ScratchStoreCleanup& operator=(const ScratchStoreCleanup&) = delete;
    ~ScratchStoreCleanup() { remove_durable_audit_files(base_path_); }

private:
    std::string base_path_;
};

using PrivateSession = BinanceWsApiUserDataSession;
using PrivatePipeline = PrivateStreamPipeline<PrivateSession>;

// The symbol rules of a recovered order's symbol, for startup recovery (PipelineRulesLookup): the registry's current
// rules, refreshed from exchangeInfo before the pipeline exists. A symbol id the registry does not hold answers
// rules_version 0, where recovery stops and the state machine degrades -- fail-closed.
SymbolRules registry_rules_for(void* registry, std::uint32_t symbol_id) noexcept {
    return static_cast<const SymbolRegistry*>(registry)->current_rules(symbol_id);
}

const char* session_end_name(WsApiSessionEnd e) noexcept {
    switch (e) {
        case WsApiSessionEnd::None: return "None";
        case WsApiSessionEnd::Requested: return "Requested";
        case WsApiSessionEnd::Failed: return "Failed";
        case WsApiSessionEnd::ServerShutdown: return "ServerShutdown";
        case WsApiSessionEnd::StreamTerminated: return "StreamTerminated";
    }
    return "?";
}

const char* private_rest_error_name(PrivateRestError e) noexcept {
    switch (e) {
        case PrivateRestError::None: return "None";
        case PrivateRestError::InvalidConfig: return "InvalidConfig";
        case PrivateRestError::Resolve: return "Resolve";
        case PrivateRestError::Connect: return "Connect";
        case PrivateRestError::TlsHandshake: return "TlsHandshake";
        case PrivateRestError::Write: return "Write";
        case PrivateRestError::Read: return "Read";
        case PrivateRestError::HttpStatus: return "HttpStatus";
        case PrivateRestError::JsonParse: return "JsonParse";
        case PrivateRestError::MalformedResponse: return "MalformedResponse";
        case PrivateRestError::CapacityExceeded: return "CapacityExceeded";
        case PrivateRestError::ClockNotFresh: return "ClockNotFresh";
        case PrivateRestError::SigningFailed: return "SigningFailed";
        case PrivateRestError::ClockSkewTooLarge: return "ClockSkewTooLarge";
        case PrivateRestError::RateLimitExhausted: return "RateLimitExhausted";
    }
    return "?";
}

const char* terminal_reason_name(FeedTerminalReason r) noexcept {
    switch (r) {
        case FeedTerminalReason::None: return "None";
        case FeedTerminalReason::Shutdown: return "Shutdown";
        case FeedTerminalReason::TooManyFailures: return "TooManyFailures";
    }
    return "?";
}

const char* kill_state_name(KillState s) noexcept {
    switch (s) {
        case KillState::Normal: return "Normal";
        case KillState::Armed: return "Armed";
        case KillState::Triggered: return "Triggered";
        case KillState::Latched: return "Latched";
    }
    return "?";
}

// One line about a WS API generation: how it ended (or stands) and what the exchange said. The owner reads these to see WHY a generation failed -- the
// stage it stopped at, the status of the subscribe answer and of the session.subscriptions listing, a ban's retryAfter -- without a debugger.
void print_session_line(const char* when, const PrivateSession& session) {
    const WsApiUserDataSessionStats st = session.stats_snapshot();
    std::printf("Private stream generation %" PRIu64 " (%s): end=%s connected=%s stage=%s ec=%s | subscribe_status=%d subscribe_error=%" PRId64
                " verify_status=%d verify_error=%" PRId64 " subscriptions_listed=%u retry_after_ms=%" PRId64 " | messages=%" PRIu64 " events=%" PRIu64
                " parse_failed=%" PRIu64 " unknown_events=%" PRIu64 " lost_events=%" PRIu64 " push_dropped=%" PRIu64 " foreign=%" PRIu64
                " server_shutdowns=%" PRIu64 " errors=%" PRIu64 "\n",
                session.generation(), when, session_end_name(session.end_cause()), session.is_connected() ? "yes" : "no",
                st.last_error_stage.empty() ? "-" : st.last_error_stage.c_str(),
                st.last_error_ec ? st.last_error_ec.message().c_str() : "-", st.subscribe_status, st.subscribe_error_code, st.verify_status,
                st.verify_error_code, st.subscriptions_listed, st.retry_after_ms, st.messages_received, st.events_delivered, st.parse_failed,
                st.unknown_events, st.lost_events, st.push_dropped, st.foreign_subscription_events, st.server_shutdowns, st.errors);
}

// Prints one line whenever something the joint run is judged on changes -- the supervisor's state, the generation, whether the session is verified, the
// barrier, startup recovery -- and one more about a generation when the next one replaces it. It also keeps the last session alive (the supervisor lets go
// of it at shutdown) for the final report. Hot thread only: it reads the pipeline's single-thread state.
class PrivateStreamWatch {
public:
    void poll(PrivatePipeline& pipeline, std::int64_t at_ms) {
        const std::shared_ptr<PrivateSession> session = pipeline.supervisor().current();
        if (session && session != last_session_) {
            if (last_session_) print_session_line("ended", *last_session_);
            last_session_ = session;
        }
        const FeedState feed = pipeline.supervisor().state();
        const std::uint64_t generation = pipeline.supervisor().generation();
        const bool verified = session != nullptr && session->is_connected();
        const BarrierState barrier = pipeline.readiness().barrier().state();
        const RunState recovery = pipeline.recovery().state();
        if (seen_ && feed == feed_ && generation == generation_ && verified == verified_ && barrier == barrier_ && recovery == recovery_) return;
        seen_ = true;
        feed_ = feed;
        generation_ = generation;
        verified_ = verified;
        barrier_ = barrier;
        recovery_ = recovery;
        std::printf("[t=%" PRId64 " ms] private stream: supervisor=%s generation=%" PRIu64 " verified=%s | barrier=%s | recovery=%s\n", at_ms,
                    feed_state_name(feed), generation, verified ? "yes" : "no", barrier_state_name(barrier), run_state_name(recovery));
    }

    [[nodiscard]] const std::shared_ptr<PrivateSession>& last_session() const noexcept { return last_session_; }

private:
    std::shared_ptr<PrivateSession> last_session_;
    bool seen_{false};
    FeedState feed_{FeedState::Idle};
    std::uint64_t generation_{0};
    bool verified_{false};
    BarrierState barrier_{BarrierState::Down};
    RunState recovery_{RunState::Bootstrapping};
};

struct ProbeCounters {
    std::uint64_t total{0};
    std::uint64_t at_confirmation_gate{0};  // OperatorNotConfirmed: every gate before the operator's confirmation passed
    std::uint64_t not_ready{0};             // refused at Gate 2b (the private stream's readiness)
};

// The joint run's report: what the owner pastes back. Plain key=value lines, one concern per line, the counters of every part the pipeline owns, and at the
// end the criteria a joint run is judged by (the owner decides what they mean; this only states which hold).
void print_private_stream_report(PrivatePipeline& pipeline, const WsApiSessionFactory& factory, const RestAccountFetcher& fetcher,
                                 const KillSwitch& kill_switch, const PrivateStreamWatch& watch, const ProbeCounters& probes) {
    std::printf("\n=== Private stream joint-run report ===\n");

    const std::string_view binding = ensure_binding_status_name(pipeline.binding_status());
    std::printf("store binding: status=%.*s may_be_used=%s\n", static_cast<int>(binding.size()), binding.data(), pipeline.store_bound() ? "yes" : "no");

    const RecoveryApplyResult& recovered = pipeline.recovery_result();
    std::printf("recovery: state=%s degraded_reason=%s applied=%s recovered_orders=%zu applied_orders=%zu\n", run_state_name(pipeline.recovery().state()),
                degraded_reason_name(pipeline.recovery().degraded_reason()), pipeline.recovery_applied() ? "yes" : "no", recovered.recovered,
                recovered.applied);

    const ReconnectBarrier& barrier = pipeline.readiness().barrier();
    const BarrierStats& bs = barrier.stats();
    std::printf("barrier: state=%s generation=%" PRIu64 " reconciled_generation=%" PRIu64 " epoch=%" PRIu64 " | generations_seen=%" PRIu64 " downs=%" PRIu64
                " sweeps_begun=%" PRIu64 " sweeps_requested=%" PRIu64 " sweeps_undelivered=%" PRIu64 " sweeps_completed=%" PRIu64 " sweeps_unresolved=%" PRIu64
                " sweep_timeouts=%" PRIu64 " stale_heartbeats=%" PRIu64 " soft_barriers=%" PRIu64 " events_refused=%" PRIu64 " late_events_dropped=%" PRIu64
                "\n",
                barrier_state_name(barrier.state()), barrier.generation(), barrier.reconciled_generation(), barrier.epoch(), bs.generations_seen, bs.downs,
                bs.sweeps_begun, bs.sweeps_requested, bs.sweeps_undelivered, bs.sweeps_completed, bs.sweeps_unresolved, bs.sweep_timeouts, bs.stale_heartbeats,
                bs.soft_barriers, bs.events_refused, bs.late_events_dropped);

    const SweepJobStats& job = pipeline.sweep_job().stats();
    const SweepPumpStats& pump = pipeline.sweep_pump().stats();
    std::printf("sweep: job requests_taken=%" PRIu64 " sweeps_started=%" PRIu64 " sweeps_completed=%" PRIu64 " account_steps=%" PRIu64
                " marker_push_failures=%" PRIu64 " | pump requests_sent=%" PRIu64 " request_push_failures=%" PRIu64 " markers_received=%" PRIu64
                " reports_account_stale=%" PRIu64 "\n",
                job.requests_taken, job.sweeps_started, job.sweeps_completed, job.account_steps, job.marker_push_failures, pump.requests_sent,
                pump.request_push_failures, pump.markers_received, pump.reports_account_stale);

    // (`auto`: the account pieces are the pipeline's to build and name -- test_barrier_sweep_guard.cpp; the harness only reads their counters.)
    const auto& held = pipeline.holder().stats();
    const auto& refresh = pipeline.refresher().stats();
    const RestAccountFetcherStats& rest = fetcher.stats();
    std::printf("account: holder has_snapshot=%s applied=%" PRIu64 " stale=%" PRIu64 " rejected=%" PRIu64 " refreshed_epoch=%" PRIu64
                " | refresher attempts=%" PRIu64 " delivered=%" PRIu64 " not_sent=%" PRIu64 " failed=%" PRIu64 " ring_full=%" PRIu64 " bad_clock=%" PRIu64
                " sweep_steps=%" PRIu64 " sweep_delivered=%" PRIu64 " | rest calls=%" PRIu64 " fetched=%" PRIu64 " not_sent=%" PRIu64 " failed=%" PRIu64
                " threw=%" PRIu64 " last_error=%s\n",
                pipeline.holder().has_snapshot() ? "yes" : "no", held.applied, held.stale, held.rejected, pipeline.holder().refreshed_epoch(),
                refresh.attempts, refresh.delivered, refresh.not_sent, refresh.failed, refresh.ring_full, refresh.bad_clock, refresh.sweep_steps,
                refresh.sweep_delivered, rest.calls, rest.fetched, rest.not_sent, rest.failed, rest.threw, private_rest_error_name(rest.last_error));

    const FeedSupervisorStats sup = pipeline.supervisor().stats();
    std::printf("supervisor: state=%s terminal_reason=%s generation=%" PRIu64 " attempts=%" PRIu64 " failures=%" PRIu64 " consecutive_failures=%u"
                " rollovers=%" PRIu64 " connect_timeouts=%" PRIu64 "\n",
                feed_state_name(sup.state), terminal_reason_name(sup.terminal_reason), sup.generation, sup.total_attempts, sup.total_failures,
                sup.consecutive_failures, sup.rollovers, sup.connect_timeouts);

    const WsApiSessionFactoryStats& fs = factory.stats();
    std::printf("factory: attempts=%" PRIu64 " created=%" PRIu64 " refused_clock=%" PRIu64 " refused_ban=%" PRIu64 " refused_endpoint=%" PRIu64
                " refused_budget=%" PRIu64 " sign_failed=%" PRIu64 " create_failed=%" PRIu64 " ban_until_server_ms=%" PRId64 "\n",
                fs.attempts, fs.created, fs.refused_clock, fs.refused_ban, fs.refused_endpoint, fs.refused_budget, fs.sign_failed, fs.create_failed,
                fs.ban_until_server_ms);

    // The last generation, if there was one: its counters are that connection's own (the io thread is joined, so they are final).
    if (const auto& last = watch.last_session()) print_session_line("last", *last);

    const ReadinessStats& rs = pipeline.readiness().stats();
    std::printf("readiness: authorized=%" PRIu64, rs.authorized);
    for (std::size_t i = 0; i < rs.refused.size(); ++i) {
        if (rs.refused[i] != 0) std::printf(" refused_%s=%" PRIu64, not_ready_reason_name(static_cast<NotReadyReason>(i)), rs.refused[i]);
    }
    std::printf("\n");

    // The durable publishers are the pipeline's own; the context it makes points at them.
    const auto wiring = pipeline.make_context();
    if (wiring.reconcile_durable != nullptr && wiring.user_data_durable != nullptr) {
        std::printf("durable: reconcile acked_frames=%" PRIu64 " failed_appends=%" PRIu64 " blocked=%s | user_data acked_frames=%" PRIu64
                    " failed_appends=%" PRIu64 " written=%" PRIu64 " refused_unreadable=%" PRIu64 " refused_untracked=%" PRIu64
                    " refused_contradiction=%" PRIu64 " blocked=%s\n",
                    wiring.reconcile_durable->acked_frames(), wiring.reconcile_durable->failed_appends(),
                    wiring.reconcile_durable->blocked() ? "yes" : "no", wiring.user_data_durable->stats().acked_frames,
                    wiring.user_data_durable->stats().failed_appends, wiring.user_data_durable->stats().written,
                    wiring.user_data_durable->stats().refused_unreadable, wiring.user_data_durable->stats().refused_untracked,
                    wiring.user_data_durable->stats().refused_contradiction, wiring.user_data_durable->blocked() ? "yes" : "no");
    }

    const LateFillStats& lf = pipeline.late_fill().stats();
    std::printf("late_fill: reports=%" PRIu64 " alarmed_orders=%" PRIu64 " trips=%" PRIu64 " audit_failed=%" PRIu64 " | kill_switch=%s\n", lf.reports,
                lf.alarmed_orders, lf.trips, lf.audit_failed, kill_state_name(kill_switch.state()));

    std::printf("pipeline: hot_ticks=%" PRIu64 " reconcile_ticks=%" PRIu64 " recoveries_applied=%" PRIu64 " binding_attempts=%" PRIu64 "\n",
                pipeline.stats().hot_ticks, pipeline.reconcile_ticks(), pipeline.stats().recoveries_applied, pipeline.stats().binding_attempts);
    std::printf("probes: total=%" PRIu64 " reached_OperatorNotConfirmed=%" PRIu64 " refused_NotReady=%" PRIu64 "\n", probes.total,
                probes.at_confirmation_gate, probes.not_ready);

    const bool reconciled_once = bs.sweeps_completed >= 1;
    const bool account_by_sweep = pipeline.holder().refreshed_epoch() >= 1;
    const bool recovery_ready = pipeline.recovery().state() == RunState::Ready;
    const bool order_path = probes.at_confirmation_gate >= 1;
    std::printf("joint-run criteria: reconciled_once=%s account_refreshed_by_sweep=%s recovery_ready=%s probe_reached_OperatorNotConfirmed=%s all_met=%s\n",
                reconciled_once ? "yes" : "no", account_by_sweep ? "yes" : "no", recovery_ready ? "yes" : "no", order_path ? "yes" : "no",
                (reconciled_once && account_by_sweep && recovery_ready && order_path) ? "yes" : "no");
}

}  // namespace

int main(int argc, char* argv[]) {
    // Unbuffered stdout: the log is the evidence of a Testnet run, and a run that dies half way (an exception, a kill) must not take the lines it already printed
    // with it. A redirected stdout is fully buffered by default (and "line buffered" does not exist on Windows); stderr is unbuffered already, so the two now
    // interleave in order when they share a file. One printf per event, a few per second at most: the cost is nothing.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

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

    // --- The cold-start chain ------------------------------------------------------------
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

    // One local rate-limit tracker for the whole run, owned by this (the hot) thread: the cold-start REST calls
    // below (clock sync, exchangeInfo, the account read) are charged to its Reconciliation lane through the client,
    // the private stream's session factory pays for every generation from the very same lane, and the orchestrator
    // further down reserves its order flow on the Strategy lane of the same tracker (SAFE-01 slice 4). Declared
    // before everything that points at it.
    SpotRateLimitTracker rate_limiter;
    rate_limiter.configure(/*weight*/ 6000, 500, /*raw*/ 60000, 5000, /*orders*/ 100, 10);

    SymbolRegistry registry;
    BinancePrivateRestClient client(binding, std::move(creds), &rate_limiter);
    {
        // Before the first account read (the uid read below, then the refresher's fetches): the declared scope, set once on the thread that owns the client.
        AccountAssetScope account_scope;
        if (!account_scope.add(kProbeBaseAsset) || !account_scope.add(kProbeQuoteAsset) || !client.set_account_scope(account_scope)) {
            std::fprintf(stderr, "FATAL: the account asset scope could not be declared.\n");
            return 1;
        }
    }
    SubmitAdapterContext submit_ctx{&client, &registry};
    // Constructed to prove H5's composite adapters still assemble against a real, freshly
    // refreshed SymbolRegistry. NEVER assigned to ctx.submit_port below (see this file's header
    // comment).
    SubmitPort real_submit_port_unused = make_binance_submit_port(submit_ctx);

    DemoControlPlaneSink cp_sink;
    ParsedExchangeInfo parsed{};

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
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: cold-start network setup failed: %s\n", e.what());
        return 1;
    }

    if (!registry.refresh_from_exchange_info(cp_sink, parsed)) {
        std::fprintf(stderr, "ERROR: SymbolRegistry::refresh_from_exchange_info() failed.\n");
        return 1;
    }

    // Read-only diagnostic: proves H5's composite adapters genuinely reach a real, freshly
    // refreshed SymbolRegistry, and (with -Werror=unused-but-set-variable active on the GCC
    // tier) keeps the compiler from flagging real_submit_port_unused as constructed-but-never-
    // read. Still never assigned to ctx.submit_port anywhere below.
    std::printf("Real SubmitPort assembled (never used for ctx.submit_port): rules_version=%u, valid=%d\n",
                real_submit_port_unused.current_rules_version(),
                real_submit_port_unused.is_valid() ? 1 : 0);

    // The one real, freshly-fetched SymbolRules this whole harness validates against --
    // captured once, right after refresh_from_exchange_info(), same "one snapshot, never
    // re-read mid-run" discipline orchestrate_submit() itself applies to
    // ctx.pre_trade_rules_snapshot.
    const SymbolRules rules = registry.current_rules(0);
    g_mock_rules_version.store(rules.rules_version, std::memory_order_relaxed);

    // What the exchange says about this symbol, once (public exchange metadata, no credential involved), so a probe the pre-trade check refuses can be read
    // against the numbers it was judged by. price = price_ticks / 10^price_scale, qty = qty_ticks / 10^qty_scale; min_notional is at the balance scale.
    std::printf("Symbol rules (as the exchange reports them): price_scale=%u qty_scale=%u | tick=%" PRId64 " min_price=%" PRId64 " max_price=%" PRId64
                " | step=%" PRId64 " min_qty=%" PRId64 " max_qty=%" PRId64 " | min_notional=%" PRId64 " (balance scale %d)\n",
                static_cast<unsigned>(rules.price_scale), static_cast<unsigned>(rules.qty_scale), rules.tick_size_ticks, rules.min_price_ticks,
                rules.max_price_ticks, rules.step_size_ticks, rules.min_qty_ticks, rules.max_qty_ticks, rules.min_notional_ticks, kBalanceScale);

    // price_mult/qty_mult MUST be derived from the real rules.price_scale/qty_scale, not a
    // hardcoded 1e8 -- otherwise the depth book's ticks (BinanceJsonParser::register_symbol())
    // and the REST snapshot's ticks (SnapshotRequest) would silently disagree with what
    // validate_pre_trade() checks rules.min_price_ticks/tick_size_ticks against.
    std::int64_t price_mult = 1, qty_mult = 1;
    if (!pow10_i64(rules.price_scale, price_mult) || !pow10_i64(rules.qty_scale, qty_mult)) {
        std::fprintf(stderr, "FATAL: registry's price_scale/qty_scale out of pow10_i64's domain.\n");
        return 1;
    }

    // The account this key trades as, as the EXCHANGE reports it (GET /api/v3/account, once): the store binding
    // below is made against this uid and never against a configured one. Read here, on the thread that bound the
    // credentials. (The value is an account identifier: only its length goes into the log.)
    std::uint64_t exchange_uid = 0;
    if (const PrivateRestError uid_error = fetch_exchange_account_uid(client, exchange_uid); uid_error != PrivateRestError::None) {
        std::fprintf(stderr, "ERROR: could not read the account uid from the exchange (PrivateRestError=%s).\n",
                     private_rest_error_name(uid_error));
        return 1;
    }
    std::printf("Exchange account uid read (%zu digits; the value is not printed)\n", std::to_string(exchange_uid).size());
    {
        // What the account answer held and what of it was kept: the entries outside the declared scope are counted here, never silently dropped.
        const AccountParseReport& read = client.last_account_report();
        std::printf("Exchange account read: entries=%" PRIu32 " represented=%" PRIu32 " zero_in_scope=%" PRIu32 " out_of_scope=%" PRIu32
                    " (scope: %.*s, %.*s; the others are counted, not converted)\n",
                    read.entries, read.represented, read.zero_balances, read.out_of_scope, static_cast<int>(kProbeBaseAsset.size()), kProbeBaseAsset.data(),
                    static_cast<int>(kProbeQuoteAsset.size()), kProbeQuoteAsset.data());
    }

    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);
    hy::configure_binance_ssl_context(ssl_ctx);

    // Real entropy for the reconnect jitter of every supervisor below (the policies' defaults are fixed seeds).
    std::random_device entropy;
    auto jitter_seed = [&entropy]() {
        return (static_cast<std::uint64_t>(entropy()) << 32) | static_cast<std::uint64_t>(entropy());
    };

    // --- The private stream's parts (M5b-1) -----------------------------------------------------------------------
    // The ring every generation's session fills and the pipeline's hot tick drains, and the parser memory all
    // generations share (allocated once, here).
    UserDataWsEventRing user_data_events;
    std::unique_ptr<WsApiCodec> ws_codec;
    try {
        ws_codec = std::make_unique<WsApiCodec>();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: the WebSocket API codec could not allocate its parser (%s).\n", e.what());
        return 1;
    }
    // The session factory signs the subscribe request, and the REST client has already taken the first binding of the
    // key (it owns it), so the factory gets its own: a second bind from the same still-protected loader, on this same
    // thread -- the only thread BoundHmacCredentials answers on.
    auto [ws_err, ws_creds] = BoundHmacCredentials::load_and_bind_credentials(binding, loader);
    if (ws_err != QuerySigningError::Ok) {
        std::fprintf(stderr, "ERROR: failed to bind the stream's credentials (QuerySigningError=%d).\n",
                     static_cast<int>(ws_err));
        return 1;
    }
    auto submit_authority = std::make_shared<SubmitAuthority>();
    WsApiUserDataSessionConfig private_session_config{};
    private_session_config.submit_authority = submit_authority;
    WsApiSessionFactory ws_factory(
        WsApiSessionFactory::Deps{ioc, ssl_ctx, user_data_events, *ws_codec, *ws_creds, client.clock_publisher(), rate_limiter},
        ws_api_endpoint_of(binding), private_session_config);
    RestAccountFetcher account_fetcher(client);

    InFlightRegistry in_flight;
    AuditRingSink audit;
    audit.set_available(true);
    PositionTruth position_truth;
    OrderFillContext fill_context;

    // --- Real KillSwitch (Normal) ---------------------------------------------------------------------------------
    KillSwitch kill_switch;
    kill_switch.operator_reset();

    // Real DurableAuditSink -- every probe writes its Intent frame here before the confirmation gate (Gate 10b), and
    // nothing else is ever written (confirmation is never bound), so an ephemeral per-run temp file (cleaned at
    // shutdown, and by ScratchStoreCleanup on every early return) is appropriate here, unlike a genuine production
    // audit trail.
    //
    // DEMO-ONLY, and NOT a recovery path (外部复核 P0-06): the pipeline's StartupRecovery runs over this log (so the
    // state machine and the store binding are exercised), but a fresh temp log with a fixture KEK can never contain a
    // previous run's orders, so nothing is ever recovered -- that part of the path runs empty. Anything that may submit
    // for real must run the same sequence over a PERSISTENT log (6b-0d builds and tests that; the 6b-2 harness wires
    // it, once the owner has decided how the audit key is provisioned).
    const std::string durable_audit_path = durable_audit_temp_path();
    remove_durable_audit_files(durable_audit_path);
    ScratchStoreCleanup scratch_store_cleanup(durable_audit_path);
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
    // Q23 S6: this Testnet-only harness is one of the two processes that turn the closure writer on (the other is live_submit_evidence_harness):
    // an order a local gate refuses after its Intent frame is closed as never sent instead of being left as an Intent orphan.
    // test_never_sent_guard.cpp pins that no other production source names NeverSentWrites.
    auto durable_audit_sink = std::make_unique<DurableAuditSink>(durable_audit_path, *key_ring, 1, kMaxIntentOrphans,
                                                                 AuditSinkOpenMode::Trading, NeverSentWrites::Enabled);

    // --- The private-stream pipeline (M5b-1, private_stream_pipeline.hpp) ----------------------------------------
    // Everything below is the process's and outlives it. The pipeline creates no thread and opens no connection by
    // itself: the supervisor connects on hot_tick(), through ws_factory, on this thread.
    PipelineParts parts{*durable_audit_sink, durable_audit_path, *key_ring, make_durable_order_audit_port(*durable_audit_sink),
                        audit, kill_switch, position_truth, in_flight, fill_context, user_data_events,
                        client.clock_publisher(), &rate_limiter};
    parts.query_port = QueryPort{&query_order_adapter, &client};
    parts.account_port = account_fetcher.port();
    parts.rules = PipelineRulesLookup{&registry_rules_for, &registry};

    PipelineConfig pipeline_cfg;
    pipeline_cfg.submit_authority = submit_authority;
    // The supervision the public feeds get: reconnect with backoff, a planned rollover well inside the 24 h limit of
    // a Binance connection, and an end to the run -- exit code 2, not a silent blind loop -- once ten attempts in a
    // row have failed.
    pipeline_cfg.feed = make_public_feed_policy(jitter_seed());
    // Generous caps -- this harness never reaches a real submit (confirmation is never bound), so these do not gate
    // any real risk; they only need to not spuriously reject the notional a probe computes. The freshness window is
    // also the one startup recovery judges the account by.
    pipeline_cfg.exposure_limits.freshness_max_age_ms = 30000;
    pipeline_cfg.exposure_limits.single_order_notional_cap = 1'000'000'000;
    pipeline_cfg.exposure_limits.total_exposure_notional_cap = 1'000'000'000;

    auto private_pipeline = std::make_unique<PrivatePipeline>(
        parts, pipeline_cfg, [&ws_factory](std::uint64_t generation) { return ws_factory(generation); });

    // The store binding: this run's scratch store, bound to the account the EXCHANGE says this key trades as (this
    // harness is Testnet-only, like the binding above). The verdict gates readiness -- an unbound store never lets the
    // process become ready -- and a refusal means the process must not run. Nothing has been started yet, so there is
    // nothing to undo.
    const EnsureBindingStatus binding_status =
        private_pipeline->bind_store(StoreEnvironment::Testnet, exchange_uid, reconcile_wall_now_ms());
    const std::string_view binding_status_text = ensure_binding_status_name(binding_status);
    std::printf("Store binding: %.*s\n", static_cast<int>(binding_status_text.size()), binding_status_text.data());
    if (!store_may_be_used(binding_status)) {
        std::fprintf(stderr, "FATAL: the store binding was refused (%.*s) -- refusing to start.\n",
                     static_cast<int>(binding_status_text.size()), binding_status_text.data());
        return 1;
    }
    // exchangeInfo is loaded (above) and registry_rules_for answers: recovery may scale what it recovers.
    private_pipeline->set_exchange_info_loaded(true);

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

    std::printf("=== Live Submit Preflight Harness (testnet) ===\n");
    std::printf("Real credentials, real clock sync, real exchangeInfo, real account read, real WebSocket API private "
                "stream, real depth+kline WS.\n");
    std::printf("ctx.submit_port is a MOCK, never the real make_binance_submit_port() result -- "
                "no real network order write is possible from this process.\n");
    std::printf("Gate 2b (SubmitReadyToken) asks the REAL readiness: the private stream, its sweep, the account and "
                "startup recovery are live objects here (store binding: %.*s).\n",
                static_cast<int>(binding_status_text.size()), binding_status_text.data());
    std::printf("Running for %d seconds (Ctrl+C to stop early).\n\n", duration_s);

    std::int64_t next_clock_sync_ms = 60'000;
    const std::int64_t loop_start_ms = get_now_ms();
    std::uint64_t poll_ticks = 0;
    std::uint64_t preflight_probes = 0;
    // Starts at the verdict of a pipeline that has not connected yet, so the first change is logged.
    SupervisedFeedInvalidReason last_feed_validity = SupervisedFeedInvalidReason::KlineDisconnected;

    PrivateStreamWatch watch;
    ProbeCounters probes;
    // The startup probe (see the header comment): at most three attempts, five seconds apart, until one reaches the confirmation gate.
    constexpr int kStartupProbeMaxAttempts = 3;
    int startup_probe_attempts = 0;
    bool startup_probe_done = false;
    std::int64_t next_startup_probe_ms = 0;

    // One probe of the order path: the order a suggested action calls for, put into a copy of the pipeline's context
    // by fill_preflight_order() (which leaves the confirmation unbound and the submit port a mock), asked of
    // orchestrate_submit(). Returns the gate it stopped at, or nothing when there was nothing to ask (no book yet, or
    // no order the symbol's rules allow). `origin` says why it ran ("bar" or "startup"); `steady_ms` is the private
    // stream's clock, the one the account's stamps are in.
    auto run_probe = [&](SuggestedAction action, const char* origin, std::int64_t steady_ms) -> std::optional<OrchestratorGate> {
        ++preflight_probes;
        const auto tob = pipeline->depth_manager().book().top_of_book();
        if (!tob) {
            std::printf("  [preflight probe #%" PRIu64 "] (%s) no book yet -- skipping\n", preflight_probes, origin);
            return std::nullopt;
        }
        // Small preflight-only quantity: the smallest one this symbol's rules accept at the probe's price (LOT_SIZE's
        // minimum on its step grid, and at least MIN_NOTIONAL) -- NOT the final 6b-2 sizing policy (that batch's own
        // plan text: "数量来自一个固定的小额配置上限"), just enough for pre-trade validation to see a genuinely valid
        // order shape. The bare minimum lot is not one: on the Testnet's BTCUSDT it is under 1 USDT against a 5 USDT minimum.
        const std::optional<PreflightProbe> plan = plan_preflight_probe(action, *tob, rules);
        if (!plan) {
            std::printf("  [preflight probe #%" PRIu64 "] (%s) no order to ask about (no usable price or quantity) -- skipping\n",
                        preflight_probes, origin);
            return std::nullopt;
        }

        auto ctx = private_pipeline->make_context();
        PreflightEnvironment env;
        env.evidence = &verified_evidence.chain();
        env.rules = &rules;
        env.base_asset = kProbeBaseAsset;
        env.quote_asset = kProbeQuoteAsset;
        env.signer_ready = true;
        env.depth_synced = (pipeline->depth_manager().state() == DepthState::Tracking);
        env.symbol_id = 0;
        env.sequence = static_cast<std::uint32_t>(preflight_probes);
        env.now_ms = steady_ms;
        env.submit_port = {mock_submit_must_never_be_called, nullptr, mock_current_rules_version};
        fill_preflight_order(ctx, *plan, env);

        const auto result = orchestrate_submit(ctx);
        ++probes.total;
        std::printf("  [preflight probe #%" PRIu64 "] (%s) %s %s price=%" PRId64 " qty=%" PRId64 " -> gate=%s\n",
                    preflight_probes, origin, action == SuggestedAction::Open ? "OPEN" : "CLOSE",
                    plan->side == OrderSide::Buy ? "BUY" : "SELL", plan->price_ticks, plan->qty_ticks,
                    gate_name(result.gate));
        if (result.gate == OrchestratorGate::OperatorNotConfirmed) {
            ++probes.at_confirmation_gate;
        } else {
            if (result.gate == OrchestratorGate::NotReady) ++probes.not_ready;
            // Which verdict stopped the chain, for the two gates that carry one: the gate's name alone does not say, and the audit ring that holds the
            // detail code is gone when the process exits.
            const char* detail_label = "";
            const char* detail = "";
            if (result.gate == OrchestratorGate::NotReady) {
                detail_label = " / reason=";
                detail = not_ready_reason_name(result.not_ready_reason);
            } else if (result.gate == OrchestratorGate::PreTradeFailed) {
                detail_label = " / reason=";
                detail = pre_trade_check_name(result.pre_trade_detail);
            }
            std::fprintf(stderr,
                         "  WARN: expected gate=OperatorNotConfirmed, got %s%s%s -- some "
                         "prerequisite this harness assembles is not yet ready (this can be "
                         "normal early in the run, e.g. depth/account/stream not synced yet).\n",
                         gate_name(result.gate), detail_label, detail);
        }
        return result.gate;
    };

    while (!g_stop.load()) {
        const std::int64_t now_ms = get_now_ms();
        // The private stream's clock: milliseconds of the steady clock since ITS epoch -- what the sessions stamp
        // their activity with and what the account is stamped in. The pipeline's ticks and every probe use it.
        const std::int64_t steady_ms = PrivateSession::steady_now_ms();

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

        // The pipeline's two halves, from this one thread (the REST adapters sign, and the credentials answer only on
        // the thread that bound them -- see the header comment). The hot half first: it polls the stream's supervisor
        // (a new generation is signed and started here), applies the account update, advances startup recovery and
        // drains the rings. Then the reconcile half: the periodic account fetch, and poll_once() with the sweep.
        const std::int64_t wall_now_ms = reconcile_wall_now_ms();
        private_pipeline->hot_tick(steady_ms, wall_now_ms);
        private_pipeline->reconcile_tick(steady_ms, wall_now_ms);
        watch.poll(*private_pipeline, now_ms);

        // A private stream whose supervisor has given up is not coming back -- nothing reconnects it -- so the run
        // cannot reach readiness again. Same end as a public feed that gave up: a non-zero exit code.
        if (private_pipeline->supervisor().state() == FeedState::Terminal) {
            std::fprintf(stderr, "ALERT: the private stream gave up (%s) -- stopping. Restart to recover.\n",
                         terminal_reason_name(private_pipeline->supervisor().stats().terminal_reason));
            exit_code = 2;
            break;
        }

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

            (void)run_probe(action, "bar", steady_ms);
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

        // The startup probe (see the header comment): the first moment the private stream is reconciled, startup recovery is Ready and the book
        // tracks, ask the order path once with an OPEN-shaped order, so a run proves the whole chain without waiting for the strategy to flip.
        if (!startup_probe_done && startup_probe_attempts < kStartupProbeMaxAttempts && now_ms >= next_startup_probe_ms &&
            private_pipeline->readiness().barrier().state() == BarrierState::Reconciled &&
            private_pipeline->recovery().state() == RunState::Ready && pipeline->depth_manager().state() == DepthState::Tracking &&
            feed.validity == SupervisedFeedInvalidReason::None) {
            ++startup_probe_attempts;
            next_startup_probe_ms = now_ms + 5'000;
            std::printf("[startup probe] the private stream is reconciled, startup recovery is Ready and the book tracks -- "
                        "asking the order path once\n");
            const std::optional<OrchestratorGate> gate = run_probe(SuggestedAction::Open, "startup", steady_ms);
            if (gate && *gate == OrchestratorGate::OperatorNotConfirmed) startup_probe_done = true;
        }

        ++poll_ticks;
        if (duration_s > 0 && (now_ms - loop_start_ms) >= static_cast<std::int64_t>(duration_s) * 1000) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Shutdown order (public_feed_pipeline.hpp's lifetime contract): tell every feed to stop, release
    // the work guard so run() returns once their pending handlers have finished, join the io thread --
    // and only then may the pipeline go out of scope. The private stream's supervisor is told the same
    // way (it stops its session and lets go of it; `watch` still holds the last one for the report).
    pipeline->shutdown();
    private_pipeline->supervisor().shutdown();
    ioc_guard.reset();
    if (io_thread.joinable()) io_thread.join();

    // One last hot pass on this thread, with no session left that could push anything: whatever the stream delivered while
    // it was stopping is drained (and made durable, or refused) before the report. No reconcile pass: it would start a REST
    // round trip nobody waits for.
    private_pipeline->hot_tick(PrivateSession::steady_now_ms(), reconcile_wall_now_ms());

    std::printf("\n=== Stats ===\n");
    std::printf("Loop ticks: %" PRIu64 "  preflight probes: %" PRIu64
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

    print_private_stream_report(*private_pipeline, ws_factory, account_fetcher, kill_switch, watch, probes);

    private_pipeline.reset();    // it points at the store: let go of it first
    durable_audit_sink.reset();  // close before removing the temp file
    key_ring.reset();
    remove_durable_audit_files(durable_audit_path);

    return exit_code;
}
