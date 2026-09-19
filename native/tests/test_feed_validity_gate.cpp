// 批次 6 6b-0a: feed_validity_gate.hpp unit tests -- pure logic, no Boost, no network.

#include <gtest/gtest.h>
#include <hengyuan/feed_validity_gate.hpp>

#include <iterator>

using hy::evaluate_feed_validity;
using hy::feed_invalid_reason_is_terminal;
using hy::feed_invalid_reason_name;
using hy::FeedHealthInputs;
using hy::FeedInvalidReason;

namespace {

FeedHealthInputs healthy() {
    FeedHealthInputs h;
    h.depth_tracking = true;
    return h;
}

}  // namespace

TEST(FeedValidityGate, HealthyFeedIsValid) {
    EXPECT_EQ(evaluate_feed_validity(healthy()), FeedInvalidReason::None);
}

// Default-constructed inputs must NOT read as healthy: depth_tracking defaults false, so an
// uninitialized caller fails closed rather than open.
TEST(FeedValidityGate, DefaultConstructedInputsFailClosed) {
    EXPECT_EQ(evaluate_feed_validity(FeedHealthInputs{}), FeedInvalidReason::DepthNotTracking);
}

TEST(FeedValidityGate, EachConditionAloneInvalidatesTheFeed) {
    {
        auto h = healthy();
        h.kline_session_stopped = true;
        EXPECT_EQ(evaluate_feed_validity(h), FeedInvalidReason::KlineSessionStopped);
    }
    {
        auto h = healthy();
        h.depth_session_stopped = true;
        EXPECT_EQ(evaluate_feed_validity(h), FeedInvalidReason::DepthSessionStopped);
    }
    {
        auto h = healthy();
        h.kline_session_suspended = true;
        EXPECT_EQ(evaluate_feed_validity(h), FeedInvalidReason::KlineSessionSuspended);
    }
    {
        auto h = healthy();
        h.consumer_continuity_broken = true;
        EXPECT_EQ(evaluate_feed_validity(h), FeedInvalidReason::ConsumerContinuityBroken);
    }
    {
        auto h = healthy();
        h.depth_tracking = false;
        EXPECT_EQ(evaluate_feed_validity(h), FeedInvalidReason::DepthNotTracking);
    }
}

// A stopped session is the terminal, restart-required condition -- it must be the one reported
// when several things are wrong at once.
TEST(FeedValidityGate, StoppedSessionTakesPriorityOverEverythingElse) {
    FeedHealthInputs h;  // depth_tracking false too
    h.kline_session_stopped = true;
    h.depth_session_stopped = true;
    h.kline_session_suspended = true;
    h.consumer_continuity_broken = true;
    EXPECT_EQ(evaluate_feed_validity(h), FeedInvalidReason::KlineSessionStopped);
    h.kline_session_stopped = false;
    EXPECT_EQ(evaluate_feed_validity(h), FeedInvalidReason::DepthSessionStopped);
    h.depth_session_stopped = false;
    EXPECT_EQ(evaluate_feed_validity(h), FeedInvalidReason::KlineSessionSuspended);
    h.kline_session_suspended = false;
    EXPECT_EQ(evaluate_feed_validity(h), FeedInvalidReason::ConsumerContinuityBroken);
}

TEST(FeedValidityGate, OnlyDepthResyncIsNonTerminal) {
    EXPECT_FALSE(feed_invalid_reason_is_terminal(FeedInvalidReason::None));
    EXPECT_FALSE(feed_invalid_reason_is_terminal(FeedInvalidReason::DepthNotTracking));
    EXPECT_TRUE(feed_invalid_reason_is_terminal(FeedInvalidReason::KlineSessionStopped));
    EXPECT_TRUE(feed_invalid_reason_is_terminal(FeedInvalidReason::DepthSessionStopped));
    EXPECT_TRUE(feed_invalid_reason_is_terminal(FeedInvalidReason::KlineSessionSuspended));
    EXPECT_TRUE(feed_invalid_reason_is_terminal(FeedInvalidReason::ConsumerContinuityBroken));
}

TEST(FeedValidityGate, EveryReasonHasADistinctName) {
    const FeedInvalidReason all[] = {
        FeedInvalidReason::None,
        FeedInvalidReason::KlineSessionStopped,
        FeedInvalidReason::DepthSessionStopped,
        FeedInvalidReason::KlineSessionSuspended,
        FeedInvalidReason::ConsumerContinuityBroken,
        FeedInvalidReason::DepthNotTracking,
    };
    for (std::size_t i = 0; i < std::size(all); ++i) {
        EXPECT_STRNE(feed_invalid_reason_name(all[i]), "?");
        for (std::size_t j = i + 1; j < std::size(all); ++j) {
            EXPECT_STRNE(feed_invalid_reason_name(all[i]), feed_invalid_reason_name(all[j]));
        }
    }
}

TEST(FeedValidityGate, DefaultMaxRunStaysBelowTheBinance24HourConnectionLimit) {
    EXPECT_GT(hy::kDefaultMaxRunSeconds, 0);
    EXPECT_LT(hy::kDefaultMaxRunSeconds, 24 * 60 * 60);
}
