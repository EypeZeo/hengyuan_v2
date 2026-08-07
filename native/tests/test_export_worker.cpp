// Tests for export_worker.hpp: the best-effort telemetry consumer for
// ExportOutboxRing. LastRemoteAckedTipStore tests touch real temp files on
// disk (same "process restart" simulation style as test_durable_audit_sink.cpp
// -- destroy one instance, construct a fresh one over the same path);
// ExportOutboxRing/FakeExternalAnchorClient stay purely in-memory.
#include <gtest/gtest.h>
#include <hengyuan/export_worker.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace hy;

namespace {

// Deliberately controllable ExternalAnchorClient -- direct virtual override
// (the interface is already shaped for injection, no extra port wrapper
// needed), same "trivial shape-proof stub" spirit as
// StubExternalAnchorClient (test_durable_control_plane_sink_interface_abi.cpp),
// plus the one bit of real behavior this file's tests actually need.
class FakeExternalAnchorClient final : public ExternalAnchorClient {
public:
    bool should_ack{true};
    int call_count{0};

    AuditAppendResult export_tip_and_wait_bounded(std::uint64_t, std::uint64_t, std::uint32_t,
                                                    std::uint64_t sequence, std::span<const std::uint8_t, 32>,
                                                    std::uint32_t) noexcept override {
        ++call_count;
        AuditAppendResult r{};
        r.status = should_ack ? AuditAppendResult::Status::Acked : AuditAppendResult::Status::Failed;
        r.sequence = sequence;
        return r;
    }
    AuditAppendResult export_and_wait_ack(const GenerationSeal&) noexcept override { return {}; }
    RecoveryScanStatus read_latest(GenerationSeal&) noexcept override { return RecoveryScanStatus::IoError; }
    RecoveryScanStatus read_latest_tip(std::uint64_t, std::uint64_t, std::uint32_t&, std::uint64_t&,
                                        std::array<std::uint8_t, 32>&, std::uint32_t&) noexcept override {
        return RecoveryScanStatus::IoError;
    }
    SealQueryStatus query_seal_by_request_id(std::uint64_t, std::uint64_t, std::uint64_t,
                                              GenerationSeal&) noexcept override {
        return SealQueryStatus::NotFound;
    }
};

ExportTuple make_tuple(std::uint64_t sequence, std::uint64_t uuid_lo = 111, std::uint64_t uuid_hi = 222,
                        std::uint32_t generation = 0, std::uint32_t key_id = 1) {
    ExportTuple t{};
    t.store_uuid_lo = uuid_lo;
    t.store_uuid_hi = uuid_hi;
    t.generation = generation;
    t.sequence = sequence;
    for (std::size_t i = 0; i < t.tip_mac.size(); ++i) {
        t.tip_mac[i] = static_cast<std::uint8_t>((sequence + i) & 0xFF);
    }
    t.key_id = key_id;
    t.enqueued_utc_ms = 1000;
    t.time_kind = static_cast<std::uint8_t>(FrameTimeKind::ServerCorrectedUtc);
    return t;
}

// Same default identity fields as make_tuple() above, so a baseline built
// from this and a tuple built from make_tuple() at the same sequence are
// the exact same logical position -- tests that want a genuine conflict
// perturb one field explicitly.
LastRemoteAckedTip make_baseline(std::uint64_t sequence, std::uint64_t uuid_lo = 111, std::uint64_t uuid_hi = 222,
                                  std::uint32_t generation = 0, std::uint32_t key_id = 1) {
    LastRemoteAckedTip b{};
    b.store_uuid_lo = uuid_lo;
    b.store_uuid_hi = uuid_hi;
    b.generation = generation;
    b.sequence = sequence;
    for (std::size_t i = 0; i < b.tip_mac.size(); ++i) {
        b.tip_mac[i] = static_cast<std::uint8_t>((sequence + i) & 0xFF);
    }
    b.key_id = key_id;
    return b;
}

class ExportWorkerTest : public ::testing::Test {
protected:
    std::string base_path_;
    std::array<std::byte, kKekSize> kek_{};

    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        base_path_ = std::string(tmp) + "hy_ew_" + std::to_string(GetCurrentProcessId()) + "_" +
                     std::to_string(counter);
#else
        base_path_ = "/tmp/hy_ew_" + std::to_string(getpid()) + "_" + std::to_string(counter);
#endif
        remove_all();
        for (std::size_t i = 0; i < kek_.size(); ++i) kek_[i] = static_cast<std::byte>(0x40 + i);
    }

    void TearDown() override { remove_all(); }

    void remove_all() {
        std::remove((base_path_ + ".unused_log").c_str());
        std::remove((base_path_ + ".lock").c_str());
        std::remove((base_path_ + ".tip").c_str());
        std::remove((base_path_ + ".tip.tmp").c_str());
    }
};

}  // namespace

// --- run_export_worker_once() ---

TEST_F(ExportWorkerTest, DrainsSingleTupleAndPersistsBaseline) {
    ExportOutboxRing ring;
    FakeExternalAnchorClient anchor;
    LastRemoteAckedTipStore store(base_path_, kek_);
    ASSERT_TRUE(store.open());

    const ExportTuple t = make_tuple(5);
    ASSERT_TRUE(ring.try_push(t));

    EXPECT_EQ(run_export_worker_once(ring, anchor, store), ExportRunStatus::Exported);
    EXPECT_EQ(anchor.call_count, 1);
    EXPECT_TRUE(ring.empty());

    LastRemoteAckedTip baseline{};
    ASSERT_EQ(store.read(baseline), LastRemoteAckedTipReadStatus::Valid);
    EXPECT_EQ(baseline.sequence, 5u);
    EXPECT_EQ(baseline.store_uuid_lo, t.store_uuid_lo);
    EXPECT_EQ(baseline.tip_mac, t.tip_mac);
}

TEST_F(ExportWorkerTest, NothingToDrainReturnsEmpty) {
    ExportOutboxRing ring;
    FakeExternalAnchorClient anchor;
    LastRemoteAckedTipStore store(base_path_, kek_);
    ASSERT_TRUE(store.open());

    EXPECT_EQ(run_export_worker_once(ring, anchor, store), ExportRunStatus::Empty);
    EXPECT_EQ(anchor.call_count, 0);
    LastRemoteAckedTip baseline{};
    EXPECT_EQ(store.read(baseline), LastRemoteAckedTipReadStatus::Absent);
}

TEST_F(ExportWorkerTest, FailedExportLeavesTupleAtHeadForRetry) {
    ExportOutboxRing ring;
    FakeExternalAnchorClient anchor;
    anchor.should_ack = false;
    LastRemoteAckedTipStore store(base_path_, kek_);
    ASSERT_TRUE(store.open());

    ASSERT_TRUE(ring.try_push(make_tuple(1)));

    EXPECT_EQ(run_export_worker_once(ring, anchor, store), ExportRunStatus::RemoteRejected);
    ExportTuple still{};
    ASSERT_TRUE(ring.peek_oldest(still));
    EXPECT_EQ(still.sequence, 1u);
    LastRemoteAckedTip baseline{};
    EXPECT_EQ(store.read(baseline), LastRemoteAckedTipReadStatus::Absent);
}

// Key regression test: baseline_store deliberately never open()'d, so
// write() fails closed -- the ring must NOT advance past an export whose
// baseline was never durably confirmed.
TEST_F(ExportWorkerTest, BaselineWriteFailureDoesNotPopRing) {
    ExportOutboxRing ring;
    FakeExternalAnchorClient anchor;
    LastRemoteAckedTipStore store(base_path_, kek_);  // NOT opened

    ASSERT_TRUE(ring.try_push(make_tuple(7)));

    EXPECT_EQ(run_export_worker_once(ring, anchor, store), ExportRunStatus::BaselineWriteFailed);
    EXPECT_EQ(anchor.call_count, 1);  // remote WAS contacted and acked
    ExportTuple still{};
    ASSERT_TRUE(ring.peek_oldest(still));
    EXPECT_EQ(still.sequence, 7u);
}

TEST_F(ExportWorkerTest, MultipleTuplesDrainedInOrder) {
    ExportOutboxRing ring;
    FakeExternalAnchorClient anchor;
    LastRemoteAckedTipStore store(base_path_, kek_);
    ASSERT_TRUE(store.open());

    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
        ASSERT_TRUE(ring.try_push(make_tuple(seq)));
    }
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(run_export_worker_once(ring, anchor, store), ExportRunStatus::Exported);
    }
    EXPECT_TRUE(ring.empty());
    LastRemoteAckedTip baseline{};
    ASSERT_EQ(store.read(baseline), LastRemoteAckedTipReadStatus::Valid);
    EXPECT_EQ(baseline.sequence, 3u);
}

TEST_F(ExportWorkerTest, RunOnceReturnsBaselineConflictWhenTupleContradictsExistingBaseline) {
    ExportOutboxRing ring;
    FakeExternalAnchorClient anchor;
    LastRemoteAckedTipStore store(base_path_, kek_);
    ASSERT_TRUE(store.open());
    ASSERT_EQ(store.write(make_baseline(10)), LastRemoteAckedTipWriteStatus::Ok);

    ASSERT_TRUE(ring.try_push(make_tuple(3)));  // behind the already-durable baseline
    EXPECT_EQ(run_export_worker_once(ring, anchor, store), ExportRunStatus::BaselineConflict);
    ExportTuple still{};
    ASSERT_TRUE(ring.peek_oldest(still));
    EXPECT_EQ(still.sequence, 3u);
}

// --- A permanent baseline conflict stops burning remote calls (audit EXPORT-HOL-029) ---
//
// Regressed/Conflicting are not transient: retrying cannot change a durable baseline
// that already contradicts the tuple. The head stayed put forever regardless, but
// every subsequent call first spent a REAL remote round-trip and only then failed,
// while the bounded ring behind it filled and began silently dropping new tuples.

TEST_F(ExportWorkerTest, PermanentBaselineConflictDoesNotKeepSpendingRemoteCalls) {
    ExportOutboxRing ring;
    FakeExternalAnchorClient anchor;
    LastRemoteAckedTipStore store(base_path_, kek_);
    ASSERT_TRUE(store.open());
    ASSERT_EQ(store.write(make_baseline(10)), LastRemoteAckedTipWriteStatus::Ok);
    ASSERT_FALSE(store.baseline_conflicted());

    ASSERT_TRUE(ring.try_push(make_tuple(3)));

    EXPECT_EQ(run_export_worker_once(ring, anchor, store), ExportRunStatus::BaselineConflict);
    EXPECT_TRUE(store.baseline_conflicted()) << "the conflict must latch";
    const int calls_after_first = anchor.call_count;
    EXPECT_GT(calls_after_first, 0) << "the first attempt legitimately tries the remote";

    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(run_export_worker_once(ring, anchor, store), ExportRunStatus::BaselineConflict);
    }
    EXPECT_EQ(anchor.call_count, calls_after_first)
        << "a permanent conflict must not spend a remote round-trip per tick";

    ExportTuple still{};
    ASSERT_TRUE(ring.peek_oldest(still)) << "the tuple must still be there for an operator to see";
    EXPECT_EQ(still.sequence, 3u);
}

TEST_F(ExportWorkerTest, OperatorCanClearABaselineConflict) {
    ExportOutboxRing ring;
    FakeExternalAnchorClient anchor;
    LastRemoteAckedTipStore store(base_path_, kek_);
    ASSERT_TRUE(store.open());
    ASSERT_EQ(store.write(make_baseline(10)), LastRemoteAckedTipWriteStatus::Ok);
    ASSERT_TRUE(ring.try_push(make_tuple(3)));
    ASSERT_EQ(run_export_worker_once(ring, anchor, store), ExportRunStatus::BaselineConflict);
    ASSERT_TRUE(store.baseline_conflicted());

    // Nothing clears this on its own -- that is the point.
    store.clear_baseline_conflict();
    EXPECT_FALSE(store.baseline_conflicted());
    // The underlying contradiction is still there, so it latches again rather than
    // silently succeeding.
    EXPECT_EQ(run_export_worker_once(ring, anchor, store), ExportRunStatus::BaselineConflict);
    EXPECT_TRUE(store.baseline_conflicted());
}

// --- LastRemoteAckedTipStore::read()/write() ---

TEST_F(ExportWorkerTest, BaselineSurvivesRestart) {
    {
        LastRemoteAckedTipStore store(base_path_, kek_);
        ASSERT_TRUE(store.open());
        ASSERT_EQ(store.write(make_baseline(3, 10, 20)), LastRemoteAckedTipWriteStatus::Ok);
    }

    LastRemoteAckedTipStore restarted(base_path_, kek_);
    ASSERT_TRUE(restarted.open());
    LastRemoteAckedTip read_back{};
    ASSERT_EQ(restarted.read(read_back), LastRemoteAckedTipReadStatus::Valid);
    EXPECT_EQ(read_back.sequence, 3u);
    EXPECT_EQ(read_back.store_uuid_lo, 10u);
    EXPECT_EQ(read_back.store_uuid_hi, 20u);
}

TEST_F(ExportWorkerTest, MissingBaselineFileReadsAsAbsent) {
    LastRemoteAckedTipStore store(base_path_, kek_);
    ASSERT_TRUE(store.open());
    LastRemoteAckedTip out{};
    EXPECT_EQ(store.read(out), LastRemoteAckedTipReadStatus::Absent);
}

TEST_F(ExportWorkerTest, TamperedBaselineFileFailsToRead) {
    {
        LastRemoteAckedTipStore store(base_path_, kek_);
        ASSERT_TRUE(store.open());
        ASSERT_EQ(store.write(make_baseline(1)), LastRemoteAckedTipWriteStatus::Ok);
    }
    {
        std::fstream f(base_path_ + ".tip", std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(f.is_open());
        char b = 0;
        f.read(&b, 1);
        b = static_cast<char>(b ^ 0x01);
        f.seekp(0);
        f.write(&b, 1);
    }

    LastRemoteAckedTipStore restarted(base_path_, kek_);
    ASSERT_TRUE(restarted.open());
    LastRemoteAckedTip out{};
    EXPECT_EQ(restarted.read(out), LastRemoteAckedTipReadStatus::Corrupt);
}

TEST_F(ExportWorkerTest, WrongKekFailsToRead) {
    {
        LastRemoteAckedTipStore store(base_path_, kek_);
        ASSERT_TRUE(store.open());
        ASSERT_EQ(store.write(make_baseline(1)), LastRemoteAckedTipWriteStatus::Ok);
    }

    std::array<std::byte, kKekSize> wrong_kek{};
    for (std::size_t i = 0; i < wrong_kek.size(); ++i) wrong_kek[i] = static_cast<std::byte>(0x99 + i);

    LastRemoteAckedTipStore restarted(base_path_, wrong_kek);
    ASSERT_TRUE(restarted.open());
    LastRemoteAckedTip out{};
    EXPECT_EQ(restarted.read(out), LastRemoteAckedTipReadStatus::Corrupt);
}

TEST_F(ExportWorkerTest, WriteRejectsRegressedSequence) {
    LastRemoteAckedTipStore store(base_path_, kek_);
    ASSERT_TRUE(store.open());
    ASSERT_EQ(store.write(make_baseline(10)), LastRemoteAckedTipWriteStatus::Ok);

    EXPECT_EQ(store.write(make_baseline(5)), LastRemoteAckedTipWriteStatus::Regressed);

    LastRemoteAckedTip out{};
    ASSERT_EQ(store.read(out), LastRemoteAckedTipReadStatus::Valid);
    EXPECT_EQ(out.sequence, 10u);  // unchanged
}

TEST_F(ExportWorkerTest, WriteRejectsConflictingSamePositionDifferentMac) {
    LastRemoteAckedTipStore store(base_path_, kek_);
    ASSERT_TRUE(store.open());
    ASSERT_EQ(store.write(make_baseline(10)), LastRemoteAckedTipWriteStatus::Ok);

    LastRemoteAckedTip conflicting = make_baseline(10);
    conflicting.tip_mac[0] ^= 0xFF;
    EXPECT_EQ(store.write(conflicting), LastRemoteAckedTipWriteStatus::Conflicting);
}

TEST_F(ExportWorkerTest, WriteRejectsMismatchedStoreUuid) {
    LastRemoteAckedTipStore store(base_path_, kek_);
    ASSERT_TRUE(store.open());
    ASSERT_EQ(store.write(make_baseline(10)), LastRemoteAckedTipWriteStatus::Ok);

    LastRemoteAckedTip other = make_baseline(11);
    other.store_uuid_lo += 1;
    EXPECT_EQ(store.write(other), LastRemoteAckedTipWriteStatus::Conflicting);
}

TEST_F(ExportWorkerTest, WriteAcceptsIdempotentReplayOfIdenticalTip) {
    LastRemoteAckedTipStore store(base_path_, kek_);
    ASSERT_TRUE(store.open());

    const LastRemoteAckedTip b = make_baseline(10);
    ASSERT_EQ(store.write(b), LastRemoteAckedTipWriteStatus::Ok);
    EXPECT_EQ(store.write(b), LastRemoteAckedTipWriteStatus::Ok);
}

TEST_F(ExportWorkerTest, WriteWithoutOpenFailsClosed) {
    LastRemoteAckedTipStore store(base_path_, kek_);
    EXPECT_EQ(store.write(make_baseline(1)), LastRemoteAckedTipWriteStatus::IoError);
}

TEST_F(ExportWorkerTest, SecondStoreOnSameLockedPathFailsClosed) {
    LastRemoteAckedTipStore first(base_path_, kek_);
    ASSERT_TRUE(first.open());

    LastRemoteAckedTipStore second(base_path_, kek_);
    EXPECT_FALSE(second.open());  // lock already held by `first`
    EXPECT_EQ(second.write(make_baseline(1)), LastRemoteAckedTipWriteStatus::IoError);

    // `first` still works normally -- it genuinely holds the lock.
    EXPECT_EQ(first.write(make_baseline(1)), LastRemoteAckedTipWriteStatus::Ok);
}

// --- export_worker_backoff_delay_ms() ---

TEST(ExportWorkerBackoffTest, GrowsExponentiallyAndSaturates) {
    ExportWorkerPolicy policy{};  // base=500, x4, cap=10000
    EXPECT_EQ(export_worker_backoff_delay_ms(policy, 0), 500);
    EXPECT_EQ(export_worker_backoff_delay_ms(policy, 1), 2000);
    EXPECT_EQ(export_worker_backoff_delay_ms(policy, 2), 8000);
    EXPECT_EQ(export_worker_backoff_delay_ms(policy, 3), 10000);  // saturated
    EXPECT_EQ(export_worker_backoff_delay_ms(policy, 100), 10000);
}

TEST(ExportWorkerBackoffTest, DegenerateConfigNeverReturnsZero) {
    ExportWorkerPolicy policy{};
    policy.base_retry_interval_ms = 0;
    EXPECT_GT(export_worker_backoff_delay_ms(policy, 0), 0);
}
