// Real-file tests for durable_log_store.hpp's DurableLogStore -- the shared
// platform I/O layer both DurableAuditSink and ControlPlaneLogSink build on
// (docs/SPEC_INVARIANTS.md's "Phase 1" entry). This file verifies the shared
// layer in isolation, before either consumer exercises it -- lock/open/
// append/fsync/tip-anchor round trips, the streaming read_chunk primitive,
// and lock contention.
#include <gtest/gtest.h>
#include <hengyuan/durable_log_store.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace hy;

namespace {

std::vector<std::byte> make_bytes(std::string_view s) {
    std::vector<std::byte> v(s.size());
    std::memcpy(v.data(), s.data(), s.size());
    return v;
}

}  // namespace

class DurableLogStoreTest : public ::testing::Test {
protected:
    std::string base_path_;

    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        base_path_ = std::string(tmp) + "hy_dls_" + std::to_string(GetCurrentProcessId()) + "_" +
                     std::to_string(counter) + ".log";
#else
        base_path_ = "/tmp/hy_dls_" + std::to_string(getpid()) + "_" + std::to_string(counter) + ".log";
#endif
        remove_all();
    }

    void TearDown() override { remove_all(); }

    void remove_all() {
        std::remove(base_path_.c_str());
        std::remove((base_path_ + ".lock").c_str());
        std::remove((base_path_ + ".tip").c_str());
        std::remove((base_path_ + ".tip.tmp").c_str());
    }

    DurableLogStore make_store() {
        return DurableLogStore(base_path_, base_path_ + ".lock", base_path_ + ".tip");
    }
};

TEST_F(DurableLogStoreTest, AcquireLockThenOpenLogSucceeds) {
    auto store = make_store();
    ASSERT_TRUE(store.acquire_lock());
    ASSERT_TRUE(store.open_log());
    EXPECT_TRUE(store.is_log_open());
}

TEST_F(DurableLogStoreTest, SecondInstanceOverSamePathFailsToAcquireLock) {
    auto store_a = make_store();
    ASSERT_TRUE(store_a.acquire_lock());
    ASSERT_TRUE(store_a.open_log());

    auto store_b = make_store();
    EXPECT_FALSE(store_b.acquire_lock());
}

TEST_F(DurableLogStoreTest, ReleaseLockAllowsSubsequentAcquire) {
    auto store_a = make_store();
    ASSERT_TRUE(store_a.acquire_lock());
    store_a.release_lock();

    auto store_b = make_store();
    EXPECT_TRUE(store_b.acquire_lock());
}

TEST_F(DurableLogStoreTest, AppendAndReadWholeLogRoundTrips) {
    auto store = make_store();
    ASSERT_TRUE(store.acquire_lock());
    ASSERT_TRUE(store.open_log());

    const auto payload_a = make_bytes("hello-frame-one");
    const auto payload_b = make_bytes("second-frame-here");
    ASSERT_TRUE(store.append_and_fsync(payload_a));
    ASSERT_TRUE(store.append_and_fsync(payload_b));

    std::vector<std::byte> content;
    ASSERT_TRUE(store.read_whole_log(content));
    ASSERT_EQ(content.size(), payload_a.size() + payload_b.size());
    EXPECT_EQ(0, std::memcmp(content.data(), payload_a.data(), payload_a.size()));
    EXPECT_EQ(0, std::memcmp(content.data() + payload_a.size(), payload_b.data(), payload_b.size()));
}

TEST_F(DurableLogStoreTest, EmptyLogReadsAsEmptyVector) {
    auto store = make_store();
    ASSERT_TRUE(store.acquire_lock());
    ASSERT_TRUE(store.open_log());

    std::vector<std::byte> content;
    ASSERT_TRUE(store.read_whole_log(content));
    EXPECT_TRUE(content.empty());
}

TEST_F(DurableLogStoreTest, LogSizeMatchesTotalAppendedBytes) {
    auto store = make_store();
    ASSERT_TRUE(store.acquire_lock());
    ASSERT_TRUE(store.open_log());

    const auto payload = make_bytes("0123456789");
    ASSERT_TRUE(store.append_and_fsync(payload));
    ASSERT_TRUE(store.append_and_fsync(payload));
    EXPECT_EQ(store.log_size(), payload.size() * 2);
}

TEST_F(DurableLogStoreTest, ReadChunkReturnsExactBytesAtOffset) {
    auto store = make_store();
    ASSERT_TRUE(store.acquire_lock());
    ASSERT_TRUE(store.open_log());

    const auto payload_a = make_bytes("AAAABBBBCCCC");  // 12 bytes
    const auto payload_b = make_bytes("DDDDEEEE");      // 8 bytes
    ASSERT_TRUE(store.append_and_fsync(payload_a));
    ASSERT_TRUE(store.append_and_fsync(payload_b));

    std::array<std::byte, 8> buf{};
    std::size_t out_read = 0;
    // Read the second frame's bytes by offset, without touching the first.
    ASSERT_TRUE(store.read_chunk(payload_a.size(), buf, payload_b.size(), out_read));
    ASSERT_EQ(out_read, payload_b.size());
    EXPECT_EQ(0, std::memcmp(buf.data(), payload_b.data(), payload_b.size()));
}

TEST_F(DurableLogStoreTest, ReadChunkPastEndOfFileReturnsShortRead) {
    auto store = make_store();
    ASSERT_TRUE(store.acquire_lock());
    ASSERT_TRUE(store.open_log());

    const auto payload = make_bytes("only-ten-b");  // 10 bytes
    ASSERT_TRUE(store.append_and_fsync(payload));

    std::array<std::byte, 32> buf{};
    std::size_t out_read = 0;
    // Ask for more than exists past a mid-file offset -- must not error, must
    // report fewer bytes than requested (this is how ControlPlaneLogSink's
    // recovery scan distinguishes "torn tail" from "I/O error").
    ASSERT_TRUE(store.read_chunk(5, buf, 32, out_read));
    EXPECT_EQ(out_read, 5u);  // only 5 bytes remain from offset 5 to EOF at 10
}

TEST_F(DurableLogStoreTest, ReadChunkAtExactEndOfFileReturnsZero) {
    auto store = make_store();
    ASSERT_TRUE(store.acquire_lock());
    ASSERT_TRUE(store.open_log());

    const auto payload = make_bytes("abc");
    ASSERT_TRUE(store.append_and_fsync(payload));

    std::array<std::byte, 8> buf{};
    std::size_t out_read = 1;  // poison, must be overwritten to 0
    ASSERT_TRUE(store.read_chunk(payload.size(), buf, 8, out_read));
    EXPECT_EQ(out_read, 0u);
}

TEST_F(DurableLogStoreTest, TipAnchorWriteThenReadRoundTrips) {
    auto store = make_store();
    ASSERT_TRUE(store.acquire_lock());
    ASSERT_TRUE(store.open_log());

    const auto anchor = make_bytes("fake-anchor-bytes-77-long-ish-payload-content");
    ASSERT_TRUE(store.write_tip_anchor(anchor));

    std::vector<std::byte> out;
    ASSERT_TRUE(store.read_tip_anchor(out));
    ASSERT_EQ(out.size(), anchor.size());
    EXPECT_EQ(0, std::memcmp(out.data(), anchor.data(), anchor.size()));
}

TEST_F(DurableLogStoreTest, TipAnchorSecondWriteAtomicallyReplacesFirst) {
    auto store = make_store();
    ASSERT_TRUE(store.acquire_lock());
    ASSERT_TRUE(store.open_log());

    ASSERT_TRUE(store.write_tip_anchor(make_bytes("first-anchor-version")));
    ASSERT_TRUE(store.write_tip_anchor(make_bytes("second-anchor-version-newer")));

    std::vector<std::byte> out;
    ASSERT_TRUE(store.read_tip_anchor(out));
    const auto expected = make_bytes("second-anchor-version-newer");
    ASSERT_EQ(out.size(), expected.size());
    EXPECT_EQ(0, std::memcmp(out.data(), expected.data(), expected.size()));
}

TEST_F(DurableLogStoreTest, ReadTipAnchorFailsCleanlyWhenAbsent) {
    auto store = make_store();
    ASSERT_TRUE(store.acquire_lock());
    ASSERT_TRUE(store.open_log());

    std::vector<std::byte> out;
    EXPECT_FALSE(store.read_tip_anchor(out));
}

TEST_F(DurableLogStoreTest, CloseThenReopenLogPreservesContent) {
    {
        auto store = make_store();
        ASSERT_TRUE(store.acquire_lock());
        ASSERT_TRUE(store.open_log());
        ASSERT_TRUE(store.append_and_fsync(make_bytes("persisted-across-close")));
        store.close_log();
        store.release_lock();
    }
    {
        auto store = make_store();
        ASSERT_TRUE(store.acquire_lock());
        ASSERT_TRUE(store.open_log());
        std::vector<std::byte> content;
        ASSERT_TRUE(store.read_whole_log(content));
        const auto expected = make_bytes("persisted-across-close");
        ASSERT_EQ(content.size(), expected.size());
        EXPECT_EQ(0, std::memcmp(content.data(), expected.data(), expected.size()));
    }
}

TEST_F(DurableLogStoreTest, WriteTipAnchorToNonexistentDirectoryFailsCleanly) {
    // A directory that does not exist -- write_tip_anchor must fail, not
    // crash or leave a half-written temp file that later confuses recovery.
    const std::string bogus_path = base_path_ + "_nonexistent_subdir/tip";
    DurableLogStore store(base_path_, base_path_ + ".lock", bogus_path);
    ASSERT_TRUE(store.acquire_lock());
    ASSERT_TRUE(store.open_log());

    EXPECT_FALSE(store.write_tip_anchor(make_bytes("wont-land")));
}
