// SPDX-License-Identifier: proprietary
// Shared Q23 S5/S6 test support for never-sent class A/B orchestrator and sink tests.
#pragma once

#include <gtest/gtest.h>

#include "account_snapshot_fixtures.hpp"

#include <hengyuan/live_submit_orchestrator.hpp>
#include <hengyuan/synthetic_readiness.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace hy::test_helpers::never_sent {

enum class TraceKind : std::uint8_t {
    DurableAppend = 0,
    ClosureAppend = 1,
};

struct TraceEntry {
    TraceKind kind{TraceKind::DurableAppend};
    AuditRecord record{};
    ::hy::never_sent::ClosureTarget target{};
    std::int64_t now_ms{0};
    AuditAppendResult::Status durable_status{AuditAppendResult::Status::Failed};
    ClosureAppendStatus closure_status{ClosureAppendStatus::NotAttempted};
    AuditAppendResult closure_append{};
};

// Fixed-capacity fake so an orchestrator call through this fake stays allocation-free.
struct FakeAudit {
    using IntentAckHook = void (*)(void* user_data, const AuditRecord& intent) noexcept;
    using PreparedAckHook = void (*)(void* user_data, const AuditRecord& prepared) noexcept;

    static constexpr std::size_t kMaxTraceEntries = 32;

    bool intent_should_fail{false};
    bool prepared_should_fail{false};
    bool other_should_fail{false};
    bool is_fenced{false};
    std::uint64_t next_sequence{0};
    std::array<TraceEntry, kMaxTraceEntries> trace{};
    std::size_t trace_count{0};
    std::size_t durable_append_calls{0};
    std::size_t closure_calls{0};
    ClosureAppendStatus closure_status{ClosureAppendStatus::Appended};
    IntentAckHook after_intent_ack{nullptr};
    PreparedAckHook after_prepared_ack{nullptr};
    void* hook_user_data{nullptr};

    static AuditAppendResult append(void* user_data, const AuditRecord& rec, std::int64_t now_ms) noexcept {
        auto& self = *static_cast<FakeAudit*>(user_data);
        ++self.durable_append_calls;

        const bool is_intent = rec.event_type == AuditEventType::OrderIntentCreated;
        const bool is_prepared = rec.event_type == AuditEventType::OrderSubmitPrepared;
        const bool fail = (is_intent && self.intent_should_fail) || (is_prepared && self.prepared_should_fail) ||
                          (!is_intent && !is_prepared && self.other_should_fail);

        AuditAppendResult result{};
        result.status = fail ? AuditAppendResult::Status::Failed : AuditAppendResult::Status::Acked;
        if (!fail) result.sequence = self.next_sequence++;

        if (self.trace_count < self.trace.size()) {
            TraceEntry& entry = self.trace[self.trace_count++];
            entry.kind = TraceKind::DurableAppend;
            entry.record = rec;
            entry.now_ms = now_ms;
            entry.durable_status = result.status;
        }

        if (fail) {
            self.is_fenced = true;
        } else if (is_intent && self.after_intent_ack != nullptr) {
            self.after_intent_ack(self.hook_user_data, rec);
        } else if (is_prepared && self.after_prepared_ack != nullptr) {
            self.after_prepared_ack(self.hook_user_data, rec);
        }
        return result;
    }

    static bool fenced(void* user_data) noexcept { return static_cast<FakeAudit*>(user_data)->is_fenced; }

    static ClosureAppendResult append_closure(void* user_data, const ::hy::never_sent::ClosureTarget& target,
                                              const AuditRecord& rec, std::int64_t now_ms) noexcept {
        auto& self = *static_cast<FakeAudit*>(user_data);
        ++self.closure_calls;

        ClosureAppendResult result{};
        result.status = self.closure_status;
        if (result.status == ClosureAppendStatus::Appended) {
            result.append.status = AuditAppendResult::Status::Acked;
            result.append.sequence = self.next_sequence++;
        } else if (result.status == ClosureAppendStatus::Failed) {
            result.append.status = AuditAppendResult::Status::Failed;
            self.is_fenced = true;
        }

        if (self.trace_count < self.trace.size()) {
            TraceEntry& entry = self.trace[self.trace_count++];
            entry.kind = TraceKind::ClosureAppend;
            entry.record = rec;
            entry.target = target;
            entry.now_ms = now_ms;
            entry.closure_status = result.status;
            entry.closure_append = result.append;
        }
        return result;
    }

    void wire(DurableOrderAuditPort& port) noexcept {
        port = {&FakeAudit::append, &FakeAudit::fenced, this, &FakeAudit::append_closure};
    }
};

class NeverSentOrchestratorFixture : public ::testing::Test {
protected:
    AuditRingSink audit_{};
    KillSwitch kill_switch_{};
    SyntheticReadiness readiness_{kill_switch_};
    DryRunEvidenceChain evidence_{};
    SpotRateLimitTracker rate_limiter_{};
    InFlightRegistry in_flight_{};
    SymbolRules rules_{};
    AccountSnapshot account_{};
    OrchestratorContext ctx_{};
    FakeAudit fake_audit_{};
    SubmitResponse submit_response_{SubmitOutcome::Accepted, 12345, 0};
    std::size_t submit_calls_{0};
    std::uint32_t last_submit_symbol_{0};
    OrderSide last_submit_side_{OrderSide::Buy};
    OrderType last_submit_type_{OrderType::Limit};
    std::int64_t last_submit_price_ticks_{0};
    std::int64_t last_submit_qty_ticks_{0};

    void SetUp() override {
        audit_.set_available(true);
        kill_switch_.operator_reset();
        fill_evidence();
        (void)rate_limiter_.configure(6000, 500, 60000, 5000, 100, 10);

        std::strncpy(rules_.symbol, "BTCUSDT", sizeof(rules_.symbol) - 1);
        rules_.is_trading = true;
        rules_.min_price_ticks = 100;
        rules_.max_price_ticks = 10000000;
        rules_.tick_size_ticks = 100;
        rules_.min_qty_ticks = 10;
        rules_.max_qty_ticks = 1000000;
        rules_.step_size_ticks = 10;
        rules_.min_notional_ticks = 1000;
        rules_.price_scale = 0;
        rules_.qty_scale = 8;
        rules_.rules_version = 1;

        account_ = hy::test_helpers::make_account(900, true, {{"USDT", 999999, 0}});

        ctx_.audit = &audit_;
        ctx_.kill_switch = &kill_switch_;
        ctx_.readiness = &readiness_.readiness();
        ctx_.evidence = &evidence_;
        ctx_.signer_ready = true;
        ctx_.depth_synced = true;
        ctx_.rate_limiter = &rate_limiter_;
        ctx_.in_flight = &in_flight_;
        ctx_.symbol_rules = &rules_;
        ctx_.account = &account_;
        ctx_.side = OrderSide::Buy;
        ctx_.order_type = OrderType::Limit;
        ctx_.base_asset = "BTC";
        ctx_.quote_asset = "USDT";
        ctx_.now_ms = 1000;
        ctx_.exposure_limits.freshness_max_age_ms = 30000;
        ctx_.exposure_limits.single_order_notional_cap = 500000;
        ctx_.exposure_limits.total_exposure_notional_cap = 1000000;
        ctx_.price_ticks = 1000;
        ctx_.qty_ticks = 10;
        ctx_.symbol_id = 1;
        ctx_.sequence = 1;
        ctx_.mode = ExecutionMode::Live;
        ctx_.order_weight = 1;

        bind_confirmation();
        ctx_.submit_port = {&NeverSentOrchestratorFixture::submit, this,
                            &NeverSentOrchestratorFixture::current_rules_version};
        fake_audit_.wire(ctx_.durable_audit);
    }

    void bind_confirmation() noexcept {
        ctx_.confirmation.confirmed = true;
        ctx_.confirmation.symbol_id = ctx_.symbol_id;
        ctx_.confirmation.side = ctx_.side;
        ctx_.confirmation.type = ctx_.order_type;
        ctx_.confirmation.price_ticks = ctx_.price_ticks;
        ctx_.confirmation.qty_ticks = ctx_.qty_ticks;
        ctx_.confirmation.max_notional = 1000000;
        ctx_.confirmation.valid_until_ms = ctx_.now_ms + 60000;
    }

    ClientOrderId expected_coid() const noexcept {
        return make_client_order_id(ctx_.now_ms, ctx_.sequence, ctx_.symbol_id);
    }

private:
    void fill_evidence() noexcept {
        evidence_.record(EvidencePath::SubmitSuccess, 1, 0xABCD, 1);
        evidence_.record(EvidencePath::SubmitReject, 2, 0xABCD, 1);
        evidence_.record(EvidencePath::SubmitAmbiguous, 3, 0xABCD, 1);
        evidence_.record(EvidencePath::KillSwitch, 4, 0xABCD, 1);
    }

    static SubmitResponse submit(const char*, std::uint32_t symbol, OrderSide side, OrderType type,
                                 std::int64_t price_ticks, std::int64_t qty_ticks,
                                 const SymbolRules&, void* user_data) {
        auto& self = *static_cast<NeverSentOrchestratorFixture*>(user_data);
        ++self.submit_calls_;
        self.last_submit_symbol_ = symbol;
        self.last_submit_side_ = side;
        self.last_submit_type_ = type;
        self.last_submit_price_ticks_ = price_ticks;
        self.last_submit_qty_ticks_ = qty_ticks;
        return self.submit_response_;
    }

    static std::uint32_t current_rules_version(void* user_data) {
        return static_cast<NeverSentOrchestratorFixture*>(user_data)->rules_.rules_version;
    }
};

}  // namespace hy::test_helpers::never_sent
