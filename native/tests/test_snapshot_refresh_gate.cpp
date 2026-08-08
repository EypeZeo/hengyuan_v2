// P2-MD-02 / Track C: SnapshotRefreshGate unit tests.
//
// Every test here injects a fake SnapshotFetcher -- no real network, no TLS, and
// deliberately NO Boost/OpenSSL dependency.
//
// AUDIT VERIF-TSAN-016: this file used to include blackhole_acceptor.hpp for one
// real-network test, which forced the whole binary behind HY_BUILD_DEMO. The TSan CI
// job builds HY_BUILD_DEMO=OFF with a hand-picked target list, so SnapshotRefreshGate --
// the one class in this codebase that spawns a std::thread and hands a mailbox across it
// -- had never run under ThreadSanitizer. That test now lives in
// test_binance_rest_snapshot.cpp (already Boost-gated, already owns the fixture), and this
// binary is registered with the `concurrency` label so TSan actually runs it.

#include <gtest/gtest.h>
#include <hengyuan/snapshot_refresh_gate.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using hy::DepthSnapshot;
using hy::SnapshotRefreshGate;
using hy::SnapshotRequest;

namespace {

// A fetcher whose completion is controlled by the test thread, so tests can deterministically
// observe the gate's non-blocking behavior while a fetch is still "in flight".
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

// A fetcher that always fails immediately (no blocking) -- used for cooldown tests.
std::optional<DepthSnapshot> always_fails(const SnapshotRequest&) { return std::nullopt; }

std::optional<DepthSnapshot> poll_until_ready(SnapshotRefreshGate& gate,
                                               const SnapshotRequest& req,
                                               int max_iterations = 2000) {
    for (int i = 0; i < max_iterations; ++i) {
        auto result = gate.poll(true, req);
        if (result.has_value()) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

// std::thread construction only guarantees the new thread has been created, not that it has
// actually started running -- checking fetcher.call_count() immediately after poll() spawns a
// worker is a genuine race (the worker may not have reached its "++call_count_" line yet). This
// helper spin-waits for the observable side effect instead of assuming synchronous-ish timing.
// A failure here (via ASSERT_TRUE at the call site) must abort the test *before* any subsequent
// code path that skips calling fetcher.unblock() -- otherwise the worker is left permanently
// blocked in its condition_variable wait, and SnapshotRefreshGate's destructor then hangs
// forever in worker_.join(), taking the whole test binary down with it.
::testing::AssertionResult WaitForCallCount(const GatedFetcher& fetcher, int expected,
                                             std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fetcher.call_count() >= expected) return ::testing::AssertionSuccess();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ::testing::AssertionFailure()
           << "call_count() never reached " << expected << " (stuck at " << fetcher.call_count()
           << ")";
}

}  // namespace

TEST(SnapshotRefreshGate, IdleWithoutResyncNeededDoesNothing) {
    GatedFetcher fetcher;
    SnapshotRefreshGate gate([&fetcher](const SnapshotRequest& r) { return fetcher(r); });
    auto result = gate.poll(false, {"BTCUSDT", 1, 1});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(fetcher.call_count(), 0);
}

TEST(SnapshotRefreshGate, SingleFlightNoConcurrentSecondFetch) {
    GatedFetcher fetcher;
    SnapshotRefreshGate gate([&fetcher](const SnapshotRequest& r) { return fetcher(r); });

    auto r1 = gate.poll(true, {"BTCUSDT", 1, 1});
    EXPECT_FALSE(r1.has_value());
    ASSERT_TRUE(WaitForCallCount(fetcher, 1, std::chrono::seconds(2)));
    EXPECT_EQ(fetcher.call_count(), 1);

    // Repeated polls while still in flight must not start a second worker.
    for (int i = 0; i < 5; ++i) {
        gate.poll(true, {"BTCUSDT", 1, 1});
    }
    EXPECT_EQ(fetcher.call_count(), 1);

    DepthSnapshot snap;
    snap.last_update_id = 1;
    fetcher.unblock(snap);
    auto got = poll_until_ready(gate, {"BTCUSDT", 1, 1});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(fetcher.call_count(), 1);
}

// --- v4 review P0-1 direct regression test ---
TEST(SnapshotRefreshGate, SecondSuccessfulCycleDoesNotBlockAndDoesNotReuseStaleSnapshot) {
    GatedFetcher fetcher;
    SnapshotRefreshGate gate([&fetcher](const SnapshotRequest& r) { return fetcher(r); });

    DepthSnapshot snap_a;
    snap_a.last_update_id = 1;
    DepthSnapshot snap_b;
    snap_b.last_update_id = 2;

    // Cycle 1: full success.
    auto r1 = gate.poll(true, {"BTCUSDT", 1, 1});
    EXPECT_FALSE(r1.has_value());
    fetcher.unblock(snap_a);
    auto got_a = poll_until_ready(gate, {"BTCUSDT", 1, 1});
    ASSERT_TRUE(got_a.has_value());
    EXPECT_EQ(got_a->last_update_id, 1u);
    gate.notify_apply_result(true);
    ASSERT_EQ(fetcher.call_count(), 1);

    // Cycle 2: start it, but deliberately do NOT unblock the fetcher yet.
    auto r2_first = gate.poll(true, {"BTCUSDT", 1, 1});
    EXPECT_FALSE(r2_first.has_value());
    ASSERT_TRUE(WaitForCallCount(fetcher, 2, std::chrono::seconds(2)));
    ASSERT_EQ(fetcher.call_count(), 2);

    // The bug this test guards against: a stale mailbox_ready_==true left over from cycle 1
    // would make this next poll() incorrectly believe worker #2 is already done and call
    // join() on it -- blocking here until the (still-gated) worker eventually returns. With
    // the fix, this must return promptly without blocking.
    auto start = std::chrono::steady_clock::now();
    auto r2_second = gate.poll(true, {"BTCUSDT", 1, 1});
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(r2_second.has_value());
    EXPECT_LT(elapsed, std::chrono::milliseconds(500));

    // Now let worker #2 finish with a snapshot distinct from cycle 1's.
    fetcher.unblock(snap_b);
    auto got_b = poll_until_ready(gate, {"BTCUSDT", 1, 1});
    ASSERT_TRUE(got_b.has_value());
    EXPECT_EQ(got_b->last_update_id, 2u);  // must not be the stale snap_a
    EXPECT_EQ(fetcher.call_count(), 2);
}

TEST(SnapshotRefreshGate, FetchFailureTriggersCooldown) {
    SnapshotRefreshGate gate(&always_fails);

    auto r1 = gate.poll(true, {"BTCUSDT", 1, 1});
    EXPECT_FALSE(r1.has_value());

    // always_fails() returns instantly, so a few short-interval polls give its worker thread
    // ample time to finish and be observed via mailbox_ready_. Every individual poll() call
    // itself must stay fast throughout -- it must never block on join() unless mailbox_ready_
    // has actually been observed true (that's the whole point of the non-blocking contract).
    for (int i = 0; i < 50; ++i) {
        auto start = std::chrono::steady_clock::now();
        gate.poll(true, {"BTCUSDT", 1, 1});
        auto elapsed = std::chrono::steady_clock::now() - start;
        EXPECT_LT(elapsed, std::chrono::milliseconds(200));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // By now the gate must have observed the failure and entered Cooldown (not still InFlight,
    // and not back in Idle immediately re-fetching): one more poll must still return nullopt.
    auto r2 = gate.poll(true, {"BTCUSDT", 1, 1});
    EXPECT_FALSE(r2.has_value());
}

TEST(SnapshotRefreshGate, ApplyRejectionTriggersCooldownEvenThoughFetchSucceeded) {
    // v4 review P0-3 regression: fetch succeeding is not the same as the caller successfully
    // applying it. notify_apply_result(false) must force the same cooldown as a fetch failure.
    GatedFetcher fetcher;
    SnapshotRefreshGate gate([&fetcher](const SnapshotRequest& r) { return fetcher(r); });

    gate.poll(true, {"BTCUSDT", 1, 1});
    DepthSnapshot snap;
    snap.last_update_id = 1;
    fetcher.unblock(snap);
    auto got = poll_until_ready(gate, {"BTCUSDT", 1, 1});
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(fetcher.call_count(), 1);

    // Simulate DepthManager::apply_snapshot() rejecting this snapshot (gap/overflow).
    gate.notify_apply_result(false);

    // Immediately re-polling must not start a new fetch -- must be in Cooldown.
    auto during_cooldown = gate.poll(true, {"BTCUSDT", 1, 1});
    EXPECT_FALSE(during_cooldown.has_value());
    EXPECT_EQ(fetcher.call_count(), 1);  // no new fetch started
}
