// 批次 6 6b-0f-6: depth_feed_driver.hpp tests -- Boost-free, no network.
//
// Same philosophy as test_kline_feed_driver.cpp: the orchestration is where a recovery bug would
// actually live (forgetting to reset on a new generation, double-counting a rejected event as
// applied, never reporting a rejected snapshot back to the gate so it hammers the source), so
// everything except the session and the REST fetcher is REAL here -- a real SnapshotRefreshGate
// running a hand-released fetcher on a real worker thread, a real DepthManager, a real
// InputValidator, a real SpscRing and a real PublicFeedSupervisor. The binary carries the
// `concurrency` label because of the gate's worker thread (same reason test_snapshot_refresh_gate
// and test_kline_feed_driver do).

#include <gtest/gtest.h>
#include <hengyuan/depth_feed_driver.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using hy::BinanceMarketEvent;
using hy::DepthFeedDriver;
using hy::DepthFeedDriverConfig;
using hy::DepthFeedTickReport;
using hy::DepthManager;
using hy::DepthSnapshot;
using hy::DepthState;
using hy::EventType;
using hy::InputValidator;
using hy::PriceLevel;
using hy::Side;
using hy::SnapshotRefreshGate;
using hy::SnapshotRequest;

namespace {

// The session the supervisor manages AND the driver watches via supervisor.generation() -- the
// driver itself never touches session methods directly, only through PublicFeedSupervisor.
struct FakeSession {
    explicit FakeSession(std::uint64_t g) : generation(g) {}
    void start() {}
    void stop() { stopped_flag.store(true); }
    bool stopped() const { return stopped_flag.load(); }
    bool is_connected() const { return true; }

    const std::uint64_t generation;
    std::atomic<bool> stopped_flag{false};
};
static_assert(hy::SupervisedFeedSession<FakeSession>);

// Identical shape/discipline to test_snapshot_refresh_gate.cpp's own GatedFetcher: completion is
// controlled by the test thread so the gate's non-blocking behavior stays deterministic.
class GatedFetcher {
public:
    std::optional<DepthSnapshot> operator()(const SnapshotRequest& req) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++call_count_;
            last_request_ = req;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return ready_; });
        ready_ = false;
        return result_;
    }

    void unblock(std::optional<DepthSnapshot> result) {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(result);
        ready_ = true;
        cv_.notify_all();
    }

    int call_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return call_count_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_{false};
    std::optional<DepthSnapshot> result_;
    int call_count_{0};
    SnapshotRequest last_request_;
};

// Same "must unblock before any early return" discipline as test_snapshot_refresh_gate.cpp's own
// WaitForCallCount -- a fetch left permanently blocked hangs SnapshotRefreshGate's destructor,
// taking the whole binary down with it.
::testing::AssertionResult WaitForCallCount(const GatedFetcher& fetcher, int expected,
                                            std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fetcher.call_count() >= expected) return ::testing::AssertionSuccess();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ::testing::AssertionFailure()
           << "call_count() never reached " << expected << " (stuck at " << fetcher.call_count() << ")";
}

hy::FeedSupervisorPolicy quiet_policy() {
    hy::FeedSupervisorPolicy p;
    p.jitter_percent = 0;
    p.initial_backoff_ms = 1;
    p.max_backoff_ms = 1;
    p.connect_deadline_ms = 5000;
    return p;
}

BinanceMarketEvent depth_event(std::uint64_t first_update_id, std::uint64_t final_update_id,
                               std::int64_t price_ticks = 100, std::int64_t qty_lots = 10,
                               Side side = Side::Buy) {
    BinanceMarketEvent ev{};
    ev.event_id = final_update_id;
    ev.aux_id = first_update_id;
    ev.price_ticks = price_ticks;
    ev.qty_lots = qty_lots;
    ev.type = EventType::DepthDelta;
    ev.side = side;
    ev.symbol_id = 0;
    ev.ts_event_ms = 1;
    return ev;
}

DepthSnapshot snapshot(std::uint64_t last_update_id) {
    DepthSnapshot snap;
    snap.last_update_id = last_update_id;
    snap.bids[0] = PriceLevel{100, 10};
    snap.bid_count = 1;
    snap.asks[0] = PriceLevel{101, 10};
    snap.ask_count = 1;
    return snap;
}

constexpr std::size_t kRingCapacity = 8192;  // comfortably above DepthManager::kMaxBuffered (4096)
                                              // for the buffer-overflow test

struct Rig {
    using Driver = DepthFeedDriver<FakeSession, kRingCapacity>;

    explicit Rig(DepthFeedDriverConfig cfg = {})
        : gate([this](const SnapshotRequest& r) { return fetcher(r); })
        , supervisor(
              [this](std::uint64_t generation) {
                  auto s = std::make_shared<FakeSession>(generation);
                  sessions.push_back(s);
                  return s;
              },
              quiet_policy())
        , driver(supervisor, *ring, validator, *depth_mgr, gate, SnapshotRequest{"BTCUSDT", 1, 1}, cfg) {}

    // A fetch still blocked at the end of a test must be released before the gate's destructor
    // joins it (members are destroyed AFTER this body runs).
    ~Rig() { fetcher.unblock(std::nullopt); }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    FakeSession& session() { return *sessions.back(); }

    void push(const BinanceMarketEvent& ev) { ASSERT_TRUE(ring->try_push(ev)); }

    DepthFeedTickReport tick(std::int64_t now_ms) { return driver.tick(now_ms); }

    // Ticks (at a frozen synthetic time) until a snapshot is collected; returns THAT tick's report.
    DepthFeedTickReport tick_until_snapshot_collected(std::int64_t now_ms) {
        DepthFeedTickReport report;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            report = tick(now_ms);
            if (report.snapshot_applied) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return report;
    }

    // Ticks until the supervisor has moved to a NEW generation (a reconnect happened). Must advance
    // its OWN now_ms each iteration -- the supervisor's backoff/Draining logic needs the clock to
    // actually pass next_attempt_ms_ to retry; a frozen timestamp would spin forever (bounded only
    // by this function's own wall-clock deadline, not by anything the supervisor does).
    void tick_until_generation(std::uint64_t target_generation, std::int64_t start_now_ms) {
        std::int64_t now_ms = start_now_ms;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            tick(now_ms);
            if (supervisor.generation() >= target_generation) return;
            now_ms += 2;  // past quiet_policy()'s 1ms backoff cap
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ADD_FAILURE() << "supervisor never reached generation " << target_generation
                      << " (stuck at " << supervisor.generation() << ")";
    }

    // Declaration order is construction order: the fetcher must outlive the gate (whose destructor
    // joins the worker that is calling it), and the session list the supervisor's factory appends to.
    // depth_mgr/ring are heap-allocated, not direct members: DepthManager's buffered-event arrays
    // (kMaxBuffered=4096 slots, ~320KB) plus an 8192-slot SpscRing<BinanceMarketEvent> (~512KB) as
    // stack-local Rig members overflowed the thread stack the instant Rig went out of scope of a
    // trivial function (STATUS_STACK_OVERFLOW, found the hard way -- see git history). The real
    // harness already heap-allocates its own depth ring for the identical reason
    // (live_submit_preflight_harness.cpp's depth_ring); this mirrors that, not a new pattern.
    GatedFetcher fetcher;
    SnapshotRefreshGate gate;
    InputValidator validator;
    std::unique_ptr<DepthManager> depth_mgr = std::make_unique<DepthManager>();
    std::unique_ptr<Driver::Ring> ring = std::make_unique<Driver::Ring>();
    std::vector<std::shared_ptr<FakeSession>> sessions;
    hy::PublicFeedSupervisor<FakeSession> supervisor;
    Driver driver;
};

// Gets a rig from a cold start to Tracking with the given last_update_id, with no buffered events
// in between (the simplest path).
void go_tracking(Rig& rig, std::uint64_t last_update_id, std::int64_t now_ms = 0) {
    (void)rig.tick(now_ms);  // starts generation 1, resets to Buffering, starts the snapshot fetch
    ASSERT_TRUE(WaitForCallCount(rig.fetcher, 1, std::chrono::seconds(2)));
    rig.fetcher.unblock(snapshot(last_update_id));
    const auto r = rig.tick_until_snapshot_collected(now_ms);
    ASSERT_TRUE(r.snapshot_apply_ok);
    ASSERT_EQ(rig.depth_mgr->state(), DepthState::Tracking);
}

}  // namespace

// --- generation resets -----------------------------------------------------------------------

TEST(DepthFeedDriver, StartupResetsDepthManagerToBufferingExactlyOnce) {
    Rig rig;
    const auto r1 = rig.tick(0);
    EXPECT_TRUE(r1.generation_reset);
    EXPECT_EQ(rig.driver.stats().generation_resets, 1U);
    EXPECT_EQ(rig.depth_mgr->state(), DepthState::Buffering);

    // The same generation on a later tick must not fire again.
    const auto r2 = rig.tick(1);
    EXPECT_FALSE(r2.generation_reset);
    EXPECT_EQ(rig.driver.stats().generation_resets, 1U);
}

TEST(DepthFeedDriver, AReconnectResetsAgainExactlyOnceAndClearsAStaleTrackingState) {
    Rig rig;
    go_tracking(rig, 100);
    ASSERT_EQ(rig.driver.stats().generation_resets, 1U);

    rig.session().stop();  // the connection drops
    rig.tick_until_generation(2, 1000);
    // The new generation's very own tick already reset it -- no extra tick needed to observe this.
    EXPECT_EQ(rig.driver.stats().generation_resets, 2U);
    EXPECT_EQ(rig.depth_mgr->state(), DepthState::Buffering)
        << "must not still read Tracking against the old generation's now-meaningless update-id baseline";
}

// --- draining events into DepthManager --------------------------------------------------------

TEST(DepthFeedDriver, ValidEventsAfterASnapshotAreAppliedAndCounted) {
    Rig rig;
    go_tracking(rig, 100);

    rig.push(depth_event(101, 101));
    rig.push(depth_event(102, 102));
    rig.push(depth_event(103, 103));
    const auto r = rig.tick(1001);
    EXPECT_EQ(r.events_drained, 3U);
    EXPECT_EQ(r.events_applied, 3U);
    EXPECT_EQ(r.events_rejected, 0U);
    EXPECT_EQ(rig.driver.stats().events_applied, 3U);
    EXPECT_EQ(rig.depth_mgr->stats().events_applied, 3U);
}

TEST(DepthFeedDriver, RejectedEventsAreCountedNotApplied) {
    Rig rig;
    go_tracking(rig, 100);

    BinanceMarketEvent bad = depth_event(101, 101);
    bad.price_ticks = -5;  // InputValidator::RejectNegativePrice
    rig.push(bad);
    const auto r = rig.tick(1001);
    EXPECT_EQ(r.events_drained, 1U);
    EXPECT_EQ(r.events_applied, 0U);
    EXPECT_EQ(r.events_rejected, 1U);
    EXPECT_EQ(rig.driver.stats().events_rejected, 1U);
    // Rejected at the validator: DepthManager must never have seen it.
    EXPECT_EQ(rig.depth_mgr->stats().events_applied, 0U);
}

// InputValidator deliberately does NOT apply DropDuplicate to EventType::DepthDelta (multiple price
// levels legitimately share one final-update-id per message -- input_validator.cpp's own comment).
// So a duplicate can only ever reach the driver's reject list through a non-depth event type; the
// rejection must still be counted (not silently ignored), even though it was never going to reach
// DepthManager either way (the type check gates that separately).
TEST(DepthFeedDriver, ADuplicateNonDepthEventIsRejectedNotSilentlyDropped) {
    Rig rig;
    go_tracking(rig, 100);

    BinanceMarketEvent trade = depth_event(101, 101);
    trade.type = EventType::Trade;
    rig.push(trade);
    const auto first = rig.tick(1001);
    EXPECT_EQ(first.events_rejected, 0U) << "not seen before: accepted, just never reaches DepthManager";

    rig.push(trade);  // exact repeat: InputValidator::DropDuplicate for a non-DepthDelta type
    const auto second = rig.tick(1002);
    EXPECT_EQ(second.events_rejected, 1U)
        << "DropDuplicate must still be counted as rejected, not fall through uncounted";
    EXPECT_EQ(second.events_applied, 0U);
}

TEST(DepthFeedDriver, AResyncRequiredEventForcesBufferingAndIsNotCountedApplied) {
    Rig rig;
    go_tracking(rig, 100);

    BinanceMarketEvent resync = depth_event(101, 101);
    resync.flags |= hy::event_flag::kResyncRequired;
    rig.push(resync);
    const auto r = rig.tick(1001);
    EXPECT_TRUE(r.resync_requested);
    EXPECT_EQ(r.events_applied, 0U);
    EXPECT_EQ(rig.driver.stats().resyncs_requested, 1U);
    EXPECT_EQ(rig.depth_mgr->state(), DepthState::Buffering);
}

TEST(DepthFeedDriver, TheDrainIsBoundedPerTick) {
    DepthFeedDriverConfig cfg;
    cfg.max_events_per_tick = 5;
    Rig rig(cfg);
    go_tracking(rig, 100);

    for (std::uint64_t i = 0; i < 8; ++i) rig.push(depth_event(101 + i, 101 + i));
    const auto r1 = rig.tick(1001);
    EXPECT_EQ(r1.events_drained, 5U) << "a burst must not starve the rest of the caller's loop";
    const auto r2 = rig.tick(1002);
    EXPECT_EQ(r2.events_drained, 3U) << "the rest of the burst is drained on the next tick, not lost";
    EXPECT_EQ(rig.driver.stats().events_applied, 8U);
}

// --- non-depth events reach the ring but must never reach DepthManager --------------------------

TEST(DepthFeedDriver, NonDepthDeltaEventsAreDrainedButNeverReachDepthManager) {
    Rig rig;
    go_tracking(rig, 100);

    BinanceMarketEvent trade = depth_event(101, 101);
    trade.type = EventType::Trade;
    rig.push(trade);
    const auto r = rig.tick(1001);
    EXPECT_EQ(r.events_drained, 1U);
    EXPECT_EQ(r.events_applied, 0U);
    EXPECT_EQ(r.events_rejected, 0U);
    EXPECT_EQ(rig.depth_mgr->stats().events_applied, 0U);
}

// --- the snapshot gate -----------------------------------------------------------------------

TEST(DepthFeedDriver, ASuccessfulSnapshotIsAppliedAndReportedInTheSameTickItArrives) {
    Rig rig;
    (void)rig.tick(0);
    ASSERT_TRUE(WaitForCallCount(rig.fetcher, 1, std::chrono::seconds(2)));
    rig.fetcher.unblock(snapshot(50));
    const auto r = rig.tick_until_snapshot_collected(0);
    EXPECT_TRUE(r.snapshot_applied);
    EXPECT_TRUE(r.snapshot_apply_ok);
    EXPECT_EQ(rig.driver.stats().snapshots_applied, 1U);
    EXPECT_EQ(rig.depth_mgr->state(), DepthState::Tracking);
}

TEST(DepthFeedDriver, ABufferOverflowDuringBufferingRejectsTheSnapshotAndTellsTheGate) {
    DepthFeedDriverConfig cfg;
    cfg.max_events_per_tick = 5000;  // one tick is enough to push past kMaxBuffered (4096)
    Rig rig(cfg);
    (void)rig.tick(0);  // generation 1, Buffering, snapshot fetch starts
    ASSERT_TRUE(WaitForCallCount(rig.fetcher, 1, std::chrono::seconds(2)));

    // Overflow DepthManager's own buffered-event ring WHILE the fetch is still in flight (the
    // realistic case: a slow snapshot fetch during a burst of activity).
    for (std::uint64_t i = 0; i < DepthManager::kMaxBuffered + 1; ++i) {
        rig.push(depth_event(1 + i, 1 + i));
    }
    const auto drained = rig.tick(1);
    EXPECT_EQ(drained.events_drained, DepthManager::kMaxBuffered + 1);

    rig.fetcher.unblock(snapshot(1));
    const auto r = rig.tick_until_snapshot_collected(2);
    EXPECT_TRUE(r.snapshot_applied);
    EXPECT_FALSE(r.snapshot_apply_ok) << "a known-incomplete buffered prefix must not be trusted";
    EXPECT_EQ(rig.driver.stats().snapshots_rejected, 1U);
    // apply_snapshot() rejecting an overflowed buffer forces DepthManager straight back to
    // Buffering on its own (depth_manager.hpp) -- still not Tracking, a fresh attempt is needed.
    EXPECT_EQ(rig.depth_mgr->state(), DepthState::Buffering);

    // The rejection MUST be reported back to the gate (notify_apply_result(false)) so ITS OWN
    // cooldown applies -- DepthManager::needs_snapshot() is true again immediately (state is back to
    // Buffering), so without that report the driver would refetch on the very next tick, hammering
    // the source. Spread over real time (not a tight loop): a forgotten report would spawn a genuinely
    // new worker thread, which needs real scheduling time to run and increment call_count() -- the
    // same race WaitForCallCount's own header comment warns about, just in the other direction here.
    for (int i = 0; i < 50; ++i) {
        rig.tick(3 + i);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(rig.fetcher.call_count(), 1)
        << "a rejected apply must reach the gate's own cooldown, not an immediate retry";
}
