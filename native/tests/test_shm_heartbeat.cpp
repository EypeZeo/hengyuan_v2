// P2-INFRA-HB-01: SHM heartbeat control block + writer tests.
// Portable (no shm_open/mmap) — tests the data structures and logic.
//
// AUDIT PERF-SHM-004: this file previously had ZERO concurrency coverage, while
// the header it tests claimed the writer used relaxed atomics (it used none) and
// the block is by construction touched by two processes at once. The seqlock
// protocol that replaced the per-write CRC32 is verified here by a real two-thread
// test, registered with the `concurrency` label so the TSan CI job actually runs
// it — an in-process pair of threads is the only way to put a cross-PROCESS
// protocol under TSan at all.
#include <gtest/gtest.h>
#include <hengyuan/shm_heartbeat.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>

using hy::kShmMagic;
using hy::kShmVersion;
using hy::ShmControlBlock;
using hy::ShmHeartbeatSnapshot;
using hy::ShmHeartbeatWriter;
using hy::shm_arm_kill;
using hy::shm_read_snapshot;
using hy::shm_snapshot_valid;
using hy::shm_verify;

namespace {
ShmHeartbeatSnapshot read_or_fail(const ShmControlBlock& blk) {
    ShmHeartbeatSnapshot s{};
    EXPECT_TRUE(shm_read_snapshot(blk, s));
    return s;
}
}  // namespace

TEST(ShmHeartbeat, ControlBlockIs64Bytes) {
    EXPECT_EQ(sizeof(ShmControlBlock), 64u);
    EXPECT_EQ(alignof(ShmControlBlock), 64u);
}

TEST(ShmHeartbeat, AtomicsAreLockFree) {
    // A non-lock-free atomic in shared memory would be backed by a process-local
    // lock table, silently making the "atomic" not atomic across the process
    // boundary. The header static_asserts this; assert it at runtime too so the
    // reason is visible in test output rather than only as a build failure.
    alignas(64) ShmControlBlock blk{};
    EXPECT_TRUE(blk.seq.is_lock_free());
    EXPECT_TRUE(blk.loop_counter.is_lock_free());
    EXPECT_TRUE(blk.kill_armed.is_lock_free());
}

TEST(ShmHeartbeat, InitSetsFieldsCorrectly) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(12345);

    auto s = read_or_fail(blk);
    EXPECT_EQ(s.magic, kShmMagic);
    EXPECT_EQ(s.version, kShmVersion);
    EXPECT_EQ(s.main_pid, 12345u);
    EXPECT_EQ(s.loop_counter, 0u);
    EXPECT_EQ(s.last_incoming_ns, 0u);
    EXPECT_EQ(s.last_outgoing_ns, 0u);
    EXPECT_EQ(s.kill_armed, 0u);
    EXPECT_TRUE(shm_verify(blk));
}

TEST(ShmHeartbeat, InitLeavesSeqEven) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);
    EXPECT_EQ(blk.seq.load(std::memory_order_relaxed) % 2u, 0u)
        << "a completed write must leave the block on a stable (even) generation";
}

TEST(ShmHeartbeat, TickIncrementsCounter) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);

    EXPECT_EQ(read_or_fail(blk).loop_counter, 0u);
    writer.tick();
    EXPECT_EQ(read_or_fail(blk).loop_counter, 1u);
    writer.tick();
    writer.tick();
    EXPECT_EQ(read_or_fail(blk).loop_counter, 3u);
    EXPECT_TRUE(shm_verify(blk));
}

TEST(ShmHeartbeat, EveryWriteAdvancesTheGeneration) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);
    const std::uint32_t after_init = blk.seq.load(std::memory_order_relaxed);

    writer.tick();
    writer.set_incoming(1);
    writer.set_outgoing(2);
    // Three writes, two seq bumps each.
    EXPECT_EQ(blk.seq.load(std::memory_order_relaxed), after_init + 6u);
}

TEST(ShmHeartbeat, TimestampsUpdate) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);

    writer.set_incoming(1000000000);
    EXPECT_EQ(read_or_fail(blk).last_incoming_ns, 1000000000u);
    EXPECT_TRUE(shm_verify(blk));

    writer.set_outgoing(2000000000);
    EXPECT_EQ(read_or_fail(blk).last_outgoing_ns, 2000000000u);
    EXPECT_TRUE(shm_verify(blk));
}

TEST(ShmHeartbeat, OddGenerationIsNeverHandedToAReader) {
    // Simulate a writer stuck mid-update (or a stomp that left seq odd): a reader
    // must refuse to hand back a snapshot rather than return a half-written one.
    // This is what replaced the old "checksum mismatch" signal, and it is strictly
    // sharper -- the old CRC was itself written non-atomically after the field it
    // covered, so a reader could see a fresh counter with a stale checksum.
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);
    ASSERT_TRUE(shm_verify(blk));

    blk.seq.store(blk.seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);

    ShmHeartbeatSnapshot s{};
    EXPECT_FALSE(shm_read_snapshot(blk, s)) << "an odd generation must never yield a snapshot";
    EXPECT_FALSE(shm_verify(blk));
}

TEST(ShmHeartbeat, BadMagicFails) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);

    blk.magic.store(0xDEADBEEF, std::memory_order_relaxed);
    ShmHeartbeatSnapshot s{};
    ASSERT_TRUE(shm_read_snapshot(blk, s)) << "the generation is still stable, only the content is wrong";
    EXPECT_FALSE(shm_snapshot_valid(s));
    EXPECT_FALSE(shm_verify(blk));
}

TEST(ShmHeartbeat, BadVersionFails) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);

    blk.version.store(99, std::memory_order_relaxed);
    EXPECT_FALSE(shm_verify(blk));
}

TEST(ShmHeartbeat, KillRequestDetected) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);

    EXPECT_FALSE(writer.kill_requested());
    shm_arm_kill(blk);
    EXPECT_TRUE(writer.kill_requested());
}

TEST(ShmHeartbeat, KillRequestIsVisibleWhileTheWriterIsMidUpdate) {
    // kill_armed sits OUTSIDE the seqlock on purpose: a writer stuck on an odd
    // generation must not be able to mask a pending kill request. This is the
    // fail-closed direction and the one place a missed observation is a safety
    // failure rather than a stale gauge.
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);
    blk.seq.store(blk.seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);

    shm_arm_kill(blk);
    EXPECT_TRUE(writer.kill_requested())
        << "a stuck writer must not be able to hide a kill request";
}

TEST(ShmHeartbeat, NullWriterSafe) {
    ShmHeartbeatWriter writer(nullptr);
    writer.init(1);       // no crash
    writer.tick();         // no crash
    writer.set_incoming(0);
    writer.set_outgoing(0);
    EXPECT_FALSE(writer.kill_requested());
    EXPECT_EQ(writer.block(), nullptr);
}

TEST(ShmHeartbeat, StableAcrossManySequentialWrites) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(42);

    for (int i = 0; i < 10000; ++i) {
        writer.tick();
        writer.set_incoming(static_cast<std::uint64_t>(i) * 1000);
        ASSERT_TRUE(shm_verify(blk)) << "Failed at tick " << i;
    }
    EXPECT_EQ(read_or_fail(blk).loop_counter, 10000u);
}

// --- Two-thread seqlock protocol (label: concurrency, runs under TSan) ---

TEST(ShmHeartbeatConcurrency, ReaderNeverObservesATornGeneration) {
    // The writer keeps loop_counter and last_incoming_ns in lockstep
    // (incoming == counter * 2). Under the old non-atomic implementation a reader
    // could observe one updated and the other not; under the seqlock it must never
    // see the two disagree. Also: with plain fields this loop is a textbook data
    // race that TSan reports immediately, which is exactly why this test is
    // registered with the `concurrency` label.
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(7);

    constexpr std::uint64_t kWrites = 200000;
    std::atomic<bool> reader_ready{false};
    std::atomic<bool> writer_done{false};

    std::thread w([&] {
        // Same latch shape as test_reconcile_concurrency.cpp: the worker must
        // not start until the other side is actually executing. Release-build
        // CI (ubuntu-24.04, -O2) can finish all 200k seqlock sections before
        // the parent reaches the first load of writer_done -- the test then
        // fails the vacuous-guard (reads+failed_reads==0) in 0 ms. Observed
        // as CI Native run 31890070101 attempts 1-3; not a seqlock tear.
        while (!reader_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::uint64_t i = 1; i <= kWrites; ++i) {
            // One seqlock section covering BOTH fields, so the invariant below is
            // the property actually under test.
            writer.tick();
            writer.set_incoming(i * 2);
        }
        writer_done.store(true, std::memory_order_release);
    });

    std::uint64_t reads = 0;
    std::uint64_t failed_reads = 0;
    std::uint64_t torn = 0;
    std::uint64_t last_counter = 0;
    // Announce only after the first poll has executed, so the vacuous-guard
    // cannot fire even if the writer later races through the whole batch
    // during a preemption window. do-while keeps polling through writer_done.
    do {
        ShmHeartbeatSnapshot s{};
        if (!shm_read_snapshot(blk, s)) {
            ++failed_reads;
        } else {
            ++reads;
            // loop_counter is bumped before last_incoming_ns within the same iteration,
            // so a consistent generation shows either (n, (n-1)*2) [mid-iteration] or
            // (n, n*2) [end of iteration]. What must NEVER appear is incoming AHEAD of
            // the counter, which is what a torn read across generations produces.
            if (s.last_incoming_ns > s.loop_counter * 2) ++torn;
            // Monotonicity: a stable generation can never go backwards.
            if (s.loop_counter < last_counter) ++torn;
            last_counter = s.loop_counter;
        }
        if (!reader_ready.load(std::memory_order_relaxed)) {
            reader_ready.store(true, std::memory_order_release);
        }
    } while (!writer_done.load(std::memory_order_acquire));
    w.join();

    EXPECT_EQ(torn, 0u) << "seqlock must never hand back an inconsistent generation";
    EXPECT_EQ(read_or_fail(blk).loop_counter, kWrites);
    // Guard against the test going vacuous if the writer finishes before the
    // reader's first iteration.
    EXPECT_GT(reads + failed_reads, 0u);
    RecordProperty("reads", static_cast<int>(reads));
    RecordProperty("failed_reads", static_cast<int>(failed_reads));
}

TEST(ShmHeartbeatConcurrency, KillRequestFromAnotherThreadIsObserved) {
    // The watchdog->trading-process direction. Before the fix these were plain
    // loads/stores with no happens-before edge at all, so nothing guaranteed the
    // kill request was ever observed.
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(7);

    std::atomic<bool> armed{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 1000; ++i) std::this_thread::yield();
        shm_arm_kill(blk);
        armed.store(true, std::memory_order_release);
    });

    // Busy hot loop, exactly as run_once() does.
    std::uint64_t spins = 0;
    while (!writer.kill_requested()) {
        writer.tick();
        ++spins;
        ASSERT_LT(spins, 100'000'000u) << "kill request was never observed by the hot loop";
    }
    watchdog.join();
    EXPECT_TRUE(armed.load(std::memory_order_acquire));
    EXPECT_TRUE(writer.kill_requested());
}
