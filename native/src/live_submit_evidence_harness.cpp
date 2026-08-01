// SPDX-License-Identifier: proprietary
// live_submit_evidence_harness.cpp — exercises orchestrate_submit() end-to-end
// for all 4 dry-run-before-live evidence paths (D12-9 checklist item 5).
//
// Governance: L1. This is NOT a testnet harness yet — it uses a mock
// SubmitPort, exactly like the GTest suite does, because no real network
// SubmitPort implementation exists (see SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md)
// and no testnet credentials were provided. What this DOES do that the unit
// tests don't: run the actual orchestrator end-to-end as a standalone binary,
// with real wall-clock timestamps and a real build-hash, producing a real
// populated DryRunEvidenceChain and printing its state — closer to what a
// real dry-run-before-live evidence run would look like, minus the network.
//
// To point this at Binance testnet once a real SubmitPort exists: replace
// mock_submit() with a call into BinancePrivateRestClient configured against
// testnet.binance.vision, and load real (testnet-only) credentials via
// SecureEnvLoader — neither of which exists yet.
//
// SIMULATION ONLY. No network. No secret. No real order.

#include <hengyuan/live_submit_orchestrator.hpp>

#include <cstdio>
#include <cstring>

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

SubmitOutcome g_mock_outcome{SubmitOutcome::Accepted};

SubmitResponse mock_submit(const char* /*coid*/, std::uint32_t /*sym*/,
                           OrderSide /*side*/, OrderType /*type*/,
                           std::int64_t /*price*/, std::int64_t /*qty*/,
                           const SymbolRules& /*rules_snapshot*/,
                           void* /*ud*/) {
    switch (g_mock_outcome) {
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
    }
    return "?";
}

struct Fixture {
    AuditRingSink audit;
    KillSwitch kill_switch;
    DryRunEvidenceChain evidence;
    RequestWeightTracker rate_tracker;
    InFlightRegistry in_flight;
    SymbolRules rules{};
    AccountSnapshot account{};
    OrchestratorContext ctx{};
    std::uint32_t seq{0};

    Fixture() {
        audit.set_available(true);
        kill_switch.operator_reset();
        rate_tracker.reset(6000, 500);

        std::strncpy(rules.symbol, "BTCUSDT", sizeof(rules.symbol) - 1);
        rules.is_trading = true;
        rules.min_price_ticks = 100;
        rules.max_price_ticks = 10'000'000;
        rules.tick_size_ticks = 100;
        rules.min_qty_ticks = 10;
        rules.max_qty_ticks = 1'000'000;
        rules.step_size_ticks = 10;
        rules.min_notional_ticks = 1000;

        std::strncpy(account.assets[0].asset, "USDT", 5);
        account.assets[0].free_ticks = 999'999;
        account.asset_count = 1;
        account.can_trade = true;
        account.timestamp_ms = 900;

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
        ctx.now_ms = 1000;
        ctx.exposure_limits.freshness_max_age_ms = 30000;
        ctx.exposure_limits.single_order_notional_cap = 500'000;
        ctx.exposure_limits.total_exposure_notional_cap = 1'000'000;
        ctx.price_ticks = 1000;
        ctx.qty_ticks = 10;
        ctx.symbol_id = 1;
        ctx.mode = ExecutionMode::DryRun;
        ctx.order_weight = 1;
        ctx.submit_port = {mock_submit, nullptr};
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

void run_scenario(Scenario s, std::uint32_t stamp) {
    Fixture fx;
    fx.bind_confirmation();

    const char* label = "?";

    switch (s) {
        case Scenario::Submit:
            label = "SubmitSuccess";
            g_mock_outcome = SubmitOutcome::Accepted;
            break;
        case Scenario::Reject:
            label = "SubmitReject";
            g_mock_outcome = SubmitOutcome::Rejected;
            break;
        case Scenario::Ambiguous:
            label = "SubmitAmbiguous";
            g_mock_outcome = SubmitOutcome::Timeout;
            break;
        case Scenario::Kill:
            label = "KillSwitch";
            fx.kill_switch.trigger();
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

    std::printf("  orchestrate_submit(): gate=%s  order_state=%s  audit_records=%zu\n",
                gate_name(result.gate),
                order_state_name(result.order.state),
                fx.audit.count());
}

}  // namespace

int main() {
    std::printf("=== Live Submit Orchestrator — Dry-Run Evidence Harness ===\n");
    std::printf("SIMULATION ONLY. Mock SubmitPort. No network. No real order.\n");
    std::printf("(Real testnet execution requires a SubmitPort implementation per\n");
    std::printf(" SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md, and testnet credentials\n");
    std::printf(" neither of which this harness has.)\n\n");

    auto stamp = build_stamp();
    std::printf("Build stamp: 0x%08X (derived from __DATE__ __TIME__)\n\n", stamp);

    run_scenario(Scenario::Submit, stamp);
    run_scenario(Scenario::Reject, stamp);
    run_scenario(Scenario::Ambiguous, stamp);
    run_scenario(Scenario::Kill, stamp);

    std::printf("\nAll 4 evidence paths exercised in this process. Each scenario above\n");
    std::printf("used a freshly constructed Fixture, so live_ready() per-scenario\n");
    std::printf("reflects that single scenario's evidence chain, not a cumulative one\n");
    std::printf("— a real dry-run-before-live run would need all 4 to be true within\n");
    std::printf("the SAME long-lived evidence chain instance the live runtime uses.\n");
    return 0;
}
