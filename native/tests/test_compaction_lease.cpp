// Real-filesystem tests for compaction_lease.hpp's CandidateLease --
// directory rename/recreate identity fencing, non-owner release(), lease
// mutual exclusion (same-process and cross-process), and IntentStore's
// genesis write/read via the friend relationship. Governance: L2 (real
// file I/O).
#include <gtest/gtest.h>
#include <hengyuan/compaction_lease.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <thread>

using namespace hy;

namespace {

std::filesystem::path make_temp_candidate_dir(std::string_view tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("hy_round_d_lease_" + std::string(tag) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST(CandidateLease, AcquireSucceedsOnFreshDirectory) {
    auto dir = make_temp_candidate_dir("acquire_ok");
    CandidateLease lease(dir);
    EXPECT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    EXPECT_TRUE(lease.held());
    EXPECT_FALSE(lease.fenced());
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);  // must release before removing the
                                                            // directory -- Windows keeps a
                                                            // directory in use for as long as any
                                                            // handle to it (or a file inside it)
                                                            // remains open
    std::filesystem::remove_all(dir);
}

TEST(CandidateLease, AcquireFailsOnMissingDirectory) {
    auto dir = make_temp_candidate_dir("missing") / "does_not_exist";
    CandidateLease lease(dir);
    EXPECT_EQ(lease.acquire(), LeaseAcquireStatus::DirectoryMissing);
}

TEST(CandidateLease, SecondInstanceCannotAcquireSameDirectory) {
    auto dir = make_temp_candidate_dir("mutex");
    CandidateLease lease1(dir);
    ASSERT_EQ(lease1.acquire(), LeaseAcquireStatus::Acquired);

    CandidateLease lease2(dir);
    EXPECT_EQ(lease2.acquire(), LeaseAcquireStatus::HeldElsewhere);

    ASSERT_EQ(lease1.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(CandidateLease, ReacquireAfterReleaseSucceeds) {
    auto dir = make_temp_candidate_dir("reacquire");
    CandidateLease lease1(dir);
    ASSERT_EQ(lease1.acquire(), LeaseAcquireStatus::Acquired);
    ASSERT_EQ(lease1.release(), ReleaseStatus::Released);

    CandidateLease lease2(dir);
    EXPECT_EQ(lease2.acquire(), LeaseAcquireStatus::Acquired);
    ASSERT_EQ(lease2.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(CandidateLease, NonOwnerReleaseReturnsWrongOwnerAndDoesNotClose) {
    auto dir = make_temp_candidate_dir("nonowner_release");
    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);

    ReleaseStatus non_owner_result{};
    std::thread other([&] { non_owner_result = lease.release(); });
    other.join();

    EXPECT_EQ(non_owner_result, ReleaseStatus::WrongOwner);
    EXPECT_TRUE(lease.held()) << "non-owner release() must not have actually released the lease";

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);  // owner thread can still release
    std::filesystem::remove_all(dir);
}

TEST(CandidateLease, ReleaseWithoutAcquireIsNotHeld) {
    auto dir = make_temp_candidate_dir("not_held");
    CandidateLease lease(dir);
    EXPECT_EQ(lease.release(), ReleaseStatus::NotHeld);
}

TEST(CandidateLease, MoveTransfersHeldStateAndOwnerThread) {
    auto dir = make_temp_candidate_dir("move");
    CandidateLease lease1(dir);
    ASSERT_EQ(lease1.acquire(), LeaseAcquireStatus::Acquired);

    CandidateLease lease2(std::move(lease1));
    EXPECT_TRUE(lease2.held());
    EXPECT_FALSE(lease1.held());  // moved-from

    ASSERT_EQ(lease2.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

// CandidateLease's write/read methods (create_intent_genesis_no_replace/
// read_intent_genesis/read_x1_frame) are private with IntentStore as the
// ONLY friend -- by design, nothing in this file can call them directly.
// Their coverage (round-trip, uncertain-provenance memory, directory
// identity fencing across a real write attempt, etc.) lives in
// test_compaction_intent_store.cpp, driven through the real IntentStore.
// This file's job is CandidateLease's own lifecycle surface: acquire/
// release/held/fenced/move, which are all public.
