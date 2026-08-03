// Real-file tests for control_plane_log_sink.hpp's ControlPlaneLogSink --
// mirrors test_durable_audit_sink.cpp's "destroy + reconstruct over the same
// path simulates a restart" idiom. Exercises append/fsync/tip-anchor/recovery
// mechanics AND the two safety-critical recovery fold rules added after the
// external review (Freeze->Clear->restart, cross-generation WaitSatisfied).
#include <gtest/gtest.h>
#include <hengyuan/control_plane_log_sink.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
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

std::array<std::byte, kKeyBlockSize> make_plaintext_key(std::uint8_t fill) {
    std::array<std::byte, kKeyBlockSize> key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::byte>(fill + i);
    return key;
}

}  // namespace

class ControlPlaneLogSinkTest : public ::testing::Test {
protected:
    std::string base_path_;
    std::unique_ptr<KeyRing> key_ring_;

    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        base_path_ = std::string(tmp) + "hy_cpls_" + std::to_string(GetCurrentProcessId()) + "_" +
                     std::to_string(counter) + ".log";
#else
        base_path_ = "/tmp/hy_cpls_" + std::to_string(getpid()) + "_" + std::to_string(counter) + ".log";
#endif
        remove_all();

        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x40 + i);
        key_ring_ = std::make_unique<KeyRing>(kek);

        WrappedKeyRecord rec{};
        ASSERT_EQ(key_ring_->add_key(1, make_plaintext_key(0x10), rec), KeyRingAddStatus::Ok);
    }

    void TearDown() override { remove_all(); }

    void remove_all() {
        std::remove(base_path_.c_str());
        std::remove((base_path_ + ".cp.lock").c_str());
        std::remove((base_path_ + ".cp.tip").c_str());
        std::remove((base_path_ + ".cp.tip.tmp").c_str());
    }
};

TEST_F(ControlPlaneLogSinkTest, FreshSinkOpensClean) {
    ControlPlaneLogSink sink(base_path_, *key_ring_, 1);
    EXPECT_TRUE(sink.is_open());
    EXPECT_FALSE(sink.fenced());
    EXPECT_EQ(sink.recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_FALSE(sink.supports_compaction());
}

TEST_F(ControlPlaneLogSinkTest, AppendEachSupportedMethodAcksWithIncrementingSequence) {
    ControlPlaneLogSink sink(base_path_, *key_ring_, 1);

    RateLimitFreezePayload freeze{};
    freeze.freeze_epoch = 1;
    auto r0 = sink.append_rate_freeze(freeze, FrameTimeKind::ServerCorrectedUtc);
    ASSERT_TRUE(r0.acked());
    EXPECT_EQ(r0.sequence, 0u);

    EndpointWeightConfig weights{};
    auto r1 = sink.append_weight_config(weights);
    ASSERT_TRUE(r1.acked());
    EXPECT_EQ(r1.sequence, 1u);

    RateLimitUsageSnapshotPayload usage{};
    auto r2 = sink.append_usage_snapshot(usage);
    ASSERT_TRUE(r2.acked());
    EXPECT_EQ(r2.sequence, 2u);

    GenerationBridgePayload bridge{};
    auto r3 = sink.append_generation_bridge(bridge);
    ASSERT_TRUE(r3.acked());
    EXPECT_EQ(r3.sequence, 3u);

    OperatorOverridePayload ov{};
    auto r4 = sink.append_operator_override(ov);
    ASSERT_TRUE(r4.acked());
    EXPECT_EQ(r4.sequence, 4u);

    FreezeProbeAttemptPayload attempt{};
    attempt.freeze_epoch = 1;
    attempt.attempt_ordinal = 1;
    auto r5 = sink.append_freeze_probe_attempt(attempt, FrameTimeKind::ServerCorrectedUtc);
    ASSERT_TRUE(r5.acked());
    EXPECT_EQ(r5.sequence, 5u);

    FreezeClearPayload clear{};
    clear.freeze_epoch = 1;
    auto r6 = sink.append_freeze_clear(clear, FrameTimeKind::ServerCorrectedUtc);
    ASSERT_TRUE(r6.acked());
    EXPECT_EQ(r6.sequence, 6u);

    FreezeWaitArmPayload arm{};
    auto r7 = sink.append_freeze_wait_arm(arm, FrameTimeKind::ServerCorrectedUtc);
    ASSERT_TRUE(r7.acked());
    EXPECT_EQ(r7.sequence, 7u);

    FreezeWaitSatisfiedPayload wait{};
    auto r8 = sink.append_freeze_wait_satisfied(wait, FrameTimeKind::ServerCorrectedUtc);
    ASSERT_TRUE(r8.acked());
    EXPECT_EQ(r8.sequence, 8u);

    auto r9 = sink.append_freeze_epoch_watermark(2, FrameTimeKind::ServerCorrectedUtc);
    ASSERT_TRUE(r9.acked());
    EXPECT_EQ(r9.sequence, 9u);

    SymbolRegistrySnapshotPayload snap{};
    snap.symbol_count = 0;
    auto r10 = sink.append_snapshot(snap, {});
    ASSERT_TRUE(r10.acked());
    EXPECT_EQ(r10.sequence, 10u);
}

TEST_F(ControlPlaneLogSinkTest, UnsupportedMethodsReturnFailedWithoutFencing) {
    ControlPlaneLogSink sink(base_path_, *key_ring_, 1);

    RateLimitFreezePayload snapshot_payload{};
    DurableControlPlaneSink::CompactionFreezeSnapshotProof proof1{};
    auto r0 = sink.append_compacted_freeze_snapshot(snapshot_payload, FrameTimeKind::ServerCorrectedUtc, proof1);
    EXPECT_FALSE(r0.acked());
    EXPECT_FALSE(sink.fenced());

    CompactedFreezeWaitEvidencePayload evidence{};
    DurableControlPlaneSink::CompactionWaitEvidenceProof proof2{};
    auto r1 = sink.append_compacted_wait_evidence(evidence, FrameTimeKind::ServerCorrectedUtc, proof2);
    EXPECT_FALSE(r1.acked());
    EXPECT_FALSE(sink.fenced());

    SealJournalAppliedView view{};
    auto r2 = sink.append_seal_journal_apply(view);
    EXPECT_FALSE(r2.acked());
    EXPECT_FALSE(sink.fenced());

    // A normal append still works after all three unsupported calls.
    EndpointWeightConfig cfg{};
    auto r3 = sink.append_weight_config(cfg);
    EXPECT_TRUE(r3.acked());
}

TEST_F(ControlPlaneLogSinkTest, RestartRoundTripsBasicLatestWinsFields) {
    {
        ControlPlaneLogSink sink(base_path_, *key_ring_, 1);
        EndpointWeightConfig weights{};
        weights.config_version = 9;
        ASSERT_TRUE(sink.append_weight_config(weights).acked());

        GenerationBridgePayload bridge{};
        bridge.prev_generation = 3;
        ASSERT_TRUE(sink.append_generation_bridge(bridge).acked());

        ASSERT_TRUE(sink.append_freeze_epoch_watermark(5, FrameTimeKind::ServerCorrectedUtc).acked());
    }
    {
        ControlPlaneLogSink sink(base_path_, *key_ring_, 1);
        ASSERT_EQ(sink.recovery_status(), RecoveryScanStatus::Recovered);

        RateLimitFreezePayload out_freeze{};
        bool out_has_freeze = false, out_permanent_latch = false;
        std::uint8_t out_uncleared = 0;
        EndpointWeightConfig out_weights{};
        bool out_has_weights = false;
        RateLimitUsageSnapshotPayload out_usage{};
        bool out_has_usage = false;
        GenerationBridgePayload out_bridge{};
        bool out_has_bridge = false;
        std::uint32_t out_next_epoch = 0;
        bool out_has_watermark = false;
        std::array<FreezeProbeAttemptPayload, 8> out_attempts{};
        std::size_t out_attempt_count = 0;
        FreezeClearPayload out_clear{};
        bool out_has_clear = false;
        FreezeWaitSatisfiedPayload out_wait_sat{};
        bool out_has_wait_sat = false;
        FreezeWaitArmPayload out_wait_arm{};
        bool out_has_wait_arm = false;

        auto status = sink.recover_control_plane(out_freeze, out_has_freeze, out_permanent_latch, out_uncleared,
                                                  out_weights, out_has_weights, out_usage, out_has_usage, out_bridge,
                                                  out_has_bridge, out_next_epoch, out_has_watermark, out_attempts,
                                                  out_attempt_count, out_clear, out_has_clear, out_wait_sat,
                                                  out_has_wait_sat, out_wait_arm, out_has_wait_arm);
        EXPECT_EQ(status, RecoveryScanStatus::Recovered);
        EXPECT_TRUE(out_has_weights);
        EXPECT_EQ(out_weights.config_version, 9u);
        EXPECT_TRUE(out_has_bridge);
        EXPECT_EQ(out_bridge.prev_generation, 3u);
        EXPECT_TRUE(out_has_watermark);
        EXPECT_EQ(out_next_epoch, 5u);
        EXPECT_FALSE(out_has_freeze);
    }
}

TEST_F(ControlPlaneLogSinkTest, FreezeThenClearThenRestartReportsNoActiveFreeze) {
    {
        ControlPlaneLogSink sink(base_path_, *key_ring_, 1);
        RateLimitFreezePayload freeze{};
        freeze.freeze_epoch = 42;
        ASSERT_TRUE(sink.append_rate_freeze(freeze, FrameTimeKind::ServerCorrectedUtc).acked());

        FreezeClearPayload clear{};
        clear.freeze_epoch = 42;
        clear.clear_kind = FreezeClearKind::OperatorAuthorized;
        ASSERT_TRUE(sink.append_freeze_clear(clear, FrameTimeKind::ServerCorrectedUtc).acked());
    }
    {
        ControlPlaneLogSink sink(base_path_, *key_ring_, 1);
        ASSERT_EQ(sink.recovery_status(), RecoveryScanStatus::Recovered);

        RateLimitFreezePayload out_freeze{};
        bool out_has_freeze = false, out_permanent_latch = false;
        std::uint8_t out_uncleared = 0;
        EndpointWeightConfig out_weights{};
        bool out_has_weights = false;
        RateLimitUsageSnapshotPayload out_usage{};
        bool out_has_usage = false;
        GenerationBridgePayload out_bridge{};
        bool out_has_bridge = false;
        std::uint32_t out_next_epoch = 0;
        bool out_has_watermark = false;
        std::array<FreezeProbeAttemptPayload, 8> out_attempts{};
        std::size_t out_attempt_count = 0;
        FreezeClearPayload out_clear{};
        bool out_has_clear = false;
        FreezeWaitSatisfiedPayload out_wait_sat{};
        bool out_has_wait_sat = false;
        FreezeWaitArmPayload out_wait_arm{};
        bool out_has_wait_arm = false;

        sink.recover_control_plane(out_freeze, out_has_freeze, out_permanent_latch, out_uncleared, out_weights,
                                    out_has_weights, out_usage, out_has_usage, out_bridge, out_has_bridge,
                                    out_next_epoch, out_has_watermark, out_attempts, out_attempt_count, out_clear,
                                    out_has_clear, out_wait_sat, out_has_wait_sat, out_wait_arm, out_has_wait_arm);

        // This is the direct verification of safety-critical rule 1: a
        // FreezeClear for the same epoch must retract out_has_freeze, not
        // leave a naive latest-frame-wins fold reporting a cleared freeze as
        // still active.
        EXPECT_FALSE(out_has_freeze);
        EXPECT_TRUE(out_has_clear);
        EXPECT_EQ(out_clear.freeze_epoch, 42u);
    }
}

TEST_F(ControlPlaneLogSinkTest, StaleGenerationWaitSatisfiedIsNotReportedAsCurrentEvidence) {
    {
        ControlPlaneLogSink sink(base_path_, *key_ring_, 1);
        // WaitSatisfied for generation 1 lands first...
        FreezeWaitSatisfiedPayload wait{};
        wait.wait_generation = 1;
        ASSERT_TRUE(sink.append_freeze_wait_satisfied(wait, FrameTimeKind::ServerCorrectedUtc).acked());

        // ...then the freeze is (re)folded at generation 2 (a new episode's
        // wait_generation advance) with no matching Satisfy for gen 2 yet.
        RateLimitFreezePayload freeze{};
        freeze.freeze_epoch = 1;
        freeze.wait_generation = 2;
        ASSERT_TRUE(sink.append_rate_freeze(freeze, FrameTimeKind::ServerCorrectedUtc).acked());
    }
    {
        ControlPlaneLogSink sink(base_path_, *key_ring_, 1);
        ASSERT_EQ(sink.recovery_status(), RecoveryScanStatus::Recovered);

        RateLimitFreezePayload out_freeze{};
        bool out_has_freeze = false, out_permanent_latch = false;
        std::uint8_t out_uncleared = 0;
        EndpointWeightConfig out_weights{};
        bool out_has_weights = false;
        RateLimitUsageSnapshotPayload out_usage{};
        bool out_has_usage = false;
        GenerationBridgePayload out_bridge{};
        bool out_has_bridge = false;
        std::uint32_t out_next_epoch = 0;
        bool out_has_watermark = false;
        std::array<FreezeProbeAttemptPayload, 8> out_attempts{};
        std::size_t out_attempt_count = 0;
        FreezeClearPayload out_clear{};
        bool out_has_clear = false;
        FreezeWaitSatisfiedPayload out_wait_sat{};
        bool out_has_wait_sat = true;  // poison -- must become false
        FreezeWaitArmPayload out_wait_arm{};
        bool out_has_wait_arm = false;

        sink.recover_control_plane(out_freeze, out_has_freeze, out_permanent_latch, out_uncleared, out_weights,
                                    out_has_weights, out_usage, out_has_usage, out_bridge, out_has_bridge,
                                    out_next_epoch, out_has_watermark, out_attempts, out_attempt_count, out_clear,
                                    out_has_clear, out_wait_sat, out_has_wait_sat, out_wait_arm, out_has_wait_arm);

        // Safety-critical rule 2: wait_generation mismatch (1 vs folded 2)
        // must NOT be reported as current evidence.
        EXPECT_TRUE(out_has_freeze);
        EXPECT_EQ(out_freeze.wait_generation, 2u);
        EXPECT_FALSE(out_has_wait_sat);
    }
}

TEST_F(ControlPlaneLogSinkTest, EightSlotProbeRingKeepsOnlyLastEightOldestFirst) {
    {
        ControlPlaneLogSink sink(base_path_, *key_ring_, 1);
        for (std::uint32_t i = 1; i <= 10; ++i) {
            FreezeProbeAttemptPayload attempt{};
            attempt.freeze_epoch = 1;
            attempt.attempt_ordinal = i;
            ASSERT_TRUE(sink.append_freeze_probe_attempt(attempt, FrameTimeKind::ServerCorrectedUtc).acked());
        }
    }
    {
        ControlPlaneLogSink sink(base_path_, *key_ring_, 1);
        RateLimitFreezePayload out_freeze{};
        bool out_has_freeze = false, out_permanent_latch = false;
        std::uint8_t out_uncleared = 0;
        EndpointWeightConfig out_weights{};
        bool out_has_weights = false;
        RateLimitUsageSnapshotPayload out_usage{};
        bool out_has_usage = false;
        GenerationBridgePayload out_bridge{};
        bool out_has_bridge = false;
        std::uint32_t out_next_epoch = 0;
        bool out_has_watermark = false;
        std::array<FreezeProbeAttemptPayload, 8> out_attempts{};
        std::size_t out_attempt_count = 0;
        FreezeClearPayload out_clear{};
        bool out_has_clear = false;
        FreezeWaitSatisfiedPayload out_wait_sat{};
        bool out_has_wait_sat = false;
        FreezeWaitArmPayload out_wait_arm{};
        bool out_has_wait_arm = false;

        sink.recover_control_plane(out_freeze, out_has_freeze, out_permanent_latch, out_uncleared, out_weights,
                                    out_has_weights, out_usage, out_has_usage, out_bridge, out_has_bridge,
                                    out_next_epoch, out_has_watermark, out_attempts, out_attempt_count, out_clear,
                                    out_has_clear, out_wait_sat, out_has_wait_sat, out_wait_arm, out_has_wait_arm);

        ASSERT_EQ(out_attempt_count, 8u);
        // Oldest-first: attempts 3..10 survive (1 and 2 evicted).
        for (std::size_t i = 0; i < 8; ++i) {
            EXPECT_EQ(out_attempts[i].attempt_ordinal, static_cast<std::uint32_t>(i + 3));
        }
    }
}

TEST_F(ControlPlaneLogSinkTest, UnknownActiveKeyIdFailsAppendWithoutFencing) {
    ControlPlaneLogSink sink(base_path_, *key_ring_, /*active_key_id=*/999);
    EndpointWeightConfig cfg{};
    auto r0 = sink.append_weight_config(cfg);
    EXPECT_FALSE(r0.acked());
    EXPECT_FALSE(sink.fenced());
}

TEST_F(ControlPlaneLogSinkTest, SecondSinkOverSamePathFailsToOpen) {
    ControlPlaneLogSink sink_a(base_path_, *key_ring_, 1);
    ASSERT_TRUE(sink_a.is_open());

    ControlPlaneLogSink sink_b(base_path_, *key_ring_, 1);
    EXPECT_FALSE(sink_b.is_open());
}

TEST_F(ControlPlaneLogSinkTest, RecoveryWithUnresolvableFrameKeyIsCorrupt) {
    {
        ControlPlaneLogSink sink(base_path_, *key_ring_, 1);
        EndpointWeightConfig cfg{};
        ASSERT_TRUE(sink.append_weight_config(cfg).acked());
    }
    // A fresh KeyRing that never loaded key_id 1 -- simulates an environment
    // mismatch (the key this log was signed under isn't available).
    std::array<std::byte, kKekSize> other_kek{};
    for (std::size_t i = 0; i < other_kek.size(); ++i) other_kek[i] = static_cast<std::byte>(0x99 + i);
    KeyRing empty_ring(other_kek);

    ControlPlaneLogSink sink(base_path_, empty_ring, 1);
    EXPECT_EQ(sink.recovery_status(), RecoveryScanStatus::Corrupt);
    EXPECT_TRUE(sink.fenced());
}
