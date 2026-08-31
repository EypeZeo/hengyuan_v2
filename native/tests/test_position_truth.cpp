// Pure in-memory struct/behavior tests for position_truth.hpp -- no I/O, no
// threading (this class carries none of its own; see the header's own
// THREAD OWNERSHIP note).
#include <gtest/gtest.h>
#include <hengyuan/position_truth.hpp>

using namespace hy;

TEST(PositionTruth, NeverObservedSymbolReadsAsFlat) {
    PositionTruth truth;
    EXPECT_EQ(truth.net_qty_ticks(42), 0);
    EXPECT_EQ(truth.tracked_symbol_count(), 0u);
}

TEST(PositionTruth, BuyFillAccumulatesPositive) {
    PositionTruth truth;
    truth.apply_fill(1, OrderSide::Buy, 100);
    EXPECT_EQ(truth.net_qty_ticks(1), 100);
    EXPECT_EQ(truth.tracked_symbol_count(), 1u);
}

TEST(PositionTruth, SellFillAccumulatesNegative) {
    PositionTruth truth;
    truth.apply_fill(1, OrderSide::Sell, 100);
    EXPECT_EQ(truth.net_qty_ticks(1), -100);
}

TEST(PositionTruth, RepeatedFillsAccumulateOnTheSameSymbol) {
    PositionTruth truth;
    truth.apply_fill(1, OrderSide::Buy, 100);
    truth.apply_fill(1, OrderSide::Buy, 50);
    truth.apply_fill(1, OrderSide::Sell, 30);
    EXPECT_EQ(truth.net_qty_ticks(1), 120);  // 100 + 50 - 30
    EXPECT_EQ(truth.tracked_symbol_count(), 1u);  // still one symbol, not three entries
}

TEST(PositionTruth, DistinctSymbolsTrackedIndependently) {
    PositionTruth truth;
    truth.apply_fill(1, OrderSide::Buy, 100);
    truth.apply_fill(2, OrderSide::Sell, 40);
    EXPECT_EQ(truth.net_qty_ticks(1), 100);
    EXPECT_EQ(truth.net_qty_ticks(2), -40);
    EXPECT_EQ(truth.tracked_symbol_count(), 2u);
}

TEST(PositionTruth, ZeroDeltaIsANoOp) {
    PositionTruth truth;
    truth.apply_fill(1, OrderSide::Buy, 0);
    EXPECT_EQ(truth.net_qty_ticks(1), 0);
    // Must not even allocate a tracked slot for a no-op call -- a symbol that
    // never had a real fill should stay indistinguishable from one that was
    // never mentioned at all.
    EXPECT_EQ(truth.tracked_symbol_count(), 0u);
}

TEST(PositionTruth, NegativeDeltaIsANoOp) {
    PositionTruth truth;
    // Defensive guard: every real caller already only calls with deltas > 0
    // (live_submit_orchestrator.hpp's direct-fill branch, order_tracker.hpp's
    // drain_reconcile_events()), but apply_fill() itself must not corrupt
    // state if ever called with a negative delta by mistake.
    truth.apply_fill(1, OrderSide::Buy, -50);
    EXPECT_EQ(truth.net_qty_ticks(1), 0);
    EXPECT_EQ(truth.tracked_symbol_count(), 0u);
}

TEST(PositionTruth, CapacityExhaustionRefusesSilentlyWithoutCorruptingOtherSlots) {
    PositionTruth truth;
    // Fill every slot up to kMaxSymbols (account_truth.hpp's definition, 64).
    for (std::uint32_t sym = 1; sym <= 64; ++sym) {
        truth.apply_fill(sym, OrderSide::Buy, 10);
    }
    EXPECT_EQ(truth.tracked_symbol_count(), 64u);
    for (std::uint32_t sym = 1; sym <= 64; ++sym) {
        EXPECT_EQ(truth.net_qty_ticks(sym), 10) << "symbol " << sym;
    }

    // One more distinct symbol beyond capacity: refused, and every existing
    // slot's value must be unchanged.
    truth.apply_fill(65, OrderSide::Buy, 999);
    EXPECT_EQ(truth.net_qty_ticks(65), 0);  // never got a slot -- reads as flat
    EXPECT_EQ(truth.tracked_symbol_count(), 64u);
    EXPECT_EQ(truth.net_qty_ticks(1), 10);  // untouched by the refused insert
}

TEST(PositionTruth, IsTriviallyCopyableStandardLayout) {
    // PositionEntry crosses no thread boundary today, but keeping it a
    // trivially-copyable POD matches every other value type in this
    // codebase's durable/cross-boundary structs.
    static_assert(std::is_trivially_copyable_v<PositionEntry>);
    static_assert(std::is_standard_layout_v<PositionEntry>);
}
