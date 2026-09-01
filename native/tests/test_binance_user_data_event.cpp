// TODO 1A.4: binance_user_data_event.hpp unit tests -- drain_user_data_events(), mirroring
// test_order_tracker.cpp's DrainEventsTest coverage for drain_reconcile_events().
#include <gtest/gtest.h>
#include <hengyuan/binance_user_data_event.hpp>

#include <cstring>

using hy::AuditEventType;
using hy::AuditRingSink;
using hy::drain_user_data_events;
using hy::InFlightRegistry;
using hy::kClientOrderIdLen;
using hy::UserDataEventKind;
using hy::UserDataWsEvent;
using hy::UserDataWsEventRing;

namespace {

class DrainUserDataEventsTest : public ::testing::Test {
protected:
    InFlightRegistry in_flight_;
    AuditRingSink audit_;
    UserDataWsEventRing events_{};
};

}  // namespace

TEST_F(DrainUserDataEventsTest, TrackedCoidExecutionReportWritesAuditRecord) {
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-A").valid());

    UserDataWsEvent ev{};
    ev.kind = UserDataEventKind::ExecutionReport;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ev.exchange_order_id = 777;
    ASSERT_TRUE(events_.try_push(ev));

    drain_user_data_events(in_flight_, &audit_, events_, 2000);

    ASSERT_NE(audit_.count(), 0u);
    EXPECT_EQ(audit_.last()->event_type, AuditEventType::UserDataStreamEventObserved);
    EXPECT_EQ(audit_.last()->exchange_order_id, 777);
    EXPECT_STREQ(audit_.last()->client_order_id, "HY-A");
    // This function only observes -- it must never release the in-flight slot or otherwise
    // mutate tracking state (see its own header comment on why: full fold-in is a future slice).
    EXPECT_TRUE(in_flight_.is_in_flight("HY-A"));
}

TEST_F(DrainUserDataEventsTest, UntrackedCoidIsSilentlyIgnoredNotCrashed) {
    // No register_submit_handle() call for "HY-UNKNOWN" -- an event for an order this process
    // isn't tracking (already resolved, or belongs to a different session) is expected, not
    // exceptional.
    UserDataWsEvent ev{};
    ev.kind = UserDataEventKind::ExecutionReport;
    std::strncpy(ev.coid.id, "HY-UNKNOWN", kClientOrderIdLen);
    ASSERT_TRUE(events_.try_push(ev));

    drain_user_data_events(in_flight_, &audit_, events_, 2000);

    EXPECT_EQ(audit_.count(), 0u);
}

TEST_F(DrainUserDataEventsTest, NonExecutionReportKindsIgnored) {
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-A").valid());

    for (auto kind : {UserDataEventKind::OutboundAccountPosition,
                       UserDataEventKind::ListenKeyExpired, UserDataEventKind::Unknown}) {
        UserDataWsEvent ev{};
        ev.kind = kind;
        std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
        ASSERT_TRUE(events_.try_push(ev));
    }

    drain_user_data_events(in_flight_, &audit_, events_, 2000);

    EXPECT_EQ(audit_.count(), 0u);
}

TEST_F(DrainUserDataEventsTest, NullAuditSinkIsToleratedNotCrashed) {
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-A").valid());

    UserDataWsEvent ev{};
    ev.kind = UserDataEventKind::ExecutionReport;
    std::strncpy(ev.coid.id, "HY-A", kClientOrderIdLen);
    ASSERT_TRUE(events_.try_push(ev));

    drain_user_data_events(in_flight_, /*audit=*/nullptr, events_, 2000);

    // No crash is the assertion; nothing else to check.
    SUCCEED();
}

TEST_F(DrainUserDataEventsTest, DrainsMultipleEventsInOneCall) {
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-A").valid());
    ASSERT_TRUE(in_flight_.register_submit_handle("HY-B").valid());

    for (const char* coid : {"HY-A", "HY-B"}) {
        UserDataWsEvent ev{};
        ev.kind = UserDataEventKind::ExecutionReport;
        std::strncpy(ev.coid.id, coid, kClientOrderIdLen);
        ASSERT_TRUE(events_.try_push(ev));
    }

    drain_user_data_events(in_flight_, &audit_, events_, 2000);

    EXPECT_EQ(audit_.count(), 2u);
}
