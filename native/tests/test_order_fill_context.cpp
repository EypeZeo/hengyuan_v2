// TODO 1A.4 batch 2: order_fill_context.hpp unit tests -- the dedup-safe delta primitive
// shared by all three fill-application paths (direct POST-accept, REST reconciliation, WS
// executionReport).
#include <gtest/gtest.h>
#include <hengyuan/order_fill_context.hpp>

#include <string>

using hy::OrderFillContext;
using hy::OrderSide;

TEST(OrderFillContext, NeverTrackedCoidFindReturnsNullptr) {
    OrderFillContext ctx;
    EXPECT_EQ(ctx.find("HY-A"), nullptr);
}

TEST(OrderFillContext, TrackStoresEntryFindableAfterward) {
    OrderFillContext ctx;
    ASSERT_TRUE(ctx.track("HY-A", 7, OrderSide::Sell, 2, 4, 6));
    const auto* e = ctx.find("HY-A");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->symbol_id, 7u);
    EXPECT_EQ(e->side, OrderSide::Sell);
    EXPECT_EQ(e->price_scale, 2);
    EXPECT_EQ(e->qty_scale, 4);
    EXPECT_EQ(e->quote_scale, 6);
    EXPECT_EQ(ctx.count(), 1u);
}

TEST(OrderFillContext, TrackRefusesDuplicateCoid) {
    OrderFillContext ctx;
    ASSERT_TRUE(ctx.track("HY-A", 1, OrderSide::Buy, 0, 0, 0));
    EXPECT_FALSE(ctx.track("HY-A", 2, OrderSide::Sell, 0, 0, 0));
    EXPECT_EQ(ctx.count(), 1u);
}

TEST(OrderFillContext, TrackRefusesEmptyOrOverlongCoid) {
    OrderFillContext ctx;
    EXPECT_FALSE(ctx.track("", 1, OrderSide::Buy, 0, 0, 0));
    std::string overlong(hy::kClientOrderIdLen + 1, 'x');
    EXPECT_FALSE(ctx.track(overlong, 1, OrderSide::Buy, 0, 0, 0));
    EXPECT_EQ(ctx.count(), 0u);
}

TEST(OrderFillContext, RemoveThenFindReturnsNullptr) {
    OrderFillContext ctx;
    ASSERT_TRUE(ctx.track("HY-A", 1, OrderSide::Buy, 0, 0, 0));
    ctx.remove("HY-A");
    EXPECT_EQ(ctx.find("HY-A"), nullptr);
    EXPECT_EQ(ctx.count(), 0u);
}

TEST(OrderFillContext, RemoveOfUntrackedCoidIsSafeNoOp) {
    OrderFillContext ctx;
    ctx.remove("HY-NEVER-TRACKED");  // must not crash
    SUCCEED();
}

TEST(OrderFillContext, RemoveOnlyAffectsItsOwnEntry) {
    OrderFillContext ctx;
    ASSERT_TRUE(ctx.track("HY-A", 1, OrderSide::Buy, 0, 0, 0));
    ASSERT_TRUE(ctx.track("HY-B", 2, OrderSide::Sell, 0, 0, 0));
    ASSERT_TRUE(ctx.track("HY-C", 3, OrderSide::Buy, 0, 0, 0));
    ctx.remove("HY-B");
    EXPECT_EQ(ctx.count(), 2u);
    EXPECT_NE(ctx.find("HY-A"), nullptr);
    EXPECT_EQ(ctx.find("HY-B"), nullptr);
    EXPECT_NE(ctx.find("HY-C"), nullptr);
}

TEST(OrderFillContext, ConsumeDeltaOnUntrackedCoidReturnsZero) {
    OrderFillContext ctx;
    EXPECT_EQ(ctx.consume_delta("HY-NEVER-TRACKED", 100), 0);
}

// The exact dedup-safety scenario from this batch's own plan, verified numerically: order
// accepted with intended qty 100.
TEST(OrderFillContext, DedupSafetyWorkedNumericScenario) {
    OrderFillContext ctx;
    ASSERT_TRUE(ctx.track("HY-A", 1, OrderSide::Buy, 0, 0, 0));

    // First executionReport z=30 -> delta = 30-0 = 30, baseline advances to 30.
    EXPECT_EQ(ctx.consume_delta("HY-A", 30), 30);

    // Duplicate/redelivered z=30 -> delta = 30-30 = 0, correctly a no-op, baseline stays 30.
    EXPECT_EQ(ctx.consume_delta("HY-A", 30), 0);

    // Stale/out-of-order z=25 (an earlier partial state redelivered) -> delta = 25-30 = -5,
    // apply_fill() (caller) no-ops on this, but the baseline here must NOT regress to 25.
    EXPECT_EQ(ctx.consume_delta("HY-A", 25), -5);

    // Proof the baseline did not regress: a subsequent genuinely-new z=60 must produce
    // delta = 60-30 = 30 (relative to the still-30 baseline), not 60-25 = 35.
    EXPECT_EQ(ctx.consume_delta("HY-A", 60), 30);

    // Final z=100 (Filled) -> delta = 100-60 = 40.
    EXPECT_EQ(ctx.consume_delta("HY-A", 100), 40);

    // Sum of all positive deltas applied: 30+30+40 = 100, matching the order's full intended
    // qty exactly once -- not 30+30-5+30+40=125 and not double-counted any other way.
}

TEST(OrderFillContext, DistinctOrdersTrackedIndependently) {
    OrderFillContext ctx;
    ASSERT_TRUE(ctx.track("HY-A", 1, OrderSide::Buy, 0, 0, 0));
    ASSERT_TRUE(ctx.track("HY-B", 2, OrderSide::Sell, 0, 0, 0));
    EXPECT_EQ(ctx.consume_delta("HY-A", 50), 50);
    EXPECT_EQ(ctx.consume_delta("HY-B", 20), 20);
    // HY-A's baseline (50) must not affect HY-B's delta computation.
    EXPECT_EQ(ctx.consume_delta("HY-B", 20), 0);
}

TEST(OrderFillContext, CapacityExhaustionRefusesWithoutCorruptingOtherEntries) {
    OrderFillContext ctx;
    for (std::uint32_t i = 0; i < 64; ++i) {
        ASSERT_TRUE(ctx.track("HY-" + std::to_string(i), i, OrderSide::Buy, 0, 0, 0));
    }
    EXPECT_EQ(ctx.count(), 64u);
    EXPECT_FALSE(ctx.track("HY-64", 64, OrderSide::Buy, 0, 0, 0));
    EXPECT_EQ(ctx.count(), 64u);
    // Existing entries untouched by the refused insert.
    EXPECT_NE(ctx.find("HY-0"), nullptr);
    EXPECT_NE(ctx.find("HY-63"), nullptr);
}
