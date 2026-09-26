// 批次 6 6b-0b: VerifiedDryRunEvidence -- evidence that has to be earned by really running the four
// scenarios, not set. Two layers:
//   * the integration layer runs run_all() against the REAL orchestrate_submit() (real gate chain, kill switch,
//     in-flight registry, rate limiter, reconcile loop, a real DurableAuditSink scratch log; only the network
//     and the reconcile query are scripted) and checks what ends up in the chain;
//   * the judgement layer feeds doctored DrillObservation values to the pure judge_* functions, so a predicate
//     that silently became "always true" cannot hide behind an orchestrator that happens to behave.
// Negative controls: an exchange mock that answers the wrong terminal state in a drill, and a kill switch that
// is never triggered, must each fail exactly their own drill and leave live_ready() false.

#include <gtest/gtest.h>
#include <hengyuan/verified_dry_run_evidence.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
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

// The real BinanceEnvironment enumerators live in binance_environment.hpp (OpenSSL); the class only stores and
// compares the value, so the test names the two values without dragging that header in.
constexpr auto kTestnet = static_cast<BinanceEnvironment>(0);
constexpr auto kProduction = static_cast<BinanceEnvironment>(1);
constexpr std::uint32_t kBuild = 0xB0B0CAFEu;
constexpr std::uint32_t kOtherBuild = 0x0BADF00Du;
constexpr std::int64_t kNow = 1'700'000'000'000;  // an epoch-ms-like clock: positive, like the harness's

std::string scratch_prefix() {
    static int counter = 0;
    ++counter;
#ifdef _WIN32
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + "hy_verified_drills_" + std::to_string(GetCurrentProcessId()) + "_" +
           std::to_string(counter);
#else
    return "/tmp/hy_verified_drills_" + std::to_string(getpid()) + "_" + std::to_string(counter);
#endif
}

// Every file a DurableAuditSink derives from its path (log, tip anchor, key-rotation and store-identity
// sidecars, each with a lock and a tip temp), for each of the four drills. Listed here independently of the
// header's own list on purpose: a suffix the header forgets must show up as a leak, not be forgiven by sharing
// the same mistake. Returns the names still present, comma-separated ("" = none) so a failure says which.
std::string leftover_scratch_files(const std::string& prefix) {
    std::string found;
    for (const char* drill : {"submit", "reject", "ambiguous", "kill"}) {
        for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".keyrotations", ".keyrotations.lock",
                                   ".keyrotations.tip", ".keyrotations.tip.tmp", ".storeid", ".storeid.lock",
                                   ".storeid.tip", ".storeid.tip.tmp"}) {
            const std::string name = prefix + "_" + drill + ".log" + suffix;
            if (std::filesystem::exists(name)) found += (found.empty() ? "" : ", ") + name;
        }
    }
    return found;
}

// Deleting a file on Windows can lag behind the call: a scanner or indexer that still holds a handle keeps the
// file "delete pending" -- and visible to exists() -- until it lets go. So "nothing left" is polled for a moment
// instead of read once; a file that is still there after that is a real leak.
std::string leftover_scratch_files_eventually(const std::string& prefix) {
    std::string left = leftover_scratch_files(prefix);
    for (int i = 0; i < 100 && !left.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        left = leftover_scratch_files(prefix);
    }
    return left;
}

bool contains(const char* haystack, std::string_view needle) {
    return haystack != nullptr && std::string_view(haystack).find(needle) != std::string_view::npos;
}

}  // namespace

// --- integration: the real orchestrator ------------------------------------------------------------------------

TEST(VerifiedDryRunEvidence, AFreshObjectIsNotLiveReadyAndReadyForNothing) {
    VerifiedDryRunEvidence v;
    EXPECT_FALSE(v.chain().live_ready());
    EXPECT_EQ(v.chain().exercised_count(), 0U);
    EXPECT_FALSE(v.ready_for(kTestnet, kBuild));
    EXPECT_FALSE(v.ready_for(kProduction, kBuild));
    // A never-run object holds the default environment (testnet, 0) and build hash (0): asking for exactly
    // those values must not read as "ready" just because they happen to match the defaults.
    EXPECT_FALSE(v.ready_for(kTestnet, 0));
}

TEST(VerifiedDryRunEvidence, AllFourDrillsReachTheirTerminalStatesAndUnlockTheChain) {
    const std::string prefix = scratch_prefix();
    VerifiedDryRunEvidence v;
    const DryRunReport report = v.run_all(kBuild, kNow, kTestnet, prefix);

    for (const DrillOutcome& d : report.drills) {
        EXPECT_TRUE(d.passed) << "path " << static_cast<int>(d.path) << ": " << d.failure;
        EXPECT_STREQ(d.failure, "");
    }
    ASSERT_TRUE(report.all_passed());

    // What each drill really saw of the real orchestrator.
    const auto& submit = report.drills[static_cast<std::size_t>(EvidencePath::SubmitSuccess)];
    EXPECT_EQ(submit.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(submit.order_state, OrderState::Accepted);
    EXPECT_EQ(submit.port_calls, 1);
    const auto& reject = report.drills[static_cast<std::size_t>(EvidencePath::SubmitReject)];
    EXPECT_EQ(reject.gate, OrchestratorGate::SubmitRejected);
    EXPECT_EQ(reject.order_state, OrderState::Rejected);
    EXPECT_EQ(reject.port_calls, 1);
    const auto& ambiguous = report.drills[static_cast<std::size_t>(EvidencePath::SubmitAmbiguous)];
    EXPECT_EQ(ambiguous.gate, OrchestratorGate::SubmitAmbiguous);
    EXPECT_EQ(ambiguous.order_state, OrderState::Ambiguous);
    EXPECT_EQ(ambiguous.port_calls, 1);
    const auto& kill = report.drills[static_cast<std::size_t>(EvidencePath::KillSwitch)];
    EXPECT_EQ(kill.gate, OrchestratorGate::KillSwitchNotNormal);
    EXPECT_EQ(kill.port_calls, 0);

    // The chain is live-ready, from one build, and every record says who produced it.
    EXPECT_TRUE(v.chain().all_paths_exercised());
    EXPECT_TRUE(v.chain().consistent_build());
    EXPECT_TRUE(v.chain().live_ready());
    for (std::size_t i = 0; i < kEvidencePathCount; ++i) {
        const EvidenceRecord& rec = v.chain().get(static_cast<EvidencePath>(i));
        EXPECT_TRUE(rec.exercised);
        EXPECT_EQ(rec.build_hash, kBuild);
        EXPECT_EQ(rec.test_suite_id, kVerifiedDrillSuiteId);
        EXPECT_EQ(rec.timestamp_ms, kNow);
    }
    EXPECT_TRUE(v.ready_for(kTestnet, kBuild));
}

TEST(VerifiedDryRunEvidence, ReadyForNeedsTheSameEnvironmentAndTheSameBuild) {
    VerifiedDryRunEvidence v;
    ASSERT_TRUE(v.run_all(kBuild, kNow, kTestnet, scratch_prefix()).all_passed());
    EXPECT_TRUE(v.ready_for(kTestnet, kBuild));
    EXPECT_FALSE(v.ready_for(kProduction, kBuild)) << "evidence earned for testnet says nothing about production";
    EXPECT_FALSE(v.ready_for(kTestnet, kOtherBuild)) << "evidence earned by another build says nothing about this one";
    EXPECT_FALSE(v.ready_for(kProduction, kOtherBuild));
}

TEST(VerifiedDryRunEvidence, TheEnvironmentItWasRunForIsTheOneItIsReadyFor) {
    // The other half of the binding: what is recorded comes from the run, it is not the testnet default. (The
    // drills are in-process and touch no network, so the environment is only ever a label they carry.)
    VerifiedDryRunEvidence v;
    ASSERT_TRUE(v.run_all(kBuild, kNow, kProduction, scratch_prefix()).all_passed());
    EXPECT_TRUE(v.ready_for(kProduction, kBuild));
    EXPECT_FALSE(v.ready_for(kTestnet, kBuild));

    // A new run replaces the binding, it does not add to it.
    ASSERT_TRUE(v.run_all(kOtherBuild, kNow, kTestnet, scratch_prefix()).all_passed());
    EXPECT_TRUE(v.ready_for(kTestnet, kOtherBuild));
    EXPECT_FALSE(v.ready_for(kProduction, kOtherBuild));
    EXPECT_FALSE(v.ready_for(kTestnet, kBuild));
}

TEST(VerifiedDryRunEvidence, TheChainIsOnlyEverHandedOutReadOnly) {
    static_assert(std::is_same_v<decltype(std::declval<const VerifiedDryRunEvidence&>().chain()),
                                 const DryRunEvidenceChain&>);
    static_assert(std::is_same_v<decltype(std::declval<VerifiedDryRunEvidence&>().chain()),
                                 const DryRunEvidenceChain&>);
    SUCCEED();
}

TEST(VerifiedDryRunEvidence, TheDrillsLeaveNoScratchFilesBehind) {
    const std::string prefix = scratch_prefix();
    VerifiedDryRunEvidence v;
    ASSERT_TRUE(v.run_all(kBuild, kNow, kTestnet, prefix).all_passed());
    EXPECT_EQ(leftover_scratch_files_eventually(prefix), "");
}

TEST(VerifiedDryRunEvidence, ANonPositiveClockRunsNothingAndUnlocksNothing) {
    for (const std::int64_t bad_now : {std::int64_t{0}, std::int64_t{-5}}) {
        const std::string prefix = scratch_prefix();
        VerifiedDryRunEvidence v;
        const DryRunReport report = v.run_all(kBuild, bad_now, kTestnet, prefix);
        EXPECT_FALSE(report.all_passed());
        for (std::size_t i = 0; i < kEvidencePathCount; ++i) {
            const DrillOutcome& d = report.drills[i];
            EXPECT_EQ(d.path, static_cast<EvidencePath>(i)) << "each refused drill still says which path it was";
            EXPECT_FALSE(d.passed);
            EXPECT_TRUE(contains(d.failure, "now_ms must be positive")) << d.failure;
            EXPECT_EQ(d.port_calls, 0);
        }
        EXPECT_EQ(v.chain().exercised_count(), 0U);
        EXPECT_FALSE(v.ready_for(kTestnet, kBuild));
        EXPECT_EQ(leftover_scratch_files_eventually(prefix), "") << "nothing ran, so nothing was created";
    }
}

TEST(VerifiedDryRunEvidence, ImmediateStartupEarnsEvidenceWithoutWaitingForTheFirstMillisecond) {
    EXPECT_EQ(drill_startup_now_ms(0), 1);
    EXPECT_EQ(drill_startup_now_ms(25), 25);
    VerifiedDryRunEvidence v;
    const DryRunReport report = v.run_all(kBuild, drill_startup_now_ms(0), kTestnet, scratch_prefix());
    for (const DrillOutcome& drill : report.drills) {
        EXPECT_TRUE(drill.passed) << drill.failure;
    }
    EXPECT_TRUE(v.ready_for(kTestnet, kBuild));
    EXPECT_EQ(v.chain().get(EvidencePath::SubmitSuccess).timestamp_ms, 1);
}

// --- integration: negative controls -------------------------------------------------------------------------------

namespace {

struct Fault {
    const char* name;
    std::function<void(DrillMocks&)> apply;
    EvidencePath failing_path;
    const char* expected_reason;  // a substring of the drill's failure
};

std::vector<Fault> faults() {
    return {
        {"the exchange rejects in the success drill",
         [](DrillMocks& m) { m.success_answer = SubmitOutcome::Rejected; }, EvidencePath::SubmitSuccess,
         "did not end at SubmitAccepted"},
        {"the exchange accepts in the reject drill",
         [](DrillMocks& m) { m.reject_answer = SubmitOutcome::Accepted; }, EvidencePath::SubmitReject,
         "did not end at SubmitRejected"},
        {"the exchange accepts in the ambiguity drill",
         [](DrillMocks& m) { m.ambiguous_answer = SubmitOutcome::Accepted; }, EvidencePath::SubmitAmbiguous,
         "did not end at SubmitAmbiguous"},
        {"the reconcile query stays inconclusive (the slot is never released)",
         [](DrillMocks& m) { m.reconcile_outcome = QueryOutcome::Inconclusive; }, EvidencePath::SubmitAmbiguous,
         "did not release the ambiguous order's slot"},
        {"the reconcile query reports the order is still ambiguous",
         [](DrillMocks& m) { m.reconcile_state = OrderState::Ambiguous; }, EvidencePath::SubmitAmbiguous,
         "did not release the ambiguous order's slot"},
        {"the kill switch is never triggered",
         [](DrillMocks& m) { m.trigger_kill_switch = false; }, EvidencePath::KillSwitch,
         "did not stop the submit"},
    };
}

}  // namespace

TEST(VerifiedDryRunEvidence, AWrongTerminalStateFailsExactlyItsOwnDrillAndNeverUnlocksTheChain) {
    for (const Fault& fault : faults()) {
        SCOPED_TRACE(fault.name);
        DrillMocks mocks;
        fault.apply(mocks);
        const std::string prefix = scratch_prefix();
        VerifiedDryRunEvidence v;
        const DryRunReport report = v.run_all(kBuild, kNow, kTestnet, prefix, mocks);

        EXPECT_FALSE(report.all_passed());
        for (std::size_t i = 0; i < kEvidencePathCount; ++i) {
            const auto path = static_cast<EvidencePath>(i);
            const DrillOutcome& d = report.drills[i];
            if (path == fault.failing_path) {
                EXPECT_FALSE(d.passed);
                EXPECT_TRUE(contains(d.failure, fault.expected_reason)) << d.failure;
                EXPECT_FALSE(v.chain().is_path_exercised(path)) << "a failed drill must leave no evidence";
            } else {
                EXPECT_TRUE(d.passed) << "path " << i << ": " << d.failure;
                EXPECT_TRUE(v.chain().is_path_exercised(path));
            }
        }
        EXPECT_EQ(v.chain().exercised_count(), static_cast<std::uint32_t>(kEvidencePathCount - 1));
        EXPECT_FALSE(v.chain().live_ready()) << "any single missing path keeps live locked";
        EXPECT_FALSE(v.ready_for(kTestnet, kBuild));
        EXPECT_EQ(leftover_scratch_files_eventually(prefix), "") << "a failing drill cleans up too";
    }
}

TEST(VerifiedDryRunEvidence, ALaterFailingRunDoesNotInheritAnEarlierRunsEvidence) {
    VerifiedDryRunEvidence v;
    ASSERT_TRUE(v.run_all(kBuild, kNow, kTestnet, scratch_prefix()).all_passed());
    ASSERT_TRUE(v.ready_for(kTestnet, kBuild));

    DrillMocks broken;
    broken.reject_answer = SubmitOutcome::Accepted;
    const DryRunReport second = v.run_all(kBuild, kNow + 1000, kTestnet, scratch_prefix(), broken);
    EXPECT_FALSE(second.all_passed());
    EXPECT_FALSE(v.chain().is_path_exercised(EvidencePath::SubmitReject))
        << "the reject path was proven by the FIRST run only: it must not survive";
    EXPECT_FALSE(v.chain().live_ready());
    EXPECT_FALSE(v.ready_for(kTestnet, kBuild));
    EXPECT_EQ(v.chain().get(EvidencePath::SubmitSuccess).timestamp_ms, kNow + 1000) << "the other paths are re-earned";
}

TEST(VerifiedDryRunEvidence, ARunAfterAFailedRunStartsClean) {
    VerifiedDryRunEvidence v;
    DrillMocks broken;
    broken.trigger_kill_switch = false;
    ASSERT_FALSE(v.run_all(kBuild, kNow, kTestnet, scratch_prefix(), broken).all_passed());
    ASSERT_FALSE(v.ready_for(kTestnet, kBuild));

    ASSERT_TRUE(v.run_all(kBuild, kNow, kTestnet, scratch_prefix()).all_passed());
    EXPECT_TRUE(v.ready_for(kTestnet, kBuild));
}

TEST(VerifiedDryRunEvidence, AStaleScratchLogFromACrashedRunIsRemovedNeverReplayed) {
    // A drill's audit log is scratch: whatever a crashed earlier run left at the drill's path must not survive
    // into (or break) the next run -- it can never be a source of "already in flight" orders.
    const std::string prefix = scratch_prefix();
    std::vector<std::string> stale;
    for (const char* drill : {"submit", "reject", "ambiguous", "kill"}) {
        stale.push_back(prefix + "_" + drill + ".log");
        std::ofstream out(stale.back(), std::ios::binary);
        out << "this is not a durable log frame";
    }
    for (const std::string& f : stale) ASSERT_TRUE(std::filesystem::exists(f));

    VerifiedDryRunEvidence v;
    EXPECT_TRUE(v.run_all(kBuild, kNow, kTestnet, prefix).all_passed());
    for (const std::string& f : stale) EXPECT_FALSE(std::filesystem::exists(f)) << f;
}

TEST(VerifiedDryRunEvidence, AnUnusableScratchLocationUnlocksNothing) {
    // The durable audit log is not optional in the orchestrator (an unacked append stops the submit), so a
    // location the log cannot be created in must fail the submit drills, and with them the whole chain.
    const std::string prefix = scratch_prefix() + "_no_such_dir/drill";
    VerifiedDryRunEvidence v;
    const DryRunReport report = v.run_all(kBuild, kNow, kTestnet, prefix);
    EXPECT_FALSE(report.all_passed());
    for (const EvidencePath p : {EvidencePath::SubmitSuccess, EvidencePath::SubmitReject, EvidencePath::SubmitAmbiguous}) {
        const DrillOutcome& d = report.drills[static_cast<std::size_t>(p)];
        EXPECT_FALSE(d.passed) << "path " << static_cast<int>(p) << " passed without a usable durable log";
        EXPECT_FALSE(v.chain().is_path_exercised(p));
    }
    EXPECT_FALSE(v.chain().live_ready());
    EXPECT_FALSE(v.ready_for(kTestnet, kBuild));
}

// --- judgement layer: the predicates against doctored observations ---------------------------------------------------

namespace {

using detail::DrillObservation;

DrillObservation ok_success() {
    DrillObservation o;
    o.control_ok = true;
    o.gate = OrchestratorGate::SubmitAccepted;
    o.order_state = OrderState::Accepted;
    o.exchange_order_id = kDrillExchangeOrderId;
    o.port_calls = 1;
    o.port_saw_validated_order = true;
    o.in_flight_count = 1;
    o.order_in_flight = true;
    o.queued_for_reconcile = 1;
    o.audit_trail_ok = true;
    return o;
}

DrillObservation ok_reject() {
    DrillObservation o;
    o.control_ok = true;
    o.gate = OrchestratorGate::SubmitRejected;
    o.order_state = OrderState::Rejected;
    o.port_calls = 1;
    o.port_saw_validated_order = true;
    o.in_flight_count = 0;
    o.queued_for_reconcile = 0;
    o.audit_trail_ok = true;
    return o;
}

DrillObservation ok_ambiguous() {
    DrillObservation o;
    o.control_ok = true;
    o.gate = OrchestratorGate::SubmitAmbiguous;
    o.order_state = OrderState::Ambiguous;
    o.port_calls = 1;
    o.port_saw_validated_order = true;
    o.in_flight_count = 1;
    o.order_in_flight = true;
    o.queued_for_reconcile = 1;
    o.query_calls = 1;
    o.query_carries_order = true;
    o.in_flight_after_reconcile = 0;
    o.tracker_after_reconcile = 0;
    o.audit_trail_ok = true;
    return o;
}

DrillObservation ok_kill() {
    DrillObservation o;
    o.control_ok = true;
    o.gate = OrchestratorGate::KillSwitchNotNormal;
    o.port_calls = 0;
    o.in_flight_count = 0;
    o.audit_trail_ok = true;
    o.kill_switch_latched = true;
    o.gate_after_latch = OrchestratorGate::KillSwitchNotNormal;
    o.port_calls_after_latch = 0;
    o.kill_switch_normal = false;
    return o;
}

struct Doctored {
    const char* name;
    std::function<void(DrillObservation&)> apply;
    const char* expected_reason;
};

void expect_each_fault_is_named(const DrillObservation& baseline,
                                const char* (*judge)(const DrillObservation&) noexcept,
                                const std::vector<Doctored>& doctored) {
    ASSERT_EQ(judge(baseline), nullptr) << "the baseline observation must be judged a pass";
    for (const Doctored& d : doctored) {
        SCOPED_TRACE(d.name);
        DrillObservation o = baseline;
        d.apply(o);
        const char* reason = judge(o);
        ASSERT_NE(reason, nullptr) << "a doctored observation was judged a pass";
        EXPECT_TRUE(contains(reason, d.expected_reason)) << reason;
    }
}

}  // namespace

TEST(VerifiedDrillJudgement, SubmitSuccessNamesEveryWayItCanFail) {
    expect_each_fault_is_named(
        ok_success(), &detail::judge_submit_success,
        {
            {"control", [](DrillObservation& o) { o.control_ok = false; }, "does not consult the dry-run evidence gate"},
            {"gate", [](DrillObservation& o) { o.gate = OrchestratorGate::SubmitRejected; }, "did not end at SubmitAccepted"},
            {"state", [](DrillObservation& o) { o.order_state = OrderState::Ambiguous; }, "did not leave the order Accepted"},
            {"exchange id", [](DrillObservation& o) { o.exchange_order_id = 0; }, "exchange order id was not recorded"},
            {"no call", [](DrillObservation& o) { o.port_calls = 0; }, "exactly once"},
            {"two calls", [](DrillObservation& o) { o.port_calls = 2; }, "exactly once"},
            {"wire order", [](DrillObservation& o) { o.port_saw_validated_order = false; }, "handed the validated order"},
            {"slot released", [](DrillObservation& o) { o.in_flight_count = 0; o.order_in_flight = false; }, "keep its in-flight slot"},
            {"slot count", [](DrillObservation& o) { o.in_flight_count = 2; }, "keep its in-flight slot"},
            {"wrong slot", [](DrillObservation& o) { o.order_in_flight = false; }, "keep its in-flight slot"},
            {"not tracked", [](DrillObservation& o) { o.queued_for_reconcile = 0; }, "not handed to reconciliation"},
            {"tracked twice", [](DrillObservation& o) { o.queued_for_reconcile = 2; }, "not handed to reconciliation"},
            {"audit", [](DrillObservation& o) { o.audit_trail_ok = false; }, "intent -> submitted -> accepted"},
        });
}

TEST(VerifiedDrillJudgement, SubmitRejectNamesEveryWayItCanFail) {
    expect_each_fault_is_named(
        ok_reject(), &detail::judge_submit_reject,
        {
            {"control", [](DrillObservation& o) { o.control_ok = false; }, "does not consult the dry-run evidence gate"},
            {"gate", [](DrillObservation& o) { o.gate = OrchestratorGate::SubmitAccepted; }, "did not end at SubmitRejected"},
            {"state", [](DrillObservation& o) { o.order_state = OrderState::Accepted; }, "did not leave the order Rejected"},
            {"no call", [](DrillObservation& o) { o.port_calls = 0; }, "exactly once"},
            {"two calls", [](DrillObservation& o) { o.port_calls = 2; }, "exactly once"},
            {"wire order", [](DrillObservation& o) { o.port_saw_validated_order = false; }, "handed the validated order"},
            {"slot kept", [](DrillObservation& o) { o.in_flight_count = 1; }, "must release its in-flight slot"},
            {"queued", [](DrillObservation& o) { o.queued_for_reconcile = 1; }, "must not be queued for reconciliation"},
            {"audit", [](DrillObservation& o) { o.audit_trail_ok = false; }, "intent -> submitted -> rejected"},
        });
}

TEST(VerifiedDrillJudgement, SubmitAmbiguousNamesEveryWayItCanFail) {
    expect_each_fault_is_named(
        ok_ambiguous(), &detail::judge_submit_ambiguous,
        {
            {"control", [](DrillObservation& o) { o.control_ok = false; }, "does not consult the dry-run evidence gate"},
            {"gate", [](DrillObservation& o) { o.gate = OrchestratorGate::SubmitAccepted; }, "did not end at SubmitAmbiguous"},
            {"state", [](DrillObservation& o) { o.order_state = OrderState::Accepted; }, "did not leave the order Ambiguous"},
            {"no call", [](DrillObservation& o) { o.port_calls = 0; }, "exactly once"},
            {"two calls", [](DrillObservation& o) { o.port_calls = 2; }, "exactly once"},
            {"wire order", [](DrillObservation& o) { o.port_saw_validated_order = false; }, "handed the validated order"},
            {"slot released early", [](DrillObservation& o) { o.in_flight_count = 0; o.order_in_flight = false; }, "keep its in-flight slot"},
            {"wrong slot", [](DrillObservation& o) { o.order_in_flight = false; }, "keep its in-flight slot"},
            {"not queued", [](DrillObservation& o) { o.queued_for_reconcile = 0; }, "not queued for reconciliation"},
            {"queued twice", [](DrillObservation& o) { o.queued_for_reconcile = 2; }, "not queued for reconciliation"},
            {"no query", [](DrillObservation& o) { o.query_calls = 0; }, "query the ambiguous order exactly once"},
            {"two queries", [](DrillObservation& o) { o.query_calls = 2; }, "query the ambiguous order exactly once"},
            {"wrong query", [](DrillObservation& o) { o.query_carries_order = false; }, "does not carry the order that was submitted"},
            {"slot never released", [](DrillObservation& o) { o.in_flight_after_reconcile = 1; }, "did not release the ambiguous order's slot"},
            {"still tracked", [](DrillObservation& o) { o.tracker_after_reconcile = 1; }, "still tracked for reconciliation"},
            {"audit", [](DrillObservation& o) { o.audit_trail_ok = false; }, "ambiguous -> reconciled"},
        });
}

TEST(VerifiedDrillJudgement, KillSwitchNamesEveryWayItCanFail) {
    expect_each_fault_is_named(
        ok_kill(), &detail::judge_kill_switch,
        {
            {"control", [](DrillObservation& o) { o.control_ok = false; }, "does not consult the dry-run evidence gate"},
            {"gate", [](DrillObservation& o) { o.gate = OrchestratorGate::SubmitAccepted; }, "did not stop the submit"},
            {"port called", [](DrillObservation& o) { o.port_calls = 1; }, "port was called with the kill switch triggered"},
            {"slot taken", [](DrillObservation& o) { o.in_flight_count = 1; }, "in-flight slot was taken"},
            {"refusal not audited", [](DrillObservation& o) { o.audit_trail_ok = false; }, "refusal was not audited"},
            {"never latched", [](DrillObservation& o) { o.kill_switch_latched = false; }, "did not reach the Latched lockout"},
            {"latched lets it through", [](DrillObservation& o) { o.gate_after_latch = OrchestratorGate::SubmitAccepted; }, "latched kill switch did not keep blocking"},
            {"latched calls the port", [](DrillObservation& o) { o.port_calls_after_latch = 1; }, "latched kill switch did not keep blocking"},
            {"back to normal", [](DrillObservation& o) { o.kill_switch_normal = true; }, "returned to Normal by itself"},
        });
}

TEST(VerifiedDrillJudgement, TheFirstFailedCheckIsTheOneReported) {
    // Earlier checks are the more fundamental: with several faults at once the reason names the first.
    DrillObservation o = ok_success();
    o.control_ok = false;
    o.gate = OrchestratorGate::SubmitRejected;
    EXPECT_TRUE(contains(detail::judge_submit_success(o), "does not consult the dry-run evidence gate"));
    o.control_ok = true;
    EXPECT_TRUE(contains(detail::judge_submit_success(o), "did not end at SubmitAccepted"));
}

// --- the observation helpers ---------------------------------------------------------------------------------------

TEST(VerifiedDrillHelpers, AuditSubsequenceCheckIsOrderedAndTolerantOfInterleavedRecords) {
    AuditRingSink audit;
    audit.set_available(true);
    for (AuditEventType t : {AuditEventType::OrderIntentCreated, AuditEventType::PreflightFailed,
                             AuditEventType::OrderSubmitted, AuditEventType::OrderAccepted}) {
        AuditRecord r{};
        r.event_type = t;
        ASSERT_TRUE(audit.append(r));
    }
    using E = AuditEventType;
    EXPECT_TRUE(detail::audit_has_in_order(audit, {E::OrderIntentCreated, E::OrderSubmitted, E::OrderAccepted}));
    EXPECT_TRUE(detail::audit_has_in_order(audit, {E::PreflightFailed}));
    EXPECT_TRUE(detail::audit_has_in_order(audit, {}));
    EXPECT_FALSE(detail::audit_has_in_order(audit, {E::OrderAccepted, E::OrderSubmitted})) << "wrong order";
    EXPECT_FALSE(detail::audit_has_in_order(audit, {E::OrderIntentCreated, E::OrderRejected})) << "missing event";
    EXPECT_FALSE(detail::audit_has_in_order(audit, {E::OrderAccepted, E::OrderAccepted})) << "one event cannot match twice";

    // `from` skips the records before it: the PreflightFailed at index 1 no longer counts from index 2 on, and
    // the whole ring is scanned from 0. A `from` past the end matches nothing but the empty subsequence.
    EXPECT_FALSE(detail::audit_has_in_order(audit, {E::PreflightFailed}, 2)) << "the only PreflightFailed is at index 1";
    EXPECT_TRUE(detail::audit_has_in_order(audit, {E::PreflightFailed}, 1));
    EXPECT_TRUE(detail::audit_has_in_order(audit, {E::OrderSubmitted, E::OrderAccepted}, 2));
    EXPECT_FALSE(detail::audit_has_in_order(audit, {E::OrderIntentCreated}, 1)) << "the intent is at index 0";
    EXPECT_FALSE(detail::audit_has_in_order(audit, {E::OrderAccepted}, 99));
    EXPECT_TRUE(detail::audit_has_in_order(audit, {}, 99));
}

TEST(VerifiedDrillHelpers, TheGate3ControlPassesForARigThatConsultsItAndFailsForOneThatDoesNot) {
    const auto rig = std::make_unique<detail::DrillRig>(kNow, scratch_prefix() + "_helper.log");
    EXPECT_TRUE(detail::scaffold_is_consulted(*rig));
    EXPECT_TRUE(rig->scaffold.live_ready()) << "the control restores the scaffold it emptied";

    // A rig whose context is wired to a chain that is always ready (not the scaffold): emptying the scaffold
    // changes nothing, the orchestrator sails past Gate 3, and the control must notice.
    DryRunEvidenceChain always_ready;
    for (std::size_t i = 0; i < kEvidencePathCount; ++i) {
        always_ready.record(static_cast<EvidencePath>(i), kNow, kBuild, 1);
    }
    rig->ctx.evidence = &always_ready;
    EXPECT_FALSE(detail::scaffold_is_consulted(*rig));
}

TEST(VerifiedDrillHelpers, TheGate3ControlOnlyCountsAStopAtGate3WithThePortUntouched) {
    // Stopping at a DIFFERENT gate proves nothing about Gate 3 (the kill switch outranks it).
    {
        const auto rig = std::make_unique<detail::DrillRig>(kNow, scratch_prefix() + "_helper.log");
        rig->kill_switch.trigger();
        EXPECT_FALSE(detail::scaffold_is_consulted(*rig));
        EXPECT_TRUE(rig->scaffold.live_ready()) << "the scaffold is restored on every path";
    }
    // A port that was already called before the control ran is not the clean "never reached the port" case.
    {
        const auto rig = std::make_unique<detail::DrillRig>(kNow, scratch_prefix() + "_helper.log");
        rig->port.calls = 1;
        EXPECT_FALSE(detail::scaffold_is_consulted(*rig));
    }
}

TEST(VerifiedDrillHelpers, AConfiguredOrderBudgetExhaustsAndStopsTheRealOrchestrator) {
    const auto rig = std::make_unique<detail::DrillRig>(kNow, scratch_prefix() + "_budget.log");
    // The rig configures 100 orders with a margin of 10, split 80/10/10: Strategy has 72 slots.
    // A default-constructed tracker is much more permissive and must not pass for a configured drill.
    const auto now = SpotRateLimitTracker::Clock::now();
    for (int i = 0; i < 72; ++i) {
        ASSERT_TRUE(rig->rate_limiter.try_reserve_order(RateLimitLane::Strategy, 1, now)) << i;
    }
    EXPECT_FALSE(rig->rate_limiter.can_send_order(RateLimitLane::Strategy, 1, now));
    EXPECT_FALSE(rig->rate_limiter.try_reserve_order(RateLimitLane::Strategy, 1, now));

    rig->bind_confirmation();
    const OrchestratorResult result = orchestrate_submit(rig->ctx);
    EXPECT_EQ(result.gate, OrchestratorGate::RateLimitExhausted);
    EXPECT_EQ(rig->port.calls, 0);
    EXPECT_EQ(rig->in_flight.count(), 0U);
}

TEST(VerifiedDrillHelpers, TheControlMovesTheAuditMarkPastItsOwnRecords) {
    const auto rig = std::make_unique<detail::DrillRig>(kNow, scratch_prefix() + "_helper.log");
    ASSERT_EQ(rig->audit_mark, 0U);
    ASSERT_TRUE(detail::scaffold_is_consulted(*rig));
    EXPECT_GT(rig->audit.count(), 0U) << "Gate 3 audits its refusal";
    EXPECT_EQ(rig->audit_mark, rig->audit.count()) << "everything written so far belongs to the control";
    EXPECT_TRUE(detail::audit_has_in_order(rig->audit, {AuditEventType::PreflightFailed}));
    EXPECT_FALSE(detail::audit_has_in_order(rig->audit, {AuditEventType::PreflightFailed}, rig->audit_mark))
        << "the control's own PreflightFailed must not satisfy a check on what comes after it";
}

TEST(VerifiedDrillHelpers, ThePortCheckComparesEveryFieldOfTheValidatedOrder) {
    const auto rig = std::make_unique<detail::DrillRig>(kNow, scratch_prefix() + "_helper.log");
    auto matching = [&] {
        detail::DrillPortState p;
        p.coid_present = true;
        p.symbol_id = rig->ctx.symbol_id;
        p.side = rig->ctx.side;
        p.type = rig->ctx.order_type;
        p.price_ticks = rig->ctx.price_ticks;
        p.qty_ticks = rig->ctx.qty_ticks;
        p.rules_version = rig->rules.rules_version;
        return p;
    };
    rig->port = matching();
    EXPECT_TRUE(detail::port_saw_the_validated_order(*rig));

    { rig->port = matching(); rig->port.coid_present = false; EXPECT_FALSE(detail::port_saw_the_validated_order(*rig)); }
    { rig->port = matching(); rig->port.symbol_id += 1; EXPECT_FALSE(detail::port_saw_the_validated_order(*rig)); }
    { rig->port = matching(); rig->port.side = OrderSide::Sell; EXPECT_FALSE(detail::port_saw_the_validated_order(*rig)); }
    { rig->port = matching(); rig->port.type = static_cast<OrderType>(1); EXPECT_FALSE(detail::port_saw_the_validated_order(*rig)); }
    { rig->port = matching(); rig->port.price_ticks += 1; EXPECT_FALSE(detail::port_saw_the_validated_order(*rig)); }
    { rig->port = matching(); rig->port.qty_ticks += 1; EXPECT_FALSE(detail::port_saw_the_validated_order(*rig)); }
    { rig->port = matching(); rig->port.rules_version += 1; EXPECT_FALSE(detail::port_saw_the_validated_order(*rig)); }
}

TEST(VerifiedDrillHelpers, TheQueryCheckComparesEveryFieldTheReconcileQueryMustCarry) {
    const auto rig = std::make_unique<detail::DrillRig>(kNow, scratch_prefix() + "_helper.log");
    OrderRecord order{};
    order.client_order_id = make_client_order_id(kNow, 1, rig->ctx.symbol_id);
    order.symbol_id = rig->ctx.symbol_id;
    order.side = rig->ctx.side;
    order.order_type = rig->ctx.order_type;
    order.intended_price_ticks = rig->ctx.price_ticks;
    order.intended_qty_ticks = rig->ctx.qty_ticks;
    order.rules_snapshot_at_submit = rig->rules;

    const OrderExpectation good = OrderExpectation::from(order);
    EXPECT_TRUE(detail::query_carries_the_order(good, order, *rig));

    { OrderExpectation e = good; e.client_order_id = make_client_order_id(kNow, 2, rig->ctx.symbol_id); EXPECT_FALSE(detail::query_carries_the_order(e, order, *rig)); }
    { OrderExpectation e = good; e.symbol_id += 1; EXPECT_FALSE(detail::query_carries_the_order(e, order, *rig)); }
    { OrderExpectation e = good; e.side = OrderSide::Sell; EXPECT_FALSE(detail::query_carries_the_order(e, order, *rig)); }
    { OrderExpectation e = good; e.order_type = static_cast<OrderType>(1); EXPECT_FALSE(detail::query_carries_the_order(e, order, *rig)); }
    { OrderExpectation e = good; e.intended_price_ticks += 1; EXPECT_FALSE(detail::query_carries_the_order(e, order, *rig)); }
    { OrderExpectation e = good; e.intended_qty_ticks += 1; EXPECT_FALSE(detail::query_carries_the_order(e, order, *rig)); }
    { OrderExpectation e = good; e.rules_snapshot_at_submit.rules_version += 1; EXPECT_FALSE(detail::query_carries_the_order(e, order, *rig)); }
}

TEST(VerifiedDrillHelpers, TheScriptedPortRecordsTheCallAndAnswersAsScripted) {
    SymbolRules rules{};
    rules.rules_version = 9;
    detail::DrillPortState s;

    s.answer = SubmitOutcome::Accepted;
    SubmitResponse r =
        detail::drill_submit("COID-1", 5, OrderSide::Sell, static_cast<OrderType>(1), 123, 456, rules, &s);
    EXPECT_EQ(s.calls, 1);
    EXPECT_TRUE(s.coid_present);
    EXPECT_EQ(s.symbol_id, 5U);
    EXPECT_EQ(s.side, OrderSide::Sell);
    EXPECT_EQ(s.type, static_cast<OrderType>(1));
    EXPECT_EQ(s.price_ticks, 123);
    EXPECT_EQ(s.qty_ticks, 456);
    EXPECT_EQ(s.rules_version, 9U);
    EXPECT_EQ(r.outcome, SubmitOutcome::Accepted);
    EXPECT_EQ(r.exchange_order_id, kDrillExchangeOrderId);
    EXPECT_EQ(r.error_code, 0);

    // A missing or empty client order id is recorded as absent, and every call is counted.
    detail::drill_submit(nullptr, 5, OrderSide::Buy, OrderType::Limit, 1, 1, rules, &s);
    EXPECT_FALSE(s.coid_present);
    detail::drill_submit("", 5, OrderSide::Buy, OrderType::Limit, 1, 1, rules, &s);
    EXPECT_FALSE(s.coid_present);
    EXPECT_EQ(s.calls, 3);

    s.answer = SubmitOutcome::Rejected;
    r = detail::drill_submit("COID-1", 5, OrderSide::Buy, OrderType::Limit, 1, 1, rules, &s);
    EXPECT_EQ(r.outcome, SubmitOutcome::Rejected);
    EXPECT_EQ(r.exchange_order_id, 0);
    EXPECT_EQ(r.error_code, kDrillRejectCode);

    s.answer = SubmitOutcome::Timeout;
    r = detail::drill_submit("COID-1", 5, OrderSide::Buy, OrderType::Limit, 1, 1, rules, &s);
    EXPECT_EQ(r.outcome, SubmitOutcome::Timeout);
    EXPECT_EQ(r.exchange_order_id, 0);
    EXPECT_EQ(r.error_code, 0);

    s.answer = SubmitOutcome::NetworkError;
    r = detail::drill_submit("COID-1", 5, OrderSide::Buy, OrderType::Limit, 1, 1, rules, &s);
    EXPECT_EQ(r.outcome, SubmitOutcome::NetworkError);
    EXPECT_EQ(r.error_code, 0);

    // A real port can never return StaleRulesVersion post-send (the orchestrator catches it before the call):
    // the script maps it to a plain network error rather than pretending to model it.
    s.answer = SubmitOutcome::StaleRulesVersion;
    r = detail::drill_submit("COID-1", 5, OrderSide::Buy, OrderType::Limit, 1, 1, rules, &s);
    EXPECT_EQ(r.outcome, SubmitOutcome::NetworkError);
    EXPECT_EQ(r.error_code, -1);

    EXPECT_EQ(detail::drill_rules_version(nullptr), kDrillRulesVersion);
}

TEST(VerifiedDrillHelpers, TheScriptedQueryRecordsWhatItWasAskedAndAnswersAsScripted) {
    detail::DrillQueryState s;
    s.outcome = QueryOutcome::Found;
    s.state = OrderState::Cancelled;
    OrderExpectation asked{};
    asked.symbol_id = 7;
    asked.intended_qty_ticks = 99;

    QueryResult r = detail::drill_query(asked, &s);
    EXPECT_EQ(s.calls, 1);
    EXPECT_EQ(s.last.symbol_id, 7U);
    EXPECT_EQ(s.last.intended_qty_ticks, 99);
    EXPECT_EQ(r.outcome, QueryOutcome::Found);
    EXPECT_EQ(r.confirmed_state, OrderState::Cancelled);

    s.outcome = QueryOutcome::Inconclusive;
    r = detail::drill_query(asked, &s);
    EXPECT_EQ(s.calls, 2);
    EXPECT_EQ(r.outcome, QueryOutcome::Inconclusive);
}

// --- the observations: what the real orchestrator did, as the judges will see it ----------------------------------------
//
// The judges are tested above against doctored values and the integration tests run the whole thing, but neither
// shows that each observation field is really read from the right place: a field wired to a constant that happens
// to equal what the healthy orchestrator produces would pass both. These run each observer against the real
// orchestrator with a scripted exchange and check every field, healthy and faulted.

namespace {

std::unique_ptr<detail::DrillRig> make_rig() {
    return std::make_unique<detail::DrillRig>(kNow, scratch_prefix() + "_obs.log");
}

// The wire is handed a different price than the one the orchestrator validated.
SubmitResponse mangling_submit(const char* coid, std::uint32_t symbol_id, OrderSide side, OrderType type,
                               std::int64_t price_ticks, std::int64_t qty_ticks, const SymbolRules& rules,
                               void* user_data) noexcept {
    return detail::drill_submit(coid, symbol_id, side, type, price_ticks + 1, qty_ticks, rules, user_data);
}

// The reconcile query is asked about a different quantity than the one that was submitted.
QueryResult mangling_query(const OrderExpectation& expected, void* user_data) {
    OrderExpectation wrong = expected;
    wrong.intended_qty_ticks += 1;
    return detail::drill_query(wrong, user_data);
}

}  // namespace

TEST(VerifiedDrillObservations, AnAcceptedSubmitIsSeenAsAcceptedWithItsSlotHeld) {
    const auto rig = make_rig();
    const DrillObservation o = detail::observe_submit_success(*rig, DrillMocks{});
    EXPECT_TRUE(o.control_ok);
    EXPECT_EQ(o.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(o.order_state, OrderState::Accepted);
    EXPECT_EQ(o.exchange_order_id, kDrillExchangeOrderId);
    EXPECT_EQ(o.port_calls, 1);
    EXPECT_TRUE(o.port_saw_validated_order);
    EXPECT_EQ(o.in_flight_count, 1U);
    EXPECT_TRUE(o.order_in_flight);
    EXPECT_EQ(o.queued_for_reconcile, 1U) << "a live order is handed to the reconciler (EXEC-INFLIGHT-003)";
    EXPECT_TRUE(o.audit_trail_ok);
    EXPECT_EQ(detail::judge_submit_success(o), nullptr);

    // The drills are dry runs, and everything they audit says so.
    ASSERT_GT(rig->audit.count(), rig->audit_mark);
    for (std::size_t i = 0; i < rig->audit.count(); ++i) {
        ASSERT_NE(rig->audit.at(i), nullptr);
        EXPECT_EQ(rig->audit.at(i)->mode, ExecutionMode::DryRun) << "audit record " << i;
    }
}

TEST(VerifiedDrillObservations, ARejectingExchangeInTheSuccessDrillIsSeenAsRejectedWithNoSlotAndNoAcceptedRecord) {
    const auto rig = make_rig();
    DrillMocks mocks;
    mocks.success_answer = SubmitOutcome::Rejected;
    const DrillObservation o = detail::observe_submit_success(*rig, mocks);
    EXPECT_TRUE(o.control_ok);
    EXPECT_EQ(o.gate, OrchestratorGate::SubmitRejected);
    EXPECT_EQ(o.order_state, OrderState::Rejected);
    EXPECT_EQ(o.exchange_order_id, 0);
    EXPECT_EQ(o.port_calls, 1);
    EXPECT_EQ(o.in_flight_count, 0U);
    EXPECT_FALSE(o.order_in_flight);
    EXPECT_EQ(o.queued_for_reconcile, 0U) << "a terminal order is not queued";
    EXPECT_FALSE(o.audit_trail_ok) << "there is no OrderAccepted record";
    EXPECT_NE(detail::judge_submit_success(o), nullptr);
}

TEST(VerifiedDrillObservations, ARejectedSubmitIsSeenAsRejectedWithItsSlotReleasedAndNothingQueued) {
    const auto rig = make_rig();
    const DrillObservation o = detail::observe_submit_reject(*rig, DrillMocks{});
    EXPECT_TRUE(o.control_ok);
    EXPECT_EQ(o.gate, OrchestratorGate::SubmitRejected);
    EXPECT_EQ(o.order_state, OrderState::Rejected);
    EXPECT_EQ(o.port_calls, 1);
    EXPECT_TRUE(o.port_saw_validated_order);
    EXPECT_EQ(o.in_flight_count, 0U);
    EXPECT_FALSE(o.order_in_flight);
    EXPECT_EQ(o.queued_for_reconcile, 0U);
    EXPECT_TRUE(o.audit_trail_ok);
    EXPECT_EQ(detail::judge_submit_reject(o), nullptr);
}

TEST(VerifiedDrillObservations, AnAcceptingExchangeInTheRejectDrillIsSeenAsAcceptedWithItsSlotHeld) {
    const auto rig = make_rig();
    DrillMocks mocks;
    mocks.reject_answer = SubmitOutcome::Accepted;
    const DrillObservation o = detail::observe_submit_reject(*rig, mocks);
    EXPECT_EQ(o.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(o.order_state, OrderState::Accepted);
    EXPECT_EQ(o.in_flight_count, 1U);
    EXPECT_TRUE(o.order_in_flight);
    EXPECT_EQ(o.queued_for_reconcile, 1U);
    EXPECT_FALSE(o.audit_trail_ok) << "there is no OrderRejected record";
    EXPECT_NE(detail::judge_submit_reject(o), nullptr);
}

TEST(VerifiedDrillObservations, AnAmbiguousSubmitIsSeenHeldQueuedQueriedAndReleasedByTheReconcileResult) {
    const auto rig = make_rig();
    const DrillObservation o = detail::observe_submit_ambiguous(*rig, DrillMocks{});
    EXPECT_TRUE(o.control_ok);
    EXPECT_EQ(o.gate, OrchestratorGate::SubmitAmbiguous);
    EXPECT_EQ(o.order_state, OrderState::Ambiguous);
    EXPECT_EQ(o.port_calls, 1);
    EXPECT_TRUE(o.port_saw_validated_order);
    EXPECT_EQ(o.in_flight_count, 1U);
    EXPECT_TRUE(o.order_in_flight);
    EXPECT_EQ(o.queued_for_reconcile, 1U);
    EXPECT_EQ(o.query_calls, 1);
    EXPECT_TRUE(o.query_carries_order);
    EXPECT_EQ(o.in_flight_after_reconcile, 0U);
    EXPECT_EQ(o.tracker_after_reconcile, 0U);
    EXPECT_TRUE(o.audit_trail_ok);
    EXPECT_EQ(detail::judge_submit_ambiguous(o), nullptr);
}

TEST(VerifiedDrillObservations, AnInconclusiveReconcileQueryLeavesTheSlotHeldAndTheOrderTracked) {
    const auto rig = make_rig();
    DrillMocks mocks;
    mocks.reconcile_outcome = QueryOutcome::Inconclusive;
    const DrillObservation o = detail::observe_submit_ambiguous(*rig, mocks);
    EXPECT_EQ(o.gate, OrchestratorGate::SubmitAmbiguous);
    EXPECT_EQ(o.query_calls, 1);
    EXPECT_EQ(o.in_flight_after_reconcile, 1U) << "an inconclusive answer must never release the slot";
    EXPECT_EQ(o.tracker_after_reconcile, 1U) << "and the order stays tracked for the next poll";
    EXPECT_FALSE(o.audit_trail_ok) << "no OrderReconciled record";
    EXPECT_NE(detail::judge_submit_ambiguous(o), nullptr);
}

TEST(VerifiedDrillObservations, ATriggeredKillSwitchIsSeenRefusingBeforeAndAfterTheLatch) {
    const auto rig = make_rig();
    const DrillObservation o = detail::observe_kill_switch(*rig, DrillMocks{});
    EXPECT_TRUE(o.control_ok);
    EXPECT_EQ(o.gate, OrchestratorGate::KillSwitchNotNormal);
    EXPECT_EQ(o.port_calls, 0);
    EXPECT_EQ(o.in_flight_count, 0U);
    EXPECT_TRUE(o.audit_trail_ok) << "the refusal itself wrote a PreflightFailed after the control's";
    EXPECT_TRUE(o.kill_switch_latched);
    EXPECT_EQ(o.gate_after_latch, OrchestratorGate::KillSwitchNotNormal);
    EXPECT_EQ(o.port_calls_after_latch, 0);
    EXPECT_FALSE(o.kill_switch_normal);
    EXPECT_EQ(rig->kill_switch.state(), KillState::Latched);
    EXPECT_EQ(detail::judge_kill_switch(o), nullptr);
}

TEST(VerifiedDrillObservations, AKillSwitchThatIsNeverTriggeredIsSeenLettingTheSubmitThrough) {
    const auto rig = make_rig();
    DrillMocks mocks;
    mocks.trigger_kill_switch = false;
    const DrillObservation o = detail::observe_kill_switch(*rig, mocks);
    EXPECT_TRUE(o.control_ok);
    EXPECT_EQ(o.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(o.port_calls, 1);
    EXPECT_EQ(o.in_flight_count, 1U);
    // The control's own PreflightFailed is still in the ring, but it is not this act's: nothing refused it.
    EXPECT_FALSE(o.audit_trail_ok);
    EXPECT_TRUE(o.kill_switch_latched);
    EXPECT_EQ(o.gate_after_latch, OrchestratorGate::KillSwitchNotNormal);
    EXPECT_EQ(o.port_calls_after_latch, 1) << "cumulative: the port was called once, before the latch";
    EXPECT_NE(detail::judge_kill_switch(o), nullptr);
}

TEST(VerifiedDrillObservations, ASubmitThatNeverReachesThePortIsSeenWithNoPortCallAndNoSlot) {
    // The kill switch outranks everything after it, so the control and the submit both stop at Gate 2.
    const auto rig = make_rig();
    rig->kill_switch.trigger();
    const DrillObservation o = detail::observe_submit_success(*rig, DrillMocks{});
    EXPECT_FALSE(o.control_ok) << "the control stopped at the kill switch, not at Gate 3";
    EXPECT_EQ(o.gate, OrchestratorGate::KillSwitchNotNormal);
    EXPECT_EQ(o.port_calls, 0);
    EXPECT_EQ(o.in_flight_count, 0U);
    EXPECT_FALSE(o.order_in_flight);
    EXPECT_EQ(o.queued_for_reconcile, 0U);
    EXPECT_FALSE(o.audit_trail_ok);
    EXPECT_NE(detail::judge_submit_success(o), nullptr);
}

TEST(VerifiedDrillObservations, AnExchangeThatRejectsInTheAmbiguityDrillIsNeverQueriedAgain) {
    const auto rig = make_rig();
    DrillMocks mocks;
    mocks.ambiguous_answer = SubmitOutcome::Rejected;
    const DrillObservation o = detail::observe_submit_ambiguous(*rig, mocks);
    EXPECT_EQ(o.gate, OrchestratorGate::SubmitRejected);
    EXPECT_EQ(o.queued_for_reconcile, 0U) << "a terminal order is not queued";
    EXPECT_EQ(o.query_calls, 0) << "and so the reconcile loop has nothing to ask about";
    EXPECT_EQ(o.in_flight_after_reconcile, 0U);
    EXPECT_EQ(o.tracker_after_reconcile, 0U);
    EXPECT_FALSE(o.audit_trail_ok) << "no OrderAmbiguous record";
    EXPECT_NE(detail::judge_submit_ambiguous(o), nullptr);
}

TEST(VerifiedDrillObservations, ARigThatDoesNotConsultTheScaffoldIsCaughtByTheControlInEveryObserver) {
    DryRunEvidenceChain always_ready;
    for (std::size_t i = 0; i < kEvidencePathCount; ++i) {
        always_ready.record(static_cast<EvidencePath>(i), kNow, kBuild, 1);
    }
    const auto reason = [](const char* r) { return r != nullptr && std::string_view(r).find("does not consult") != std::string_view::npos; };

    { const auto rig = make_rig(); rig->ctx.evidence = &always_ready;
      const DrillObservation o = detail::observe_submit_success(*rig, DrillMocks{});
      EXPECT_FALSE(o.control_ok); EXPECT_TRUE(reason(detail::judge_submit_success(o))); }
    { const auto rig = make_rig(); rig->ctx.evidence = &always_ready;
      const DrillObservation o = detail::observe_submit_reject(*rig, DrillMocks{});
      EXPECT_FALSE(o.control_ok); EXPECT_TRUE(reason(detail::judge_submit_reject(o))); }
    { const auto rig = make_rig(); rig->ctx.evidence = &always_ready;
      const DrillObservation o = detail::observe_submit_ambiguous(*rig, DrillMocks{});
      EXPECT_FALSE(o.control_ok); EXPECT_TRUE(reason(detail::judge_submit_ambiguous(o))); }
    { const auto rig = make_rig(); rig->ctx.evidence = &always_ready;
      const DrillObservation o = detail::observe_kill_switch(*rig, DrillMocks{});
      EXPECT_FALSE(o.control_ok); EXPECT_TRUE(reason(detail::judge_kill_switch(o))); }
}

TEST(VerifiedDrillObservations, AWireThatIsHandedAMangledOrderIsNoticed) {
    const auto rig = make_rig();
    rig->ctx.submit_port = SubmitPort{&mangling_submit, &rig->port, &detail::drill_rules_version};
    const DrillObservation o = detail::observe_submit_success(*rig, DrillMocks{});
    EXPECT_EQ(o.gate, OrchestratorGate::SubmitAccepted) << "the exchange still accepted: only the order was wrong";
    EXPECT_FALSE(o.port_saw_validated_order);
    const char* reason = detail::judge_submit_success(o);
    ASSERT_NE(reason, nullptr);
    EXPECT_TRUE(contains(reason, "handed the validated order"));
}

TEST(VerifiedDrillObservations, AReconcileQueryThatCarriesAMangledOrderIsNoticed) {
    const auto rig = make_rig();
    rig->query_port = QueryPort{&mangling_query, &rig->query};
    const DrillObservation o = detail::observe_submit_ambiguous(*rig, DrillMocks{});
    EXPECT_EQ(o.query_calls, 1);
    EXPECT_FALSE(o.query_carries_order);
    EXPECT_EQ(o.in_flight_after_reconcile, 0U) << "the reconcile itself still resolved the order";
    const char* reason = detail::judge_submit_ambiguous(o);
    ASSERT_NE(reason, nullptr);
    EXPECT_TRUE(contains(reason, "does not carry the order that was submitted"));
}
