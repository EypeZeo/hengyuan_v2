// S5 final authorization: recheck every local authorization after Prepared is durable and before submit.
#include "test_helpers/never_sent_fixture.hpp"
#include "test_helpers/alloc_probe.hpp"
#include "test_helpers/temp_directory.hpp"

#include <hengyuan/durable_audit_sink.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace {

using hy::AuditEventType;
using hy::AuditRecord;
using hy::ClosureAppendStatus;
using hy::OrchestratorGate;
using hy::OrchestratorResult;
using hy::OrderState;
using hy::never_sent::Reason;
using hy::test_helpers::never_sent::FakeAudit;
using hy::test_helpers::never_sent::NeverSentOrchestratorFixture;
using hy::test_helpers::never_sent::TraceKind;

struct FakeMonotonicClock {
    std::int64_t now_ms{10};
    static std::int64_t now(void* user_data) noexcept {
        return static_cast<FakeMonotonicClock*>(user_data)->now_ms;
    }
};

struct DynamicReadinessState {
    std::uint64_t generation{1};
    bool connected{true};
    bool clock_ready{true};
    FakeMonotonicClock* clock_to_advance{nullptr};
    std::size_t advance_on_feed_call{0};
    std::size_t feed_calls{0};
    std::int64_t advanced_clock_ms{10};
};

class DynamicReadiness {
public:
    explicit DynamicReadiness(const hy::KillSwitch& kill_switch) noexcept
        : recovery_(60'000),
          readiness_(recovery_, kill_switch,
                     hy::ReadinessProbe{&feed, &recovery_observation, &clock, &state_}) {
        hy::RecoveryObservation recovered{};
        recovered.clock_synced = true;
        recovered.exchange_info_loaded = true;
        recovered.sink_open = true;
        recovered.sink_fenced = false;
        recovered.scan_status = hy::RecoveryScanStatus::Clean;
        recovered.recovery_applied = true;
        recovered.account_fresh = true;
        recovered.user_data_connected = true;
        (void)recovery_.update(recovered, 0);

        readiness_.observe();
        const std::int64_t now_ms = steady_now_ms();
        if (const auto sweep = readiness_.barrier().take_sweep_request(now_ms)) {
            readiness_.barrier().on_sweep_report(hy::SweepReport{*sweep, 0, true}, now_ms);
        }
    }

    hy::SubmitReadiness& readiness() noexcept { return readiness_; }
    DynamicReadinessState& state() noexcept { return state_; }

private:
    static std::int64_t steady_now_ms() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static hy::FeedObservation feed(void* user_data) noexcept {
        const auto& state = *static_cast<DynamicReadinessState*>(user_data);
        auto& mutable_state = *static_cast<DynamicReadinessState*>(user_data);
        ++mutable_state.feed_calls;
        if (mutable_state.clock_to_advance != nullptr &&
            mutable_state.feed_calls == mutable_state.advance_on_feed_call) {
            mutable_state.clock_to_advance->now_ms = mutable_state.advanced_clock_ms;
        }
        const std::int64_t now_ms = steady_now_ms();
        hy::FeedObservation observation{};
        observation.generation = state.generation;
        observation.connected = state.connected;
        observation.last_activity_ms = now_ms;
        observation.sampled_at_ms = now_ms;
        return observation;
    }

    static hy::RecoveryObservation recovery_observation(void*) noexcept {
        hy::RecoveryObservation observation{};
        observation.clock_synced = true;
        observation.exchange_info_loaded = true;
        observation.sink_open = true;
        observation.sink_fenced = false;
        observation.scan_status = hy::RecoveryScanStatus::Clean;
        observation.recovery_applied = true;
        observation.account_fresh = true;
        observation.user_data_connected = true;
        return observation;
    }

    static bool clock(void* user_data) noexcept {
        return static_cast<DynamicReadinessState*>(user_data)->clock_ready;
    }

    hy::StartupRecovery recovery_;
    DynamicReadinessState state_{};
    hy::SubmitReadiness readiness_;
};

enum class PreparedMutation : std::uint8_t {
    None,
    KillTrigger,
    KillTriggerAndReset,
    AuthorityRevoke,
    FeedDisconnect,
    FeedGenerationChange,
    FeedClockNotReady,
    AdvanceClockDuringFinalProbe,
    ContextKillTriggerAndReset,
    AuditUnavailable,
    SignerUnavailable,
    DepthUnavailable,
    AdvanceClock,
    ChangeAccount,
    LoseAndReuseSlot,
    ReconfigureLimiter,
    MutateContextOrder,
    ChangeRulesVersion,
};

struct MutationState {
    PreparedMutation mutation{PreparedMutation::None};
    hy::KillSwitch* kill_switch{nullptr};
    hy::SubmitReadiness* readiness{nullptr};
    DynamicReadinessState* feed{nullptr};
    FakeMonotonicClock* monotonic_clock{nullptr};
    hy::AccountSnapshot* account{nullptr};
    hy::InFlightRegistry* in_flight{nullptr};
    hy::SpotRateLimitTracker* rate_limiter{nullptr};
    hy::SymbolRules* rules{nullptr};
    hy::AuditRingSink* audit{nullptr};
    hy::OrchestratorContext* context{nullptr};
    char client_order_id[hy::kClientOrderIdLen + 1]{};
    std::int64_t new_clock_ms{10};
    bool invoked{false};
    bool limiter_configured{false};
    bool replacement_registered{false};
};

void mutate_after_prepared(void* user_data, const AuditRecord&) noexcept {
    auto& state = *static_cast<MutationState*>(user_data);
    state.invoked = true;
    switch (state.mutation) {
        case PreparedMutation::None: break;
        case PreparedMutation::KillTrigger: state.kill_switch->trigger(); break;
        case PreparedMutation::KillTriggerAndReset:
            state.kill_switch->trigger();
            state.kill_switch->operator_reset();
            break;
        case PreparedMutation::AuthorityRevoke: state.readiness->authority().revoke(); break;
        case PreparedMutation::FeedDisconnect: state.feed->connected = false; break;
        case PreparedMutation::FeedGenerationChange: ++state.feed->generation; break;
        case PreparedMutation::FeedClockNotReady: state.feed->clock_ready = false; break;
        case PreparedMutation::AdvanceClockDuringFinalProbe:
            state.feed->clock_to_advance = state.monotonic_clock;
            state.feed->advance_on_feed_call = state.feed->feed_calls + 1;
            state.feed->advanced_clock_ms = state.new_clock_ms;
            break;
        case PreparedMutation::ContextKillTriggerAndReset:
            state.kill_switch->trigger();
            state.kill_switch->operator_reset();
            break;
        case PreparedMutation::AuditUnavailable: state.audit->set_available(false); break;
        case PreparedMutation::SignerUnavailable: state.context->signer_ready = false; break;
        case PreparedMutation::DepthUnavailable: state.context->depth_synced = false; break;
        case PreparedMutation::AdvanceClock: state.monotonic_clock->now_ms = state.new_clock_ms; break;
        case PreparedMutation::ChangeAccount:
            *state.account = hy::test_helpers::make_account(900, true, {{"USDT", 0, 0}});
            break;
        case PreparedMutation::LoseAndReuseSlot:
            state.in_flight->mark_resolved(std::string_view(state.client_order_id));
            state.replacement_registered = state.in_flight->register_submit("HY-S5-REUSED");
            break;
        case PreparedMutation::ReconfigureLimiter:
            state.limiter_configured = state.rate_limiter->configure(
                1, 0, 1, 0, 1, 0, hy::RateLimitBudgetSplit{100, 0, 0});
            break;
        case PreparedMutation::MutateContextOrder:
            state.context->symbol_id = 99;
            state.context->side = hy::OrderSide::Sell;
            state.context->price_ticks = 2000;
            state.context->qty_ticks = 20;
            break;
        case PreparedMutation::ChangeRulesVersion: ++state.rules->rules_version; break;
    }
}

struct HookedDurableAudit {
    hy::DurableOrderAuditPort inner{};
    MutationState* mutation{nullptr};

    static hy::AuditAppendResult append(void* user_data, const AuditRecord& record, std::int64_t now_ms) noexcept {
        auto& self = *static_cast<HookedDurableAudit*>(user_data);
        const auto result = self.inner.append_durable(record, now_ms);
        if (result.acked() && record.event_type == AuditEventType::OrderSubmitPrepared && self.mutation != nullptr) {
            mutate_after_prepared(self.mutation, record);
        }
        return result;
    }

    static bool fenced(void* user_data) noexcept {
        return static_cast<HookedDurableAudit*>(user_data)->inner.fenced();
    }

    static hy::ClosureAppendResult append_closure(void* user_data, const hy::never_sent::ClosureTarget& target,
                                                   const AuditRecord& record, std::int64_t now_ms) noexcept {
        return static_cast<HookedDurableAudit*>(user_data)->inner.append_never_sent(target, record, now_ms);
    }

    void wire(hy::DurableAuditSink& sink, MutationState& hook) noexcept {
        inner = hy::make_durable_order_audit_port(sink);
        mutation = &hook;
    }

    hy::DurableOrderAuditPort port() noexcept {
        return {&HookedDurableAudit::append, &HookedDurableAudit::fenced, this,
                &HookedDurableAudit::append_closure};
    }
};

class SubmitFinalGateTest : public NeverSentOrchestratorFixture {
protected:
    SubmitFinalGateTest()
        : dynamic_readiness_(kill_switch_), independent_readiness_kill_(),
          independent_readiness_(independent_readiness_kill_) {}

    void SetUp() override {
        NeverSentOrchestratorFixture::SetUp();
        fill_context_ = {};
        ctx_.fill_context = &fill_context_;
        clock_.now_ms = 10;
        ctx_.monotonic_now = &FakeMonotonicClock::now;
        ctx_.monotonic_clock = &clock_;
        ctx_.authorization_ttl_ms = 100;
        ASSERT_TRUE(rate_limiter_.configure(1, 0, 1, 0, 1, 0, hy::RateLimitBudgetSplit{100, 0, 0}));
    }

    void use_dynamic_readiness() noexcept { ctx_.readiness = &dynamic_readiness_.readiness(); }

    void arm_mutation(PreparedMutation mutation, std::int64_t new_clock_ms = 10) noexcept {
        mutation_ = {};
        mutation_.mutation = mutation;
        mutation_.kill_switch = &kill_switch_;
        mutation_.readiness = ctx_.readiness;
        mutation_.feed = &dynamic_readiness_.state();
        mutation_.monotonic_clock = &clock_;
        mutation_.account = &account_;
        mutation_.in_flight = &in_flight_;
        mutation_.rate_limiter = &rate_limiter_;
        mutation_.rules = &rules_;
        mutation_.audit = &audit_;
        mutation_.context = &ctx_;
        mutation_.new_clock_ms = new_clock_ms;
        const auto id = expected_coid();
        std::memcpy(mutation_.client_order_id, id.view().data(), id.view().size());
        mutation_.client_order_id[id.view().size()] = '\0';
        fake_audit_.after_prepared_ack = &mutate_after_prepared;
        fake_audit_.hook_user_data = &mutation_;
    }

    OrchestratorResult run_refusal(Reason reason, ClosureAppendStatus status = ClosureAppendStatus::Appended) {
        fake_audit_.closure_status = status;
        if (status == ClosureAppendStatus::NotWired) ctx_.durable_audit.closure_fn = nullptr;
        const OrchestratorResult result = hy::orchestrate_submit(ctx_);
        EXPECT_EQ(result.gate, OrchestratorGate::FinalAuthorizationRefused);
        EXPECT_EQ(result.final_refusal, reason);
        EXPECT_EQ(result.closure, status);
        EXPECT_TRUE(mutation_.invoked);
        EXPECT_EQ(submit_calls_, 0u);
        EXPECT_EQ(fake_audit_.trace[0].record.event_type, AuditEventType::OrderIntentCreated);
        EXPECT_EQ(fake_audit_.trace[1].record.event_type, AuditEventType::OrderSubmitPrepared);
        EXPECT_EQ(fake_audit_.trace[1].durable_status, hy::AuditAppendResult::Status::Acked);
        EXPECT_EQ(fake_audit_.trace[1].record.resulting_state, OrderState::Submitting);
        EXPECT_STREQ(fake_audit_.trace[1].record.client_order_id, result.order.client_order_id.id);

        if (status == ClosureAppendStatus::NotWired) {
            EXPECT_EQ(fake_audit_.closure_calls, 0u);
            EXPECT_EQ(fake_audit_.trace_count, 2u);
        } else {
            EXPECT_EQ(fake_audit_.closure_calls, 1u);
            EXPECT_EQ(fake_audit_.trace_count, 3u);
            const auto& closure_entry = fake_audit_.trace[2];
            EXPECT_EQ(closure_entry.kind, TraceKind::ClosureAppend);
            EXPECT_EQ(closure_entry.closure_status, status);
            EXPECT_EQ(closure_entry.target.state, OrderState::Submitting);
            EXPECT_EQ(closure_entry.record.event_type, AuditEventType::OrderNeverSent);
            EXPECT_EQ(closure_entry.record.resulting_state, OrderState::NeverSent);
            EXPECT_EQ(closure_entry.record.detail_code, static_cast<std::int64_t>(reason));
            EXPECT_STREQ(closure_entry.record.client_order_id, result.order.client_order_id.id);
            EXPECT_EQ(closure_entry.record.symbol_id, fake_audit_.trace[0].record.symbol_id);
            EXPECT_EQ(closure_entry.record.side, fake_audit_.trace[0].record.side);
            EXPECT_EQ(closure_entry.record.mode, fake_audit_.trace[0].record.mode);
            EXPECT_EQ(closure_entry.record.price_ticks, fake_audit_.trace[0].record.price_ticks);
            EXPECT_EQ(closure_entry.record.qty_ticks, fake_audit_.trace[0].record.qty_ticks);
            EXPECT_EQ(closure_entry.record.exchange_order_id, 0);
            EXPECT_EQ(closure_entry.record.filled_qty_ticks, 0);
            EXPECT_EQ(closure_entry.record.avg_fill_price_ticks, 0);
            EXPECT_EQ(closure_entry.target.symbol_id, fake_audit_.trace[0].record.symbol_id);
            EXPECT_EQ(closure_entry.target.side, fake_audit_.trace[0].record.side);
            EXPECT_EQ(closure_entry.target.mode, fake_audit_.trace[0].record.mode);
            EXPECT_EQ(closure_entry.target.price_ticks, fake_audit_.trace[0].record.price_ticks);
            EXPECT_EQ(closure_entry.target.qty_ticks, fake_audit_.trace[0].record.qty_ticks);
        }

        const bool appended = status == ClosureAppendStatus::Appended;
        const std::size_t expected_slots = appended && mutation_.replacement_registered ? 1u : (appended ? 0u : 1u);
        EXPECT_EQ(result.order.state, appended ? OrderState::NeverSent : OrderState::Submitting);
        EXPECT_EQ(in_flight_.count(), expected_slots);
        EXPECT_EQ(fill_context_.count(), appended ? 0u : 1u);
        EXPECT_EQ(rate_limiter_.can_send_order(hy::RateLimitLane::Strategy, 1), appended);
        return result;
    }

    FakeMonotonicClock clock_{};
    DynamicReadiness dynamic_readiness_;
    hy::KillSwitch independent_readiness_kill_{};
    DynamicReadiness independent_readiness_;
    hy::OrderFillContext fill_context_{};
    MutationState mutation_{};
};

TEST_F(SubmitFinalGateTest, KillTriggerAfterPreparedWritesClassBClosureAndReleasesResources) {
    arm_mutation(PreparedMutation::KillTrigger);
    const auto result = run_refusal(Reason::ReadinessRevoked);
    EXPECT_EQ(kill_switch_.state(), hy::KillState::Triggered);
    EXPECT_EQ(result.not_ready_reason, hy::NotReadyReason::KillSwitchNotNormal);
}

TEST_F(SubmitFinalGateTest, KillTriggerAndResetStillInvalidatesThePreparedAuthorization) {
    arm_mutation(PreparedMutation::KillTriggerAndReset);
    const auto result = run_refusal(Reason::ReadinessRevoked);
    EXPECT_EQ(kill_switch_.state(), hy::KillState::Normal);
    EXPECT_EQ(result.not_ready_reason, hy::NotReadyReason::EpochChanged);
}

TEST_F(SubmitFinalGateTest, RevokedAuthorityRefusesThePreparedOrder) {
    arm_mutation(PreparedMutation::AuthorityRevoke);
    const auto result = run_refusal(Reason::ReadinessRevoked);
    EXPECT_EQ(result.not_ready_reason, hy::NotReadyReason::EpochChanged);
}

TEST_F(SubmitFinalGateTest, DisconnectedFeedRefusesThePreparedOrder) {
    use_dynamic_readiness();
    arm_mutation(PreparedMutation::FeedDisconnect);
    const auto result = run_refusal(Reason::ReadinessRevoked);
    EXPECT_EQ(result.not_ready_reason, hy::NotReadyReason::FeedDown);
}

TEST_F(SubmitFinalGateTest, ChangedFeedGenerationRequiresANewSweepBeforeThePreparedOrderCanProceed) {
    use_dynamic_readiness();
    arm_mutation(PreparedMutation::FeedGenerationChange);
    const auto result = run_refusal(Reason::ReadinessRevoked);
    EXPECT_EQ(result.not_ready_reason, hy::NotReadyReason::BarrierSweeping);
}

TEST_F(SubmitFinalGateTest, InvalidSigningClockRefusesThePreparedOrder) {
    use_dynamic_readiness();
    arm_mutation(PreparedMutation::FeedClockNotReady);
    const auto result = run_refusal(Reason::ReadinessRevoked);
    EXPECT_EQ(result.not_ready_reason, hy::NotReadyReason::ClockNotReady);
}

TEST_F(SubmitFinalGateTest, TtlIsCheckedAfterTheFinalReadinessProbeRuns) {
    use_dynamic_readiness();
    arm_mutation(PreparedMutation::AdvanceClockDuringFinalProbe, 110);
    const auto result = run_refusal(Reason::AuthorizationExpired);
    EXPECT_EQ(result.not_ready_reason, hy::NotReadyReason::FinalCheckFailed);
    EXPECT_EQ(clock_.now_ms, 110);
}

TEST_F(SubmitFinalGateTest, ContextKillSwitchRevisionIsCheckedSeparatelyFromReadinessKillSwitch) {
    ctx_.readiness = &independent_readiness_.readiness();
    arm_mutation(PreparedMutation::ContextKillTriggerAndReset);
    const auto result = run_refusal(Reason::ReadinessRevoked);
    EXPECT_EQ(kill_switch_.state(), hy::KillState::Normal);
    EXPECT_EQ(result.not_ready_reason, hy::NotReadyReason::FinalCheckFailed);
}

TEST_F(SubmitFinalGateTest, FinalAuditAvailabilityIsRecheckedAfterPrepared) {
    arm_mutation(PreparedMutation::AuditUnavailable);
    const auto result = run_refusal(Reason::PreconditionsChanged);
    EXPECT_EQ(result.not_ready_reason, hy::NotReadyReason::FinalCheckFailed);
    EXPECT_FALSE(hy::can_submit_order(audit_));
}

TEST_F(SubmitFinalGateTest, FinalSignerReadinessIsRecheckedAfterPrepared) {
    arm_mutation(PreparedMutation::SignerUnavailable);
    const auto result = run_refusal(Reason::PreconditionsChanged);
    EXPECT_EQ(result.not_ready_reason, hy::NotReadyReason::FinalCheckFailed);
}

TEST_F(SubmitFinalGateTest, FinalDepthSynchronizationIsRecheckedAfterPrepared) {
    arm_mutation(PreparedMutation::DepthUnavailable);
    const auto result = run_refusal(Reason::PreconditionsChanged);
    EXPECT_EQ(result.not_ready_reason, hy::NotReadyReason::FinalCheckFailed);
}

TEST_F(SubmitFinalGateTest, FinalRulesVersionIsRecheckedAfterPrepared) {
    arm_mutation(PreparedMutation::ChangeRulesVersion);
    const auto result = run_refusal(Reason::PreconditionsChanged);
    EXPECT_EQ(result.not_ready_reason, hy::NotReadyReason::FinalCheckFailed);
}

TEST_F(SubmitFinalGateTest, AuthorizationTtlExpiresAtTheInclusiveBoundary) {
    arm_mutation(PreparedMutation::AdvanceClock, 110);
    const auto result = run_refusal(Reason::AuthorizationExpired);
    EXPECT_EQ(result.final_refusal, Reason::AuthorizationExpired);
}

TEST_F(SubmitFinalGateTest, AuthorizationOneMillisecondInsideTtlSubmitsOnceWithoutClosure) {
    arm_mutation(PreparedMutation::AdvanceClock, 109);
    const auto result = hy::orchestrate_submit(ctx_);
    EXPECT_TRUE(mutation_.invoked);
    EXPECT_EQ(result.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_FALSE(result.final_refusal.has_value());
    EXPECT_EQ(result.closure, ClosureAppendStatus::NotAttempted);
    EXPECT_EQ(fake_audit_.closure_calls, 0u);
    EXPECT_EQ(submit_calls_, 1u);
    EXPECT_EQ(fake_audit_.trace_count, 3u);
    EXPECT_EQ(fake_audit_.trace[0].record.event_type, AuditEventType::OrderIntentCreated);
    EXPECT_EQ(fake_audit_.trace[1].record.event_type, AuditEventType::OrderSubmitPrepared);
    EXPECT_EQ(fake_audit_.trace[2].record.event_type, AuditEventType::OrderAccepted);
}

TEST_F(SubmitFinalGateTest, FinalSubmitUsesTheImmutablePreparedOrderAfterContextMutation) {
    arm_mutation(PreparedMutation::MutateContextOrder);
    const auto result = hy::orchestrate_submit(ctx_);
    EXPECT_EQ(result.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(result.closure, ClosureAppendStatus::NotAttempted);
    EXPECT_EQ(submit_calls_, 1u);
    EXPECT_EQ(last_submit_symbol_, 1u);
    EXPECT_EQ(last_submit_side_, hy::OrderSide::Buy);
    EXPECT_EQ(last_submit_type_, hy::OrderType::Limit);
    EXPECT_EQ(last_submit_price_ticks_, 1000);
    EXPECT_EQ(last_submit_qty_ticks_, 10);
    EXPECT_EQ(fake_audit_.trace[1].record.symbol_id, 1u);
    EXPECT_EQ(fake_audit_.trace[1].record.side, hy::OrderSide::Buy);
    EXPECT_EQ(fake_audit_.trace[1].record.price_ticks, 1000);
    EXPECT_EQ(fake_audit_.trace[1].record.qty_ticks, 10);
    EXPECT_EQ(fake_audit_.trace[2].record.symbol_id, 1u);
    EXPECT_EQ(fake_audit_.trace[2].record.side, hy::OrderSide::Buy);
}

TEST_F(SubmitFinalGateTest, ConfirmationExpiryAfterPreparedWritesClassBClosure) {
    ctx_.confirmation.valid_until_ms = ctx_.now_ms + 1;
    arm_mutation(PreparedMutation::AdvanceClock, 12);
    const auto result = run_refusal(Reason::ConfirmationExpired);
    EXPECT_EQ(result.final_refusal, Reason::ConfirmationExpired);
}

TEST_F(SubmitFinalGateTest, ConfirmationExpiryIsInclusiveAtTheFinalWallClockBoundary) {
    ctx_.confirmation.valid_until_ms = ctx_.now_ms + 1;
    arm_mutation(PreparedMutation::AdvanceClock, 11);
    const auto result = hy::orchestrate_submit(ctx_);
    EXPECT_EQ(result.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(result.closure, ClosureAppendStatus::NotAttempted);
    EXPECT_EQ(submit_calls_, 1u);
    EXPECT_EQ(fake_audit_.closure_calls, 0u);
}

TEST_F(SubmitFinalGateTest, ReusedSlotGenerationCannotBeReleasedByTheOldPreparedOrder) {
    arm_mutation(PreparedMutation::LoseAndReuseSlot);
    const auto result = run_refusal(Reason::SlotOwnershipLost);
    EXPECT_TRUE(mutation_.replacement_registered);
    EXPECT_TRUE(in_flight_.is_in_flight("HY-S5-REUSED"));
    EXPECT_EQ(in_flight_.count(), 1u);
    EXPECT_EQ(fill_context_.count(), 0u);
    EXPECT_TRUE(rate_limiter_.can_send_order(hy::RateLimitLane::Strategy, 1));
    EXPECT_EQ(result.order.state, OrderState::NeverSent);
}

TEST_F(SubmitFinalGateTest, ReconfiguredLimiterInvalidatesTheHeldReservation) {
    arm_mutation(PreparedMutation::ReconfigureLimiter);
    const auto result = run_refusal(Reason::ReservationLost);
    EXPECT_EQ(result.final_refusal, Reason::ReservationLost);
    EXPECT_TRUE(mutation_.limiter_configured);
}

TEST_F(SubmitFinalGateTest, ChangedAccountSnapshotRefusesThePreparedOrder) {
    arm_mutation(PreparedMutation::ChangeAccount);
    const auto result = run_refusal(Reason::AccountChanged);
    EXPECT_EQ(result.final_refusal, Reason::AccountChanged);
}

TEST_F(SubmitFinalGateTest, FailedClosureRetainsSubmittingSlotFillContextAndRateReservation) {
    arm_mutation(PreparedMutation::AuthorityRevoke);
    (void)run_refusal(Reason::ReadinessRevoked, ClosureAppendStatus::Failed);
}

TEST_F(SubmitFinalGateTest, NotEnabledClosureRetainsSubmittingResources) {
    arm_mutation(PreparedMutation::AuthorityRevoke);
    (void)run_refusal(Reason::ReadinessRevoked, ClosureAppendStatus::NotEnabled);
}

TEST_F(SubmitFinalGateTest, NotWiredClosureRetainsSubmittingResources) {
    arm_mutation(PreparedMutation::AuthorityRevoke);
    (void)run_refusal(Reason::ReadinessRevoked, ClosureAppendStatus::NotWired);
}

TEST_F(SubmitFinalGateTest, RefusedClosureRetainsSubmittingResources) {
    arm_mutation(PreparedMutation::AuthorityRevoke);
    (void)run_refusal(Reason::ReadinessRevoked, ClosureAppendStatus::Refused);
}

TEST_F(SubmitFinalGateTest, FailedPreparedAppendDoesNotClaimAuthorizationOrWriteClosure) {
    arm_mutation(PreparedMutation::AuthorityRevoke);
    fake_audit_.prepared_should_fail = true;
    const auto epoch_before = readiness_.readiness().authority().epoch();
    const auto result = hy::orchestrate_submit(ctx_);
    EXPECT_EQ(result.gate, OrchestratorGate::AuditWriteNotAcked);
    EXPECT_EQ(result.order.state, OrderState::Intent);
    EXPECT_EQ(result.closure, ClosureAppendStatus::NotAttempted);
    EXPECT_FALSE(mutation_.invoked);
    EXPECT_EQ(readiness_.readiness().authority().epoch(), epoch_before);
    EXPECT_EQ(fake_audit_.durable_append_calls, 2u);
    EXPECT_EQ(fake_audit_.closure_calls, 0u);
    EXPECT_EQ(submit_calls_, 0u);
    EXPECT_EQ(in_flight_.count(), 1u);
    EXPECT_EQ(fill_context_.count(), 1u);
    EXPECT_FALSE(rate_limiter_.can_send_order(hy::RateLimitLane::Strategy, 1));
}

TEST_F(SubmitFinalGateTest, SuccessfulSubmitDoesNotAllocate) {
    arm_mutation(PreparedMutation::None);
    OrchestratorResult accepted{};
    const alloc_probe::Window window;
    accepted = hy::orchestrate_submit(ctx_);
    EXPECT_EQ(accepted.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(window.calls(), 0u);
}

TEST_F(SubmitFinalGateTest, FinalRefusalDoesNotAllocate) {
    arm_mutation(PreparedMutation::AuthorityRevoke);
    OrchestratorResult refused{};
    const alloc_probe::Window window;
    refused = hy::orchestrate_submit(ctx_);
    EXPECT_EQ(refused.gate, OrchestratorGate::FinalAuthorizationRefused);
    EXPECT_EQ(refused.closure, ClosureAppendStatus::Appended);
    EXPECT_EQ(window.calls(), 0u);
}

class SubmitFinalGateDurableTest : public SubmitFinalGateTest {
protected:
    void SetUp() override {
        SubmitFinalGateTest::SetUp();
        std::array<std::byte, hy::kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x30U + i);
        key_ring_ = std::make_unique<hy::KeyRing>(kek);
        std::array<std::byte, 24> plaintext_key{};
        for (std::size_t i = 0; i < plaintext_key.size(); ++i) plaintext_key[i] = static_cast<std::byte>(0x60U + i);
        hy::WrappedKeyRecord wrapped{};
        ASSERT_EQ(key_ring_->add_key(1, plaintext_key, wrapped), hy::KeyRingAddStatus::Ok);
    }

    void open_sink(std::uint64_t max_frames) {
        sink_ = std::make_unique<hy::DurableAuditSink>(
            log_path_, *key_ring_, 1, hy::kMaxIntentOrphans, hy::AuditSinkOpenMode::Trading,
            hy::NeverSentWrites::Enabled, max_frames);
        ASSERT_TRUE(sink_->is_open());
        ASSERT_FALSE(sink_->fenced());
        hooked_audit_.wire(*sink_, mutation_);
        ctx_.durable_audit = hooked_audit_.port();
    }

    hy::test_helpers::TempDirectory scratch_{};
    std::string log_path_{scratch_.file("orders.log")};
    std::unique_ptr<hy::KeyRing> key_ring_{};
    std::unique_ptr<hy::DurableAuditSink> sink_{};
    HookedDurableAudit hooked_audit_{};
};

TEST_F(SubmitFinalGateDurableTest, AcknowledgedClassBClosureIsRecoveredAsNeverSent) {
    open_sink(8);
    arm_mutation(PreparedMutation::AuthorityRevoke);
    const auto result = hy::orchestrate_submit(ctx_);
    ASSERT_EQ(result.gate, OrchestratorGate::FinalAuthorizationRefused);
    ASSERT_EQ(result.final_refusal, Reason::ReadinessRevoked);
    ASSERT_EQ(result.closure, ClosureAppendStatus::Appended);
    ASSERT_EQ(result.order.state, OrderState::NeverSent);
    EXPECT_EQ(sink_->frame_count(), 3u);
    EXPECT_EQ(submit_calls_, 0u);
    EXPECT_EQ(in_flight_.count(), 0u);
    EXPECT_EQ(fill_context_.count(), 0u);

    sink_.reset();
    open_sink(8);
    EXPECT_EQ(sink_->recovery_status(), hy::RecoveryScanStatus::Clean);
    EXPECT_EQ(sink_->recovered_never_sent(), 1u);
    EXPECT_EQ(sink_->recovered_intent_orphans(), 0u);
    EXPECT_TRUE(sink_->recovered_checkpoints().empty());
}

TEST_F(SubmitFinalGateDurableTest, FailedClassBClosureKeepsPreparedOrderForRecovery) {
    open_sink(2);
    arm_mutation(PreparedMutation::AuthorityRevoke);
    const auto result = hy::orchestrate_submit(ctx_);
    ASSERT_EQ(result.gate, OrchestratorGate::FinalAuthorizationRefused);
    ASSERT_EQ(result.final_refusal, Reason::ReadinessRevoked);
    ASSERT_EQ(result.closure, ClosureAppendStatus::Failed);
    ASSERT_EQ(result.order.state, OrderState::Submitting);
    EXPECT_EQ(sink_->frame_count(), 2u);
    EXPECT_TRUE(sink_->fenced());
    EXPECT_EQ(sink_->fence_reason(), hy::FenceReason::LogLimit);
    EXPECT_EQ(submit_calls_, 0u);
    EXPECT_EQ(in_flight_.count(), 1u);
    EXPECT_EQ(fill_context_.count(), 1u);
    EXPECT_FALSE(rate_limiter_.can_send_order(hy::RateLimitLane::Strategy, 1));

    sink_.reset();
    open_sink(2);
    ASSERT_EQ(sink_->recovery_status(), hy::RecoveryScanStatus::Recovered);
    EXPECT_EQ(sink_->recovered_never_sent(), 0u);
    EXPECT_EQ(sink_->recovered_intent_orphans(), 0u);
    ASSERT_EQ(sink_->recovered_checkpoints().size(), 1u);
    EXPECT_EQ(sink_->recovered_checkpoints()[0].resulting_state, OrderState::Ambiguous);
}

}  // namespace
