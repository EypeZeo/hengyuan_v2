// P2-EXEC-LIVE-01 D12-7: audit trail tests.
#include <gtest/gtest.h>
#include <hengyuan/audit_trail.hpp>

using hy::AuditEventType;
using hy::AuditRecord;
using hy::AuditRingSink;
using hy::AuditSinkStatus;
using hy::ExecutionMode;
using hy::audit_event_name;
using hy::can_submit_order;

static AuditRecord make_record(AuditEventType type, ExecutionMode mode,
                                std::int64_t ts = 1000) {
    AuditRecord r{};
    r.timestamp_ms = ts;
    r.event_type = type;
    r.mode = mode;
    r.symbol_id = 1;
    return r;
}

// --- Event names ---

TEST(AuditTrail, AllEventNamesNotNull) {
    for (int i = 0; i <= 19; ++i) {
        EXPECT_NE(audit_event_name(static_cast<AuditEventType>(i)), nullptr);
        EXPECT_STRNE(audit_event_name(static_cast<AuditEventType>(i)), "UNKNOWN");
    }
}

// --- AuditRecord ---

TEST(AuditTrail, SetClientOrderId) {
    AuditRecord r{};
    r.set_client_order_id("HY-1000-1-0042");
    EXPECT_STREQ(r.client_order_id, "HY-1000-1-0042");
}

TEST(AuditTrail, SetDetailTruncates) {
    AuditRecord r{};
    std::string long_msg(200, 'x');
    r.set_detail(long_msg.c_str());
    EXPECT_EQ(std::strlen(r.detail_msg), 63u);  // truncated to 63 + null
}

// --- AuditRingSink ---

TEST(AuditRingSink, InitiallyAvailable) {
    AuditRingSink sink;
    EXPECT_EQ(sink.status(), AuditSinkStatus::Available);
    EXPECT_EQ(sink.count(), 0u);
}

TEST(AuditRingSink, AppendAndRetrieve) {
    AuditRingSink sink;
    auto rec = make_record(AuditEventType::OrderSubmitted, ExecutionMode::Live);
    EXPECT_TRUE(sink.append(rec));
    EXPECT_EQ(sink.count(), 1u);
    auto* last = sink.last();
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(last->event_type, AuditEventType::OrderSubmitted);
    EXPECT_EQ(last->mode, ExecutionMode::Live);
}

TEST(AuditRingSink, UnavailableRejectsAppend) {
    AuditRingSink sink;
    sink.set_available(false);
    auto rec = make_record(AuditEventType::OrderSubmitted, ExecutionMode::Live);
    EXPECT_FALSE(sink.append(rec));
    EXPECT_EQ(sink.count(), 0u);
}

TEST(AuditRingSink, OrderGateBlockedWhenUnavailable) {
    AuditRingSink sink;
    EXPECT_TRUE(can_submit_order(sink));
    sink.set_available(false);
    EXPECT_FALSE(can_submit_order(sink));
}

TEST(AuditRingSink, MultipleAppends) {
    AuditRingSink sink;
    for (int i = 0; i < 10; ++i) {
        auto rec = make_record(AuditEventType::OrderIntentCreated, ExecutionMode::DryRun, i);
        EXPECT_TRUE(sink.append(rec));
    }
    EXPECT_EQ(sink.count(), 10u);
    auto* first = sink.at(0);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->timestamp_ms, 0);
    auto* last = sink.last();
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(last->timestamp_ms, 9);
}

TEST(AuditRingSink, AtOutOfRange) {
    AuditRingSink sink;
    EXPECT_EQ(sink.at(0), nullptr);
    auto rec = make_record(AuditEventType::OrderFilled, ExecutionMode::Live);
    sink.append(rec);
    EXPECT_NE(sink.at(0), nullptr);
    EXPECT_EQ(sink.at(1), nullptr);
}

TEST(AuditRingSink, ClearResets) {
    AuditRingSink sink;
    sink.append(make_record(AuditEventType::OrderFilled, ExecutionMode::Live));
    sink.set_available(false);
    sink.clear();
    EXPECT_EQ(sink.count(), 0u);
    EXPECT_EQ(sink.status(), AuditSinkStatus::Available);
}

TEST(AuditRingSink, DryRunAndLiveSeparated) {
    AuditRingSink sink;
    sink.append(make_record(AuditEventType::OrderSubmitted, ExecutionMode::DryRun, 1));
    sink.append(make_record(AuditEventType::OrderSubmitted, ExecutionMode::Live, 2));
    auto* r0 = sink.at(0);
    auto* r1 = sink.at(1);
    ASSERT_NE(r0, nullptr);
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ(r0->mode, ExecutionMode::DryRun);
    EXPECT_EQ(r1->mode, ExecutionMode::Live);
}

TEST(AuditRingSink, LastReturnsNullWhenEmpty) {
    AuditRingSink sink;
    EXPECT_EQ(sink.last(), nullptr);
}

// --- F5 regression: fail-closed once full, never silently overwrite (D10) ---

TEST(AuditRingSink, FillsToCapacityWithoutOverwriting) {
    AuditRingSink sink;
    EXPECT_FALSE(sink.is_full());
    for (int i = 0; i < 1024; ++i) {
        auto rec = make_record(AuditEventType::OrderIntentCreated, ExecutionMode::DryRun, i);
        EXPECT_TRUE(sink.append(rec));
    }
    EXPECT_EQ(sink.count(), 1024u);
    EXPECT_TRUE(sink.is_full());
    // The very first record must still be intact — nothing got overwritten.
    auto* first = sink.at(0);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->timestamp_ms, 0);
}

TEST(AuditRingSink, RejectsAppendOnceFull) {
    AuditRingSink sink;
    for (int i = 0; i < 1024; ++i) {
        sink.append(make_record(AuditEventType::OrderIntentCreated, ExecutionMode::DryRun, i));
    }
    ASSERT_TRUE(sink.is_full());

    auto one_more = make_record(AuditEventType::OrderIntentCreated, ExecutionMode::DryRun, 9999);
    EXPECT_FALSE(sink.append(one_more));
    EXPECT_EQ(sink.count(), 1024u);  // unchanged — the append was refused, not silently wrapped
    // The oldest record is still exactly what it was — not overwritten by the rejected append.
    auto* first = sink.at(0);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->timestamp_ms, 0);
}

TEST(AuditRingSink, FullSinkBlocksOrderGate) {
    AuditRingSink sink;
    for (int i = 0; i < 1024; ++i) {
        sink.append(make_record(AuditEventType::OrderIntentCreated, ExecutionMode::DryRun, i));
    }
    // A full ring must fail-closed the same way an explicitly-unavailable sink
    // does (ADR-019 D10) — no new orders once we can no longer guarantee
    // evidence retention.
    EXPECT_EQ(sink.status(), AuditSinkStatus::Unavailable);
    EXPECT_FALSE(can_submit_order(sink));
}
