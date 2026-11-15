// Q23 S6 class A: an acknowledged Intent refused before Prepared is either closed as NeverSent or remains an Intent orphan.
#include "test_helpers/alloc_probe.hpp"
#include "test_helpers/never_sent_fixture.hpp"
#include "test_helpers/temp_directory.hpp"

#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/order_attempt_watermark.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
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

struct AllocationCounts {
    std::size_t calls{0};
    std::size_t bytes{0};
};

template <class Fn>
AllocationCounts count_allocations(Fn&& fn) {
    const alloc_probe::Window window;
    fn();
    return {window.calls(), window.bytes()};
}

struct RegistryMutation {
    hy::InFlightRegistry* registry{nullptr};
    bool filled{false};
};

void fill_registry_after_intent(void* user_data, const AuditRecord&) noexcept {
    auto& mutation = *static_cast<RegistryMutation*>(user_data);
    mutation.filled = true;
    for (std::size_t i = 0; i < hy::kMaxInFlight; ++i) {
        char id[16]{};
        const int written = std::snprintf(id, sizeof(id), "HY-CAP%02u", static_cast<unsigned>(i));
        if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(id) ||
            !mutation.registry->register_submit(std::string_view(id))) {
            mutation.filled = false;
            return;
        }
    }
}

struct LimiterMutation {
    hy::SpotRateLimitTracker* limiter{nullptr};
    bool configured{false};
    bool consumed{false};
};

void exhaust_limiter_after_intent(void* user_data, const AuditRecord&) noexcept {
    auto& mutation = *static_cast<LimiterMutation*>(user_data);
    mutation.configured = mutation.limiter->configure(1, 0, 1, 0, 1, 0, hy::RateLimitBudgetSplit{100, 0, 0});
    mutation.consumed = mutation.limiter->try_reserve_order(hy::RateLimitLane::Strategy, 1);
}

void expect_class_a_trace(const FakeAudit& fake, const hy::OrchestratorContext& ctx, const hy::OrderRecord& order,
                          Reason reason,
                          ClosureAppendStatus result_status, bool closure_function_wired = true) {
    const OrderState expected_state = result_status == ClosureAppendStatus::Appended ? OrderState::NeverSent
                                                                                     : OrderState::Intent;
    EXPECT_EQ(order.state, expected_state);
    EXPECT_EQ(fake.durable_append_calls, 1u);
    EXPECT_EQ(fake.closure_calls, closure_function_wired ? 1u : 0u);
    ASSERT_EQ(fake.trace_count, closure_function_wired ? 2u : 1u);

    ASSERT_GE(fake.trace_count, 1u);
    const auto& intent_entry = fake.trace[0];
    EXPECT_EQ(intent_entry.kind, TraceKind::DurableAppend);
    EXPECT_EQ(intent_entry.durable_status, hy::AuditAppendResult::Status::Acked);
    EXPECT_EQ(intent_entry.record.event_type, AuditEventType::OrderIntentCreated);
    EXPECT_EQ(intent_entry.record.resulting_state, OrderState::Intent);
    EXPECT_EQ(intent_entry.record.timestamp_ms, ctx.now_ms);
    EXPECT_EQ(intent_entry.record.symbol_id, ctx.symbol_id);
    EXPECT_EQ(intent_entry.record.side, ctx.side);
    EXPECT_EQ(intent_entry.record.mode, ctx.mode);
    EXPECT_EQ(intent_entry.record.price_ticks, ctx.price_ticks);
    EXPECT_EQ(intent_entry.record.qty_ticks, ctx.qty_ticks);
    EXPECT_EQ(intent_entry.record.exchange_order_id, 0);
    EXPECT_EQ(intent_entry.record.filled_qty_ticks, 0);
    EXPECT_EQ(intent_entry.record.avg_fill_price_ticks, 0);

    if (!closure_function_wired) {
        EXPECT_EQ(result_status, ClosureAppendStatus::NotWired);
        return;
    }

    const auto& closure_entry = fake.trace[1];
    EXPECT_EQ(closure_entry.kind, TraceKind::ClosureAppend);
    EXPECT_EQ(closure_entry.closure_status, result_status);
    EXPECT_EQ(closure_entry.now_ms, ctx.now_ms);
    if (result_status == ClosureAppendStatus::Appended) {
        EXPECT_EQ(closure_entry.closure_append.status, hy::AuditAppendResult::Status::Acked);
    } else if (result_status == ClosureAppendStatus::Failed) {
        EXPECT_EQ(closure_entry.closure_append.status, hy::AuditAppendResult::Status::Failed);
    }

    const AuditRecord& intent = intent_entry.record;
    const AuditRecord& closure = closure_entry.record;
    EXPECT_EQ(closure.timestamp_ms, ctx.now_ms);
    EXPECT_EQ(closure.event_type, AuditEventType::OrderNeverSent);
    EXPECT_EQ(closure.resulting_state, OrderState::NeverSent);
    EXPECT_STREQ(closure.client_order_id, intent.client_order_id);
    EXPECT_EQ(closure.symbol_id, intent.symbol_id);
    EXPECT_EQ(closure.side, intent.side);
    EXPECT_EQ(closure.mode, intent.mode);
    EXPECT_EQ(closure.price_ticks, intent.price_ticks);
    EXPECT_EQ(closure.qty_ticks, intent.qty_ticks);
    EXPECT_EQ(closure.exchange_order_id, 0);
    EXPECT_EQ(closure.filled_qty_ticks, 0);
    EXPECT_EQ(closure.avg_fill_price_ticks, 0);
    EXPECT_EQ(closure.detail_code, static_cast<std::int64_t>(reason));
    EXPECT_STREQ(closure.detail_msg, "");

    const auto& target = closure_entry.target;
    EXPECT_EQ(target.state, OrderState::Intent);
    EXPECT_EQ(target.exchange_order_id, 0);
    EXPECT_EQ(target.filled_qty_ticks, 0);
    EXPECT_EQ(target.symbol_id, intent.symbol_id);
    EXPECT_EQ(target.side, intent.side);
    EXPECT_EQ(target.mode, intent.mode);
    EXPECT_EQ(target.price_ticks, intent.price_ticks);
    EXPECT_EQ(target.qty_ticks, intent.qty_ticks);
}

}  // namespace

class NeverSentDurableLogLimitTest : public NeverSentOrchestratorFixture {
protected:
    hy::test_helpers::TempDirectory scratch_{};
    std::string log_path_{scratch_.file("orders.log")};
    std::unique_ptr<hy::KeyRing> key_ring_{};
    std::unique_ptr<hy::DurableAuditSink> sink_{};

    void SetUp() override {
        NeverSentOrchestratorFixture::SetUp();
        std::array<std::byte, hy::kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) {
            kek[i] = static_cast<std::byte>(0x40U + static_cast<unsigned int>(i));
        }
        key_ring_ = std::make_unique<hy::KeyRing>(kek);
        std::array<std::byte, 24> plaintext_key{};
        for (std::size_t i = 0; i < plaintext_key.size(); ++i) {
            plaintext_key[i] = static_cast<std::byte>(0x70U + static_cast<unsigned int>(i));
        }
        hy::WrappedKeyRecord wrapped{};
        ASSERT_EQ(key_ring_->add_key(1, plaintext_key, wrapped), hy::KeyRingAddStatus::Ok);
    }

    void open_sink(std::uint64_t max_order_frames) {
        sink_ = std::make_unique<hy::DurableAuditSink>(log_path_, *key_ring_, 1, hy::kMaxIntentOrphans,
                                                       hy::AuditSinkOpenMode::Trading,
                                                       hy::NeverSentWrites::Enabled, max_order_frames);
        ASSERT_TRUE(sink_->is_open());
        ASSERT_FALSE(sink_->fenced());
        ctx_.durable_audit = hy::make_durable_order_audit_port(*sink_);
    }

    void set_refusal_attempt(std::uint32_t sequence) noexcept {
        ctx_.sequence = sequence;
        ctx_.confirmation.confirmed = false;
    }

    std::uintmax_t log_size() const { return std::filesystem::file_size(log_path_); }
};

TEST_F(NeverSentDurableLogLimitTest, FourFrameLimitStopsBeforeTheThirdIntentAndRestartsCleanly) {
    constexpr std::uint64_t kFrameLimit = 4;
    open_sink(kFrameLimit);

    for (std::uint32_t sequence = 1; sequence <= 2; ++sequence) {
        set_refusal_attempt(sequence);
        const OrchestratorResult result = hy::orchestrate_submit(ctx_);
        ASSERT_EQ(result.gate, OrchestratorGate::OperatorNotConfirmed);
        ASSERT_EQ(result.closure, ClosureAppendStatus::Appended);
        EXPECT_EQ(result.order.state, OrderState::NeverSent);
        EXPECT_EQ(submit_calls_, 0u);
    }
    ASSERT_EQ(sink_->frame_count(), kFrameLimit);
    ASSERT_EQ(log_size(), static_cast<std::uintmax_t>(kFrameLimit * hy::kOrderEventFrameSize));

    set_refusal_attempt(3);
    const OrchestratorResult third = hy::orchestrate_submit(ctx_);
    EXPECT_EQ(third.gate, OrchestratorGate::AuditWriteNotAcked);
    EXPECT_EQ(third.order.state, OrderState::Intent);
    EXPECT_EQ(third.closure, ClosureAppendStatus::NotAttempted);
    EXPECT_EQ(sink_->frame_count(), kFrameLimit);
    EXPECT_TRUE(sink_->fenced());
    EXPECT_EQ(sink_->fence_reason(), hy::FenceReason::LogLimit);
    EXPECT_EQ(submit_calls_, 0u);

    sink_.reset();
    open_sink(kFrameLimit);
    EXPECT_EQ(sink_->recovery_status(), hy::RecoveryScanStatus::Clean);
    EXPECT_FALSE(sink_->fenced());
    EXPECT_EQ(sink_->frame_count(), kFrameLimit);
    EXPECT_EQ(sink_->recovered_never_sent(), 2u);
    EXPECT_EQ(sink_->recovered_intent_orphans(), 0u);
    EXPECT_TRUE(sink_->recovered_checkpoints().empty());
}

TEST_F(NeverSentDurableLogLimitTest, FiveFrameLimitKeepsThirdIntentWhenItsClosureHitsTheLimit) {
    constexpr std::uint64_t kFrameLimit = 5;
    open_sink(kFrameLimit);

    for (std::uint32_t sequence = 1; sequence <= 3; ++sequence) {
        set_refusal_attempt(sequence);
        const OrchestratorResult result = hy::orchestrate_submit(ctx_);
        ASSERT_EQ(result.gate, OrchestratorGate::OperatorNotConfirmed);
        if (sequence < 3) {
            ASSERT_EQ(result.closure, ClosureAppendStatus::Appended);
            EXPECT_EQ(result.order.state, OrderState::NeverSent);
        } else {
            EXPECT_EQ(result.closure, ClosureAppendStatus::Failed);
            EXPECT_EQ(result.order.state, OrderState::Intent);
            EXPECT_TRUE(ctx_.recently_closed.contains(result.order.client_order_id.view()));
        }
        EXPECT_EQ(submit_calls_, 0u);
    }
    ASSERT_EQ(sink_->frame_count(), kFrameLimit);
    ASSERT_EQ(log_size(), static_cast<std::uintmax_t>(kFrameLimit * hy::kOrderEventFrameSize));
    EXPECT_TRUE(sink_->fenced());
    EXPECT_EQ(sink_->fence_reason(), hy::FenceReason::LogLimit);

    sink_.reset();
    open_sink(kFrameLimit);
    EXPECT_EQ(sink_->recovery_status(), hy::RecoveryScanStatus::Clean);
    EXPECT_FALSE(sink_->fenced());
    EXPECT_EQ(sink_->frame_count(), kFrameLimit);
    EXPECT_EQ(sink_->recovered_never_sent(), 2u);
    EXPECT_EQ(sink_->recovered_intent_orphans(), 1u);
    EXPECT_TRUE(sink_->recovered_checkpoints().empty());
}

TEST_F(NeverSentOrchestratorFixture, MissingConfirmationClosesTheAcknowledgedIntent) {
    ctx_.confirmation.confirmed = false;
    OrchestratorResult result{};
    const AllocationCounts allocations = count_allocations([&] { result = hy::orchestrate_submit(ctx_); });

    EXPECT_EQ(result.gate, OrchestratorGate::OperatorNotConfirmed);
    EXPECT_EQ(result.order.state, OrderState::NeverSent);
    EXPECT_EQ(result.closure, ClosureAppendStatus::Appended);
    EXPECT_EQ(submit_calls_, 0u);
    EXPECT_EQ(in_flight_.count(), 0u);
    EXPECT_EQ(allocations.calls, 0u);
    expect_class_a_trace(fake_audit_, ctx_, result.order, Reason::ConfirmationMissing, ClosureAppendStatus::Appended);
}

TEST_F(NeverSentOrchestratorFixture, ConfirmationMismatchClosesTheAcknowledgedIntent) {
    ctx_.price_ticks = 2000;
    OrchestratorResult result{};
    const AllocationCounts allocations = count_allocations([&] { result = hy::orchestrate_submit(ctx_); });

    EXPECT_EQ(result.gate, OrchestratorGate::ConfirmationMismatch);
    EXPECT_EQ(result.closure, ClosureAppendStatus::Appended);
    EXPECT_EQ(submit_calls_, 0u);
    EXPECT_EQ(in_flight_.count(), 0u);
    EXPECT_EQ(allocations.calls, 0u);
    expect_class_a_trace(fake_audit_, ctx_, result.order, Reason::ConfirmationMismatch, ClosureAppendStatus::Appended);
}

TEST_F(NeverSentOrchestratorFixture, InvalidSubmitPortClosesTheAcknowledgedIntent) {
    ctx_.submit_port.fn = nullptr;
    OrchestratorResult result{};
    const AllocationCounts allocations = count_allocations([&] { result = hy::orchestrate_submit(ctx_); });

    EXPECT_EQ(result.gate, OrchestratorGate::SubmitPortInvalid);
    EXPECT_EQ(result.closure, ClosureAppendStatus::Appended);
    EXPECT_EQ(submit_calls_, 0u);
    EXPECT_EQ(in_flight_.count(), 0u);
    EXPECT_EQ(allocations.calls, 0u);
    expect_class_a_trace(fake_audit_, ctx_, result.order, Reason::SubmitPortInvalid, ClosureAppendStatus::Appended);
}

TEST_F(NeverSentOrchestratorFixture, MissingInFlightRegistryWritesItsClassAClosure) {
    ctx_.in_flight = nullptr;
    OrchestratorResult result{};
    const AllocationCounts allocations = count_allocations([&] { result = hy::orchestrate_submit(ctx_); });

    EXPECT_EQ(result.gate, OrchestratorGate::InFlightRegistryUnavailable);
    EXPECT_EQ(result.closure, ClosureAppendStatus::Appended);
    EXPECT_EQ(submit_calls_, 0u);
    EXPECT_EQ(allocations.calls, 0u);
    expect_class_a_trace(fake_audit_, ctx_, result.order, Reason::InFlightRegistryUnavailable, ClosureAppendStatus::Appended);
}

TEST_F(NeverSentOrchestratorFixture, LimiterExhaustedDuringIntentAckIsClosedAsNeverSent) {
    LimiterMutation mutation{&rate_limiter_};
    fake_audit_.after_intent_ack = &exhaust_limiter_after_intent;
    fake_audit_.hook_user_data = &mutation;
    OrchestratorResult result{};
    const AllocationCounts allocations = count_allocations([&] { result = hy::orchestrate_submit(ctx_); });

    EXPECT_TRUE(mutation.configured);
    EXPECT_TRUE(mutation.consumed);
    EXPECT_EQ(result.gate, OrchestratorGate::RateLimitExhausted);
    EXPECT_EQ(result.closure, ClosureAppendStatus::Appended);
    EXPECT_EQ(submit_calls_, 0u);
    EXPECT_EQ(in_flight_.count(), 0u);
    EXPECT_TRUE(ctx_.recently_closed.contains(expected_coid().view()));
    EXPECT_EQ(allocations.calls, 0u);
    expect_class_a_trace(fake_audit_, ctx_, result.order, Reason::RateLimitExhausted, ClosureAppendStatus::Appended);
}

TEST_F(NeverSentOrchestratorFixture, RegistryFilledDuringIntentAckRejectsBeforeSubmit) {
    RegistryMutation mutation{&in_flight_};
    fake_audit_.after_intent_ack = &fill_registry_after_intent;
    fake_audit_.hook_user_data = &mutation;
    OrchestratorResult result{};
    const AllocationCounts allocations = count_allocations([&] { result = hy::orchestrate_submit(ctx_); });

    EXPECT_TRUE(mutation.filled);
    EXPECT_EQ(in_flight_.count(), hy::kMaxInFlight);
    EXPECT_FALSE(in_flight_.is_in_flight(expected_coid().view()));
    EXPECT_EQ(result.gate, OrchestratorGate::DuplicateInFlight);
    EXPECT_EQ(result.closure, ClosureAppendStatus::Appended);
    EXPECT_EQ(submit_calls_, 0u);
    EXPECT_EQ(allocations.calls, 0u);
    expect_class_a_trace(fake_audit_, ctx_, result.order, Reason::DuplicateInFlight, ClosureAppendStatus::Appended);
}

TEST_F(NeverSentOrchestratorFixture, NonAppendedClosureStatusesPreserveTheOriginalRefusalAndIntentState) {
    constexpr std::array<ClosureAppendStatus, 4> statuses{
        ClosureAppendStatus::NotEnabled,
        ClosureAppendStatus::NotWired,
        ClosureAppendStatus::Refused,
        ClosureAppendStatus::Failed,
    };

    ctx_.confirmation.confirmed = false;
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        if (i != 0) fake_audit_ = FakeAudit{};
        fake_audit_.wire(ctx_.durable_audit);
        fake_audit_.closure_status = statuses[i];
        ctx_.sequence = static_cast<std::uint32_t>(i + 1);
        if (statuses[i] == ClosureAppendStatus::NotWired) ctx_.durable_audit.closure_fn = nullptr;

        OrchestratorResult result{};
        const AllocationCounts allocations = count_allocations([&] { result = hy::orchestrate_submit(ctx_); });

        EXPECT_EQ(result.gate, OrchestratorGate::OperatorNotConfirmed);
        EXPECT_EQ(result.closure, statuses[i]);
        EXPECT_EQ(result.order.state, OrderState::Intent);
        EXPECT_EQ(submit_calls_, 0u);
        EXPECT_EQ(allocations.calls, 0u);
        expect_class_a_trace(fake_audit_, ctx_, result.order, Reason::ConfirmationMissing, statuses[i],
                             statuses[i] != ClosureAppendStatus::NotWired);
    }
}

TEST_F(NeverSentOrchestratorFixture, IntentAppendFailureNeverAttemptsClosure) {
    fake_audit_.intent_should_fail = true;
    const OrchestratorResult result = hy::orchestrate_submit(ctx_);

    EXPECT_EQ(result.gate, OrchestratorGate::AuditWriteNotAcked);
    EXPECT_EQ(result.closure, ClosureAppendStatus::NotAttempted);
    EXPECT_EQ(fake_audit_.durable_append_calls, 1u);
    EXPECT_EQ(fake_audit_.trace_count, 1u);
    EXPECT_EQ(fake_audit_.trace[0].record.event_type, AuditEventType::OrderIntentCreated);
    EXPECT_EQ(fake_audit_.trace[0].durable_status, hy::AuditAppendResult::Status::Failed);
    EXPECT_EQ(fake_audit_.closure_calls, 0u);
    EXPECT_EQ(submit_calls_, 0u);
}

TEST_F(NeverSentOrchestratorFixture, EarlyRefusalNeverWritesIntentOrClosure) {
    audit_.set_available(false);
    const OrchestratorResult result = hy::orchestrate_submit(ctx_);

    EXPECT_EQ(result.gate, OrchestratorGate::AuditUnavailable);
    EXPECT_EQ(result.closure, ClosureAppendStatus::NotAttempted);
    EXPECT_EQ(fake_audit_.durable_append_calls, 0u);
    EXPECT_EQ(fake_audit_.closure_calls, 0u);
    EXPECT_EQ(fake_audit_.trace_count, 0u);
    EXPECT_EQ(submit_calls_, 0u);
}

TEST_F(NeverSentOrchestratorFixture, FailedPreparedAppendNeverAttemptsClassAClosure) {
    fake_audit_.prepared_should_fail = true;
    const OrchestratorResult result = hy::orchestrate_submit(ctx_);

    EXPECT_EQ(result.gate, OrchestratorGate::AuditWriteNotAcked);
    EXPECT_EQ(result.closure, ClosureAppendStatus::NotAttempted);
    EXPECT_EQ(fake_audit_.durable_append_calls, 2u);
    EXPECT_EQ(fake_audit_.trace_count, 2u);
    EXPECT_EQ(fake_audit_.trace[0].record.event_type, AuditEventType::OrderIntentCreated);
    EXPECT_EQ(fake_audit_.trace[1].record.event_type, AuditEventType::OrderSubmitPrepared);
    EXPECT_EQ(fake_audit_.trace[1].durable_status, hy::AuditAppendResult::Status::Failed);
    EXPECT_EQ(fake_audit_.closure_calls, 0u);
    EXPECT_EQ(submit_calls_, 0u);
}

TEST_F(NeverSentOrchestratorFixture, APreparedAndSubmittedOrderNeverGetsAClassAClosure) {
    const OrchestratorResult result = hy::orchestrate_submit(ctx_);

    EXPECT_EQ(result.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(result.closure, ClosureAppendStatus::NotAttempted);
    EXPECT_EQ(fake_audit_.durable_append_calls, 3u);
    EXPECT_EQ(fake_audit_.trace[0].record.event_type, AuditEventType::OrderIntentCreated);
    EXPECT_EQ(fake_audit_.trace[1].record.event_type, AuditEventType::OrderSubmitPrepared);
    EXPECT_EQ(fake_audit_.trace[1].durable_status, hy::AuditAppendResult::Status::Acked);
    EXPECT_EQ(fake_audit_.trace[2].record.event_type, AuditEventType::OrderAccepted);
    EXPECT_EQ(fake_audit_.closure_calls, 0u);
    EXPECT_EQ(submit_calls_, 1u);
}

TEST_F(NeverSentOrchestratorFixture, RecentlyClosedCoidIsRejectedBeforeAnotherIntent) {
    ctx_.confirmation.confirmed = false;
    const OrchestratorResult first = hy::orchestrate_submit(ctx_);
    ASSERT_EQ(first.gate, OrchestratorGate::OperatorNotConfirmed);
    ASSERT_EQ(first.closure, ClosureAppendStatus::Appended);
    ASSERT_TRUE(ctx_.recently_closed.contains(expected_coid().view()));

    const OrchestratorResult retry = hy::orchestrate_submit(ctx_);

    EXPECT_EQ(retry.gate, OrchestratorGate::DuplicateInFlight);
    EXPECT_EQ(retry.closure, ClosureAppendStatus::NotAttempted);
    EXPECT_EQ(fake_audit_.durable_append_calls, 1u);
    EXPECT_EQ(fake_audit_.closure_calls, 1u);
    EXPECT_EQ(fake_audit_.trace_count, 2u);
    EXPECT_EQ(submit_calls_, 0u);
}

TEST_F(NeverSentOrchestratorFixture, FailedClosureAlsoProtectsItsCoidFromAnotherIntent) {
    ctx_.confirmation.confirmed = false;
    fake_audit_.closure_status = ClosureAppendStatus::Failed;
    const OrchestratorResult first = hy::orchestrate_submit(ctx_);
    ASSERT_EQ(first.gate, OrchestratorGate::OperatorNotConfirmed);
    ASSERT_EQ(first.closure, ClosureAppendStatus::Failed);
    ASSERT_TRUE(ctx_.recently_closed.contains(expected_coid().view()));

    const OrchestratorResult retry = hy::orchestrate_submit(ctx_);

    EXPECT_EQ(retry.gate, OrchestratorGate::DuplicateInFlight);
    EXPECT_EQ(retry.closure, ClosureAppendStatus::NotAttempted);
    EXPECT_EQ(fake_audit_.durable_append_calls, 1u);
    EXPECT_EQ(fake_audit_.closure_calls, 1u);
    EXPECT_EQ(fake_audit_.trace_count, 2u);
    EXPECT_EQ(submit_calls_, 0u);
}

TEST(OrderPathAllocation, TheProbeDetectsTheAllocationFormsUsedByTheHarness) {
    const alloc_probe::SelfCheck check = alloc_probe::selfcheck();
    EXPECT_EQ(check.after_array_new, 1u);
    EXPECT_EQ(check.after_nothrow_array_new, 2u);
    EXPECT_EQ(check.after_string_growth, 3u);
    EXPECT_EQ(check.outside_window, 0u);
}

TEST(OrderAttemptWatermark, SequenceOrderingIsNumericAcrossDigitWidths) {
    hy::OrderAttemptWatermark watermark;
    watermark.observe("HY-1000-9-0001");

    EXPECT_TRUE(watermark.permits("HY-1000-10-0001"));
    watermark.observe("HY-1000-10-0001");
    EXPECT_FALSE(watermark.permits("HY-1000-9-0001"));
}

TEST(OrderAttemptWatermark, TimestampTakesPriorityOverSequence) {
    hy::OrderAttemptWatermark watermark;
    watermark.observe("HY-1000-4294967295-0001");

    EXPECT_TRUE(watermark.permits("HY-1001-0-0001"));
    watermark.observe("HY-1001-0-0001");
    EXPECT_FALSE(watermark.permits("HY-1000-4294967295-0001"));
    EXPECT_FALSE(watermark.permits("HY-999-4294967295-0001"));
}

TEST(OrderAttemptWatermark, SameTimestampAndSequenceWithAnotherSymbolIsConservativelyRejected) {
    hy::OrderAttemptWatermark watermark;
    watermark.observe("HY-2000-9-0001");

    EXPECT_FALSE(watermark.permits("HY-2000-9-0002"));
    EXPECT_FALSE(watermark.permits("HY-2000-9-0001"));
}

TEST(OrderAttemptWatermark, InvalidGeneratedIdsAndLegacyIdsDoNotChangeTheWatermark) {
    hy::OrderAttemptWatermark watermark;
    watermark.observe("HY-3000-9-0001");
    watermark.observe("legacy-client-order-id");
    watermark.observe("HY-not-a-timestamp-0001");
    watermark.observe("HY-3000-4294967296-0001");

    EXPECT_TRUE(watermark.permits("legacy-client-order-id"));
    EXPECT_TRUE(watermark.permits("HY-not-a-timestamp-0001"));
    EXPECT_TRUE(watermark.permits("HY-3000-4294967296-0001"));
    EXPECT_FALSE(watermark.permits("HY-3000-9-0001"));
    EXPECT_TRUE(watermark.permits("HY-3000-10-0001"));
}

TEST(OrderAttemptWatermark, MaximumParsedValuesAndNumericOverflowAreHandled) {
    hy::OrderAttemptWatermark watermark;
    watermark.observe("HY-9223372036854775807-4294967295-FFFF");

    EXPECT_FALSE(watermark.permits("HY-9223372036854775807-4294967295-FFFF"));
    EXPECT_FALSE(watermark.permits("HY-9223372036854775807-4294967294-FFFF"));
    EXPECT_TRUE(watermark.permits("HY-9223372036854775808-0-0001"));
    EXPECT_TRUE(watermark.permits("HY-0-4294967296-FFFF"));
    EXPECT_TRUE(watermark.permits("legacy-attempt"));
}
