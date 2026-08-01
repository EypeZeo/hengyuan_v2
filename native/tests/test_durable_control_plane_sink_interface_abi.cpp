// DurableControlPlaneSink interface family (轨道 C) -- the L4-owned interface
// surface of §10, plus the 7 payload structs its method signatures depend on.
// Split out from test_durable_control_plane_abi.cpp / test_durable_control_
// plane_freeze_abi.cpp for the same reason those files split: a clean,
// self-contained vertical slice maps naturally to its own file.
//
// See test_durable_control_plane_abi.cpp's own header comment for the full
// caution about what these tests can and cannot prove -- short version: only
// tools/spec_enum_diff.py proves a type was transcribed from the spec, not
// from this file being green. That tool has NO equivalent for interface
// method signatures (it only parses `enum class` blocks), and this round adds
// three abstract classes -- the compensating check here is the stub
// subclasses below: each overrides every pure virtual of its interface, and a
// signature that doesn't exactly match the base class's declaration is a
// compile error, the same "proof of shape" role static_assert(is_trivially_
// copyable_v<T>) plays for a struct.
//
// One deliberate asymmetry: DurableControlPlaneSink::append_seal_journal_apply
// takes a `const SealJournalAppliedView&`, and that type is still an
// incomplete forward declaration in durable_control_plane.hpp (the
// seal-journal family stays out of scope this round). The stub below declares
// and overrides that method (proving the signature itself compiles), but the
// test body cannot construct an argument to actually CALL it -- an argument
// of incomplete type cannot be constructed. Every other method on all three
// interfaces IS called with real constructed argument values below, which is
// a strictly stronger proof for those.
//
// Governance: L1, no network/token/order. GTest only.

#include <gtest/gtest.h>
#include <hengyuan/durable_control_plane.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <type_traits>

using hy::AuditAppendResult;
using hy::DurableControlPlaneSink;
using hy::EndpointWeightConfig;
using hy::ExternalAnchorClient;
using hy::FrameTimeKind;
using hy::FreezeClearPayload;
using hy::FreezeProbeAttemptPayload;
using hy::FreezeWaitArmPayload;
using hy::FreezeWaitSatisfiedPayload;
using hy::GenerationBridgePayload;
using hy::GenerationSeal;
using hy::LastRemoteAckedTip;
using hy::OperatorOverridePayload;
using hy::OperatorOverrideSidecar;
using hy::RateLimitFreezePayload;
using hy::RateLimitUsageSnapshotPayload;
using hy::RecoveryScanStatus;
using hy::SealQueryStatus;
using hy::SymbolRegistrySnapshotPayload;
using hy::SymbolRules;

// ===========================================================================
// The 7 dependency structs -- shape only, no behavior.
// ===========================================================================

// --- LastRemoteAckedTip (spec L4 §10 / BINANCE:4586) ---

TEST(LastRemoteAckedTip, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<LastRemoteAckedTip>);
    static_assert(std::is_standard_layout_v<LastRemoteAckedTip>);
    SUCCEED();
}

// --- EndpointWeightConfig (spec §7.4 / BINANCE:2339) ---

TEST(EndpointWeightConfig, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<EndpointWeightConfig>);
    static_assert(std::is_standard_layout_v<EndpointWeightConfig>);
    SUCCEED();
}

// --- SymbolRegistrySnapshotPayload (spec L4 §10 / BINANCE:2880) ---

TEST(SymbolRegistrySnapshotPayload, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<SymbolRegistrySnapshotPayload>);
    static_assert(std::is_standard_layout_v<SymbolRegistrySnapshotPayload>);
    SUCCEED();
}

// --- RateLimitUsageSnapshotPayload (spec L4 §10 / BINANCE:2889) ---

TEST(RateLimitUsageSnapshotPayload, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<RateLimitUsageSnapshotPayload>);
    static_assert(std::is_standard_layout_v<RateLimitUsageSnapshotPayload>);
    SUCCEED();
}

// --- OperatorOverridePayload (spec L4 §10 / BINANCE:2903) ---

TEST(OperatorOverridePayload, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<OperatorOverridePayload>);
    static_assert(std::is_standard_layout_v<OperatorOverridePayload>);
    SUCCEED();
}

// --- GenerationBridgePayload (spec L4 §10 / BINANCE:2940) ---

TEST(GenerationBridgePayload, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<GenerationBridgePayload>);
    static_assert(std::is_standard_layout_v<GenerationBridgePayload>);
    SUCCEED();
}

// --- GenerationSeal (spec L4 §10 / BINANCE:2973) ---

TEST(GenerationSeal, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<GenerationSeal>);
    static_assert(std::is_standard_layout_v<GenerationSeal>);
    SUCCEED();
}

// None of the 7 structs above have a load-bearing non-zero default the way
// e.g. FreezeEpochWatermarkPayload::next_freeze_epoch (freeze-episode round)
// or FreezeWaitArmPayload::arm_ordinal do -- every field here legitimately
// defaults to zero per the spec text. No extra default-value tests are
// warranted; noted explicitly rather than silently omitted.

// ===========================================================================
// Interface abstractness/shape.
// ===========================================================================

TEST(DurableControlPlaneSink, IsAbstractWithVirtualDestructor) {
    static_assert(std::is_abstract_v<DurableControlPlaneSink>);
    static_assert(std::has_virtual_destructor_v<DurableControlPlaneSink>);
    SUCCEED();
}

TEST(ExternalAnchorClient, IsAbstractWithVirtualDestructor) {
    static_assert(std::is_abstract_v<ExternalAnchorClient>);
    static_assert(std::has_virtual_destructor_v<ExternalAnchorClient>);
    SUCCEED();
}

TEST(OperatorOverrideSidecar, IsAbstractWithVirtualDestructor) {
    static_assert(std::is_abstract_v<OperatorOverrideSidecar>);
    static_assert(std::has_virtual_destructor_v<OperatorOverrideSidecar>);
    SUCCEED();
}

TEST(DurableControlPlaneSinkNestedProofs, IsTriviallyCopyableStandardLayout) {
    static_assert(std::is_trivially_copyable_v<DurableControlPlaneSink::CompactionFreezeSnapshotProof>);
    static_assert(std::is_standard_layout_v<DurableControlPlaneSink::CompactionFreezeSnapshotProof>);
    static_assert(std::is_trivially_copyable_v<DurableControlPlaneSink::CompactionWaitEvidenceProof>);
    static_assert(std::is_standard_layout_v<DurableControlPlaneSink::CompactionWaitEvidenceProof>);
    SUCCEED();
}

// ===========================================================================
// Stub subclasses -- shape-proof only. Every override's signature must match
// the base class's pure virtual exactly, or this file fails to compile.
// Bodies are trivial placeholders; no real logic, per this header's own
// "no behavior" rule for interfaces.
// ===========================================================================

class StubExternalAnchorClient final : public ExternalAnchorClient {
public:
    AuditAppendResult export_tip_and_wait_bounded(
        std::uint64_t, std::uint64_t, std::uint32_t, std::uint64_t,
        std::span<const std::uint8_t, 32>, std::uint32_t) noexcept override {
        return {};
    }
    AuditAppendResult export_and_wait_ack(const GenerationSeal&) noexcept override {
        return {};
    }
    RecoveryScanStatus read_latest(GenerationSeal&) noexcept override {
        return RecoveryScanStatus::IoError;
    }
    RecoveryScanStatus read_latest_tip(
        std::uint64_t, std::uint64_t, std::uint32_t&, std::uint64_t&,
        std::array<std::uint8_t, 32>&, std::uint32_t&) noexcept override {
        return RecoveryScanStatus::IoError;
    }
    SealQueryStatus query_seal_by_request_id(
        std::uint64_t, std::uint64_t, std::uint64_t, GenerationSeal&) noexcept override {
        return SealQueryStatus::NotFound;
    }
};

class StubOperatorOverrideSidecar final : public OperatorOverrideSidecar {
public:
    AuditAppendResult write_override(const OperatorOverridePayload&) noexcept override {
        return {};
    }
    bool read_override(OperatorOverridePayload&) const noexcept override {
        return false;
    }
    AuditAppendResult consume_after_successful_admission(std::uint64_t) noexcept override {
        return {};
    }
};

class StubDurableControlPlaneSink final : public DurableControlPlaneSink {
public:
    AuditAppendResult append_rate_freeze(
        const RateLimitFreezePayload&, FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_compacted_freeze_snapshot(
        const RateLimitFreezePayload&, FrameTimeKind,
        const CompactionFreezeSnapshotProof&) noexcept override {
        return {};
    }
    AuditAppendResult append_compacted_wait_evidence(
        const hy::CompactedFreezeWaitEvidencePayload&, FrameTimeKind,
        const CompactionWaitEvidenceProof&) noexcept override {
        return {};
    }
    AuditAppendResult append_snapshot(
        const SymbolRegistrySnapshotPayload&, std::span<const SymbolRules>) noexcept override {
        return {};
    }
    AuditAppendResult append_weight_config(const EndpointWeightConfig&) noexcept override {
        return {};
    }
    AuditAppendResult append_usage_snapshot(
        const RateLimitUsageSnapshotPayload&) noexcept override {
        return {};
    }
    AuditAppendResult append_generation_bridge(
        const GenerationBridgePayload&) noexcept override {
        return {};
    }
    AuditAppendResult append_operator_override(
        const OperatorOverridePayload&) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_probe_attempt(
        const FreezeProbeAttemptPayload&, FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_clear(
        const FreezeClearPayload&, FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_wait_arm(
        const FreezeWaitArmPayload&, FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_wait_satisfied(
        const FreezeWaitSatisfiedPayload&, FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_epoch_watermark(
        std::uint32_t, FrameTimeKind) noexcept override {
        return {};
    }
    // Cannot be called from a test body -- SealJournalAppliedView is
    // deliberately still incomplete this round (see the file-level comment
    // above and durable_control_plane.hpp's forward declaration). Overriding
    // it here still proves the signature itself is well-formed C++.
    AuditAppendResult append_seal_journal_apply(
        const hy::SealJournalAppliedView&) noexcept override {
        return {};
    }
    RecoveryScanStatus recover_control_plane(
        RateLimitFreezePayload&, bool&, bool&, std::uint8_t&,
        EndpointWeightConfig&, bool&,
        RateLimitUsageSnapshotPayload&, bool&,
        GenerationBridgePayload&, bool&,
        std::uint32_t&, bool&,
        std::array<FreezeProbeAttemptPayload, 8>&, std::size_t&,
        FreezeClearPayload&, bool&,
        FreezeWaitSatisfiedPayload&, bool&,
        FreezeWaitArmPayload&, bool&) noexcept override {
        return RecoveryScanStatus::IoError;
    }
};

// ===========================================================================
// Instantiate through the base reference and call every method that CAN be
// called (everything except append_seal_journal_apply -- see above) with
// real constructed argument values. Only compilation and return-type shape
// are asserted; this is an ABI-shape test, not a behavior test.
// ===========================================================================

TEST(ExternalAnchorClientStub, AllMethodsCallableThroughBaseReference) {
    StubExternalAnchorClient stub;
    ExternalAnchorClient& iface = stub;

    std::array<std::uint8_t, 32> tip_mac{};
    EXPECT_FALSE(iface.export_tip_and_wait_bounded(
        0, 0, 1, 1, std::span<const std::uint8_t, 32>(tip_mac), 0).acked());

    GenerationSeal seal{};
    EXPECT_FALSE(iface.export_and_wait_ack(seal).acked());

    GenerationSeal out_seal{};
    EXPECT_EQ(iface.read_latest(out_seal), RecoveryScanStatus::IoError);

    std::uint32_t out_gen{};
    std::uint64_t out_seq{};
    std::array<std::uint8_t, 32> out_tip_mac{};
    std::uint32_t out_key_id{};
    EXPECT_EQ(iface.read_latest_tip(0, 0, out_gen, out_seq, out_tip_mac, out_key_id),
              RecoveryScanStatus::IoError);

    GenerationSeal query_out{};
    EXPECT_EQ(iface.query_seal_by_request_id(0, 0, 0, query_out), SealQueryStatus::NotFound);
}

TEST(OperatorOverrideSidecarStub, AllMethodsCallableThroughBaseReference) {
    StubOperatorOverrideSidecar stub;
    OperatorOverrideSidecar& iface = stub;

    OperatorOverridePayload ov{};
    EXPECT_FALSE(iface.write_override(ov).acked());

    OperatorOverridePayload out_ov{};
    EXPECT_FALSE(iface.read_override(out_ov));

    EXPECT_FALSE(iface.consume_after_successful_admission(1).acked());
}

TEST(DurableControlPlaneSinkStub, AllCallableMethodsThroughBaseReference) {
    StubDurableControlPlaneSink stub;
    DurableControlPlaneSink& iface = stub;

    RateLimitFreezePayload freeze{};
    EXPECT_FALSE(iface.append_rate_freeze(freeze, FrameTimeKind::UnknownBootstrap).acked());

    DurableControlPlaneSink::CompactionFreezeSnapshotProof freeze_proof{};
    EXPECT_FALSE(iface.append_compacted_freeze_snapshot(
        freeze, FrameTimeKind::UnknownBootstrap, freeze_proof).acked());

    hy::CompactedFreezeWaitEvidencePayload evidence{};
    DurableControlPlaneSink::CompactionWaitEvidenceProof wait_proof{};
    EXPECT_FALSE(iface.append_compacted_wait_evidence(
        evidence, FrameTimeKind::UnknownBootstrap, wait_proof).acked());

    SymbolRegistrySnapshotPayload snap{};
    std::array<SymbolRules, 1> rules{};
    EXPECT_FALSE(iface.append_snapshot(snap, std::span<const SymbolRules>(rules)).acked());

    EndpointWeightConfig weights{};
    EXPECT_FALSE(iface.append_weight_config(weights).acked());

    RateLimitUsageSnapshotPayload usage{};
    EXPECT_FALSE(iface.append_usage_snapshot(usage).acked());

    GenerationBridgePayload bridge{};
    EXPECT_FALSE(iface.append_generation_bridge(bridge).acked());

    OperatorOverridePayload ov{};
    EXPECT_FALSE(iface.append_operator_override(ov).acked());

    FreezeProbeAttemptPayload attempt{};
    EXPECT_FALSE(iface.append_freeze_probe_attempt(attempt, FrameTimeKind::UnknownBootstrap).acked());

    FreezeClearPayload clear{};
    EXPECT_FALSE(iface.append_freeze_clear(clear, FrameTimeKind::UnknownBootstrap).acked());

    FreezeWaitArmPayload arm{};
    EXPECT_FALSE(iface.append_freeze_wait_arm(arm, FrameTimeKind::UnknownBootstrap).acked());

    FreezeWaitSatisfiedPayload wait{};
    EXPECT_FALSE(iface.append_freeze_wait_satisfied(wait, FrameTimeKind::UnknownBootstrap).acked());

    EXPECT_FALSE(iface.append_freeze_epoch_watermark(1, FrameTimeKind::UnknownBootstrap).acked());

    RateLimitFreezePayload out_freeze{};
    bool out_has_freeze{};
    bool out_permanent_latch{};
    std::uint8_t out_uncleared_epoch_count{};
    EndpointWeightConfig out_weights{};
    bool out_has_weights{};
    RateLimitUsageSnapshotPayload out_usage{};
    bool out_has_usage{};
    GenerationBridgePayload out_bridge{};
    bool out_has_bridge{};
    std::uint32_t out_next_freeze_epoch{};
    bool out_has_freeze_epoch_watermark{};
    std::array<FreezeProbeAttemptPayload, 8> out_attempts{};
    std::size_t out_attempt_count{};
    FreezeClearPayload out_clear{};
    bool out_has_clear{};
    FreezeWaitSatisfiedPayload out_wait_satisfied{};
    bool out_has_wait_satisfied{};
    FreezeWaitArmPayload out_wait_arm{};
    bool out_has_wait_arm{};
    EXPECT_EQ(iface.recover_control_plane(
                  out_freeze, out_has_freeze, out_permanent_latch, out_uncleared_epoch_count,
                  out_weights, out_has_weights, out_usage, out_has_usage, out_bridge,
                  out_has_bridge, out_next_freeze_epoch, out_has_freeze_epoch_watermark,
                  out_attempts, out_attempt_count, out_clear, out_has_clear,
                  out_wait_satisfied, out_has_wait_satisfied, out_wait_arm, out_has_wait_arm),
              RecoveryScanStatus::IoError);
}
