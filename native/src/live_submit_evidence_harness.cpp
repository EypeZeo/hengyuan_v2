// SPDX-License-Identifier: proprietary
// live_submit_evidence_harness.cpp — exercises orchestrate_submit() end-to-end
// for all 4 dry-run-before-live evidence paths (D12-9 checklist item 5).
//
// Governance: L1. The executable uses an in-process SubmitPort and a
// temporary durable store to check the orchestrator independently of Binance.
//
// SIMULATION ONLY. No network. No secret. No real order.

#include <hengyuan/live_submit_orchestrator.hpp>
#include <hengyuan/synthetic_readiness.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
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

// A build "hash" that's at least derived from something real (compile
// timestamp), rather than a hardcoded magic number like the unit tests use.
// Not a git hash — this harness doesn't have access to git metadata at
// compile time without a build-system change, which is out of scope here.
std::uint32_t build_stamp() noexcept {
    std::uint32_t h = 2166136261u;  // FNV-1a seed
    for (const char* p = __DATE__ __TIME__; *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 16777619u;
    }
    return h;
}

// The one knob that decides which of the 4 paths this run exercises.
enum class Scenario { Submit, Reject, Ambiguous, Kill };

constexpr std::uint32_t kHarnessRulesVersion = 1;

struct MockSubmitState {
    SubmitOutcome outcome{SubmitOutcome::Accepted};
    unsigned calls{0};
};

std::uint32_t mock_rules_version(void*) noexcept { return kHarnessRulesVersion; }

SubmitResponse mock_submit(const char* /*coid*/, std::uint32_t /*sym*/,
                           OrderSide /*side*/, OrderType /*type*/,
                           std::int64_t /*price*/, std::int64_t /*qty*/,
                           const SymbolRules& /*rules_snapshot*/,
                           void* userdata) {
    auto& mock = *static_cast<MockSubmitState*>(userdata);
    ++mock.calls;
    switch (mock.outcome) {
        case SubmitOutcome::Accepted:     return {SubmitOutcome::Accepted, 999001, 0};
        case SubmitOutcome::Rejected:     return {SubmitOutcome::Rejected, 0, -1013};
        case SubmitOutcome::Timeout:      return {SubmitOutcome::Timeout, 0, 0};
        case SubmitOutcome::NetworkError: return {SubmitOutcome::NetworkError, 0, 0};
        // See live_submit_orchestrator.hpp's SubmitOutcome::StaleRulesVersion doc
        // comment: unreachable via a real SubmitFn under this codebase's gate
        // ordering (Gate 1 catches it before call() is ever invoked). Included
        // only so this switch stays exhaustive under -Wswitch.
        case SubmitOutcome::StaleRulesVersion: return {SubmitOutcome::NetworkError, 0, -1};
    }
    return {SubmitOutcome::NetworkError, 0, -1};
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
        case OrchestratorGate::TradeScopeNotAuthorized: return "TradeScopeNotAuthorized";
        case OrchestratorGate::TradeScopeBudgetExhausted: return "TradeScopeBudgetExhausted";
        case OrchestratorGate::AccountPeriodNotAuthorized: return "AccountPeriodNotAuthorized";
        case OrchestratorGate::RestrictedExitNotAuthorized: return "RestrictedExitNotAuthorized";
        case OrchestratorGate::InFlightCapacityExceeded: return "InFlightCapacityExceeded";
        case OrchestratorGate::IntentOrphanBudgetExceeded: return "IntentOrphanBudgetExceeded";
    }
    return "?";
}

// Real KeyRing + DurableAuditSink, one temp file per Fixture instance --
// this is what lets the Submit/Reject/Ambiguous scenarios below actually
// reach their intended gate instead of failing closed at the new Gate 10b
// (Intent durable Ack). Mirrors test_durable_audit_sink.cpp's fixture.
std::string durable_audit_temp_path() {
    static int counter = 0;
    ++counter;
#ifdef _WIN32
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + "hy_evidence_harness_" + std::to_string(GetCurrentProcessId()) +
           "_" + std::to_string(counter) + ".log";
#else
    return "/tmp/hy_evidence_harness_" + std::to_string(getpid()) + "_" +
           std::to_string(counter) + ".log";
#endif
}

void remove_durable_audit_files(const std::string& base_path) {
    std::remove(base_path.c_str());
    std::remove((base_path + ".lock").c_str());
    std::remove((base_path + ".tip").c_str());
    std::remove((base_path + ".tip.tmp").c_str());
}

struct Fixture {
    AuditRingSink audit;
    KillSwitch kill_switch;
    SyntheticReadiness synthetic_readiness{kill_switch};  // after kill_switch: it refers to it. See the banner in main().
    DryRunEvidenceChain evidence;
    SpotRateLimitTracker rate_limiter;
    InFlightRegistry in_flight;
    SymbolRules rules{};
    AccountSnapshot account{};
    MockSubmitState mock{};
    OrchestratorContext ctx{};
    std::uint32_t seq{0};

    std::string durable_audit_path;
    std::unique_ptr<KeyRing> key_ring;
    std::unique_ptr<DurableAuditSink> durable_audit_sink;

    Fixture() {
        audit.set_available(true);
        kill_switch.operator_reset();
        rate_limiter.configure(/*weight*/ 6000, 500, /*raw*/ 60000, 5000, /*orders*/ 100, 10);

        durable_audit_path = durable_audit_temp_path();
        remove_durable_audit_files(durable_audit_path);
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x20 + i);
        key_ring = std::make_unique<KeyRing>(kek);
        WrappedKeyRecord rec{};
        key_ring->add_key(1, std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}, rec);
        // Q23 S6: one of the two harnesses that turn the closure writer on (see live_submit_preflight_harness.cpp).
        durable_audit_sink = std::make_unique<DurableAuditSink>(durable_audit_path, *key_ring, 1, kMaxIntentOrphans,
                                                                AuditSinkOpenMode::Trading, NeverSentWrites::Enabled);
        ctx.durable_audit = make_durable_order_audit_port(*durable_audit_sink);

        std::strncpy(rules.symbol, "BTCUSDT", sizeof(rules.symbol) - 1);
        rules.is_trading = true;
        rules.min_price_ticks = 100;
        rules.max_price_ticks = 10'000'000;
        rules.tick_size_ticks = 100;
        rules.min_qty_ticks = 10;
        rules.max_qty_ticks = 1'000'000;
        rules.step_size_ticks = 10;
        rules.min_notional_ticks = 1000;
        rules.qty_scale = kBalanceScale;
        rules.rules_version = kHarnessRulesVersion;

        const AssetBalanceInput usdt[] = {{"USDT", 999'999, 0}};
        if (make_account_snapshot(900, /*can_trade=*/true, usdt, account) != AccountSnapshotError::None) {
            std::fprintf(stderr, "FATAL: the fixture's account snapshot was refused\n");
            std::abort();
        }

        ctx.audit = &audit;
        ctx.kill_switch = &kill_switch;
        ctx.readiness = &synthetic_readiness.readiness();
        ctx.evidence = &evidence;
        ctx.signer_ready = true;
        ctx.depth_synced = true;
        ctx.rate_limiter = &rate_limiter;
        ctx.in_flight = &in_flight;
        ctx.symbol_rules = &rules;
        ctx.account = &account;
        ctx.side = OrderSide::Buy;
        ctx.order_type = OrderType::Limit;
        ctx.base_asset = "BTC";
        ctx.quote_asset = "USDT";
        ctx.now_ms = 1000;
        ctx.exposure_limits.freshness_max_age_ms = 30000;
        ctx.exposure_limits.single_order_notional_cap = 500'000;
        ctx.exposure_limits.total_exposure_notional_cap = 1'000'000;
        ctx.price_ticks = 1000;
        ctx.qty_ticks = 10;
        ctx.symbol_id = 1;
        ctx.mode = ExecutionMode::DryRun;
        ctx.order_weight = 1;
        ctx.submit_port = {mock_submit, &mock, mock_rules_version};
    }

    ~Fixture() {
        durable_audit_sink.reset();
        key_ring.reset();
        remove_durable_audit_files(durable_audit_path);
    }

    void bind_confirmation() {
        ++seq;
        ctx.sequence = seq;
        ctx.confirmation.confirmed = true;
        ctx.confirmation.symbol_id = ctx.symbol_id;
        ctx.confirmation.side = ctx.side;
        ctx.confirmation.type = ctx.order_type;
        ctx.confirmation.price_ticks = ctx.price_ticks;
        ctx.confirmation.qty_ticks = ctx.qty_ticks;
        ctx.confirmation.max_notional = 1'000'000;
        ctx.confirmation.valid_until_ms = ctx.now_ms + 60'000;
    }
};

bool run_scenario(Scenario s, std::uint32_t stamp) {
    Fixture fx;
    fx.bind_confirmation();

    const char* label = "?";
    OrchestratorGate expected_gate = OrchestratorGate::Passed;
    OrderState expected_state = OrderState::Intent;
    bool hold_slot = false;
    unsigned expected_calls = 1;

    switch (s) {
        case Scenario::Submit:
            label = "SubmitSuccess";
            expected_gate = OrchestratorGate::SubmitAccepted;
            expected_state = OrderState::Accepted;
            hold_slot = true;
            break;
        case Scenario::Reject:
            label = "SubmitReject";
            fx.mock.outcome = SubmitOutcome::Rejected;
            expected_gate = OrchestratorGate::SubmitRejected;
            expected_state = OrderState::Rejected;
            break;
        case Scenario::Ambiguous:
            label = "SubmitAmbiguous";
            fx.mock.outcome = SubmitOutcome::Timeout;
            expected_gate = OrchestratorGate::SubmitAmbiguous;
            expected_state = OrderState::Ambiguous;
            hold_slot = true;
            break;
        case Scenario::Kill:
            label = "KillSwitch";
            fx.kill_switch.trigger();
            expected_gate = OrchestratorGate::KillSwitchNotNormal;
            expected_calls = 0;
            break;
    }

    // DryRunEvidenceChain models "all 4 paths have ALREADY been proven in
    // prior dry-run testing" as a precondition Gate 3 checks, not something
    // the orchestrator populates live during the run under test — so the
    // fixture pre-seeds all 4 paths (this harness's own prior runs are the
    // "prior testing") before exercising each scenario, matching how the
    // GTest fixture's fill_evidence() works, except with a real build stamp
    // instead of a hardcoded constant.
    for (int i = 0; i < static_cast<int>(kEvidencePathCount); ++i) {
        fx.evidence.record(static_cast<EvidencePath>(i), fx.ctx.now_ms, stamp, 1);
    }

    std::printf("[%s] pre-seeded evidence: all_paths_exercised=%s consistent_build=%s live_ready=%s\n",
                label,
                fx.evidence.all_paths_exercised() ? "true" : "false",
                fx.evidence.consistent_build() ? "true" : "false",
                fx.evidence.live_ready() ? "true" : "false");

    auto result = orchestrate_submit(fx.ctx);

    const bool slot_held = !result.order.client_order_id.empty() &&
                           fx.in_flight.is_in_flight(result.order.client_order_id.view());
    std::printf("  orchestrate_submit(): gate=%s  order_state=%s  audit_records=%zu\n",
                gate_name(result.gate),
                order_state_name(result.order.state),
                fx.audit.count());
    const bool passed = result.gate == expected_gate && result.order.state == expected_state &&
                        fx.mock.calls == expected_calls && slot_held == hold_slot;
    if (!passed) {
        std::fprintf(stderr, "%s: expected gate=%s, state=%s, calls=%u, slot=%d; got calls=%u, slot=%d, pretrade=%u\n",
                     label, gate_name(expected_gate), order_state_name(expected_state), expected_calls,
                     static_cast<int>(hold_slot), fx.mock.calls, static_cast<int>(slot_held),
                     static_cast<unsigned>(result.pre_trade_detail));
    }
    return passed;
}

}  // namespace

int main() {
    std::printf("=== Live Submit Orchestrator — Dry-Run Evidence Harness ===\n");
    std::printf("SIMULATION ONLY. Mock SubmitPort. No network. No real order.\n");
    std::printf("Gate 2b (SubmitReadyToken) is wired to a SYNTHETIC readiness here: this harness has no private\n");
    std::printf("user-data stream, so the stream/sweep part of the gate is exercised by the unit tests, not by this run.\n\n");

    auto stamp = build_stamp();
    std::printf("Build stamp: 0x%08X (derived from __DATE__ __TIME__)\n\n", stamp);

    unsigned passed = 0;
    for (const auto scenario : {Scenario::Submit, Scenario::Reject, Scenario::Ambiguous, Scenario::Kill}) {
        if (run_scenario(scenario, stamp)) ++passed;
    }

    std::printf("\n%u/4 offline execution scenarios passed.\n", passed);
    return passed == 4 ? 0 : 1;
}
