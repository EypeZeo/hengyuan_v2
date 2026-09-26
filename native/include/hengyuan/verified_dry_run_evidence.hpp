// SPDX-License-Identifier: proprietary
// verified_dry_run_evidence.hpp — 批次 6 6b-0b: dry-run evidence that has to be EARNED, not set.
//
// WHY THIS EXISTS (外部复核 P0-01). DryRunEvidenceChain::record() is a public setter: the 6b-1 preflight
// harness "replayed the four EvidencePath scenarios" by looping over the enum and calling it, so live_ready()
// came out true without a single scenario having run. ADR-019 D11 wants evidence that each of the four paths
// (submit, reject, ambiguity, kill) was actually exercised, and that all of it comes from one build.
//
// WHAT IT DOES. VerifiedDryRunEvidence owns a DryRunEvidenceChain PRIVATELY and exposes it read-only
// (chain()). The only way evidence gets into it is run_all(), which really runs the four scenarios through
// the REAL orchestrate_submit() -- real gate chain, real KillSwitch, InFlightRegistry, rate limiter,
// OrderTracker/reconcile loop and a real DurableAuditSink on a scratch file; only the SubmitPort (the network)
// and the reconcile QueryPort are scripted -- and records a path only after checking that the scenario ended
// in the terminal state it is supposed to end in, with the side conditions that matter:
//
//   SubmitSuccess    SubmitAccepted / order Accepted; the port was handed exactly the validated order and rules
//                    snapshot, called once; the in-flight slot is KEPT (an accepted order may be live: a blind
//                    resubmit must stay blocked) and the order is handed to the reconcile queue (a live order
//                    nobody tracks would hold its slot forever: AUDIT EXEC-INFLIGHT-003); the audit trail reads
//                    Intent -> Submitted -> Accepted.
//   SubmitReject     SubmitRejected / order Rejected; called once; the slot is RELEASED (terminal) and nothing
//                    was queued for reconciliation; Intent -> Submitted -> Rejected.
//   SubmitAmbiguous  SubmitAmbiguous / order Ambiguous; the slot is HELD and the order was queued for
//                    reconciliation; then the real reconcile loop (poll_once + drain_reconcile_events) runs with
//                    a scripted query, the query carries the order that was submitted, and the slot is released
//                    by the terminal result it reports; Intent -> Submitted -> Ambiguous -> Reconciled.
//   KillSwitch       KillSwitchNotNormal; the port was never called and no slot was taken; the refusal was
//                    audited (a record written AFTER the control's own -- see below); latch() reaches the Latched
//                    lockout, which keeps blocking submits and has no way back to Normal by itself.
//
// Each drill is an OBSERVATION (act on the real orchestrator, capture what happened in a plain struct) followed
// by a JUDGEMENT (a pure function of that struct that names the first check that failed). The judgements are
// public in detail:: precisely so that they can be tested against doctored observations: a predicate that
// silently became "always true" would defeat the whole point of this file.
//
// Every drill also runs a CONTROL first: with the scaffold evidence chain emptied, the same context must stop
// at the dry-run-evidence gate and never reach the port -- proving the drill rig really consults Gate 3. The
// control writes audit records of its own (Gate 3 audits its refusal as PreflightFailed, just as the kill
// switch gate does), so every audit check reads only the records written AFTER the control (audit_mark): the
// control's PreflightFailed must not be able to stand in for the kill switch's.
//
// THE SCAFFOLD. To reach the gates the drills are about, each drill's context needs a chain that satisfies
// Gate 3 (live_ready()). That is a separate, private scaffold chain inside the drill rig, seeded with a
// scaffold build hash; it is never exposed and never copied into the verified chain. Gate 3's own behaviour is
// what the control above and test_live_submit_orchestrator.cpp prove.
//
// ALSO RECORDED: the build hash and the BinanceEnvironment the drills ran under. ready_for(env, hash) is true
// only if all four paths passed in THIS object, for exactly that environment and build -- a caller that is
// about to use a different environment (or a different binary) gets false.
//
// run_all() always starts from an empty chain: a later failing run never inherits an earlier run's evidence.
//
// HONEST BOUNDARY. This is in-process self-verification. It guards against "set the flag by hand" and against
// an orchestrator regression that a drill would notice; it does not defend against a malicious binary, and the
// final authority remains the owner's L5 review (ADR-019 D2). It does not change DryRunEvidenceChain (record()
// stays public: existing tests use it) or the orchestrator's ABI.

#pragma once

#include <hengyuan/live_submit_orchestrator.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hy {

// The definition (binance_environment.hpp) drags in OpenSSL and the secret-handling layer; this header is pure
// logic, so it only needs the type. An opaque enum declaration with the same fixed underlying type is a
// complete type: values can be stored and compared here, and a caller that names an enumerator includes the
// real header.
enum class BinanceEnvironment : std::uint8_t;

inline constexpr std::uint32_t kVerifiedDrillSuiteId = 0x44525950;  // 'DRYP': what every verified record carries
inline constexpr std::uint32_t kScaffoldBuildHash = 0x5CAFF01D;     // scaffold chain only, never in chain()
inline constexpr std::uint32_t kDrillRulesVersion = 7;
inline constexpr std::int64_t kDrillExchangeOrderId = 424242;
inline constexpr std::int32_t kDrillRejectCode = -1013;

// The preflight harness measures time since process start in whole milliseconds. Its first reading can be zero,
// while run_all() requires a positive timestamp for its fresh account snapshot. Preserve elapsed-time semantics
// and only move the unrepresentable startup instant to the first valid millisecond.
inline constexpr std::int64_t drill_startup_now_ms(std::int64_t elapsed_ms) noexcept {
    return elapsed_ms > 0 ? elapsed_ms : 1;
}

// What the scripted exchange answers in each drill. The defaults are the answers that make each drill's
// expected terminal state reachable; a test overrides one to prove that a WRONG terminal state is never
// recorded (an exchange that accepts in the reject drill, a kill switch that is never triggered, ...).
struct DrillMocks {
    SubmitOutcome success_answer{SubmitOutcome::Accepted};
    SubmitOutcome reject_answer{SubmitOutcome::Rejected};
    SubmitOutcome ambiguous_answer{SubmitOutcome::Timeout};
    bool trigger_kill_switch{true};
    // The reconcile query of the ambiguity drill: a positively identified terminal state by default.
    QueryOutcome reconcile_outcome{QueryOutcome::Found};
    OrderState reconcile_state{OrderState::Rejected};
};

struct DrillOutcome {
    EvidencePath path{EvidencePath::SubmitSuccess};
    bool passed{false};
    // The first check that failed (a string literal), or "" when the drill passed.
    const char* failure{"not run"};
    OrchestratorGate gate{OrchestratorGate::Passed};  // what orchestrate_submit() returned
    OrderState order_state{OrderState::Intent};
    int port_calls{0};  // how often the scripted SubmitPort was called
};

struct DryRunReport {
    std::array<DrillOutcome, kEvidencePathCount> drills{};

    bool all_passed() const noexcept {
        for (const DrillOutcome& d : drills) {
            if (!d.passed) return false;
        }
        return true;
    }
};

namespace detail {

// What one drill saw. Plain data, so the judgements below can be exercised against doctored values. A field a
// drill does not use keeps its default and its judgement does not read it.
struct DrillObservation {
    bool control_ok{false};  // with the scaffold emptied, the same context stopped at Gate 3 and never hit the port
    OrchestratorGate gate{OrchestratorGate::Passed};
    OrderState order_state{OrderState::Intent};
    std::int64_t exchange_order_id{0};
    int port_calls{0};
    bool port_saw_validated_order{false};  // symbol/side/type/price/qty/rules snapshot are what was validated
    std::size_t in_flight_count{0};        // after the submit
    bool order_in_flight{false};           // the submitted coid still holds a slot
    std::size_t queued_for_reconcile{0};   // after the submit
    bool audit_trail_ok{false};            // the drill's expected event subsequence is in the audit ring
    // Ambiguity drill, after the real reconcile loop ran:
    int query_calls{0};
    bool query_carries_order{false};
    std::size_t in_flight_after_reconcile{0};
    std::size_t tracker_after_reconcile{0};
    // Kill drill, after latch():
    bool kill_switch_latched{false};  // the state really is Latched (the terminal lockout)
    OrchestratorGate gate_after_latch{OrchestratorGate::Passed};
    int port_calls_after_latch{0};
    bool kill_switch_normal{false};  // it went back to Normal without an operator reset
};

// Each returns nullptr when the drill reached its expected terminal state, else the first failed check (a
// string literal). The order of the checks is part of the contract: earlier ones are the more fundamental.

inline const char* judge_submit_success(const DrillObservation& o) noexcept {
    if (!o.control_ok) return "the drill rig does not consult the dry-run evidence gate";
    if (o.gate != OrchestratorGate::SubmitAccepted) return "an accepted submit did not end at SubmitAccepted";
    if (o.order_state != OrderState::Accepted) return "an accepted submit did not leave the order Accepted";
    if (o.exchange_order_id != kDrillExchangeOrderId) return "the exchange order id was not recorded";
    if (o.port_calls != 1) return "the submit port was not called exactly once";
    if (!o.port_saw_validated_order) return "the port was not handed the validated order and rules snapshot";
    if (o.in_flight_count != 1 || !o.order_in_flight) {
        return "an accepted order must keep its in-flight slot (no blind resubmit)";
    }
    if (o.queued_for_reconcile != 1) return "an accepted, still-live order was not handed to reconciliation";
    if (!o.audit_trail_ok) return "the audit trail is not intent -> submitted -> accepted";
    return nullptr;
}

inline const char* judge_submit_reject(const DrillObservation& o) noexcept {
    if (!o.control_ok) return "the drill rig does not consult the dry-run evidence gate";
    if (o.gate != OrchestratorGate::SubmitRejected) return "a rejected submit did not end at SubmitRejected";
    if (o.order_state != OrderState::Rejected) return "a rejected submit did not leave the order Rejected";
    if (o.port_calls != 1) return "the submit port was not called exactly once";
    if (!o.port_saw_validated_order) return "the port was not handed the validated order and rules snapshot";
    if (o.in_flight_count != 0) return "a rejected order (terminal) must release its in-flight slot";
    if (o.queued_for_reconcile != 0) return "a rejected order must not be queued for reconciliation";
    if (!o.audit_trail_ok) return "the audit trail is not intent -> submitted -> rejected";
    return nullptr;
}

inline const char* judge_submit_ambiguous(const DrillObservation& o) noexcept {
    if (!o.control_ok) return "the drill rig does not consult the dry-run evidence gate";
    if (o.gate != OrchestratorGate::SubmitAmbiguous) return "a timed-out submit did not end at SubmitAmbiguous";
    if (o.order_state != OrderState::Ambiguous) return "a timed-out submit did not leave the order Ambiguous";
    if (o.port_calls != 1) return "the submit port was not called exactly once";
    if (!o.port_saw_validated_order) return "the port was not handed the validated order and rules snapshot";
    if (o.in_flight_count != 1 || !o.order_in_flight) {
        return "an ambiguous order must keep its in-flight slot (no blind retry)";
    }
    if (o.queued_for_reconcile != 1) return "an ambiguous order was not queued for reconciliation";
    if (o.query_calls != 1) return "the reconcile loop did not query the ambiguous order exactly once";
    if (!o.query_carries_order) return "the reconcile query does not carry the order that was submitted";
    if (o.in_flight_after_reconcile != 0) return "the reconcile result did not release the ambiguous order's slot";
    if (o.tracker_after_reconcile != 0) return "the resolved order is still tracked for reconciliation";
    if (!o.audit_trail_ok) return "the audit trail is not intent -> submitted -> ambiguous -> reconciled";
    return nullptr;
}

inline const char* judge_kill_switch(const DrillObservation& o) noexcept {
    if (!o.control_ok) return "the drill rig does not consult the dry-run evidence gate";
    if (o.gate != OrchestratorGate::KillSwitchNotNormal) return "a triggered kill switch did not stop the submit";
    if (o.port_calls != 0) return "the submit port was called with the kill switch triggered";
    if (o.in_flight_count != 0) return "an in-flight slot was taken with the kill switch triggered";
    if (!o.audit_trail_ok) return "the refusal was not audited";
    if (!o.kill_switch_latched) return "the kill switch did not reach the Latched lockout";
    if (o.gate_after_latch != OrchestratorGate::KillSwitchNotNormal || o.port_calls_after_latch != 0) {
        return "a latched kill switch did not keep blocking submits";
    }
    if (o.kill_switch_normal) return "the kill switch returned to Normal by itself";
    return nullptr;
}

// --- the scripted network side ---------------------------------------------------------------------------

struct DrillPortState {
    SubmitOutcome answer{SubmitOutcome::Accepted};
    int calls{0};
    // What the LAST call was asked, to prove the validated order and snapshot are what reaches the wire.
    bool coid_present{false};
    std::uint32_t symbol_id{0};
    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Limit};
    std::int64_t price_ticks{0};
    std::int64_t qty_ticks{0};
    std::uint32_t rules_version{0};
};

inline SubmitResponse drill_submit(const char* coid, std::uint32_t symbol_id, OrderSide side, OrderType type,
                                   std::int64_t price_ticks, std::int64_t qty_ticks, const SymbolRules& rules,
                                   void* user_data) noexcept {
    auto* s = static_cast<DrillPortState*>(user_data);
    ++s->calls;
    s->coid_present = coid != nullptr && coid[0] != '\0';
    s->symbol_id = symbol_id;
    s->side = side;
    s->type = type;
    s->price_ticks = price_ticks;
    s->qty_ticks = qty_ticks;
    s->rules_version = rules.rules_version;
    switch (s->answer) {
        case SubmitOutcome::Accepted: return SubmitResponse{SubmitOutcome::Accepted, kDrillExchangeOrderId, 0};
        case SubmitOutcome::Rejected: return SubmitResponse{SubmitOutcome::Rejected, 0, kDrillRejectCode};
        case SubmitOutcome::Timeout: return SubmitResponse{SubmitOutcome::Timeout, 0, 0};
        case SubmitOutcome::NetworkError: return SubmitResponse{SubmitOutcome::NetworkError, 0, 0};
        case SubmitOutcome::StaleRulesVersion: return SubmitResponse{SubmitOutcome::NetworkError, 0, -1};
    }
    return SubmitResponse{SubmitOutcome::NetworkError, 0, -1};
}

inline std::uint32_t drill_rules_version(void*) noexcept { return kDrillRulesVersion; }

struct DrillQueryState {
    QueryOutcome outcome{QueryOutcome::Found};
    OrderState state{OrderState::Rejected};
    int calls{0};
    OrderExpectation last{};
};

inline QueryResult drill_query(const OrderExpectation& expected, void* user_data) noexcept {
    auto* s = static_cast<DrillQueryState*>(user_data);
    ++s->calls;
    s->last = expected;
    QueryResult r{};
    r.outcome = s->outcome;
    r.confirmed_state = s->state;
    return r;  // no fill, no exchange id: valid for every terminal state a never-filled order can reach
}

// --- one drill's world -----------------------------------------------------------------------------------

// Every file a DurableAuditSink derives from its path: the main log and the key-rotation and store-identity
// sidecars, each with its lock file, its tip anchor and the tip's temp file. All of them, or every run leaks
// files into the scratch directory.
inline constexpr const char* kScratchSuffixes[] = {
    "",                  ".lock",              ".tip",               ".tip.tmp",
    ".keyrotations",     ".keyrotations.lock", ".keyrotations.tip",  ".keyrotations.tip.tmp",
    ".storeid",          ".storeid.lock",      ".storeid.tip",       ".storeid.tip.tmp"};

inline void remove_scratch_files(const std::string& base) {
    for (const char* suffix : kScratchSuffixes) std::remove((base + suffix).c_str());
}

// Heap-allocated by the caller (AuditRingSink alone is ~1024 records).
struct DrillRig {
    AuditRingSink audit;
    KillSwitch kill_switch;
    DryRunEvidenceChain scaffold;  // gets the drill's context past Gate 3 -- never exposed (see the header)
    SpotRateLimitTracker rate_limiter;
    InFlightRegistry in_flight;
    SymbolRules rules{};
    AccountSnapshot account{};
    OrchestratorContext ctx{};
    DrillPortState port{};
    DrillQueryState query{};
    QueryPort query_port{};  // the ambiguity drill's reconcile query; like ctx.submit_port, replaceable by a test
    // Audit records at index >= audit_mark were written by the act under test, not by the control before it.
    std::size_t audit_mark{0};
    OrderTracker tracker;
    ToReconcileRing to_reconcile;
    ReconcileEventRing reconcile_events;
    std::string scratch_path;
    std::unique_ptr<KeyRing> key_ring;
    std::unique_ptr<DurableAuditSink> durable_sink;

    DrillRig(std::int64_t now_ms, std::string path) : scratch_path(std::move(path)) {
        audit.set_available(true);
        kill_switch.operator_reset();
        rate_limiter.configure(/*weight*/ 6000, 500, /*raw*/ 60000, 5000, /*orders*/ 100, 10);

        // A scratch log with a fixed KEK: this can never hold a previous run's orders, so it is a drill's
        // audit port and NOT a recovery path (startup_recovery.hpp is that, over a persistent log).
        remove_scratch_files(scratch_path);
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x20 + i);
        key_ring = std::make_unique<KeyRing>(kek);
        WrappedKeyRecord key_record{};
        key_ring->add_key(1, std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}, key_record);
        durable_sink = std::make_unique<DurableAuditSink>(scratch_path, *key_ring, 1);

        // Self-consistent synthetic rules: a nonzero price/qty scale is what makes validate_pre_trade()'s
        // notional rescale (native scale -> kBalanceScale = 8) land where the balance and caps below expect it.
        std::strncpy(rules.symbol, "BTCUSDT", sizeof(rules.symbol) - 1);
        rules.is_trading = true;
        rules.rules_version = kDrillRulesVersion;
        rules.price_scale = 2;
        rules.qty_scale = 2;
        rules.min_price_ticks = 100;
        rules.max_price_ticks = 10'000'000;
        rules.tick_size_ticks = 100;
        rules.min_qty_ticks = 10;
        rules.max_qty_ticks = 1'000'000;
        rules.step_size_ticks = 10;
        rules.min_notional_ticks = 1000;

        // price 1000 * qty 10 at native scale 2+2 = 10000, rescaled to 8 decimals = 1e8; 10x headroom.
        std::strncpy(account.assets[0].asset, "USDT", 5);
        account.assets[0].free_ticks = 1'000'000'000;
        account.asset_count = 1;
        account.can_trade = true;
        account.timestamp_ms = now_ms;

        seed_scaffold(now_ms);
        port.answer = SubmitOutcome::Accepted;
        query.outcome = QueryOutcome::Found;
        query_port = QueryPort{&drill_query, &query};

        ctx.audit = &audit;
        ctx.kill_switch = &kill_switch;
        ctx.evidence = &scaffold;
        ctx.signer_ready = true;
        ctx.depth_synced = true;
        ctx.rate_limiter = &rate_limiter;
        ctx.in_flight = &in_flight;
        ctx.durable_audit = make_durable_order_audit_port(*durable_sink);
        ctx.symbol_rules = &rules;
        ctx.account = &account;
        ctx.side = OrderSide::Buy;
        ctx.order_type = OrderType::Limit;
        ctx.base_asset = "BTC";
        ctx.quote_asset = "USDT";
        ctx.now_ms = now_ms;
        ctx.exposure_limits.freshness_max_age_ms = 30000;
        ctx.exposure_limits.single_order_notional_cap = 1'000'000'000;
        ctx.exposure_limits.total_exposure_notional_cap = 1'000'000'000;
        ctx.price_ticks = 1000;
        ctx.qty_ticks = 10;
        ctx.symbol_id = 1;
        ctx.sequence = 1;
        ctx.mode = ExecutionMode::DryRun;
        ctx.order_weight = 1;
        ctx.submit_port = SubmitPort{&drill_submit, &port, &drill_rules_version};
        ctx.to_reconcile = &to_reconcile;
        ctx.reconcile_events = &reconcile_events;
    }

    ~DrillRig() {
        durable_sink.reset();  // close the file before removing it
        key_ring.reset();
        remove_scratch_files(scratch_path);
    }

    DrillRig(const DrillRig&) = delete;
    DrillRig& operator=(const DrillRig&) = delete;

    void seed_scaffold(std::int64_t now_ms) noexcept {
        for (std::size_t i = 0; i < kEvidencePathCount; ++i) {
            scaffold.record(static_cast<EvidencePath>(i), now_ms, kScaffoldBuildHash, kVerifiedDrillSuiteId);
        }
    }

    void bind_confirmation() noexcept {
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

// --- observation helpers -------------------------------------------------------------------------------------

// The audit ring, from index `from` on, holds these event types as a subsequence, in this order (other records
// may sit between them).
inline bool audit_has_in_order(const AuditRingSink& audit, std::initializer_list<AuditEventType> wanted,
                               std::size_t from = 0) noexcept {
    const AuditEventType* next = wanted.begin();
    for (std::size_t i = from; i < audit.count() && next != wanted.end(); ++i) {
        const AuditRecord* r = audit.at(i);
        if (r != nullptr && r->event_type == *next) ++next;
    }
    return next == wanted.end();
}

// The control: with the scaffold emptied, the very same context must stop at the dry-run-evidence gate and
// never reach the port. Restores the scaffold and moves the audit mark past the records the control wrote.
// False = the drill rig does not really consult Gate 3, so the drill would prove less than it claims.
inline bool scaffold_is_consulted(DrillRig& rig) noexcept {
    rig.scaffold.reset();
    const OrchestratorResult r = orchestrate_submit(rig.ctx);
    const bool stopped_at_gate3 = r.gate == OrchestratorGate::DryRunEvidenceIncomplete && rig.port.calls == 0;
    rig.seed_scaffold(rig.ctx.now_ms);
    rig.audit_mark = rig.audit.count();
    return stopped_at_gate3;
}

inline bool port_saw_the_validated_order(const DrillRig& rig) noexcept {
    const DrillPortState& p = rig.port;
    return p.coid_present && p.symbol_id == rig.ctx.symbol_id && p.side == rig.ctx.side &&
           p.type == rig.ctx.order_type && p.price_ticks == rig.ctx.price_ticks &&
           p.qty_ticks == rig.ctx.qty_ticks && p.rules_version == rig.rules.rules_version;
}

inline bool query_carries_the_order(const OrderExpectation& asked, const OrderRecord& order, const DrillRig& rig) noexcept {
    return asked.client_order_id.view() == order.client_order_id.view() && asked.symbol_id == rig.ctx.symbol_id &&
           asked.side == rig.ctx.side && asked.order_type == rig.ctx.order_type &&
           asked.intended_price_ticks == rig.ctx.price_ticks && asked.intended_qty_ticks == rig.ctx.qty_ticks &&
           asked.rules_snapshot_at_submit.rules_version == rig.rules.rules_version;
}

// --- the four observations -----------------------------------------------------------------------------------

// The common act: control, confirm, one real orchestrate_submit(), and what the submit left behind.
inline DrillObservation observe_submit(DrillRig& rig, OrchestratorResult& result_out) noexcept {
    DrillObservation o;
    o.control_ok = scaffold_is_consulted(rig);
    rig.bind_confirmation();
    result_out = orchestrate_submit(rig.ctx);
    o.gate = result_out.gate;
    o.order_state = result_out.order.state;
    o.exchange_order_id = result_out.order.exchange_order_id;
    o.port_calls = rig.port.calls;
    o.port_saw_validated_order = port_saw_the_validated_order(rig);
    o.in_flight_count = rig.in_flight.count();
    o.order_in_flight = rig.in_flight.is_in_flight(result_out.order.client_order_id.view());
    o.queued_for_reconcile = rig.to_reconcile.size_approx();
    return o;
}

inline DrillObservation observe_submit_success(DrillRig& rig, const DrillMocks& mocks) noexcept {
    rig.port.answer = mocks.success_answer;
    OrchestratorResult r{};
    DrillObservation o = observe_submit(rig, r);
    o.audit_trail_ok = audit_has_in_order(
        rig.audit, {AuditEventType::OrderIntentCreated, AuditEventType::OrderSubmitted, AuditEventType::OrderAccepted},
        rig.audit_mark);
    return o;
}

inline DrillObservation observe_submit_reject(DrillRig& rig, const DrillMocks& mocks) noexcept {
    rig.port.answer = mocks.reject_answer;
    OrchestratorResult r{};
    DrillObservation o = observe_submit(rig, r);
    o.audit_trail_ok = audit_has_in_order(
        rig.audit, {AuditEventType::OrderIntentCreated, AuditEventType::OrderSubmitted, AuditEventType::OrderRejected},
        rig.audit_mark);
    return o;
}

inline DrillObservation observe_submit_ambiguous(DrillRig& rig, const DrillMocks& mocks) noexcept {
    rig.port.answer = mocks.ambiguous_answer;
    rig.query.outcome = mocks.reconcile_outcome;
    rig.query.state = mocks.reconcile_state;
    OrchestratorResult r{};
    DrillObservation o = observe_submit(rig, r);

    // The real reconcile loop, with a scripted query.
    const ReconcilePollPolicy policy{};
    poll_once(rig.tracker, rig.to_reconcile, rig.reconcile_events, rig.query_port, policy, rig.ctx.now_ms + 1);
    drain_reconcile_events(rig.in_flight, &rig.audit, rig.reconcile_events, rig.ctx.now_ms + 2);

    o.query_calls = rig.query.calls;
    o.query_carries_order = query_carries_the_order(rig.query.last, r.order, rig);
    o.in_flight_after_reconcile = rig.in_flight.count();
    o.tracker_after_reconcile = rig.tracker.count();
    o.audit_trail_ok = audit_has_in_order(rig.audit,
                                          {AuditEventType::OrderIntentCreated, AuditEventType::OrderSubmitted,
                                           AuditEventType::OrderAmbiguous, AuditEventType::OrderReconciled},
                                          rig.audit_mark);
    return o;
}

inline DrillObservation observe_kill_switch(DrillRig& rig, const DrillMocks& mocks) noexcept {
    DrillObservation o;
    // The control comes first: the kill switch outranks Gate 3, so once it is triggered the control could no
    // longer show anything.
    o.control_ok = scaffold_is_consulted(rig);
    rig.bind_confirmation();
    if (mocks.trigger_kill_switch) rig.kill_switch.trigger();

    const OrchestratorResult r = orchestrate_submit(rig.ctx);
    o.gate = r.gate;
    o.order_state = r.order.state;
    o.port_calls = rig.port.calls;
    o.in_flight_count = rig.in_flight.count();
    // Only what the refusal itself wrote counts: the control's own PreflightFailed is before audit_mark.
    o.audit_trail_ok = audit_has_in_order(rig.audit, {AuditEventType::PreflightFailed}, rig.audit_mark);

    // Latched is terminal: still blocked, and no automatic path back to Normal.
    rig.kill_switch.latch();
    o.kill_switch_latched = rig.kill_switch.state() == KillState::Latched;
    const OrchestratorResult again = orchestrate_submit(rig.ctx);
    o.gate_after_latch = again.gate;
    o.port_calls_after_latch = rig.port.calls;
    o.kill_switch_normal = rig.kill_switch.state() == KillState::Normal;
    return o;
}

}  // namespace detail

class VerifiedDryRunEvidence {
public:
    // Runs the four drills against a fresh rig each (a durable-audit scratch log at
    // `<scratch_prefix>_<drill>.log`, removed afterwards) and records exactly the paths whose drill reached its
    // expected terminal state. `now_ms` must be positive (an account snapshot stamped 0 reads as "never
    // fetched"). Always starts from an empty chain. Never throws: an exception inside a drill is that drill
    // failing.
    DryRunReport run_all(std::uint32_t build_hash, std::int64_t now_ms, BinanceEnvironment environment,
                         std::string_view scratch_prefix, const DrillMocks& mocks = {}) noexcept {
        chain_.reset();
        ran_ok_ = false;

        DryRunReport report;
        for (std::size_t i = 0; i < kEvidencePathCount; ++i) {
            report.drills[i].path = static_cast<EvidencePath>(i);
            report.drills[i].failure = "not run";
        }
        if (now_ms <= 0) {
            for (DrillOutcome& d : report.drills) d.failure = "now_ms must be positive";
            return report;
        }

        for (std::size_t i = 0; i < kEvidencePathCount; ++i) {
            const auto path = static_cast<EvidencePath>(i);
            const DrillOutcome outcome = run_one(path, now_ms, scratch_prefix, mocks);
            report.drills[i] = outcome;
            if (outcome.passed) chain_.record(path, now_ms, build_hash, kVerifiedDrillSuiteId);
        }

        if (chain_.live_ready()) {
            ran_ok_ = true;
            environment_ = environment;
            build_hash_ = build_hash;
        }
        return report;
    }

    // Read-only on purpose: this is the ONLY handle the caller gets. ctx.evidence takes exactly this.
    const DryRunEvidenceChain& chain() const noexcept { return chain_; }

    // All four paths passed in THIS object, for exactly this environment and this build.
    bool ready_for(BinanceEnvironment environment, std::uint32_t build_hash) const noexcept {
        return ran_ok_ && chain_.live_ready() && environment_ == environment && build_hash_ == build_hash;
    }

private:
    static DrillOutcome make_outcome(EvidencePath path, const detail::DrillObservation& o, const char* failure) noexcept {
        DrillOutcome out;
        out.path = path;
        out.passed = failure == nullptr;
        out.failure = failure == nullptr ? "" : failure;
        out.gate = o.gate;
        out.order_state = o.order_state;
        out.port_calls = o.port_calls;
        return out;
    }

    DrillOutcome run_one(EvidencePath path, std::int64_t now_ms, std::string_view scratch_prefix,
                         const DrillMocks& mocks) noexcept {
        try {
            static constexpr const char* kNames[kEvidencePathCount] = {"submit", "reject", "ambiguous", "kill"};
            std::string file(scratch_prefix);
            file += '_';
            file += kNames[static_cast<std::size_t>(path)];
            file += ".log";
            const auto rig = std::make_unique<detail::DrillRig>(now_ms, std::move(file));
            switch (path) {
                case EvidencePath::SubmitSuccess: {
                    const detail::DrillObservation o = detail::observe_submit_success(*rig, mocks);
                    return make_outcome(path, o, detail::judge_submit_success(o));
                }
                case EvidencePath::SubmitReject: {
                    const detail::DrillObservation o = detail::observe_submit_reject(*rig, mocks);
                    return make_outcome(path, o, detail::judge_submit_reject(o));
                }
                case EvidencePath::SubmitAmbiguous: {
                    const detail::DrillObservation o = detail::observe_submit_ambiguous(*rig, mocks);
                    return make_outcome(path, o, detail::judge_submit_ambiguous(o));
                }
                case EvidencePath::KillSwitch: {
                    const detail::DrillObservation o = detail::observe_kill_switch(*rig, mocks);
                    return make_outcome(path, o, detail::judge_kill_switch(o));
                }
            }
            return make_outcome(path, {}, "unknown evidence path");
        } catch (...) {
            return make_outcome(path, {}, "the drill threw");
        }
    }

    DryRunEvidenceChain chain_;
    bool ran_ok_{false};
    BinanceEnvironment environment_{};
    std::uint32_t build_hash_{0};
};

}  // namespace hy
