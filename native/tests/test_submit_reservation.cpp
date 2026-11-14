#include <gtest/gtest.h>
#include <hengyuan/spot_rate_limit_budget.hpp>

#include <chrono>
#include <type_traits>
#include <utility>

namespace {

using hy::RateLimitLane;
using hy::SpotRateLimitTracker;
using TimePoint = SpotRateLimitTracker::TimePoint;

static_assert(!std::is_copy_constructible_v<SpotRateLimitTracker::OrderReservation>);
static_assert(std::is_move_constructible_v<SpotRateLimitTracker::OrderReservation>);

void configure_strategy_orders(SpotRateLimitTracker& tracker, TimePoint now,
                               std::uint32_t orders_limit = 1,
                               std::uint32_t window_seconds = 30) {
    const hy::RateLimitBudgetSplit strategy_only{100, 0, 0};
    ASSERT_TRUE(tracker.configure(/*weight*/ 100, /*weight margin*/ 0,
                                  /*raw*/ 100, /*raw margin*/ 0,
                                  /*orders*/ orders_limit, /*orders margin*/ 0,
                                  strategy_only, now,
                                  window_seconds, window_seconds, window_seconds));
}

TEST(SubmitReservation, MovingTicketInvalidatesTheSourceAndMovedTicketCanReleaseIt) {
    SpotRateLimitTracker tracker;
    const TimePoint now{};
    configure_strategy_orders(tracker, now);

    auto reservation = tracker.reserve_order(RateLimitLane::Strategy, 1, now);
    ASSERT_TRUE(reservation.has_value());
    auto moved_ticket = std::move(*reservation);

    EXPECT_FALSE(tracker.reservation_valid(*reservation, now));
    EXPECT_TRUE(tracker.reservation_valid(moved_ticket, now));

    tracker.release_order(*reservation, now);
    EXPECT_FALSE(tracker.can_send_order(RateLimitLane::Strategy, 1, now));
    tracker.release_order(moved_ticket, now);
    EXPECT_FALSE(tracker.reservation_valid(moved_ticket, now));
    EXPECT_TRUE(tracker.reserve_order(RateLimitLane::Strategy, 1, now).has_value());
}

TEST(SubmitReservation, TicketFromAnotherTrackerCannotRefundEitherTracker) {
    SpotRateLimitTracker owner;
    SpotRateLimitTracker other;
    const TimePoint now{};
    configure_strategy_orders(owner, now);
    configure_strategy_orders(other, now, /*orders_limit*/ 2);

    auto owner_reservation = owner.reserve_order(RateLimitLane::Strategy, 1, now);
    ASSERT_TRUE(owner_reservation.has_value());
    auto foreign_ticket = std::move(*owner_reservation);
    auto other_reservation = other.reserve_order(RateLimitLane::Strategy, 1, now);
    ASSERT_TRUE(other_reservation.has_value());

    EXPECT_FALSE(other.reservation_valid(foreign_ticket, now));
    other.release_order(foreign_ticket, now);
    EXPECT_FALSE(owner.reservation_valid(foreign_ticket, now));
    EXPECT_FALSE(owner.can_send_order(RateLimitLane::Strategy, 1, now));
    EXPECT_TRUE(other.can_send_order(RateLimitLane::Strategy, 1, now));

    EXPECT_TRUE(other.reserve_order(RateLimitLane::Strategy, 1, now).has_value());
    EXPECT_FALSE(other.can_send_order(RateLimitLane::Strategy, 1, now));
}

TEST(SubmitReservation, ConfigurationUpdateInvalidatesOldTicketWithoutRefundingNewBudget) {
    SpotRateLimitTracker tracker;
    const TimePoint first_window{};
    const TimePoint next_window = first_window + std::chrono::seconds(1);
    configure_strategy_orders(tracker, first_window);

    auto first = tracker.reserve_order(RateLimitLane::Strategy, 1, first_window);
    ASSERT_TRUE(first.has_value());
    auto stale_ticket = std::move(*first);

    configure_strategy_orders(tracker, next_window);
    EXPECT_FALSE(tracker.reservation_valid(stale_ticket, next_window));

    auto current = tracker.reserve_order(RateLimitLane::Strategy, 1, next_window);
    ASSERT_TRUE(current.has_value());
    tracker.release_order(stale_ticket, next_window);
    EXPECT_FALSE(tracker.can_send_order(RateLimitLane::Strategy, 1, next_window));
}

TEST(SubmitReservation, ExpiredTicketReleasedAfterNewWindowConsumptionDoesNotRefundIt) {
    SpotRateLimitTracker tracker;
    const TimePoint first_window{};
    constexpr auto kWindow = std::chrono::seconds(1);
    configure_strategy_orders(tracker, first_window, /*orders_limit*/ 1, /*window_seconds*/ 1);

    auto first = tracker.reserve_order(RateLimitLane::Strategy, 1, first_window);
    ASSERT_TRUE(first.has_value());
    auto expired_ticket = std::move(*first);
    const TimePoint expired_at = first_window + kWindow;
    EXPECT_FALSE(tracker.reservation_valid(expired_ticket, expired_at));

    const TimePoint new_window_after_decay = first_window + 2 * kWindow;
    auto current = tracker.reserve_order(RateLimitLane::Strategy, 1, new_window_after_decay);
    ASSERT_TRUE(current.has_value());
    tracker.release_order(expired_ticket, new_window_after_decay);
    EXPECT_FALSE(tracker.reservation_valid(expired_ticket, new_window_after_decay));
    EXPECT_TRUE(tracker.reservation_valid(*current, new_window_after_decay));
    EXPECT_FALSE(tracker.can_send_order(RateLimitLane::Strategy, 1, new_window_after_decay));
}

TEST(SubmitReservation, ReleasingTheSameTicketTwiceDoesNotRefundTwice) {
    SpotRateLimitTracker tracker;
    const TimePoint now{};
    configure_strategy_orders(tracker, now);

    auto first = tracker.reserve_order(RateLimitLane::Strategy, 1, now);
    ASSERT_TRUE(first.has_value());
    auto first_ticket = std::move(*first);
    tracker.release_order(first_ticket, now);
    EXPECT_FALSE(tracker.reservation_valid(first_ticket, now));

    auto second = tracker.reserve_order(RateLimitLane::Strategy, 1, now);
    ASSERT_TRUE(second.has_value());
    tracker.release_order(first_ticket, now);
    EXPECT_TRUE(tracker.reservation_valid(*second, now));
    EXPECT_FALSE(tracker.can_send_order(RateLimitLane::Strategy, 1, now));
}

TEST(SubmitReservation, WindowDeadlineSaturatesAtTimePointMax) {
    SpotRateLimitTracker tracker;
    const TimePoint near_max = TimePoint::max() - std::chrono::seconds(1);
    configure_strategy_orders(tracker, near_max, /*orders_limit*/ 1, /*window_seconds*/ 30);

    auto reservation = tracker.reserve_order(RateLimitLane::Strategy, 1, near_max);
    ASSERT_TRUE(reservation.has_value());
    EXPECT_TRUE(tracker.reservation_valid(*reservation, near_max));
    EXPECT_TRUE(tracker.reservation_valid(*reservation, TimePoint::max() - SpotRateLimitTracker::Clock::duration{1}));
    EXPECT_FALSE(tracker.reservation_valid(*reservation, TimePoint::max()));
}

}  // namespace
