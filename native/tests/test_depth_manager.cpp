// P2-CORE-OB-02: DepthManager unit tests — snapshot bootstrap + gap detection.
#include <gtest/gtest.h>
#include <hengyuan/depth_manager.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

using hy::BinanceMarketEvent;
using hy::DepthManager;
using hy::DepthSnapshot;
using hy::DepthState;
using hy::EventType;
using hy::PriceLevel;
using hy::Side;

static BinanceMarketEvent make_depth(std::int64_t price, std::int64_t qty,
                                      Side side, std::uint64_t final_u) {
    BinanceMarketEvent ev{};
    ev.event_id = final_u;
    ev.aux_id = final_u;  // U == u for simplicity in most tests
    ev.price_ticks = price;
    ev.qty_lots = qty;
    ev.ts_event_ms = 1000;
    ev.type = EventType::DepthDelta;
    ev.side = side;
    return ev;
}

TEST(DepthManager, StartsInBufferingState) {
    DepthManager dm;
    EXPECT_EQ(dm.state(), DepthState::Buffering);
    EXPECT_TRUE(dm.needs_snapshot());
}

TEST(DepthManager, BuffersEventsBeforeSnapshot) {
    DepthManager dm;
    auto ev = make_depth(100, 10, Side::Buy, 5);
    bool applied = dm.on_depth_event(ev, 5, 5);
    EXPECT_FALSE(applied);
    EXPECT_EQ(dm.stats().events_buffered, 1u);
    EXPECT_EQ(dm.state(), DepthState::Buffering);
}

TEST(DepthManager, BufferingDoesNotCacheReversedUpdateRange) {
    DepthManager dm;

    const auto reversed = make_depth(100, 10, Side::Buy, 9);
    EXPECT_FALSE(dm.on_depth_event(reversed, 10, 9));
    EXPECT_EQ(dm.state(), DepthState::Buffering);
    EXPECT_EQ(dm.stats().events_buffered, 0u);

    const auto valid = make_depth(100, 20, Side::Buy, 10);
    EXPECT_FALSE(dm.on_depth_event(valid, 10, 10));
    EXPECT_EQ(dm.stats().events_buffered, 1u);

    DepthSnapshot snap;
    snap.last_update_id = 9;
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;

    EXPECT_TRUE(dm.apply_snapshot(snap));
    EXPECT_EQ(dm.state(), DepthState::Tracking);
    EXPECT_EQ(dm.stats().events_applied, 1u);
    ASSERT_EQ(dm.book().bid_count(), 2u);
    EXPECT_EQ(dm.book().bids()[0].price_ticks, 100);
    EXPECT_EQ(dm.book().bids()[0].qty_lots, 20);
}

TEST(DepthManager, SnapshotTransitionsToTracking) {
    DepthManager dm;

    // Buffer some events with u > snapshot's lastUpdateId
    auto ev1 = make_depth(100, 10, Side::Buy, 15);
    auto ev2 = make_depth(101, 20, Side::Sell, 15);
    dm.on_depth_event(ev1, 10, 15);  // U=10, u=15
    dm.on_depth_event(ev2, 10, 15);

    // Snapshot with lastUpdateId=12 (events U=10..u=15 span it)
    DepthSnapshot snap;
    snap.last_update_id = 12;
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;

    bool ok = dm.apply_snapshot(snap);
    EXPECT_TRUE(ok);
    EXPECT_EQ(dm.state(), DepthState::Tracking);

    // Snapshot levels + buffered events applied
    auto tob = dm.book().top_of_book();
    ASSERT_TRUE(tob.has_value());
}

TEST(DepthManager, DropsBufferedEventsBeforeSnapshot) {
    DepthManager dm;

    // Buffer events: u=5 (before snapshot), u=15 (after snapshot)
    auto old_ev = make_depth(100, 10, Side::Buy, 5);
    auto new_ev = make_depth(101, 20, Side::Sell, 15);
    dm.on_depth_event(old_ev, 3, 5);
    dm.on_depth_event(new_ev, 10, 15);

    DepthSnapshot snap;
    snap.last_update_id = 12;
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;

    bool ok = dm.apply_snapshot(snap);
    EXPECT_TRUE(ok);
    EXPECT_EQ(dm.stats().events_dropped, 1u);  // old_ev dropped
    EXPECT_EQ(dm.stats().events_applied, 1u);   // new_ev applied
}

TEST(DepthManager, ResyncOnGapInBufferedEvents) {
    DepthManager dm;

    // Buffer event with U=20 but snapshot lastUpdateId=5
    // Gap: first buffered U(20) > lastUpdateId+1(6)
    auto ev = make_depth(100, 10, Side::Buy, 25);
    dm.on_depth_event(ev, 20, 25);

    DepthSnapshot snap;
    snap.last_update_id = 5;
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;

    bool ok = dm.apply_snapshot(snap);
    EXPECT_FALSE(ok);
    EXPECT_EQ(dm.state(), DepthState::Buffering);  // back to buffering
    EXPECT_EQ(dm.stats().resyncs, 1u);
}

TEST(DepthManager, SnapshotReplayGapRejectsAllBufferedEventsAndRecoversWithFreshSnapshot) {
    DepthManager dm;

    const auto before_gap = make_depth(100, 10, Side::Buy, 101);
    const auto after_gap = make_depth(101, 20, Side::Buy, 103);
    EXPECT_FALSE(dm.on_depth_event(before_gap, 101, 101));
    EXPECT_FALSE(dm.on_depth_event(after_gap, 103, 103));

    DepthSnapshot stale_snapshot;
    stale_snapshot.last_update_id = 100;
    stale_snapshot.bids[0] = {99, 50};
    stale_snapshot.bid_count = 1;
    stale_snapshot.asks[0] = {102, 30};
    stale_snapshot.ask_count = 1;

    EXPECT_FALSE(dm.apply_snapshot(stale_snapshot));
    EXPECT_EQ(dm.state(), DepthState::Buffering);
    EXPECT_TRUE(dm.needs_snapshot());
    EXPECT_EQ(dm.book().bid_count(), 0u);
    EXPECT_EQ(dm.book().ask_count(), 0u);
    EXPECT_EQ(dm.stats().resyncs, 1u);
    EXPECT_EQ(dm.stats().events_applied, 0u);

    const auto recovery_event = make_depth(100, 25, Side::Buy, 104);
    EXPECT_FALSE(dm.on_depth_event(recovery_event, 104, 104));

    DepthSnapshot fresh_snapshot;
    fresh_snapshot.last_update_id = 103;
    fresh_snapshot.bids[0] = {99, 50};
    fresh_snapshot.bid_count = 1;
    fresh_snapshot.asks[0] = {102, 30};
    fresh_snapshot.ask_count = 1;

    EXPECT_TRUE(dm.apply_snapshot(fresh_snapshot));
    EXPECT_EQ(dm.state(), DepthState::Tracking);
    EXPECT_FALSE(dm.needs_snapshot());
    EXPECT_EQ(dm.stats().resyncs, 1u);
    EXPECT_EQ(dm.stats().events_applied, 1u);
    ASSERT_EQ(dm.book().bid_count(), 2u);
    EXPECT_EQ(dm.book().bids()[0].price_ticks, 100);
    EXPECT_EQ(dm.book().bids()[0].qty_lots, 25);
    EXPECT_EQ(dm.book().bids()[1].price_ticks, 99);
    EXPECT_EQ(dm.book().bids()[1].qty_lots, 50);
}

TEST(DepthManager, TrackingAppliesEventsDirectly) {
    DepthManager dm;

    DepthSnapshot snap;
    snap.last_update_id = 100;
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;

    dm.apply_snapshot(snap);
    EXPECT_EQ(dm.state(), DepthState::Tracking);

    // Apply events in tracking mode
    auto ev = make_depth(100, 25, Side::Buy, 101);
    bool applied = dm.on_depth_event(ev, 101, 101);
    EXPECT_TRUE(applied);
    EXPECT_EQ(dm.stats().events_applied, 1u);
    EXPECT_EQ(dm.book().bid_count(), 2u);
}

TEST(DepthManager, TrackingDropsOlderRangeWithoutRegressingSequence) {
    DepthManager dm;

    DepthSnapshot snap;
    snap.last_update_id = 100;
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;
    ASSERT_TRUE(dm.apply_snapshot(snap));

    const auto newer = make_depth(100, 25, Side::Buy, 105);
    ASSERT_TRUE(dm.on_depth_event(newer, 101, 105));

    const auto older = make_depth(99, 0, Side::Buy, 104);
    EXPECT_FALSE(dm.on_depth_event(older, 102, 104));
    EXPECT_EQ(dm.state(), DepthState::Tracking);
    EXPECT_EQ(dm.stats().resyncs, 0u);
    EXPECT_EQ(dm.stats().gap_events, 0u);
    EXPECT_EQ(dm.stats().events_applied, 1u);
    ASSERT_EQ(dm.book().bid_count(), 2u);
    EXPECT_EQ(dm.book().bids()[0].price_ticks, 100);
    EXPECT_EQ(dm.book().bids()[0].qty_lots, 25);
    EXPECT_EQ(dm.book().bids()[1].price_ticks, 99);
    EXPECT_EQ(dm.book().bids()[1].qty_lots, 50);

    const auto next = make_depth(101, 10, Side::Buy, 106);
    EXPECT_TRUE(dm.on_depth_event(next, 106, 106));
    EXPECT_EQ(dm.state(), DepthState::Tracking);
    EXPECT_EQ(dm.stats().resyncs, 0u);
    ASSERT_EQ(dm.book().bid_count(), 3u);
    EXPECT_EQ(dm.book().bids()[0].price_ticks, 101);
    EXPECT_EQ(dm.book().bids()[0].qty_lots, 10);
}

TEST(DepthManager, SnapshotReplayDropsOlderRangeAndAllowsSameUpdateIdLevels) {
    DepthManager dm;

    const auto newer = make_depth(100, 25, Side::Buy, 105);
    const auto older = make_depth(99, 0, Side::Buy, 104);
    const auto same_id_bid = make_depth(101, 10, Side::Buy, 105);
    const auto same_id_ask = make_depth(103, 15, Side::Sell, 105);
    EXPECT_FALSE(dm.on_depth_event(newer, 101, 105));
    EXPECT_FALSE(dm.on_depth_event(older, 102, 104));
    EXPECT_FALSE(dm.on_depth_event(same_id_bid, 105, 105));
    EXPECT_FALSE(dm.on_depth_event(same_id_ask, 105, 105));

    DepthSnapshot snap;
    snap.last_update_id = 100;
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;

    EXPECT_TRUE(dm.apply_snapshot(snap));
    EXPECT_EQ(dm.state(), DepthState::Tracking);
    EXPECT_EQ(dm.stats().resyncs, 0u);
    EXPECT_EQ(dm.stats().events_applied, 3u);
    ASSERT_EQ(dm.book().bid_count(), 3u);
    EXPECT_EQ(dm.book().bids()[0].price_ticks, 101);
    EXPECT_EQ(dm.book().bids()[0].qty_lots, 10);
    EXPECT_EQ(dm.book().bids()[1].price_ticks, 100);
    EXPECT_EQ(dm.book().bids()[1].qty_lots, 25);
    EXPECT_EQ(dm.book().bids()[2].price_ticks, 99);
    EXPECT_EQ(dm.book().bids()[2].qty_lots, 50);
    ASSERT_EQ(dm.book().ask_count(), 2u);
    EXPECT_EQ(dm.book().asks()[0].price_ticks, 102);
    EXPECT_EQ(dm.book().asks()[1].price_ticks, 103);
    EXPECT_EQ(dm.book().asks()[1].qty_lots, 15);
}

TEST(DepthManager, TrackingRejectsReversedRangeWithoutResyncOrBookChange) {
    DepthManager dm;

    DepthSnapshot snap;
    snap.last_update_id = 100;
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;
    ASSERT_TRUE(dm.apply_snapshot(snap));
    ASSERT_EQ(dm.state(), DepthState::Tracking);

    const auto reversed = make_depth(99, 0, Side::Buy, 99);
    EXPECT_FALSE(dm.on_depth_event(reversed, 101, 99));
    EXPECT_EQ(dm.state(), DepthState::Tracking);
    EXPECT_EQ(dm.stats().resyncs, 0u);
    EXPECT_EQ(dm.stats().gap_events, 0u);
    EXPECT_EQ(dm.stats().events_applied, 0u);
    ASSERT_EQ(dm.book().bid_count(), 1u);
    EXPECT_EQ(dm.book().bids()[0].price_ticks, 99);
    EXPECT_EQ(dm.book().bids()[0].qty_lots, 50);

    const auto next_contiguous = make_depth(100, 25, Side::Buy, 101);
    EXPECT_TRUE(dm.on_depth_event(next_contiguous, 101, 101));
    EXPECT_EQ(dm.state(), DepthState::Tracking);
    EXPECT_EQ(dm.stats().resyncs, 0u);
    ASSERT_EQ(dm.book().bid_count(), 2u);
    EXPECT_EQ(dm.book().bids()[0].price_ticks, 100);
    EXPECT_EQ(dm.book().bids()[0].qty_lots, 25);
    EXPECT_EQ(dm.book().bids()[1].price_ticks, 99);
    EXPECT_EQ(dm.book().bids()[1].qty_lots, 50);
}

TEST(DepthManager, TrackingDetectsGap) {
    DepthManager dm;

    DepthSnapshot snap;
    snap.last_update_id = 100;
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;

    dm.apply_snapshot(snap);

    // Apply event 101 (OK)
    auto ev1 = make_depth(100, 25, Side::Buy, 101);
    EXPECT_TRUE(dm.on_depth_event(ev1, 101, 101));

    // Gap: skip 102-104, jump to 105
    auto ev2 = make_depth(98, 15, Side::Buy, 105);
    bool applied = dm.on_depth_event(ev2, 105, 105);
    EXPECT_FALSE(applied);
    EXPECT_EQ(dm.state(), DepthState::Buffering);
    EXPECT_EQ(dm.stats().resyncs, 1u);
    EXPECT_EQ(dm.stats().gap_events, 1u);
}

TEST(DepthManager, TrackingAllowsMultiLevelSameUpdateId) {
    DepthManager dm;

    DepthSnapshot snap;
    snap.last_update_id = 100;
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;

    dm.apply_snapshot(snap);

    // Multiple levels from the same depthUpdate (U=101, u=101)
    auto bid = make_depth(98, 20, Side::Buy, 101);
    auto ask = make_depth(103, 15, Side::Sell, 101);
    // First level: U=101, u=101 (101 <= 100+1 = 101, OK)
    EXPECT_TRUE(dm.on_depth_event(bid, 101, 101));
    // Second level: same U/u, last_applied_u_ is now 101
    // U=101 <= 101+1 = 102, OK
    EXPECT_TRUE(dm.on_depth_event(ask, 101, 101));

    EXPECT_EQ(dm.book().bid_count(), 2u);
    EXPECT_EQ(dm.book().ask_count(), 2u);
}

TEST(DepthManager, TrackingAllowsSameUpdateIdAtUint64Maximum) {
    DepthManager dm;
    const std::uint64_t max_id = std::numeric_limits<std::uint64_t>::max();

    DepthSnapshot snap;
    snap.last_update_id = max_id - 1;
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;
    ASSERT_TRUE(dm.apply_snapshot(snap));

    const auto bid = make_depth(100, 25, Side::Buy, max_id);
    const auto ask = make_depth(103, 15, Side::Sell, max_id);
    EXPECT_TRUE(dm.on_depth_event(bid, max_id, max_id));
    EXPECT_TRUE(dm.on_depth_event(ask, max_id, max_id));

    EXPECT_EQ(dm.state(), DepthState::Tracking);
    EXPECT_EQ(dm.stats().resyncs, 0u);
    EXPECT_EQ(dm.stats().gap_events, 0u);
    EXPECT_EQ(dm.stats().events_applied, 2u);
    ASSERT_EQ(dm.book().bid_count(), 2u);
    EXPECT_EQ(dm.book().bids()[0].price_ticks, 100);
    ASSERT_EQ(dm.book().ask_count(), 2u);
    EXPECT_EQ(dm.book().asks()[1].price_ticks, 103);
}

TEST(DepthManager, RemoveLevelViaZeroQty) {
    DepthManager dm;

    DepthSnapshot snap;
    snap.last_update_id = 100;
    snap.bids[0] = {99, 50};
    snap.bids[1] = {98, 30};
    snap.bid_count = 2;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;

    dm.apply_snapshot(snap);
    EXPECT_EQ(dm.book().bid_count(), 2u);

    // Remove bid at 99 (qty=0)
    auto ev = make_depth(99, 0, Side::Buy, 101);
    EXPECT_TRUE(dm.on_depth_event(ev, 101, 101));
    EXPECT_EQ(dm.book().bid_count(), 1u);
    EXPECT_EQ(dm.book().bids()[0].price_ticks, 98);
}

TEST(DepthManager, StartBufferingClearsState) {
    DepthManager dm;

    DepthSnapshot snap;
    snap.last_update_id = 100;
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;

    dm.apply_snapshot(snap);
    EXPECT_EQ(dm.state(), DepthState::Tracking);

    dm.start_buffering();
    EXPECT_EQ(dm.state(), DepthState::Buffering);
    EXPECT_EQ(dm.book().bid_count(), 0u);
    EXPECT_EQ(dm.book().ask_count(), 0u);
}

// --- P2-MD-02 / Track C: buffer-overflow fail-closed regression tests ---

TEST(DepthManager, BufferOverflowIsCountedNotSilent) {
    DepthManager dm;
    // Fill exactly to capacity, then push one more -- that one must be counted as overflow,
    // not silently dropped with zero observability (the previous behavior).
    for (std::size_t i = 0; i < DepthManager::kMaxBuffered; ++i) {
        auto u = static_cast<std::uint64_t>(i + 1);
        auto ev = make_depth(100, 10, Side::Buy, u);
        dm.on_depth_event(ev, u, u);
    }
    EXPECT_EQ(dm.stats().buffer_overflow_count, 0u);

    auto overflow_u1 = static_cast<std::uint64_t>(DepthManager::kMaxBuffered + 1);
    auto overflow_ev = make_depth(100, 10, Side::Buy, overflow_u1);
    dm.on_depth_event(overflow_ev, overflow_u1, overflow_u1);
    EXPECT_EQ(dm.stats().buffer_overflow_count, 1u);

    // A second overflow event increments further.
    auto overflow_u2 = static_cast<std::uint64_t>(DepthManager::kMaxBuffered + 2);
    dm.on_depth_event(overflow_ev, overflow_u2, overflow_u2);
    EXPECT_EQ(dm.stats().buffer_overflow_count, 2u);
}

TEST(DepthManager, OverflowedBufferForcesResyncInsteadOfTracking) {
    DepthManager dm;
    // Fill to capacity + overflow by one, using U/u chosen so a naive replay against this
    // snapshot's lastUpdateId would otherwise look "valid" (first buffered event continues
    // cleanly from lastUpdateId) -- the point of this test is that overflow must still force a
    // resync regardless of whether the buffered prefix itself looks internally consistent.
    for (std::size_t i = 0; i < DepthManager::kMaxBuffered; ++i) {
        auto u = static_cast<std::uint64_t>(i + 1);
        auto ev = make_depth(100, 10, Side::Buy, u);
        dm.on_depth_event(ev, u, u);
    }
    auto overflow_u = static_cast<std::uint64_t>(DepthManager::kMaxBuffered + 1);
    auto overflow_ev = make_depth(100, 10, Side::Buy, overflow_u);
    dm.on_depth_event(overflow_ev, overflow_u, overflow_u);
    ASSERT_EQ(dm.stats().buffer_overflow_count, 1u);

    DepthSnapshot snap;
    snap.last_update_id = 0;  // buffered event #1 has U=1, so U <= lastUpdateId+1 holds
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;

    bool ok = dm.apply_snapshot(snap);
    EXPECT_FALSE(ok);
    EXPECT_EQ(dm.state(), DepthState::Buffering);  // must not have entered Tracking
    EXPECT_EQ(dm.stats().snapshots, 0u);  // this attempt was discarded before being counted
    EXPECT_GE(dm.stats().resyncs, 1u);
}

TEST(DepthManager, OverflowFlagResetsOnFreshBufferingEpisode) {
    DepthManager dm;
    for (std::size_t i = 0; i < DepthManager::kMaxBuffered; ++i) {
        auto u = static_cast<std::uint64_t>(i + 1);
        auto ev = make_depth(100, 10, Side::Buy, u);
        dm.on_depth_event(ev, u, u);
    }
    auto overflow_u = static_cast<std::uint64_t>(DepthManager::kMaxBuffered + 1);
    auto overflow_ev = make_depth(100, 10, Side::Buy, overflow_u);
    dm.on_depth_event(overflow_ev, overflow_u, overflow_u);

    DepthSnapshot bad_snap;
    bad_snap.last_update_id = 0;
    bad_snap.bids[0] = {99, 50};
    bad_snap.bid_count = 1;
    bad_snap.asks[0] = {102, 30};
    bad_snap.ask_count = 1;
    ASSERT_FALSE(dm.apply_snapshot(bad_snap));  // discarded due to overflow, back to Buffering

    // A fresh, non-overflowing snapshot attempt on the new Buffering episode must succeed --
    // start_buffering() (called internally by the discard above) must have reset the flag.
    DepthSnapshot good_snap;
    good_snap.last_update_id = 100;
    good_snap.bids[0] = {99, 50};
    good_snap.bid_count = 1;
    good_snap.asks[0] = {102, 30};
    good_snap.ask_count = 1;
    EXPECT_TRUE(dm.apply_snapshot(good_snap));
    EXPECT_EQ(dm.state(), DepthState::Tracking);
}

TEST(DepthManager, StartBufferingDoesNotUseStaleBookForTrades) {
    // Regression/documentation test for the existing fail-closed behavior that must not be
    // broken by this round's changes: start_buffering() clears the book, so any code that
    // gates simulated trades on top_of_book() being present will correctly see "no book" while
    // Buffering, rather than trading against a stale pre-resync book.
    DepthManager dm;
    DepthSnapshot snap;
    snap.last_update_id = 100;
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {102, 30};
    snap.ask_count = 1;
    dm.apply_snapshot(snap);
    ASSERT_TRUE(dm.book().top_of_book().has_value());

    dm.start_buffering();
    EXPECT_FALSE(dm.book().top_of_book().has_value());
}

// --- Realistic-scale snapshot + top-N window (audit MD-BOOK-002) ---
//
// Every pre-existing test in this file uses bid_count/ask_count of 1 or 2, so the
// entire "book is already at kMaxLevels after a real bootstrap" regime was never
// exercised. These cover the full chain the audit repro walked: a default-config
// REST snapshot (limit=1000, binance_rest_snapshot.hpp) -> DepthManager ->
// OrderBook's 256-level window -> the next live depth delta.

namespace {
DepthSnapshot make_full_snapshot(std::uint64_t last_update_id, std::size_t levels) {
    DepthSnapshot s;
    s.last_update_id = last_update_id;
    for (std::size_t i = 0; i < levels; ++i) {
        s.bids[i] = {static_cast<std::int64_t>(50'000 - static_cast<std::int64_t>(i)), 10};
        s.asks[i] = {static_cast<std::int64_t>(50'001 + static_cast<std::int64_t>(i)), 10};
    }
    s.bid_count = levels;
    s.ask_count = levels;
    return s;
}
}  // namespace

TEST(DepthManager, ThousandLevelSnapshotKeepsBestWindow) {
    DepthManager dm;
    ASSERT_TRUE(dm.apply_snapshot(make_full_snapshot(1000, 1000)));
    EXPECT_EQ(dm.book().bid_count(), hy::kMaxLevels);
    EXPECT_EQ(dm.book().ask_count(), hy::kMaxLevels);

    auto tob = dm.book().top_of_book();
    ASSERT_TRUE(tob.has_value());
    EXPECT_EQ(tob->first, 50'000) << "the BEST bid must survive truncation";
    EXPECT_EQ(tob->second, 50'001);
}

TEST(DepthManager, NewBestBidAfterFullSnapshotReachesTopOfBook) {
    DepthManager dm;
    ASSERT_TRUE(dm.apply_snapshot(make_full_snapshot(1000, 1000)));

    BinanceMarketEvent ev{};
    ev.type = EventType::DepthDelta;
    ev.side = Side::Buy;
    ev.price_ticks = 50'001;  // strictly better than the current best bid
    ev.qty_lots = 42;
    ASSERT_TRUE(dm.on_depth_event(ev, 1001, 1001));

    auto tob = dm.book().top_of_book();
    ASSERT_TRUE(tob.has_value());
    EXPECT_EQ(tob->first, 50'001) << "a full book must not freeze the top of book";
    EXPECT_EQ(dm.stats().deltas_outside_window, 0u);
}

TEST(DepthManager, DeepDeltaCountsAsOutsideWindowWithoutResync) {
    DepthManager dm;
    ASSERT_TRUE(dm.apply_snapshot(make_full_snapshot(1000, 1000)));
    const auto resyncs_before = dm.stats().resyncs;

    BinanceMarketEvent ev{};
    ev.type = EventType::DepthDelta;
    ev.side = Side::Buy;
    ev.price_ticks = 1;  // far worse than the 256th best bid
    ev.qty_lots = 5;
    ASSERT_TRUE(dm.on_depth_event(ev, 1001, 1001));

    EXPECT_EQ(dm.stats().deltas_outside_window, 1u);
    EXPECT_EQ(dm.stats().resyncs, resyncs_before)
        << "a level outside the maintained window is normal, never a resync trigger";
    EXPECT_EQ(dm.state(), DepthState::Tracking);
    EXPECT_EQ(dm.book().bid_count(), hy::kMaxLevels);
}
