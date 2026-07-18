// P2-EXEC-LIVE-01 D12-8: dry-run-before-live evidence chain tests.
#include <gtest/gtest.h>
#include <hengyuan/dry_run_evidence.hpp>

using hy::DryRunEvidenceChain;
using hy::EvidencePath;

static constexpr std::uint32_t kBuildHash = 0xDEADBEEF;
static constexpr std::uint32_t kSuiteId = 1;

TEST(DryRunEvidence, InitiallyNoPathExercised) {
    DryRunEvidenceChain chain;
    EXPECT_FALSE(chain.is_path_exercised(EvidencePath::SubmitSuccess));
    EXPECT_FALSE(chain.is_path_exercised(EvidencePath::SubmitReject));
    EXPECT_FALSE(chain.is_path_exercised(EvidencePath::SubmitAmbiguous));
    EXPECT_FALSE(chain.is_path_exercised(EvidencePath::KillSwitch));
    EXPECT_FALSE(chain.all_paths_exercised());
    EXPECT_FALSE(chain.live_ready());
    EXPECT_EQ(chain.exercised_count(), 0u);
}

TEST(DryRunEvidence, RecordSinglePath) {
    DryRunEvidenceChain chain;
    chain.record(EvidencePath::SubmitSuccess, 1000, kBuildHash, kSuiteId);
    EXPECT_TRUE(chain.is_path_exercised(EvidencePath::SubmitSuccess));
    EXPECT_FALSE(chain.is_path_exercised(EvidencePath::SubmitReject));
    EXPECT_EQ(chain.exercised_count(), 1u);
    EXPECT_FALSE(chain.all_paths_exercised());
}

TEST(DryRunEvidence, AllPathsExercised) {
    DryRunEvidenceChain chain;
    chain.record(EvidencePath::SubmitSuccess, 1000, kBuildHash, kSuiteId);
    chain.record(EvidencePath::SubmitReject, 1001, kBuildHash, kSuiteId);
    chain.record(EvidencePath::SubmitAmbiguous, 1002, kBuildHash, kSuiteId);
    chain.record(EvidencePath::KillSwitch, 1003, kBuildHash, kSuiteId);
    EXPECT_TRUE(chain.all_paths_exercised());
    EXPECT_EQ(chain.exercised_count(), 4u);
}

TEST(DryRunEvidence, LiveReadyWhenAllPathsAndConsistentBuild) {
    DryRunEvidenceChain chain;
    chain.record(EvidencePath::SubmitSuccess, 1000, kBuildHash, kSuiteId);
    chain.record(EvidencePath::SubmitReject, 1001, kBuildHash, kSuiteId);
    chain.record(EvidencePath::SubmitAmbiguous, 1002, kBuildHash, kSuiteId);
    chain.record(EvidencePath::KillSwitch, 1003, kBuildHash, kSuiteId);
    EXPECT_TRUE(chain.live_ready());
}

TEST(DryRunEvidence, NotReadyWithInconsistentBuild) {
    DryRunEvidenceChain chain;
    chain.record(EvidencePath::SubmitSuccess, 1000, kBuildHash, kSuiteId);
    chain.record(EvidencePath::SubmitReject, 1001, kBuildHash, kSuiteId);
    chain.record(EvidencePath::SubmitAmbiguous, 1002, 0xCAFEBABE, kSuiteId);  // different build
    chain.record(EvidencePath::KillSwitch, 1003, kBuildHash, kSuiteId);
    EXPECT_TRUE(chain.all_paths_exercised());
    EXPECT_FALSE(chain.consistent_build());
    EXPECT_FALSE(chain.live_ready());
}

TEST(DryRunEvidence, NotReadyWithMissingPath) {
    DryRunEvidenceChain chain;
    chain.record(EvidencePath::SubmitSuccess, 1000, kBuildHash, kSuiteId);
    chain.record(EvidencePath::SubmitReject, 1001, kBuildHash, kSuiteId);
    chain.record(EvidencePath::KillSwitch, 1003, kBuildHash, kSuiteId);
    // Missing: SubmitAmbiguous
    EXPECT_FALSE(chain.all_paths_exercised());
    EXPECT_FALSE(chain.live_ready());
}

TEST(DryRunEvidence, GetReturnsRecord) {
    DryRunEvidenceChain chain;
    chain.record(EvidencePath::SubmitSuccess, 42000, kBuildHash, 99);
    auto& rec = chain.get(EvidencePath::SubmitSuccess);
    EXPECT_TRUE(rec.exercised);
    EXPECT_EQ(rec.timestamp_ms, 42000);
    EXPECT_EQ(rec.build_hash, kBuildHash);
    EXPECT_EQ(rec.test_suite_id, 99u);
}

TEST(DryRunEvidence, ResetClearsAll) {
    DryRunEvidenceChain chain;
    chain.record(EvidencePath::SubmitSuccess, 1000, kBuildHash, kSuiteId);
    chain.record(EvidencePath::SubmitReject, 1001, kBuildHash, kSuiteId);
    chain.reset();
    EXPECT_EQ(chain.exercised_count(), 0u);
    EXPECT_FALSE(chain.all_paths_exercised());
}

TEST(DryRunEvidence, ConsistentBuildWithNoEvidenceIsTrue) {
    DryRunEvidenceChain chain;
    // No evidence at all — vacuously consistent
    EXPECT_TRUE(chain.consistent_build());
}

TEST(DryRunEvidence, OverwritePathUpdatesRecord) {
    DryRunEvidenceChain chain;
    chain.record(EvidencePath::SubmitSuccess, 1000, 0x1111, kSuiteId);
    chain.record(EvidencePath::SubmitSuccess, 2000, 0x2222, kSuiteId);
    auto& rec = chain.get(EvidencePath::SubmitSuccess);
    EXPECT_EQ(rec.timestamp_ms, 2000);
    EXPECT_EQ(rec.build_hash, 0x2222u);
}
