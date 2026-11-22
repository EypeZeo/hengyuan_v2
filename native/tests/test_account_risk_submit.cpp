// SPDX-License-Identifier: proprietary
#include "test_helpers/never_sent_fixture.hpp"
#include "test_helpers/temp_directory.hpp"
#include "binance_clock_sync_test_hooks.hpp"

#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/owner_trade_scope_approval.hpp>
#include <hengyuan/position_truth.hpp>
#include <hengyuan/store_binding_writer.hpp>
#include <hengyuan/trade_scope_execution.hpp>

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using hy::OrchestratorGate;
using hy::OrchestratorResult;
using hy::OrderFillContext;
using hy::OrderState;
using hy::OrderType;
using hy::StoreEnvironment;
using hy::TradeScopeBinding;
using hy::TradeScopeExecution;
using hy::test_helpers::never_sent::NeverSentOrchestratorFixture;

struct P256Fixture {
    EVP_PKEY* key{nullptr};
    std::vector<unsigned char> public_der;

    P256Fixture() {
        key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1");
        if (!key) return;
        const int size = i2d_PUBKEY(key, nullptr);
        if (size <= 0) return;
        public_der.resize(static_cast<std::size_t>(size));
        unsigned char* cursor = public_der.data();
        if (i2d_PUBKEY(key, &cursor) != size) public_der.clear();
    }

    ~P256Fixture() { EVP_PKEY_free(key); }
    P256Fixture(const P256Fixture&) = delete;
    P256Fixture& operator=(const P256Fixture&) = delete;

    std::vector<unsigned char> sign(std::span<const std::byte> payload) const {
        std::vector<unsigned char> signature;
        if (!key) return signature;
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        if (!context) return signature;
        if (EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key) != 1 ||
            EVP_DigestSignUpdate(context, payload.data(), payload.size()) != 1) {
            EVP_MD_CTX_free(context);
            return signature;
        }
        std::size_t size = 0;
        if (EVP_DigestSignFinal(context, nullptr, &size) != 1) {
            EVP_MD_CTX_free(context);
            return signature;
        }
        signature.resize(size);
        if (EVP_DigestSignFinal(context, signature.data(), &size) != 1) signature.clear();
        else signature.resize(size);
        EVP_MD_CTX_free(context);
        return signature;
    }
};

struct FakeMonotonicClock {
    std::int64_t now_ms{10};
    static std::int64_t now(void* user_data) noexcept {
        return static_cast<FakeMonotonicClock*>(user_data)->now_ms;
    }
};

struct PublishNextDayClockOnFinalCheck {
    hy::ClockOffsetPublisher* publisher{nullptr};
    std::int64_t monotonic_ms{10};
    std::size_t calls{0};
    bool published{false};

    static std::int64_t now(void* user_data) noexcept {
        auto& self = *static_cast<PublishNextDayClockOnFinalCheck*>(user_data);
        ++self.calls;
        if (self.calls == 2 && self.publisher != nullptr) {
            const auto sample = hy::fetch_clock_pair();
            hy::ClockOffsetSnapshot snapshot{};
            const auto next_day = hy::account_risk_audit::utc_day(sample.system_ms()) + 1;
            const auto target_ms = next_day * hy::account_risk_audit::kUtcDayMs +
                                   hy::account_risk_audit::kUtcDayMs / 2;
            snapshot.offset_ms = target_ms - sample.system_ms();
            snapshot.system_at_fetch_ms = sample.system_ms();
            snapshot.steady_at_fetch_ms = sample.steady_ms();
            self.published = self.publisher->publish(snapshot);
        }
        return self.monotonic_ms;
    }
};

std::int64_t wall_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class AccountRiskSubmitTest : public NeverSentOrchestratorFixture {
protected:
    void SetUp() override {
        NeverSentOrchestratorFixture::SetUp();
        now_ms_ = wall_now_ms();
        ctx_.now_ms = now_ms_;
        ctx_.order_wall_ms = now_ms_;
        ctx_.account = &account_;
        account_ = hy::test_helpers::make_account(now_ms_, true,
                                                   {{"USDT", 999999, 0}, {"BTC", 1000, 0}});
        ctx_.side = hy::OrderSide::Sell;
        ctx_.order_type = OrderType::Limit;
        ctx_.price_ticks = 1000;
        ctx_.qty_ticks = 10;
        ctx_.scope_reference_price_ticks = 1000;
        ctx_.base_asset = "BTC";
        ctx_.quote_asset = "USDT";
        ctx_.trade_scope_required = true;
        ctx_.restricted_exit = true;
        ctx_.fill_context = &fill_context_;
        ctx_.monotonic_now = &FakeMonotonicClock::now;
        ctx_.monotonic_clock = &monotonic_;
        ctx_.authorization_ttl_ms = 100;

        std::array<std::byte, hy::kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x30U + i);
        key_ring_ = std::make_unique<hy::KeyRing>(kek);
        std::array<std::byte, hy::kKeyBlockSize> plaintext_key{};
        for (std::size_t i = 0; i < plaintext_key.size(); ++i) plaintext_key[i] = static_cast<std::byte>(0x60U + i);
        hy::WrappedKeyRecord wrapped{};
        ASSERT_EQ(key_ring_->add_key(1, plaintext_key, wrapped), hy::KeyRingAddStatus::Ok);

        log_path_ = scratch_.file("account-risk-orders.log");
        sink_ = std::make_unique<hy::DurableAuditSink>(
            log_path_, *key_ring_, 1, hy::kMaxIntentOrphans, hy::AuditSinkOpenMode::Trading,
            hy::NeverSentWrites::Enabled);
        ASSERT_TRUE(sink_->is_open());
        ASSERT_FALSE(sink_->fenced());
        ASSERT_EQ(hy::ensure_store_binding(*sink_, log_path_, *key_ring_, StoreEnvironment::Testnet,
                                           42, now_ms_), hy::EnsureBindingStatus::BoundNew);
        const auto stored = sink_->read_binding();
        ASSERT_EQ(stored.status, hy::StoreBindingStatus::Bound);
        store_scope_ = {stored.binding.environment, stored.binding.account_uid,
                        sink_->store_uuid_lo(), sink_->store_uuid_hi()};

        const auto sample = hy::fetch_clock_pair();
        hy::ClockOffsetSnapshot clock_snapshot{};
        const auto current_day = hy::account_risk_audit::utc_day(sample.system_ms());
        const auto same_day_noon = current_day * hy::account_risk_audit::kUtcDayMs +
                                   hy::account_risk_audit::kUtcDayMs / 2;
        clock_snapshot.offset_ms = same_day_noon - sample.system_ms();
        clock_snapshot.system_at_fetch_ms = sample.system_ms();
        clock_snapshot.steady_at_fetch_ms = sample.steady_ms();
        ASSERT_TRUE(clock_.publish(clock_snapshot));

        ASSERT_NE(signer_.key, nullptr);
        ASSERT_FALSE(signer_.public_der.empty());
        ASSERT_TRUE(install_policy(1, 100, 100000));
        bind_execution(1);
        ctx_.durable_audit = hy::make_durable_order_audit_port(*sink_);

        ASSERT_TRUE(in_flight_.register_submit("HY-KNOWN-EXIT-ORDER"));
        ASSERT_TRUE(fill_context_.track("HY-KNOWN-EXIT-ORDER", ctx_.symbol_id, hy::OrderSide::Buy,
                                        rules_.price_scale, rules_.qty_scale, rules_.quote_scale, 10, "BTCUSDT"));
        fill_context_.durable_acked("HY-KNOWN-EXIT-ORDER", OrderState::Accepted, 0, 12345);
    }

    bool install_policy(std::uint64_t version, std::int64_t exit_base_cap,
                        std::int64_t exit_order_quote_cap) {
        hy::AccountRiskPolicy policy{};
        policy.store = store_scope_;
        policy.key_id = kOwnerKeyId;
        policy.limits = {version, 100000, exit_base_cap, exit_order_quote_cap, 0};
        const auto payload = hy::encode_account_risk_policy(policy);
        if (!payload) return false;
        const auto signature = signer_.sign(*payload);
        const auto verified = hy::VerifiedAccountRiskPolicy::verify(
            *payload, signature, signer_.public_der, store_scope_, kOwnerKeyId, version);
        if (!verified) return false;
        return sink_->append_account_risk_policy(*verified, ctx_.symbol_id, now_ms_).acked();
    }

    void bind_execution(std::uint64_t policy_version) {
        TradeScopeBinding binding{};
        binding.store = store_scope_;
        binding.policy_version = policy_version;
        execution_ = std::make_unique<TradeScopeExecution>(
            *sink_, kill_switch_, ctx_.readiness->authority(), clock_, binding);
        ctx_.trade_scope = execution_.get();
    }

    static constexpr std::uint64_t kOwnerKeyId = 17;
    hy::test_helpers::TempDirectory scratch_{};
    std::string log_path_{};
    std::unique_ptr<hy::KeyRing> key_ring_{};
    std::unique_ptr<hy::DurableAuditSink> sink_{};
    P256Fixture signer_{};
    hy::OperatorResetScope store_scope_{};
    hy::ClockOffsetPublisher clock_{};
    std::unique_ptr<TradeScopeExecution> execution_{};
    OrderFillContext fill_context_{};
    FakeMonotonicClock monotonic_{};
    std::int64_t now_ms_{0};
};

TEST(AccountRiskClockAdmission, RefusesBoundsAcrossUtcMidnightAndAcceptsSameDayBounds) {
    constexpr std::int64_t day_start = 20'000LL * hy::account_risk_audit::kUtcDayMs;
    hy::ClockOffsetPublisher publisher;

    hy::ClockOffsetSnapshot crossing{};
    crossing.system_at_fetch_ms = day_start + 500;
    crossing.steady_at_fetch_ms = 1000;
    ASSERT_TRUE(publisher.publish(crossing));
    EXPECT_FALSE(hy::account_order_time_ms(
        publisher.load(), hy::ClockPairSampleTestHooks::make(day_start + 500, 1000)).has_value());

    hy::ClockOffsetSnapshot safe{};
    safe.system_at_fetch_ms = day_start + 5'000;
    safe.steady_at_fetch_ms = 2'000;
    ASSERT_TRUE(publisher.publish(safe));
    const auto admitted = hy::account_order_time_ms(
        publisher.load(), hy::ClockPairSampleTestHooks::make(day_start + 5'000, 2'000));
    ASSERT_TRUE(admitted.has_value());
    EXPECT_EQ(hy::account_risk_audit::utc_day(*admitted), hy::account_risk_audit::utc_day(day_start + 5'000));
}

TEST_F(AccountRiskSubmitTest, RestrictedExitAcceptsKnownSellWithoutActiveTradeGrant) {
    ASSERT_NE(execution_, nullptr);
    EXPECT_EQ(execution_->active(), nullptr);

    const OrchestratorResult result = hy::orchestrate_submit(ctx_);

    EXPECT_EQ(result.gate, OrchestratorGate::SubmitAccepted);
    EXPECT_EQ(submit_calls_, 1u);
    EXPECT_EQ(last_submit_side_, hy::OrderSide::Sell);
    EXPECT_EQ(last_submit_type_, OrderType::Limit);
    EXPECT_EQ(sink_->account_risk().base_reserved(), 10);
}

TEST_F(AccountRiskSubmitTest, FilledReplyWithoutExchangeOrderIdStaysAmbiguousAndKeepsExitReservation) {
    hy::PositionTruth position_truth;
    ctx_.position_truth = &position_truth;
    submit_response_.outcome = hy::SubmitOutcome::Accepted;
    submit_response_.exchange_order_id = 0;
    submit_response_.exchange_status = OrderState::Filled;
    submit_response_.filled_qty_ticks = ctx_.qty_ticks;
    submit_response_.avg_fill_price_ticks = ctx_.price_ticks;
    const auto frames_before_submit = sink_->frame_count();

    const OrchestratorResult result = hy::orchestrate_submit(ctx_);

    EXPECT_EQ(submit_calls_, 1u);
    EXPECT_EQ(result.gate, OrchestratorGate::SubmitNetworkError);
    EXPECT_EQ(result.order.state, OrderState::Ambiguous);
    EXPECT_EQ(result.order.exchange_order_id, 0);
    EXPECT_EQ(result.order.filled_qty_ticks, 0);
    EXPECT_EQ(result.order.avg_fill_price_ticks, 0);
    EXPECT_EQ(result.closure, hy::ClosureAppendStatus::NotAttempted);
    EXPECT_EQ(sink_->frame_count(), frames_before_submit + 3U);
    EXPECT_EQ(sink_->account_risk().day_used(), 0);
    EXPECT_EQ(sink_->account_risk().base_reserved(), 10);
    EXPECT_EQ(position_truth.net_qty_ticks(ctx_.symbol_id), 0);
    EXPECT_EQ(position_truth.tracked_symbol_count(), 0U);
    EXPECT_TRUE(in_flight_.is_in_flight(result.order.client_order_id.view()));

    execution_.reset();
    sink_.reset();
    sink_ = std::make_unique<hy::DurableAuditSink>(
        log_path_, *key_ring_, 1, hy::kMaxIntentOrphans, hy::AuditSinkOpenMode::Trading,
        hy::NeverSentWrites::Enabled);
    ASSERT_TRUE(sink_->is_open());
    ASSERT_EQ(sink_->recovery_status(), hy::RecoveryScanStatus::Recovered);
    EXPECT_EQ(sink_->recovered_never_sent(), 0U);
    ASSERT_EQ(sink_->recovered_checkpoints().size(), 1U);
    EXPECT_STREQ(sink_->recovered_checkpoints()[0].client_order_id.id,
                 result.order.client_order_id.id);
    EXPECT_EQ(sink_->recovered_checkpoints()[0].resulting_state, OrderState::Ambiguous);
    EXPECT_EQ(sink_->recovered_checkpoints()[0].exchange_order_id, 0);
    EXPECT_EQ(sink_->recovered_checkpoints()[0].filled_qty_ticks, 0);
    EXPECT_EQ(sink_->account_risk().day_used(), 0);
    EXPECT_EQ(sink_->account_risk().base_reserved(), 10);
}

TEST_F(AccountRiskSubmitTest, RestrictedExitRefusesInsufficientSignedBaseAmount) {
    ASSERT_TRUE(install_policy(2, 5, 100000));
    bind_execution(2);

    const OrchestratorResult result = hy::orchestrate_submit(ctx_);

    EXPECT_EQ(result.gate, OrchestratorGate::RestrictedExitNotAuthorized);
    EXPECT_EQ(result.closure, hy::ClosureAppendStatus::Appended);
    EXPECT_EQ(submit_calls_, 0u);
    EXPECT_EQ(sink_->account_risk().base_reserved(), 0);
}

TEST_F(AccountRiskSubmitTest, RestrictedExitRefusesWhenSignedPolicyDisablesExits) {
    ASSERT_TRUE(install_policy(2, 0, 0));
    bind_execution(2);

    const OrchestratorResult result = hy::orchestrate_submit(ctx_);

    EXPECT_EQ(result.gate, OrchestratorGate::RestrictedExitNotAuthorized);
    EXPECT_EQ(result.closure, hy::ClosureAppendStatus::Appended);
    EXPECT_EQ(submit_calls_, 0u);
}

TEST_F(AccountRiskSubmitTest, RestrictedExitRefusesStaleExchangeClock) {
    hy::ClockOffsetSnapshot stale{};
    ASSERT_TRUE(clock_.publish(stale));

    const OrchestratorResult result = hy::orchestrate_submit(ctx_);

    EXPECT_EQ(result.gate, OrchestratorGate::RestrictedExitNotAuthorized);
    EXPECT_EQ(result.closure, hy::ClosureAppendStatus::Appended);
    EXPECT_EQ(submit_calls_, 0u);
}

TEST_F(AccountRiskSubmitTest, RestrictedExitRefusesWhenKillSwitchIsTriggered) {
    kill_switch_.trigger();

    const OrchestratorResult result = hy::orchestrate_submit(ctx_);

    EXPECT_EQ(result.gate, OrchestratorGate::KillSwitchNotNormal);
    EXPECT_EQ(submit_calls_, 0u);
}

TEST_F(AccountRiskSubmitTest, RestrictedExitRefusesWhenReadinessBarrierIsDown) {
    readiness_.readiness().barrier().on_generation_ended(1);

    const OrchestratorResult result = hy::orchestrate_submit(ctx_);

    EXPECT_EQ(result.gate, OrchestratorGate::NotReady);
    EXPECT_EQ(result.not_ready_reason, hy::NotReadyReason::FeedDown);
    EXPECT_EQ(submit_calls_, 0u);
}

TEST_F(AccountRiskSubmitTest, FinalClaimRejectsNextUtcDayAndPersistsNeverSentRefund) {
    PublishNextDayClockOnFinalCheck final_clock{&clock_};
    ctx_.monotonic_now = &PublishNextDayClockOnFinalCheck::now;
    ctx_.monotonic_clock = &final_clock;

    const OrchestratorResult result = hy::orchestrate_submit(ctx_);

    EXPECT_TRUE(final_clock.published);
    EXPECT_EQ(result.gate, OrchestratorGate::FinalAuthorizationRefused);
    ASSERT_TRUE(result.final_refusal.has_value());
    EXPECT_EQ(*result.final_refusal, hy::never_sent::Reason::AccountPeriodChanged);
    EXPECT_EQ(result.closure, hy::ClosureAppendStatus::Appended);
    EXPECT_EQ(result.order.state, OrderState::NeverSent);
    EXPECT_EQ(submit_calls_, 0u);
    EXPECT_EQ(sink_->account_risk().base_reserved(), 0);

    execution_.reset();
    sink_.reset();
    sink_ = std::make_unique<hy::DurableAuditSink>(
        log_path_, *key_ring_, 1, hy::kMaxIntentOrphans, hy::AuditSinkOpenMode::Trading,
        hy::NeverSentWrites::Enabled);
    ASSERT_TRUE(sink_->is_open());
    EXPECT_EQ(sink_->recovered_never_sent(), 1U);
    EXPECT_EQ(sink_->account_risk().base_reserved(), 0);
}

}  // namespace
