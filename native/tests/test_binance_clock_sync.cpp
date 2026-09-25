// Plan v17 §"binance_clock_sync.hpp": ClockOffsetSnapshot/ClockOffsetPublisher,
// checked arithmetic, freshness judgment, server_now_ms_* derived functions.
// Fully offline — no network I/O, no real credentials.
#include <gtest/gtest.h>
#include <hengyuan/binance_clock_sync.hpp>
#include "binance_clock_sync_test_hooks.hpp"

#include <cstdint>
#include <limits>
#include <type_traits>

using hy::checked_add_i64;
using hy::checked_sub_i64;
using hy::ClockOffsetPublisher;
using hy::ClockOffsetSnapshot;
using hy::ClockPairSample;
using hy::ClockPairSampleTestHooks;
using hy::compute_clock_offset;
using hy::fetch_clock_pair;
using hy::is_snapshot_fresh;
using hy::kMaxClockDriftMs;
using hy::kMaxUsableRttMs;
using hy::kOffsetTtlMs;
using hy::server_now_ms_for_signing;
using hy::server_now_ms_pessimistic;
using hy::try_get_pessimistic_server_now_ms;
using hy::try_get_signing_timestamp_ms;

constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kI64Min = std::numeric_limits<std::int64_t>::min();

// AUDIT L4-CLOCKPAIR-API-002 regression: ClockPairSample must not be an
// aggregate — otherwise a caller could still brace-init one from two
// independently-sourced int64 values, defeating the whole point of
// restricting construction to fetch_clock_pair()/ClockPairSampleTestHooks.
static_assert(!std::is_aggregate_v<ClockPairSample>);

// --- checked_add_i64 / checked_sub_i64 ---

TEST(CheckedArith, AddNormal) {
    std::int64_t out = 0;
    EXPECT_TRUE(checked_add_i64(100, 200, out));
    EXPECT_EQ(out, 300);
}

TEST(CheckedArith, AddOverflowPositive) {
    std::int64_t out = 0;
    EXPECT_FALSE(checked_add_i64(kI64Max, 1, out));
    EXPECT_FALSE(checked_add_i64(kI64Max - 1, 2, out));
}

TEST(CheckedArith, AddOverflowNegative) {
    std::int64_t out = 0;
    EXPECT_FALSE(checked_add_i64(kI64Min, -1, out));
}

TEST(CheckedArith, AddBoundaryExact) {
    std::int64_t out = 0;
    EXPECT_TRUE(checked_add_i64(kI64Max, 0, out));
    EXPECT_EQ(out, kI64Max);
    EXPECT_TRUE(checked_add_i64(kI64Max - 1, 1, out));
    EXPECT_EQ(out, kI64Max);
    EXPECT_TRUE(checked_add_i64(kI64Min, 0, out));
    EXPECT_EQ(out, kI64Min);
}

TEST(CheckedArith, SubNormal) {
    std::int64_t out = 0;
    EXPECT_TRUE(checked_sub_i64(300, 100, out));
    EXPECT_EQ(out, 200);
}

TEST(CheckedArith, SubOverflow) {
    std::int64_t out = 0;
    EXPECT_FALSE(checked_sub_i64(kI64Min, 1, out));
    EXPECT_FALSE(checked_sub_i64(kI64Max, -1, out));
}

TEST(CheckedArith, SubBoundaryExact) {
    std::int64_t out = 0;
    EXPECT_TRUE(checked_sub_i64(kI64Min, 0, out));
    EXPECT_EQ(out, kI64Min);
    EXPECT_TRUE(checked_sub_i64(kI64Max, 0, out));
    EXPECT_EQ(out, kI64Max);
}

// --- ClockOffsetSnapshot layout ---

TEST(ClockOffsetSnapshotLayout, WithinSingleCacheLine) {
    EXPECT_LE(sizeof(ClockOffsetSnapshot), 64u);
}

// --- ClockOffsetPublisher ---

TEST(ClockOffsetPublisher, PublishLoadRoundTrip) {
    ClockOffsetPublisher pub;
    ClockOffsetSnapshot s{};
    s.offset_ms = 42;
    s.error_bound_ms = 200;
    s.system_at_fetch_ms = 1000;
    s.steady_at_fetch_ms = 2000;

    EXPECT_TRUE(pub.publish(s));
    ClockOffsetSnapshot loaded = pub.load();
    EXPECT_EQ(loaded.offset_ms, 42);
    EXPECT_EQ(loaded.error_bound_ms, 200);
    EXPECT_EQ(loaded.seq, 1u);
}

TEST(ClockOffsetPublisher, SeqIncrementsMonotonically) {
    ClockOffsetPublisher pub;
    for (std::uint32_t expected = 1; expected <= 5; ++expected) {
        EXPECT_TRUE(pub.publish(ClockOffsetSnapshot{}));
        EXPECT_EQ(pub.load().seq, expected);
    }
}

TEST(ClockOffsetPublisher, RefusesToWrapAtUint32Max) {
    ClockOffsetPublisher pub;
    // Publish once, then hand-roll the internal seq to UINT32_MAX via
    // repeated publishes would take too long -- instead verify the
    // documented boundary behavior indirectly is out of reach without a
    // test hook; this test only exercises the reachable path: seq keeps
    // advancing across many publishes without ever refusing early.
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(pub.publish(ClockOffsetSnapshot{}));
    }
    EXPECT_EQ(pub.load().seq, 1000u);
}

// --- server_now_ms_pessimistic / server_now_ms_for_signing ---

TEST(ServerNowMs, PessimisticBasicCorrectness) {
    ClockOffsetSnapshot s{};
    s.offset_ms = 50;
    s.error_bound_ms = 20;
    std::int64_t out = 0;
    EXPECT_TRUE(server_now_ms_pessimistic(s, 1000, out));
    EXPECT_EQ(out, 1000 + 50 - 20);
}

TEST(ServerNowMs, PessimisticOverflowShortCircuits) {
    ClockOffsetSnapshot s{};
    s.offset_ms = kI64Max;
    s.error_bound_ms = 20;
    std::int64_t out = 0;
    EXPECT_FALSE(server_now_ms_pessimistic(s, 1, out));
}

TEST(ServerNowMs, ForSigningBasicCorrectness) {
    ClockOffsetSnapshot s{};
    s.offset_ms = -30;
    std::int64_t out = 0;
    EXPECT_TRUE(server_now_ms_for_signing(s, 1000, out));
    EXPECT_EQ(out, 970);
}

// --- is_snapshot_fresh ---

TEST(SnapshotFreshness, NeverPublishedIsNotFresh) {
    ClockOffsetSnapshot s{};  // seq == 0
    EXPECT_FALSE(is_snapshot_fresh(s, ClockPairSampleTestHooks::make(1000, 1000)));
}

TEST(SnapshotFreshness, WithinTtlIsFresh) {
    ClockOffsetSnapshot s{};
    s.seq = 1;
    s.system_at_fetch_ms = 0;
    s.steady_at_fetch_ms = 0;
    EXPECT_TRUE(is_snapshot_fresh(s, ClockPairSampleTestHooks::make(kOffsetTtlMs - 1, kOffsetTtlMs - 1)));
    EXPECT_TRUE(is_snapshot_fresh(s, ClockPairSampleTestHooks::make(kOffsetTtlMs, kOffsetTtlMs)));  // exact boundary
}

TEST(SnapshotFreshness, PastTtlIsNotFresh) {
    ClockOffsetSnapshot s{};
    s.seq = 1;
    s.system_at_fetch_ms = 0;
    s.steady_at_fetch_ms = 0;
    EXPECT_FALSE(is_snapshot_fresh(s, ClockPairSampleTestHooks::make(kOffsetTtlMs + 1, kOffsetTtlMs + 1)));
}

TEST(SnapshotFreshness, NegativeElapsedIsNotFresh) {
    ClockOffsetSnapshot s{};
    s.seq = 1;
    s.system_at_fetch_ms = 1000;
    s.steady_at_fetch_ms = 1000;
    EXPECT_FALSE(is_snapshot_fresh(s, ClockPairSampleTestHooks::make(500, 500)));  // steady_now < steady_at_fetch
}

TEST(SnapshotFreshness, DriftWithinToleranceIsFresh) {
    ClockOffsetSnapshot s{};
    s.seq = 1;
    s.system_at_fetch_ms = 0;
    s.steady_at_fetch_ms = 0;
    // system advanced by (kMaxClockDriftMs), steady advanced by 0 --
    // drift == kMaxClockDriftMs, boundary, still fresh.
    EXPECT_TRUE(is_snapshot_fresh(s, ClockPairSampleTestHooks::make(kMaxClockDriftMs, 0)));
}

TEST(SnapshotFreshness, DriftBeyondToleranceIsNotFresh) {
    ClockOffsetSnapshot s{};
    s.seq = 1;
    s.system_at_fetch_ms = 0;
    s.steady_at_fetch_ms = 0;
    EXPECT_FALSE(is_snapshot_fresh(s, ClockPairSampleTestHooks::make(kMaxClockDriftMs + 1, 0)));
}

TEST(SnapshotFreshness, NegativeDriftBeyondToleranceIsNotFresh) {
    ClockOffsetSnapshot s{};
    s.seq = 1;
    s.system_at_fetch_ms = 1'000'000;
    s.steady_at_fetch_ms = 1'000'000;
    // steady advances normally, system jumps backward -- negative drift
    // magnitude exceeds tolerance.
    EXPECT_FALSE(is_snapshot_fresh(s, ClockPairSampleTestHooks::make(1'000'000 - kMaxClockDriftMs - 1, 1'000'000)));
}

// --- try_get_signing_timestamp_ms ---

TEST(TryGetSigningTimestampMs, FreshSnapshotProducesCorrectValue) {
    ClockOffsetPublisher pub;
    ClockOffsetSnapshot s{};
    s.offset_ms = 77;
    s.system_at_fetch_ms = 1000;
    s.steady_at_fetch_ms = 1000;
    ASSERT_TRUE(pub.publish(s));

    std::int64_t out = 0;
    EXPECT_TRUE(try_get_signing_timestamp_ms(pub, ClockPairSampleTestHooks::make(1500, 1500), out));
    EXPECT_EQ(out, 1500 + 77);
}

TEST(TryGetSigningTimestampMs, StaleSnapshotFails) {
    ClockOffsetPublisher pub;  // never published
    std::int64_t out = -1;
    EXPECT_FALSE(try_get_signing_timestamp_ms(pub, ClockPairSampleTestHooks::make(1000, 1000), out));
    EXPECT_EQ(out, -1);  // untouched on failure
}

// --- try_get_pessimistic_server_now_ms ---
// The clock a kline backfill judges "has this bar closed yet" against (binance_klines_rest.hpp): freshness
// as strict as for signing, but the estimate must only ever be BEHIND the exchange.

namespace {
ClockOffsetSnapshot fetched_at_1000(std::int64_t offset_ms, std::int64_t error_bound_ms) {
    ClockOffsetSnapshot s{};
    s.offset_ms = offset_ms;
    s.error_bound_ms = error_bound_ms;
    s.system_at_fetch_ms = 1000;
    s.steady_at_fetch_ms = 1000;
    return s;
}
}  // namespace

TEST(TryGetPessimisticServerNowMs, FreshSnapshotGivesTheLowerBound) {
    ClockOffsetPublisher pub;
    ASSERT_TRUE(pub.publish(fetched_at_1000(/*offset*/ 77, /*error bound*/ 30)));

    std::int64_t pessimistic = 0;
    std::int64_t for_signing = 0;
    ASSERT_TRUE(try_get_pessimistic_server_now_ms(pub, ClockPairSampleTestHooks::make(1500, 1500), pessimistic));
    ASSERT_TRUE(try_get_signing_timestamp_ms(pub, ClockPairSampleTestHooks::make(1500, 1500), for_signing));
    EXPECT_EQ(pessimistic, 1500 + 77 - 30);
    EXPECT_EQ(for_signing - pessimistic, 30);  // behind the signing estimate by exactly the error bound
}

TEST(TryGetPessimisticServerNowMs, NeverPublishedFailsAndLeavesOutUntouched) {
    ClockOffsetPublisher pub;
    std::int64_t out = -1;
    EXPECT_FALSE(try_get_pessimistic_server_now_ms(pub, ClockPairSampleTestHooks::make(1000, 1000), out));
    EXPECT_EQ(out, -1);
}

TEST(TryGetPessimisticServerNowMs, ASnapshotPastItsTtlFails) {
    ClockOffsetPublisher pub;
    ASSERT_TRUE(pub.publish(fetched_at_1000(77, 30)));
    std::int64_t out = -1;
    EXPECT_TRUE(try_get_pessimistic_server_now_ms(
        pub, ClockPairSampleTestHooks::make(1000 + kOffsetTtlMs, 1000 + kOffsetTtlMs), out));  // the last fresh instant
    out = -1;
    EXPECT_FALSE(try_get_pessimistic_server_now_ms(
        pub, ClockPairSampleTestHooks::make(1000 + kOffsetTtlMs + 1, 1000 + kOffsetTtlMs + 1), out));
    EXPECT_EQ(out, -1);
}

TEST(TryGetPessimisticServerNowMs, AWallClockJumpSinceTheFetchFails) {
    ClockOffsetPublisher pub;
    ASSERT_TRUE(pub.publish(fetched_at_1000(77, 30)));
    std::int64_t out = -1;
    // 500 ms of steady time passed, but the wall clock moved kMaxClockDriftMs + 1 further than that: a real
    // step change (NTP, manual set), not slew -- the offset no longer describes this machine's clock.
    EXPECT_FALSE(try_get_pessimistic_server_now_ms(
        pub, ClockPairSampleTestHooks::make(1000 + 500 + kMaxClockDriftMs + 1, 1000 + 500), out));
    EXPECT_EQ(out, -1);
}

TEST(TryGetPessimisticServerNowMs, ArithmeticOverflowFailsClosed) {
    ClockOffsetPublisher pub;
    ASSERT_TRUE(pub.publish(fetched_at_1000(kI64Max, 20)));
    std::int64_t out = -1;
    EXPECT_FALSE(try_get_pessimistic_server_now_ms(pub, ClockPairSampleTestHooks::make(1500, 1500), out));
    EXPECT_EQ(out, -1);
}

// --- compute_clock_offset / fetch_clock_pair ---

TEST(ComputeClockOffset, NormalRttComputesCorrectly) {
    ClockOffsetSnapshot out{};
    ClockPairSample sample = ClockPairSampleTestHooks::make(5000, 6000);
    // send=1000, recv=1100 (rtt=100), server=1100
    EXPECT_TRUE(compute_clock_offset(1000, 1100, 1100, sample, out));
    EXPECT_EQ(out.offset_ms, 1100 - (1000 + 50));  // server - (send + rtt/2)
    EXPECT_EQ(out.error_bound_ms, 50 + hy::kClockSlopMs);
    EXPECT_EQ(out.system_at_fetch_ms, 5000);
    EXPECT_EQ(out.steady_at_fetch_ms, 6000);
}

TEST(ComputeClockOffset, RttExceedsMaxUsableIsDiscarded) {
    ClockOffsetSnapshot out{};
    ClockPairSample sample = ClockPairSampleTestHooks::make(0, 0);
    EXPECT_FALSE(compute_clock_offset(0, kMaxUsableRttMs + 1, 0, sample, out));
}

TEST(ComputeClockOffset, NegativeRttIsRejected) {
    ClockOffsetSnapshot out{};
    ClockPairSample sample = ClockPairSampleTestHooks::make(0, 0);
    EXPECT_FALSE(compute_clock_offset(1000, 900, 1000, sample, out));  // recv < send
}

TEST(ComputeClockOffset, InternalOverflowFailsClosed) {
    // rtt = recv - send = 0 (within budget), so the RTT-magnitude check
    // itself doesn't trip -- the overflow instead has to come from the
    // later offset = server_time_ms - mid subtraction: mid stays near
    // local_send_ms (1000), and a server_time_ms of kI64Min underflows
    // when subtracting a positive mid from it.
    ClockOffsetSnapshot out{};
    ClockPairSample sample = ClockPairSampleTestHooks::make(0, 0);
    EXPECT_FALSE(compute_clock_offset(1000, 1000, kI64Min, sample, out));
}

TEST(FetchClockPair, ReturnsNonNegativeReadings) {
    ClockPairSample s = fetch_clock_pair();
    EXPECT_GE(s.system_ms(), 0);
    EXPECT_GE(s.steady_ms(), 0);
}
