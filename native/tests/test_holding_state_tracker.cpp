// 批次 6 6a-2: holding_state_tracker.hpp unit tests.
//
// Covers all four transition cases the plan calls out explicitly: Flat->Long and Long->Flat
// must produce a suggested action; Long->Long and Flat->Flat must stay silent (None) -- this is
// the load-bearing test for the external-review-driven fix (see this file's own header comment
// in holding_state_tracker.hpp) that a naive per-bar threshold check would have gotten wrong.

#include <gtest/gtest.h>
#include <hengyuan/holding_state_tracker.hpp>

using hy::HoldingState;
using hy::HoldingStateTracker;
using hy::SuggestedAction;

TEST(HoldingStateTracker, StartsFlat) {
    HoldingStateTracker tracker;
    EXPECT_EQ(tracker.state(), HoldingState::Flat);
}

TEST(HoldingStateTracker, FlatToFlatIsSilent) {
    HoldingStateTracker tracker;
    EXPECT_EQ(tracker.on_target_position(0.0), SuggestedAction::None);
    EXPECT_EQ(tracker.state(), HoldingState::Flat);
    EXPECT_EQ(tracker.on_target_position(0.0), SuggestedAction::None);
    EXPECT_EQ(tracker.state(), HoldingState::Flat);
}

TEST(HoldingStateTracker, FlatToLongEmitsOpen) {
    HoldingStateTracker tracker;
    EXPECT_EQ(tracker.on_target_position(1.0), SuggestedAction::Open);
    EXPECT_EQ(tracker.state(), HoldingState::Long);
}

// The load-bearing case: a sustained trend must NOT re-trigger Open on every bar.
TEST(HoldingStateTracker, LongToLongIsSilentEvenAcrossManyBars) {
    HoldingStateTracker tracker;
    ASSERT_EQ(tracker.on_target_position(1.0), SuggestedAction::Open);
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(tracker.on_target_position(1.0), SuggestedAction::None);
        EXPECT_EQ(tracker.state(), HoldingState::Long);
    }
}

// The other load-bearing case: falling back to 0.0 must emit a closing action, not silence.
TEST(HoldingStateTracker, LongToFlatEmitsClose) {
    HoldingStateTracker tracker;
    ASSERT_EQ(tracker.on_target_position(1.0), SuggestedAction::Open);
    EXPECT_EQ(tracker.on_target_position(0.0), SuggestedAction::Close);
    EXPECT_EQ(tracker.state(), HoldingState::Flat);
}

TEST(HoldingStateTracker, FullRoundTripCycleAcrossMultipleTrades) {
    HoldingStateTracker tracker;
    EXPECT_EQ(tracker.on_target_position(0.0), SuggestedAction::None);   // Flat->Flat
    EXPECT_EQ(tracker.on_target_position(1.0), SuggestedAction::Open);   // Flat->Long
    EXPECT_EQ(tracker.on_target_position(1.0), SuggestedAction::None);  // Long->Long
    EXPECT_EQ(tracker.on_target_position(1.0), SuggestedAction::None);  // Long->Long
    EXPECT_EQ(tracker.on_target_position(0.0), SuggestedAction::Close);  // Long->Flat
    EXPECT_EQ(tracker.on_target_position(0.0), SuggestedAction::None);   // Flat->Flat
    EXPECT_EQ(tracker.on_target_position(1.0), SuggestedAction::Open);   // Flat->Long again
    EXPECT_EQ(tracker.state(), HoldingState::Long);
}

// Any nonzero value (not just exactly 1.0) counts as Long -- defensive coverage in case a caller
// passes a scaled-mode value by mistake; the boolean-mode contract this batch relies on
// guarantees exactly 0.0/1.0, but the tracker's own comparison (!= 0.0) is intentionally not
// stricter than that.
TEST(HoldingStateTracker, AnyNonzeroValueCountsAsLong) {
    HoldingStateTracker tracker;
    EXPECT_EQ(tracker.on_target_position(0.5), SuggestedAction::Open);
    EXPECT_EQ(tracker.state(), HoldingState::Long);
}
