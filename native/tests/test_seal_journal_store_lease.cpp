// Real-filesystem tests for seal_journal_store_lease.hpp's
// SealJournalStoreLease -- directory rename/recreate identity fencing,
// non-owner release(), lease mutual exclusion, and the two Round E
// breadcrumb L2 loader read methods (docs/SPEC_INVARIANTS.md's "Seal-journal
// Round E breadcrumb L2 loaders" entry). Mirrors test_compaction_lease.cpp's
// structure closely -- SealJournalStoreLease follows CandidateLease's shape
// verbatim. Governance: L2 (real file I/O).
#include <gtest/gtest.h>
#include <hengyuan/seal_journal_store_lease.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace hy;

namespace {

std::filesystem::path make_temp_store_dir(std::string_view tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("hy_seal_journal_store_" + std::string(tag) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

void write_raw_file(const std::filesystem::path& dir, std::string_view filename, std::span<const std::byte> bytes) {
    std::ofstream out(dir / std::string(filename), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

template <std::size_t N>
std::array<std::byte, N> pattern_bytes(std::uint8_t seed) {
    std::array<std::byte, N> buf{};
    for (std::size_t i = 0; i < N; ++i) buf[i] = static_cast<std::byte>(seed + i);
    return buf;
}

}  // namespace

namespace hy::test_only {

// Exposes SealJournalStoreLease's two friend-only read methods as public
// static wrappers, purely for this test file -- see the friend declaration
// and its comment inside seal_journal_store_lease.hpp. NOT production code.
class SealJournalStoreLeaseTestAccess {
public:
    static SealJournalLeaseReadResult read_seal_journal_commit_watermark(
        SealJournalStoreLease& lease, std::uint64_t candidate_id,
        std::span<std::byte, kSealJournalCommitWatermarkWireBytes> out) noexcept {
        return lease.read_seal_journal_commit_watermark(candidate_id, out);
    }
    static SealJournalLeaseReadResult read_seal_journal_tombstone(
        SealJournalStoreLease& lease, std::uint64_t candidate_id, std::uint64_t journal_seq,
        std::span<std::byte, kSealJournalTombstoneBytes> out) noexcept {
        return lease.read_seal_journal_tombstone(candidate_id, journal_seq, out);
    }
};

}  // namespace hy::test_only

using hy::test_only::SealJournalStoreLeaseTestAccess;

// ===========================================================================
// Lifecycle: acquire/release/held/fenced/move -- same coverage shape as
// test_compaction_lease.cpp's CandidateLease lifecycle tests.
// ===========================================================================

TEST(SealJournalStoreLease, AcquireSucceedsOnFreshDirectory) {
    auto dir = make_temp_store_dir("acquire_ok");
    SealJournalStoreLease lease(dir);
    EXPECT_EQ(lease.acquire(), SealJournalLeaseAcquireStatus::Acquired);
    EXPECT_TRUE(lease.held());
    EXPECT_FALSE(lease.fenced());
    ASSERT_EQ(lease.release(), SealJournalLeaseReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealJournalStoreLease, AcquireFailsOnMissingDirectory) {
    auto dir = make_temp_store_dir("missing") / "does_not_exist";
    SealJournalStoreLease lease(dir);
    EXPECT_EQ(lease.acquire(), SealJournalLeaseAcquireStatus::DirectoryMissing);
}

TEST(SealJournalStoreLease, SecondInstanceCannotAcquireSameDirectory) {
    auto dir = make_temp_store_dir("mutex");
    SealJournalStoreLease lease1(dir);
    ASSERT_EQ(lease1.acquire(), SealJournalLeaseAcquireStatus::Acquired);

    SealJournalStoreLease lease2(dir);
    EXPECT_EQ(lease2.acquire(), SealJournalLeaseAcquireStatus::HeldElsewhere);

    ASSERT_EQ(lease1.release(), SealJournalLeaseReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealJournalStoreLease, ReacquireAfterReleaseSucceeds) {
    auto dir = make_temp_store_dir("reacquire");
    SealJournalStoreLease lease1(dir);
    ASSERT_EQ(lease1.acquire(), SealJournalLeaseAcquireStatus::Acquired);
    ASSERT_EQ(lease1.release(), SealJournalLeaseReleaseStatus::Released);

    SealJournalStoreLease lease2(dir);
    EXPECT_EQ(lease2.acquire(), SealJournalLeaseAcquireStatus::Acquired);
    ASSERT_EQ(lease2.release(), SealJournalLeaseReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealJournalStoreLease, NonOwnerReleaseReturnsWrongOwnerAndDoesNotClose) {
    auto dir = make_temp_store_dir("nonowner_release");
    SealJournalStoreLease lease(dir);
    ASSERT_EQ(lease.acquire(), SealJournalLeaseAcquireStatus::Acquired);

    SealJournalLeaseReleaseStatus non_owner_result{};
    std::thread other([&] { non_owner_result = lease.release(); });
    other.join();

    EXPECT_EQ(non_owner_result, SealJournalLeaseReleaseStatus::WrongOwner);
    EXPECT_TRUE(lease.held()) << "non-owner release() must not have actually released the lease";

    ASSERT_EQ(lease.release(), SealJournalLeaseReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealJournalStoreLease, ReleaseWithoutAcquireIsNotHeld) {
    auto dir = make_temp_store_dir("not_held");
    SealJournalStoreLease lease(dir);
    EXPECT_EQ(lease.release(), SealJournalLeaseReleaseStatus::NotHeld);
}

TEST(SealJournalStoreLease, MoveTransfersHeldStateAndOwnerThread) {
    auto dir = make_temp_store_dir("move");
    SealJournalStoreLease lease1(dir);
    ASSERT_EQ(lease1.acquire(), SealJournalLeaseAcquireStatus::Acquired);

    SealJournalStoreLease lease2(std::move(lease1));
    EXPECT_TRUE(lease2.held());
    EXPECT_FALSE(lease1.held());  // moved-from

    ASSERT_EQ(lease2.release(), SealJournalLeaseReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

// ===========================================================================
// Round E breadcrumb L2 loaders: the two friend-only read methods, driven
// through SealJournalStoreLeaseTestAccess. Confirms per-store (not
// per-candidate) addressing: multiple candidate_id/journal_seq combinations
// coexist in the same directory, selected purely by filename.
// ===========================================================================

TEST(SealJournalStoreLeaseLoaders, RoundTripsCommitWatermarkForOneCandidate) {
    auto dir = make_temp_store_dir("commit_watermark_rt");
    const std::uint64_t candidate_id = 0x1122334455667788ull;
    const auto content = pattern_bytes<kSealJournalCommitWatermarkWireBytes>(0x10);
    write_raw_file(dir, "1122334455667788.jhw", content);

    SealJournalStoreLease lease(dir);
    ASSERT_EQ(lease.acquire(), SealJournalLeaseAcquireStatus::Acquired);

    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> out{};
    const auto result =
        SealJournalStoreLeaseTestAccess::read_seal_journal_commit_watermark(lease, candidate_id, out);
    EXPECT_EQ(result.outcome, SealJournalLeaseIoOutcome::Ok);
    EXPECT_EQ(result.read.status, seal_journal_store_detail::ReadFixedStatus::Ok);
    EXPECT_EQ(0, std::memcmp(out.data(), content.data(), content.size()));

    ASSERT_EQ(lease.release(), SealJournalLeaseReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealJournalStoreLeaseLoaders, RoundTripsTombstoneForOneCandidateAndSeq) {
    auto dir = make_temp_store_dir("tombstone_rt");
    const std::uint64_t candidate_id = 0x00000000000000AAull;
    const std::uint64_t journal_seq = 0x00000000000000BBull;
    const auto content = pattern_bytes<kSealJournalTombstoneBytes>(0x20);
    write_raw_file(dir, "00000000000000aa-00000000000000bb.jts", content);

    SealJournalStoreLease lease(dir);
    ASSERT_EQ(lease.acquire(), SealJournalLeaseAcquireStatus::Acquired);

    std::array<std::byte, kSealJournalTombstoneBytes> out{};
    const auto result =
        SealJournalStoreLeaseTestAccess::read_seal_journal_tombstone(lease, candidate_id, journal_seq, out);
    EXPECT_EQ(result.outcome, SealJournalLeaseIoOutcome::Ok);
    EXPECT_EQ(result.read.status, seal_journal_store_detail::ReadFixedStatus::Ok);
    EXPECT_EQ(0, std::memcmp(out.data(), content.data(), content.size()));

    ASSERT_EQ(lease.release(), SealJournalLeaseReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

// Per-STORE, not per-candidate: many candidates' commit watermarks and many
// (candidate, seq) tombstones coexist in ONE directory, addressed purely by
// filename -- this is the exact property the ledger entry and the outbound
// Module 2 prompt both call out as easy to misread.
TEST(SealJournalStoreLeaseLoaders, MultipleCandidatesAndSeqsCoexistInOneDirectory) {
    auto dir = make_temp_store_dir("multi_candidate");
    const auto content_a = pattern_bytes<kSealJournalCommitWatermarkWireBytes>(0x30);
    const auto content_b = pattern_bytes<kSealJournalCommitWatermarkWireBytes>(0x50);
    write_raw_file(dir, "0000000000000001.jhw", content_a);
    write_raw_file(dir, "0000000000000002.jhw", content_b);
    const auto tombstone_1_1 = pattern_bytes<kSealJournalTombstoneBytes>(0x70);
    const auto tombstone_1_2 = pattern_bytes<kSealJournalTombstoneBytes>(0x90);
    write_raw_file(dir, "0000000000000001-0000000000000001.jts", tombstone_1_1);
    write_raw_file(dir, "0000000000000001-0000000000000002.jts", tombstone_1_2);

    SealJournalStoreLease lease(dir);
    ASSERT_EQ(lease.acquire(), SealJournalLeaseAcquireStatus::Acquired);

    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> out_a{};
    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> out_b{};
    ASSERT_EQ(SealJournalStoreLeaseTestAccess::read_seal_journal_commit_watermark(lease, 1, out_a).outcome,
              SealJournalLeaseIoOutcome::Ok);
    ASSERT_EQ(SealJournalStoreLeaseTestAccess::read_seal_journal_commit_watermark(lease, 2, out_b).outcome,
              SealJournalLeaseIoOutcome::Ok);
    EXPECT_EQ(0, std::memcmp(out_a.data(), content_a.data(), content_a.size()));
    EXPECT_EQ(0, std::memcmp(out_b.data(), content_b.data(), content_b.size()));
    EXPECT_NE(0, std::memcmp(out_a.data(), out_b.data(), out_a.size()));

    std::array<std::byte, kSealJournalTombstoneBytes> ts_out_1{};
    std::array<std::byte, kSealJournalTombstoneBytes> ts_out_2{};
    ASSERT_EQ(SealJournalStoreLeaseTestAccess::read_seal_journal_tombstone(lease, 1, 1, ts_out_1).outcome,
              SealJournalLeaseIoOutcome::Ok);
    ASSERT_EQ(SealJournalStoreLeaseTestAccess::read_seal_journal_tombstone(lease, 1, 2, ts_out_2).outcome,
              SealJournalLeaseIoOutcome::Ok);
    EXPECT_EQ(0, std::memcmp(ts_out_1.data(), tombstone_1_1.data(), tombstone_1_1.size()));
    EXPECT_EQ(0, std::memcmp(ts_out_2.data(), tombstone_1_2.data(), tombstone_1_2.size()));

    // candidate_id=1 has no seq=3 tombstone -- must be NotFound, not an
    // accidental match against some other file.
    std::array<std::byte, kSealJournalTombstoneBytes> ts_out_missing{};
    EXPECT_EQ(SealJournalStoreLeaseTestAccess::read_seal_journal_tombstone(lease, 1, 3, ts_out_missing).read.status,
              seal_journal_store_detail::ReadFixedStatus::NotFound);

    ASSERT_EQ(lease.release(), SealJournalLeaseReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealJournalStoreLeaseLoaders, ReturnsNotFoundWhenFileMissing) {
    auto dir = make_temp_store_dir("notfound");
    SealJournalStoreLease lease(dir);
    ASSERT_EQ(lease.acquire(), SealJournalLeaseAcquireStatus::Acquired);

    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> out{};
    EXPECT_EQ(SealJournalStoreLeaseTestAccess::read_seal_journal_commit_watermark(lease, 999, out).read.status,
              seal_journal_store_detail::ReadFixedStatus::NotFound);

    std::array<std::byte, kSealJournalTombstoneBytes> ts_out{};
    EXPECT_EQ(SealJournalStoreLeaseTestAccess::read_seal_journal_tombstone(lease, 999, 1, ts_out).read.status,
              seal_journal_store_detail::ReadFixedStatus::NotFound);

    ASSERT_EQ(lease.release(), SealJournalLeaseReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealJournalStoreLeaseLoaders, ReturnsWrongSizeWhenFileTruncated) {
    auto dir = make_temp_store_dir("wrongsize");
    std::array<std::byte, kSealJournalCommitWatermarkWireBytes - 1> truncated{};
    write_raw_file(dir, "0000000000000001.jhw", truncated);

    SealJournalStoreLease lease(dir);
    ASSERT_EQ(lease.acquire(), SealJournalLeaseAcquireStatus::Acquired);

    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> out{};
    EXPECT_EQ(SealJournalStoreLeaseTestAccess::read_seal_journal_commit_watermark(lease, 1, out).read.status,
              seal_journal_store_detail::ReadFixedStatus::WrongSize);

    ASSERT_EQ(lease.release(), SealJournalLeaseReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealJournalStoreLeaseLoaders, NotHeldWhenLeaseNeverAcquired) {
    auto dir = make_temp_store_dir("notheld");
    SealJournalStoreLease lease(dir);  // never acquire()'d

    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> out{};
    EXPECT_EQ(SealJournalStoreLeaseTestAccess::read_seal_journal_commit_watermark(lease, 1, out).outcome,
              SealJournalLeaseIoOutcome::NotHeld);
}

TEST(SealJournalStoreLeaseLoaders, WrongOwnerWhenCalledFromNonOwnerThread) {
    auto dir = make_temp_store_dir("wrongowner");
    SealJournalStoreLease lease(dir);
    ASSERT_EQ(lease.acquire(), SealJournalLeaseAcquireStatus::Acquired);

    SealJournalLeaseIoOutcome non_owner_outcome{};
    std::thread other([&] {
        std::array<std::byte, kSealJournalCommitWatermarkWireBytes> out{};
        non_owner_outcome = SealJournalStoreLeaseTestAccess::read_seal_journal_commit_watermark(lease, 1, out).outcome;
    });
    other.join();

    EXPECT_EQ(non_owner_outcome, SealJournalLeaseIoOutcome::WrongOwner);

    ASSERT_EQ(lease.release(), SealJournalLeaseReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

// POSIX-only -- same already-documented platform finding as
// test_compaction_lease.cpp's SealBreadcrumbLoadersDirectoryFencing test and
// test_compaction_intent_store.cpp's IntentStoreDirectoryFencing test (see
// either's comment): on Windows, any handle held without FILE_SHARE_DELETE
// blocks rename/delete of the entire directory subtree, up to and including
// the top-level parent, from any process -- structurally unreachable there
// given SealJournalStoreLease's current open flags (mirrors CandidateLease's).
#ifndef _WIN32
TEST(SealJournalStoreLeaseDirectoryFencing, RenameAwayThenRestoreProvesStickyFence) {
    auto parent = make_temp_store_dir("fence_parent");
    write_raw_file(parent, "0000000000000001.jhw", pattern_bytes<kSealJournalCommitWatermarkWireBytes>(0x01));

    SealJournalStoreLease lease(parent);
    ASSERT_EQ(lease.acquire(), SealJournalLeaseAcquireStatus::Acquired);
    ASSERT_FALSE(lease.fenced());

    auto renamed_parent = parent;
    renamed_parent += "_renamed_away";
    std::filesystem::rename(parent, renamed_parent);

    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> out{};
    const auto first = SealJournalStoreLeaseTestAccess::read_seal_journal_commit_watermark(lease, 1, out);
    EXPECT_EQ(first.outcome, SealJournalLeaseIoOutcome::DirectoryIdentityChanged);
    EXPECT_TRUE(lease.fenced());
    ASSERT_TRUE(lease.last_identity_diagnostic().has_value());
    EXPECT_TRUE(lease.last_identity_diagnostic()->observed_path_missing);

    // Sticky: even after the directory is restored, this instance stays
    // fenced and refuses to re-touch the filesystem.
    std::filesystem::rename(renamed_parent, parent);
    const auto second = SealJournalStoreLeaseTestAccess::read_seal_journal_commit_watermark(lease, 1, out);
    EXPECT_EQ(second.outcome, SealJournalLeaseIoOutcome::StoreDirFenced);

    ASSERT_EQ(lease.release(), SealJournalLeaseReleaseStatus::Released);
    std::filesystem::remove_all(parent);
}
#endif  // !_WIN32
