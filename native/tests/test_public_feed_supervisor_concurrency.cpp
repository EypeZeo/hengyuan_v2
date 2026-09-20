// 批次 6 6b-0f-2: real two-thread test of the one arrangement public_feed_supervisor.hpp relies on
// and cannot prove by itself -- consecutive session generations pushing into ONE shared SpscRing.
//
// SpscRing keeps a plain producer-private cached_tail_, so it is only legal for two sessions to share
// it if every push comes from the same thread. The repo's topology guarantees that (a single
// dedicated I/O thread runs ioc.run(), so every session's handlers -- old and new -- execute on it);
// FakeIoThread below is that thread. The main thread plays the hot/submit thread: it polls the
// supervisor and is the ring's only consumer. Run under TSan (label `concurrency`, same as the other
// two-thread tests) any data race between the I/O thread, the supervisor's reads of the sessions'
// atomics, and the consumer fails the job; on the arm64 job it runs 10x on real weak-memory hardware.
//
// The consumer checks what an in-order, lossless hand-over looks like from the outside:
//   * generations never go backwards (an old generation's event never appears after a newer one's);
//   * inside a generation, sequence numbers are contiguous from 0 (no loss, no duplicate).

#include <gtest/gtest.h>
#include <hengyuan/public_feed_supervisor.hpp>
#include <hengyuan/spsc_ring.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct Event {
    std::uint64_t generation;
    std::uint64_t seq;
};

using Ring = hy::SpscRing<Event, 64>;

// The stand-in for the process's one I/O thread: runs posted tasks in order on a single thread.
class FakeIoThread {
public:
    FakeIoThread() : thread_([this] { run(); }) {}

    ~FakeIoThread() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }

    FakeIoThread(const FakeIoThread&) = delete;
    FakeIoThread& operator=(const FakeIoThread&) = delete;

    void post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
                if (quit_) return;  // pending steps are dropped; the sessions are fakes
                fn = std::move(queue_.front());
                queue_.pop_front();
            }
            fn();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool quit_{false};
    std::thread thread_;  // last: starts running only once everything above is constructed
};

// A stop request takes effect only after this many more pushes: a real session's already-issued
// reads still complete (and deliver bars) after stop() posts its cancellation. Without this lag the
// fake would stop on its very next step, and a supervisor that started the successor too early
// would go undetected -- the two generations' events could not interleave.
constexpr std::uint64_t kStopLagPushes = 20;

// Mirrors the real sessions: enable_shared_from_this, work posted to the I/O thread, relaxed
// stopped()/is_connected() atomics, and a dropped session that stays alive through its own posted
// steps until it finishes.
class RingSession : public std::enable_shared_from_this<RingSession> {
public:
    // pushes_before_dying < 0: run until stop() is requested.
    RingSession(FakeIoThread& io, Ring& ring, std::uint64_t generation, std::int64_t pushes_before_dying)
        : io_(io), ring_(ring), generation_(generation), limit_(pushes_before_dying) {}

    void start() {
        io_.post([self = shared_from_this()] { self->connect_step(); });
    }
    void stop() { stop_requested_.store(true, std::memory_order_release); }
    bool stopped() const { return stopped_.load(std::memory_order_relaxed); }
    bool is_connected() const { return connected_.load(std::memory_order_relaxed); }

private:
    void connect_step() {
        connected_.store(true, std::memory_order_relaxed);
        push_step();
    }

    void push_step() {
        const bool limit_reached = limit_ >= 0 && static_cast<std::int64_t>(seq_) >= limit_;
        const bool stopping = stop_requested_.load(std::memory_order_acquire);
        if (limit_reached || (stopping && lag_pushes_done_ >= kStopLagPushes)) {
            stopped_.store(true, std::memory_order_relaxed);  // after the last push, like fail()
            return;
        }
        if (ring_.try_push(Event{generation_, seq_})) {
            ++seq_;
            if (stopping) ++lag_pushes_done_;
        } else {
            std::this_thread::yield();  // full: the consumer is behind; retry without advancing seq
        }
        io_.post([self = shared_from_this()] { self->push_step(); });
    }

    FakeIoThread& io_;
    Ring& ring_;
    const std::uint64_t generation_;
    const std::int64_t limit_;
    std::uint64_t seq_{0};  // touched only on the I/O thread
    std::uint64_t lag_pushes_done_{0};  // likewise: pushes made since stop() was observed
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> stopped_{false};
    std::atomic<bool> connected_{false};
};

// The consumer side of the arrangement: in-order, lossless, generation-aware.
struct Checker {
    void consume(const Event& e) {
        if (e.generation < last_generation) ++order_violations;  // an old generation after a newer one
        if (e.generation != last_generation) {
            last_generation = e.generation;
            next_seq = 0;
        }
        if (e.seq != next_seq) ++sequence_violations;  // a hole or a duplicate inside the generation
        next_seq = e.seq + 1;
        if (per_generation.size() <= e.generation) per_generation.resize(e.generation + 1, 0);
        ++per_generation[e.generation];
        ++total;
    }

    void drain(Ring& ring) {
        Event e{};
        while (ring.try_pop(e)) consume(e);
    }

    std::uint64_t last_generation{0};
    std::uint64_t next_seq{0};
    std::uint64_t order_violations{0};
    std::uint64_t sequence_violations{0};
    std::uint64_t total{0};
    std::vector<std::uint64_t> per_generation;
};

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

hy::FeedSupervisorPolicy fast_policy() {
    hy::FeedSupervisorPolicy p;
    p.initial_backoff_ms = 1;
    p.max_backoff_ms = 2;
    p.jitter_percent = 0;
    p.connect_deadline_ms = 5000;
    p.drain_timeout_ms = 500;
    p.stable_after_ms = 1'000'000;  // never "stable": failures accumulate, exercising the backoff path
    p.max_consecutive_failures = 0;
    return p;
}

}  // namespace

// Sessions that die by themselves (the failure path: backoff -> drain -> next generation).
TEST(PublicFeedSupervisorConcurrency, SelfTerminatingGenerationsHandOverASharedRingInOrderAndLosslessly) {
    constexpr std::int64_t kPushesPerGeneration = 300;
    constexpr std::uint64_t kGenerationsToRun = 8;

    // Declaration order is destruction order in reverse: the I/O thread must be joined (io) BEFORE
    // the ring it pushes into is destroyed, and the supervisor (whose factory captures both) goes first.
    Ring ring;
    Checker checker;
    std::shared_ptr<RingSession> last_session;  // main thread only: the factory runs inside poll()
    FakeIoThread io;

    hy::PublicFeedSupervisor<RingSession> sup(
        [&](std::uint64_t generation) {
            last_session = std::make_shared<RingSession>(io, ring, generation, kPushesPerGeneration);
            return last_session;
        },
        fast_policy(), [&] {
            checker.drain(ring);  // the drain ack: consume what the old generation left behind
            return true;
        });

    const std::int64_t deadline = now_ms() + 60'000;
    while (sup.generation() < kGenerationsToRun + 1 && now_ms() < deadline) {
        sup.poll(now_ms());
        checker.drain(ring);
        std::this_thread::yield();
    }
    ASSERT_GE(sup.generation(), kGenerationsToRun + 1) << "the supervisor stopped handing generations over";

    sup.shutdown();
    ASSERT_NE(last_session, nullptr);
    // The live generation keeps pushing until its I/O-thread step notices stop(); once stopped() it
    // pushes nothing more. Keep consuming meanwhile so a full ring cannot hold it up.
    const std::int64_t settle = now_ms() + 10'000;
    while (!last_session->stopped() && now_ms() < settle) {
        checker.drain(ring);
        std::this_thread::yield();
    }
    ASSERT_TRUE(last_session->stopped()) << "the last session never noticed stop()";
    checker.drain(ring);

    EXPECT_EQ(checker.order_violations, 0U);
    EXPECT_EQ(checker.sequence_violations, 0U);
    // Every generation that ran to completion delivered exactly its 300 events -- none lost across
    // the hand-over. (The generation that was live at shutdown may have delivered fewer.)
    for (std::uint64_t g = 1; g <= kGenerationsToRun; ++g) {
        ASSERT_LT(g, checker.per_generation.size());
        EXPECT_EQ(checker.per_generation[g], static_cast<std::uint64_t>(kPushesPerGeneration)) << "generation " << g;
    }
    EXPECT_GE(sup.stats().total_failures, kGenerationsToRun);
}

// Sessions that never die by themselves; the supervisor replaces them (the planned-rollover path,
// where stop() is requested from the hot thread while the I/O thread is mid-stream).
TEST(PublicFeedSupervisorConcurrency, PlannedRolloversStopASessionMidStreamWithoutLossOrReordering) {
    constexpr std::uint64_t kRolloversToRun = 6;

    Ring ring;  // destruction order: see the first test
    Checker checker;
    std::shared_ptr<RingSession> last_session;
    FakeIoThread io;

    hy::FeedSupervisorPolicy policy = fast_policy();
    policy.max_connection_age_ms = 5;

    hy::PublicFeedSupervisor<RingSession> sup(
        [&](std::uint64_t generation) {
            last_session = std::make_shared<RingSession>(io, ring, generation, /*run until stopped*/ -1);
            return last_session;
        },
        policy, [&] {
            checker.drain(ring);
            return true;
        });

    const std::int64_t deadline = now_ms() + 60'000;
    while (sup.stats().rollovers < kRolloversToRun && now_ms() < deadline) {
        sup.poll(now_ms());
        checker.drain(ring);
        std::this_thread::yield();
    }
    ASSERT_GE(sup.stats().rollovers, kRolloversToRun) << "no rollover happened";

    sup.shutdown();
    ASSERT_NE(last_session, nullptr);
    const std::int64_t settle = now_ms() + 10'000;
    while (!last_session->stopped() && now_ms() < settle) {
        checker.drain(ring);
        std::this_thread::yield();
    }
    ASSERT_TRUE(last_session->stopped()) << "the last session never noticed stop()";
    checker.drain(ring);

    EXPECT_EQ(checker.order_violations, 0U);
    EXPECT_EQ(checker.sequence_violations, 0U);
    EXPECT_GT(checker.total, 0U);
    EXPECT_EQ(sup.stats().total_failures, 0U) << "a planned rollover was counted as a failure";
    EXPECT_EQ(sup.stats().consecutive_failures, 0U);
}
