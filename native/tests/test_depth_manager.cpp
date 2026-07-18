// P2-CORE-OB-02: DepthManager unit tests — snapshot bootstrap + gap detection.
#include <gtest/gtest.h>
#include <hengyuan/depth_manager.hpp>

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
