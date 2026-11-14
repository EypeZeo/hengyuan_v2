// Q23 S6: class A closures through the real orchestrator and durable sink.
#include <gtest/gtest.h>

#include "test_helpers/never_sent_fixture.hpp"
#include "test_helpers/temp_directory.hpp"

#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/live_submit_orchestrator.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

using namespace hy;

namespace {

constexpr std::uint32_t kKeyId = 1;
constexpr std::size_t kSmallOrphanBudget = 1;
constexpr std::size_t kClosedAttempts = 65;

std::array<std::byte, kKekSize> make_kek() {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) {
        kek[i] = static_cast<std::byte>(0x40U + static_cast<unsigned int>(i));
    }
    return kek;
}

std::array<std::byte, 24> make_plaintext_key() {
    std::array<std::byte, 24> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::byte>(0x70U + static_cast<unsigned int>(i));
    }
    return key;
}

class RestartedOrchestratorContext final : public test_helpers::never_sent::NeverSentOrchestratorFixture {
public:
    void initialize() { SetUp(); }

    void install_audit_port(DurableOrderAuditPort port) noexcept { ctx_.durable_audit = port; }

    void refuse_confirmation() noexcept { ctx_.confirmation.confirmed = false; }

    void choose_attempt(std::int64_t now_ms, std::uint32_t sequence) noexcept {
        ctx_.now_ms = now_ms;
        ctx_.sequence = sequence;
    }

    OrchestratorResult submit() { return orchestrate_submit(ctx_); }

private:
    void TestBody() override {}
};

}  // namespace

class NeverSentOrphanTest : public test_helpers::never_sent::NeverSentOrchestratorFixture {
protected:
    test_helpers::TempDirectory scratch_;
    std::string log_path_{scratch_.file("orders.log")};
    std::unique_ptr<KeyRing> key_ring_;
    std::unique_ptr<DurableAuditSink> sink_;

    void SetUp() override {
        test_helpers::never_sent::NeverSentOrchestratorFixture::SetUp();
        const auto kek = make_kek();
        key_ring_ = std::make_unique<KeyRing>(kek);
        const auto plain_key = make_plaintext_key();
        WrappedKeyRecord wrapped{};
        ASSERT_EQ(key_ring_->add_key(kKeyId, plain_key, wrapped), KeyRingAddStatus::Ok);
    }

    void open_sink(NeverSentWrites writes, std::size_t max_intent_orphans = kSmallOrphanBudget) {
        sink_ = std::make_unique<DurableAuditSink>(log_path_, *key_ring_, kKeyId, max_intent_orphans,
                                                   AuditSinkOpenMode::Trading, writes);
        ASSERT_TRUE(sink_->is_open());
        ASSERT_FALSE(sink_->fenced());
        ctx_.durable_audit = make_durable_order_audit_port(*sink_);
    }

    std::uintmax_t log_size() const { return std::filesystem::file_size(log_path_); }

    void set_class_a_refusal_attempt(std::int64_t now_ms, std::uint32_t sequence) noexcept {
        ctx_.now_ms = now_ms;
        ctx_.sequence = sequence;
        ctx_.confirmation.confirmed = false;
    }
};

TEST_F(NeverSentOrphanTest, RepeatedClosuresStayBelowSmallBudgetAndWatermarkSurvivesEvictionAndRestart) {
    open_sink(NeverSentWrites::Enabled);

    const std::int64_t initial_time = ctx_.now_ms;
    const std::uint32_t initial_sequence = ctx_.sequence;
    ClientOrderId first_id{};
    for (std::size_t i = 0; i < kClosedAttempts; ++i) {
        set_class_a_refusal_attempt(initial_time, initial_sequence + static_cast<std::uint32_t>(i));
        const OrchestratorResult result = orchestrate_submit(ctx_);
        ASSERT_EQ(result.gate, OrchestratorGate::OperatorNotConfirmed) << "attempt " << i;
        ASSERT_EQ(result.closure, ClosureAppendStatus::Appended) << "attempt " << i;
        EXPECT_EQ(result.order.state, OrderState::NeverSent) << "an acknowledged closure is reflected in the result";
        EXPECT_EQ(submit_calls_, 0u) << "a class A refusal must not call the submit port";
        if (i == 0) first_id = result.order.client_order_id;
    }

    ASSERT_EQ(sink_->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(sink_->max_intent_orphans(), kSmallOrphanBudget);
    EXPECT_EQ(sink_->frame_count(), kClosedAttempts * 2);
    EXPECT_EQ(sink_->write_format_version(), kFrameFormatVersionV6);

    const std::uintmax_t before_reuse = log_size();
    ASSERT_EQ(before_reuse, kClosedAttempts * 2 * kOrderEventFrameSize);

    // The 65th close evicted the first id from the context's 64-entry recent list. The durable
    // watermark still refuses the old generated identity before another Intent frame is written.
    set_class_a_refusal_attempt(initial_time, initial_sequence);
    const OrchestratorResult evicted_id = orchestrate_submit(ctx_);
    EXPECT_EQ(evicted_id.order.client_order_id.view(), first_id.view());
    EXPECT_EQ(evicted_id.gate, OrchestratorGate::AuditWriteNotAcked);
    EXPECT_EQ(evicted_id.closure, ClosureAppendStatus::NotAttempted);
    EXPECT_EQ(log_size(), before_reuse);

    sink_.reset();
    open_sink(NeverSentWrites::Enabled);
    ASSERT_EQ(sink_->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(sink_->recovered_never_sent(), kClosedAttempts);
    EXPECT_EQ(sink_->recovered_intent_orphans(), 0u);
    EXPECT_TRUE(sink_->recovered_checkpoints().empty());

    // A fresh context has no in-memory recently-closed ids. Recovery of the real sink alone must
    // reject the old (time, sequence) pair, then allow monotone identities to continue.
    RestartedOrchestratorContext restarted;
    restarted.initialize();
    restarted.install_audit_port(make_durable_order_audit_port(*sink_));
    restarted.refuse_confirmation();
    restarted.choose_attempt(initial_time, initial_sequence);
    const OrchestratorResult recovered_old_id = restarted.submit();
    EXPECT_EQ(recovered_old_id.order.client_order_id.view(), first_id.view());
    EXPECT_EQ(recovered_old_id.gate, OrchestratorGate::AuditWriteNotAcked);
    EXPECT_EQ(recovered_old_id.closure, ClosureAppendStatus::NotAttempted);
    EXPECT_EQ(log_size(), before_reuse);

    restarted.choose_attempt(initial_time, initial_sequence + static_cast<std::uint32_t>(kClosedAttempts));
    const OrchestratorResult same_time_new_sequence = restarted.submit();
    EXPECT_EQ(same_time_new_sequence.gate, OrchestratorGate::OperatorNotConfirmed);
    EXPECT_EQ(same_time_new_sequence.closure, ClosureAppendStatus::Appended);

    restarted.choose_attempt(initial_time + 1, initial_sequence);
    const OrchestratorResult newer_time = restarted.submit();
    EXPECT_EQ(newer_time.gate, OrchestratorGate::OperatorNotConfirmed);
    EXPECT_EQ(newer_time.closure, ClosureAppendStatus::Appended);

    sink_.reset();
    open_sink(NeverSentWrites::Disabled);
    EXPECT_EQ(sink_->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(sink_->recovered_never_sent(), kClosedAttempts + 2);
    EXPECT_EQ(sink_->recovered_intent_orphans(), 0u);
    EXPECT_TRUE(sink_->recovered_checkpoints().empty());
}

TEST_F(NeverSentOrphanTest, DefaultDisabledSinkLeavesTheClassAIntentOrphan) {
    open_sink(NeverSentWrites::Disabled);
    set_class_a_refusal_attempt(ctx_.now_ms, ctx_.sequence);

    const OrchestratorResult refused = orchestrate_submit(ctx_);
    EXPECT_EQ(refused.gate, OrchestratorGate::OperatorNotConfirmed);
    EXPECT_EQ(refused.closure, ClosureAppendStatus::NotEnabled);
    EXPECT_EQ(sink_->write_format_version(), kFrameFormatVersion);
    ASSERT_EQ(log_size(), kOrderEventFrameSize);

    // With closure writes disabled, the old behavior remains: the repeated Intent is still an
    // orphan because no closure attempt advanced the durable watermark.
    const OrchestratorResult duplicate = orchestrate_submit(ctx_);
    EXPECT_EQ(duplicate.gate, OrchestratorGate::OperatorNotConfirmed);
    EXPECT_EQ(duplicate.closure, ClosureAppendStatus::NotEnabled);
    EXPECT_EQ(log_size(), 2 * kOrderEventFrameSize);

    sink_.reset();
    open_sink(NeverSentWrites::Disabled);
    EXPECT_EQ(sink_->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(sink_->recovered_never_sent(), 0u);
    EXPECT_EQ(sink_->recovered_intent_orphans(), 1u);
    EXPECT_EQ(sink_->frame_count(), 2u);
    EXPECT_TRUE(sink_->recovered_checkpoints().empty());
}

TEST_F(NeverSentOrphanTest, LegacyNonHyphenatedIntentIdsRemainReadable) {
    open_sink(NeverSentWrites::Enabled);

    AuditRecord intent{};
    intent.timestamp_ms = ctx_.now_ms;
    intent.event_type = AuditEventType::OrderIntentCreated;
    intent.mode = ExecutionMode::Live;
    intent.symbol_id = ctx_.symbol_id;
    intent.set_client_order_id("legacy-order-17");
    intent.price_ticks = ctx_.price_ticks;
    intent.qty_ticks = ctx_.qty_ticks;
    intent.resulting_state = OrderState::Intent;
    intent.side = ctx_.side;
    ASSERT_TRUE(sink_->append_durable(intent, ctx_.now_ms).acked());

    sink_.reset();
    open_sink(NeverSentWrites::Disabled);
    EXPECT_EQ(sink_->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(sink_->recovered_intent_orphans(), 1u);
    EXPECT_EQ(sink_->recovered_never_sent(), 0u);
    EXPECT_TRUE(sink_->recovered_checkpoints().empty());
}
