// 批次 6 6b-0a: feed_validity_gate.hpp unit tests -- pure logic, no Boost, no network.
// 批次 6 6b-0f-5 adds the SupervisedFeedInputs/evaluate_supervised_feed_validity tests further down.

#include <gtest/gtest.h>
#include <hengyuan/feed_validity_gate.hpp>

#include <iterator>

using hy::evaluate_feed_validity;
using hy::feed_invalid_reason_is_terminal;
using hy::feed_invalid_reason_name;
using hy::FeedHealthInputs;
using hy::FeedInvalidReason;

using hy::evaluate_supervised_feed_validity;
using hy::supervised_feed_invalid_reason_is_terminal;
using hy::supervised_feed_invalid_reason_name;
using hy::FeedState;
using hy::SupervisedFeedInputs;
using hy::SupervisedFeedInvalidReason;

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

// --- SupervisedFeedInputs / evaluate_supervised_feed_validity (6b-0f-5) --------------------------

namespace {

SupervisedFeedInputs supervised_healthy() {
    SupervisedFeedInputs h;
    h.kline_feed_state = FeedState::Connected;
    h.depth_feed_state = FeedState::Connected;
    h.kline_sync_live = true;
    h.depth_tracking = true;
    return h;
}

}  // namespace

TEST(SupervisedFeedValidityGate, HealthyFeedIsValid) {
    EXPECT_EQ(evaluate_supervised_feed_validity(supervised_healthy()), SupervisedFeedInvalidReason::None);
}

// Default-constructed inputs (every FeedState::Idle, every bool false) must NOT read as healthy.
TEST(SupervisedFeedValidityGate, DefaultConstructedInputsFailClosed) {
    EXPECT_EQ(evaluate_supervised_feed_validity(SupervisedFeedInputs{}), SupervisedFeedInvalidReason::KlineDisconnected);
}

TEST(SupervisedFeedValidityGate, EachConditionAloneInvalidatesTheFeed) {
    {
        auto h = supervised_healthy();
        h.kline_feed_state = FeedState::Terminal;
        EXPECT_EQ(evaluate_supervised_feed_validity(h), SupervisedFeedInvalidReason::KlineFeedGaveUp);
    }
    {
        auto h = supervised_healthy();
        h.depth_feed_state = FeedState::Terminal;
        EXPECT_EQ(evaluate_supervised_feed_validity(h), SupervisedFeedInvalidReason::DepthFeedGaveUp);
    }
    // Every non-Connected, non-Terminal state (Idle/Connecting/Draining/Backoff) reads the same
    // way: "disconnected", not a distinct reason each -- the caller doesn't need to distinguish them.
    for (FeedState s : {FeedState::Idle, FeedState::Connecting, FeedState::Draining, FeedState::Backoff}) {
        auto h = supervised_healthy();
        h.kline_feed_state = s;
        EXPECT_EQ(evaluate_supervised_feed_validity(h), SupervisedFeedInvalidReason::KlineDisconnected);
        auto h2 = supervised_healthy();
        h2.depth_feed_state = s;
        EXPECT_EQ(evaluate_supervised_feed_validity(h2), SupervisedFeedInvalidReason::DepthDisconnected);
    }
    {
        auto h = supervised_healthy();
        h.kline_sync_live = false;
        EXPECT_EQ(evaluate_supervised_feed_validity(h), SupervisedFeedInvalidReason::KlineNotSynced);
    }
    {
        auto h = supervised_healthy();
        h.depth_tracking = false;
        EXPECT_EQ(evaluate_supervised_feed_validity(h), SupervisedFeedInvalidReason::DepthNotTracking);
    }
}

// A supervisor that has given up is reported ahead of everything else, kline ahead of depth --
// matching evaluate_feed_validity's own priority-ordering discipline above.
TEST(SupervisedFeedValidityGate, GaveUpTakesPriorityOverEverythingElse) {
    SupervisedFeedInputs h;  // every field at its unhealthy default
    h.kline_feed_state = FeedState::Terminal;
    h.depth_feed_state = FeedState::Terminal;
    EXPECT_EQ(evaluate_supervised_feed_validity(h), SupervisedFeedInvalidReason::KlineFeedGaveUp);
    h.kline_feed_state = FeedState::Idle;
    EXPECT_EQ(evaluate_supervised_feed_validity(h), SupervisedFeedInvalidReason::DepthFeedGaveUp);
    h.depth_feed_state = FeedState::Idle;
    EXPECT_EQ(evaluate_supervised_feed_validity(h), SupervisedFeedInvalidReason::KlineDisconnected);
    h.kline_feed_state = FeedState::Connected;
    EXPECT_EQ(evaluate_supervised_feed_validity(h), SupervisedFeedInvalidReason::DepthDisconnected);
    h.depth_feed_state = FeedState::Connected;
    EXPECT_EQ(evaluate_supervised_feed_validity(h), SupervisedFeedInvalidReason::KlineNotSynced);
    h.kline_sync_live = true;
    EXPECT_EQ(evaluate_supervised_feed_validity(h), SupervisedFeedInvalidReason::DepthNotTracking);
}

// Unlike the unsupervised gate, a mere disconnect is no longer terminal -- that is the entire
// point of wrapping a feed in PublicFeedSupervisor. Only a supervisor that has itself given up
// (FeedState::Terminal) is.
TEST(SupervisedFeedValidityGate, OnlyGaveUpReasonsAreTerminal) {
    EXPECT_FALSE(supervised_feed_invalid_reason_is_terminal(SupervisedFeedInvalidReason::None));
    EXPECT_FALSE(supervised_feed_invalid_reason_is_terminal(SupervisedFeedInvalidReason::KlineDisconnected));
    EXPECT_FALSE(supervised_feed_invalid_reason_is_terminal(SupervisedFeedInvalidReason::DepthDisconnected));
    EXPECT_FALSE(supervised_feed_invalid_reason_is_terminal(SupervisedFeedInvalidReason::KlineNotSynced));
    EXPECT_FALSE(supervised_feed_invalid_reason_is_terminal(SupervisedFeedInvalidReason::DepthNotTracking));
    EXPECT_TRUE(supervised_feed_invalid_reason_is_terminal(SupervisedFeedInvalidReason::KlineFeedGaveUp));
    EXPECT_TRUE(supervised_feed_invalid_reason_is_terminal(SupervisedFeedInvalidReason::DepthFeedGaveUp));
}

TEST(SupervisedFeedValidityGate, EveryReasonHasADistinctName) {
    const SupervisedFeedInvalidReason all[] = {
        SupervisedFeedInvalidReason::None,
        SupervisedFeedInvalidReason::KlineFeedGaveUp,
        SupervisedFeedInvalidReason::DepthFeedGaveUp,
        SupervisedFeedInvalidReason::KlineDisconnected,
        SupervisedFeedInvalidReason::DepthDisconnected,
        SupervisedFeedInvalidReason::KlineNotSynced,
        SupervisedFeedInvalidReason::DepthNotTracking,
    };
    for (std::size_t i = 0; i < std::size(all); ++i) {
        EXPECT_STRNE(supervised_feed_invalid_reason_name(all[i]), "?");
        for (std::size_t j = i + 1; j < std::size(all); ++j) {
            EXPECT_STRNE(supervised_feed_invalid_reason_name(all[i]), supervised_feed_invalid_reason_name(all[j]));
        }
    }
}
