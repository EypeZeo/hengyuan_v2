// 批次 6 6b-1: proves the core property live_submit_preflight_harness.cpp's real-network
// wiring depends on -- given a fully gate-satisfying OrchestratorContext (kill switch Normal,
// dry-run evidence 4/4 + consistent build, depth synced, signer ready, rate limiter configured,
// a real & Acked durable-audit sink, valid symbol_rules + a fresh account snapshot) but
// ctx.confirmation NEVER bound, orchestrate_submit() reaches EXACTLY
// OrchestratorGate::OperatorNotConfirmed -- not an earlier gate (which would mean some
// prerequisite this harness assembles is subtly wrong) and not ConfirmationMismatch (which would
// mean confirmation ended up accidentally bound to something).
//
// No real network anywhere in this file -- same "no real network" test discipline as every
// other orchestrate_submit() test in this codebase (test_live_submit_orchestrator.cpp).
// submit_port.call() is asserted to be UNREACHABLE via a defensive trap that fails the test if
// it is ever invoked -- this is the same safety property live_submit_preflight_harness.cpp's
// own mock submit_port relies on: since ctx.confirmation.confirmed is never set anywhere before
// Gate 12 (SubmitPortInvalid) even gets checked, .call() can never legitimately run.
//
// Fixture mirrors live_submit_evidence_harness.cpp's own Fixture almost exactly (same real
// KillSwitch/DryRunEvidenceChain/SpotRateLimitTracker/InFlightRegistry/KeyRing/DurableAuditSink
// construction) -- the one deliberate difference is this Fixture never calls
// bind_confirmation(), which is the entire point of this test.

#include <gtest/gtest.h>
#include <hengyuan/live_submit_orchestrator.hpp>

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __linux__
#include <unistd.h>
#endif

using namespace hy;

namespace {

// Defensive trap: this must be UNREACHABLE in every test below (confirmation is never bound).
// A test that ever sees this counter go nonzero has a bug elsewhere in its Fixture setup, not a
// real "the orchestrator decided to submit" outcome.
std::atomic<int> g_submit_call_count{0};

SubmitResponse trap_submit_must_never_be_called(const char*, std::uint32_t, OrderSide, OrderType,
                                                 std::int64_t, std::int64_t, const SymbolRules&,
                                                 void*) noexcept {
    g_submit_call_count.fetch_add(1, std::memory_order_relaxed);
    return SubmitResponse{SubmitOutcome::NetworkError, 0, -1};
}

std::uint32_t g_rules_version_for_test{0};
std::uint32_t trap_current_rules_version(void*) noexcept { return g_rules_version_for_test; }

std::string durable_audit_temp_path() {
    static int counter = 0;
    ++counter;
#ifdef _WIN32
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + "hy_preflight_harness_test_" +
           std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter) + ".log";
#else
    return "/tmp/hy_preflight_harness_test_" + std::to_string(getpid()) + "_" +
           std::to_string(counter) + ".log";
#endif
}

void remove_durable_audit_files(const std::string& base_path) {
    std::remove(base_path.c_str());
    std::remove((base_path + ".lock").c_str());
    std::remove((base_path + ".tip").c_str());
    std::remove((base_path + ".tip.tmp").c_str());
}

// Mirrors live_submit_evidence_harness.cpp's own Fixture (see that file's own comment for why
// each real component -- not a further mock -- is used: KillSwitch/DryRunEvidenceChain/
// SpotRateLimitTracker/InFlightRegistry/DurableAuditSink are all cheap, in-process, and the
// whole point of both harnesses is proving the REAL gate-chain wiring, not a re-mocked stand-in
// for it). The one deliberate difference from that Fixture: confirmation is NEVER bound here.
struct Fixture {
    AuditRingSink audit;
    KillSwitch kill_switch;
    DryRunEvidenceChain evidence;
    SpotRateLimitTracker rate_limiter;
    InFlightRegistry in_flight;
    SymbolRules rules{};
    AccountSnapshot account{};
    OrchestratorContext ctx{};

    std::string durable_audit_path;
    std::unique_ptr<KeyRing> key_ring;
    std::unique_ptr<DurableAuditSink> durable_audit_sink;

    explicit Fixture(std::uint32_t rules_version = 7) {
        audit.set_available(true);
        kill_switch.operator_reset();
        rate_limiter.configure(/*weight*/ 6000, 500, /*raw*/ 60000, 5000, /*orders*/ 100, 10);

        durable_audit_path = durable_audit_temp_path();
        remove_durable_audit_files(durable_audit_path);
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x20 + i);
        key_ring = std::make_unique<KeyRing>(kek);
        WrappedKeyRecord rec{};
        key_ring->add_key(1, std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}},
                           rec);
        durable_audit_sink = std::make_unique<DurableAuditSink>(durable_audit_path, *key_ring, 1);
        ctx.durable_audit = make_durable_order_audit_port(*durable_audit_sink);

        std::strncpy(rules.symbol, "BTCUSDT", sizeof(rules.symbol) - 1);
        rules.is_trading = true;
        rules.rules_version = rules_version;
        // price_scale/qty_scale left at 0 would make validate_pre_trade()'s notional rescale
        // (native scale price_scale+qty_scale -> kBalanceScale=8) blow the resulting notional
        // up by 10^8 relative to a real symbol's -- a real SymbolRules always carries a
        // nonzero, symbol-derived scale (this is exactly why the real harness reads it from a
        // freshly fetched SymbolRegistry rather than ever synthesizing one). 2/2 here is an
        // arbitrary but self-consistent choice, sized to match the free_ticks/caps below.
        rules.price_scale = 2;
        rules.qty_scale = 2;
        rules.min_price_ticks = 100;
        rules.max_price_ticks = 10'000'000;
        rules.tick_size_ticks = 100;
        rules.min_qty_ticks = 10;
        rules.max_qty_ticks = 1'000'000;
        rules.step_size_ticks = 10;
        rules.min_notional_ticks = 1000;

        // Must cover the rescaled notional: price_ticks(1000) * qty_ticks(10) at native scale
        // (price_scale+qty_scale=4) = 10000, rescaled to kBalanceScale(8) = 10000 * 10^4 = 1e8.
        std::strncpy(account.assets[0].asset, "USDT", 5);
        account.assets[0].free_ticks = 1'000'000'000;  // 10x headroom over the 1e8 notional
        account.asset_count = 1;
        account.can_trade = true;
        account.timestamp_ms = 900;

        g_rules_version_for_test = rules_version;

        ctx.audit = &audit;
        ctx.kill_switch = &kill_switch;
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
        // Both caps sized with headroom over the 1e8 rescaled notional (see the account setup
        // comment above for the derivation).
        ctx.exposure_limits.single_order_notional_cap = 1'000'000'000;
        ctx.exposure_limits.total_exposure_notional_cap = 1'000'000'000;
        ctx.price_ticks = 1000;
        ctx.qty_ticks = 10;
        ctx.symbol_id = 1;
        ctx.sequence = 1;
        // Live, matching live_submit_preflight_harness.cpp's own ctx.mode -- this Fixture is
        // proving the same gate-chain recipe that harness uses, not a DryRun-mode scenario.
        ctx.mode = ExecutionMode::Live;
        ctx.order_weight = 1;
        // Mock, but reachable-and-consistent (matching the plan's own requirement that this
        // harness proves the chain reaches "just missing confirmation", not "fails earlier for
        // an unrelated reason like a stale rules_version mismatch").
        ctx.submit_port = {trap_submit_must_never_be_called, nullptr, trap_current_rules_version};

        // Pre-seed all 4 evidence paths within THIS SAME long-lived chain instance -- matching
        // 批次 6 计划's own emphasis that Gate 3 needs one persistent chain, not a fresh one per
        // scenario (unlike live_submit_evidence_harness.cpp's own per-scenario Fixture, which
        // deliberately does NOT need that property since it is proving something different).
        for (int i = 0; i < static_cast<int>(kEvidencePathCount); ++i) {
            evidence.record(static_cast<EvidencePath>(i), ctx.now_ms, /*build_hash=*/0xABCDEF01u,
                             /*suite_id=*/1);
        }
    }

    ~Fixture() {
        durable_audit_sink.reset();
        key_ring.reset();
        remove_durable_audit_files(durable_audit_path);
    }
};

}  // namespace

TEST(LiveSubmitPreflightHarness, EvidenceChainIsLiveReadyBeforeOrchestrateSubmitRuns) {
    Fixture fx;
    EXPECT_TRUE(fx.evidence.all_paths_exercised());
    EXPECT_TRUE(fx.evidence.consistent_build());
    EXPECT_TRUE(fx.evidence.live_ready());
}

// The single most important assertion for 批次 6 6b-1: every gate up to and including Gate 10b
// (durable audit Ack) passes, and the chain stops EXACTLY at OperatorNotConfirmed.
TEST(LiveSubmitPreflightHarness, FullyAssembledContextWithoutConfirmationStopsAtOperatorNotConfirmed) {
    Fixture fx;
    ASSERT_FALSE(fx.ctx.confirmation.confirmed);  // never bound -- the whole point of this test

    auto result = orchestrate_submit(fx.ctx);

    EXPECT_EQ(result.gate, OrchestratorGate::OperatorNotConfirmed);
    // Not ConfirmationMismatch either -- that would mean confirmation ended up accidentally
    // bound to something rather than staying at its default {confirmed=false}.
    EXPECT_NE(result.gate, OrchestratorGate::ConfirmationMismatch);
    EXPECT_EQ(g_submit_call_count.load(), 0);  // submit_port.call() must never have run
}

// Negative control: if ANY of the prerequisites this harness is responsible for assembling is
// missing, the chain must stop at THAT gate, strictly before OperatorNotConfirmed is ever
// reached -- proves OperatorNotConfirmed above is not a false positive from a chain that would
// have stopped early regardless of what this harness assembles.
TEST(LiveSubmitPreflightHarness, MissingKillSwitchNormalStopsBeforeConfirmationGate) {
    Fixture fx;
    fx.kill_switch.trigger();
    fx.kill_switch.trigger();  // Armed -> Triggered -> Latched, matches KillState progression
    auto result = orchestrate_submit(fx.ctx);
    EXPECT_NE(result.gate, OrchestratorGate::OperatorNotConfirmed);
    EXPECT_EQ(g_submit_call_count.load(), 0);
}

TEST(LiveSubmitPreflightHarness, MissingDepthSyncStopsBeforeConfirmationGate) {
    Fixture fx;
    fx.ctx.depth_synced = false;
    auto result = orchestrate_submit(fx.ctx);
    EXPECT_EQ(result.gate, OrchestratorGate::DepthNotSynced);
    EXPECT_EQ(g_submit_call_count.load(), 0);
}

TEST(LiveSubmitPreflightHarness, IncompleteEvidenceChainStopsBeforeConfirmationGate) {
    Fixture fx;
    fx.evidence.reset();  // simulates a fresh process that hasn't replayed the 4 paths yet
    auto result = orchestrate_submit(fx.ctx);
    EXPECT_EQ(result.gate, OrchestratorGate::DryRunEvidenceIncomplete);
    EXPECT_EQ(g_submit_call_count.load(), 0);
}

TEST(LiveSubmitPreflightHarness, StaleAccountSnapshotStopsBeforeConfirmationGate) {
    Fixture fx;
    fx.account.timestamp_ms = 0;  // now_ms=1000, freshness_max_age_ms=30000 -- still stale enough
    fx.ctx.now_ms = 1000 + 30001;  // push now_ms past the freshness window instead
    auto result = orchestrate_submit(fx.ctx);
    EXPECT_EQ(result.gate, OrchestratorGate::PreTradeFailed);
    EXPECT_EQ(g_submit_call_count.load(), 0);
}

// Confirming this proceeds all the way to a submit IS possible with the same Fixture (proves
// the Fixture itself is not artificially blocked at OperatorNotConfirmed for some unrelated
// reason) -- binding confirmation correctly reaches the mock submit_port this time.
TEST(LiveSubmitPreflightHarness, BindingConfirmationReachesSubmitPort) {
    Fixture fx;
    fx.ctx.confirmation.confirmed = true;
    fx.ctx.confirmation.symbol_id = fx.ctx.symbol_id;
    fx.ctx.confirmation.side = fx.ctx.side;
    fx.ctx.confirmation.type = fx.ctx.order_type;
    fx.ctx.confirmation.price_ticks = fx.ctx.price_ticks;
    fx.ctx.confirmation.qty_ticks = fx.ctx.qty_ticks;
    fx.ctx.confirmation.max_notional = 1'000'000;
    fx.ctx.confirmation.valid_until_ms = fx.ctx.now_ms + 60'000;

    auto result = orchestrate_submit(fx.ctx);

    EXPECT_NE(result.gate, OrchestratorGate::OperatorNotConfirmed);
    EXPECT_EQ(g_submit_call_count.load(), 1);
}
