// SPDX-License-Identifier: proprietary
// test_symbol_registry_fakes.hpp — test-only DurableControlPlaneSink fake + fixture builder
// shared between test_symbol_registry.cpp (plain unit tests) and
// test_symbol_registry_concurrency.cpp (the TSan-verified multi-reader/single-writer test) --
// same split-by-concurrency-label pattern as test_spsc_ring.cpp/test_spsc_concurrency.cpp.
//
// Only included by test .cpp files.

#pragma once

#include <hengyuan/symbol_registry.hpp>

#include <cstring>

namespace hy {

// A configurable-success/failure DurableControlPlaneSink test double. Every method besides
// append_snapshot() is unused by SymbolRegistry and just returns a default/failure value --
// mirrors test_durable_control_plane_sink_interface_abi.cpp's StubDurableControlPlaneSink
// "every pure virtual must be overridden" shape, but with append_snapshot() made configurable
// (that file's stub is fixed-return and private to itself, not reusable here).
class FakeSymbolRegistrySink final : public DurableControlPlaneSink {
public:
    void set_next_append_acked(bool acked) noexcept { next_acked_ = acked; }
    std::size_t append_snapshot_call_count() const noexcept { return call_count_; }
    const SymbolRegistrySnapshotPayload& last_payload() const noexcept { return last_payload_; }

    AuditAppendResult append_snapshot(const SymbolRegistrySnapshotPayload& snap,
                                       std::span<const SymbolRules> /*entries*/) noexcept override {
        ++call_count_;
        last_payload_ = snap;
        if (!next_acked_) return AuditAppendResult{};  // default-constructed == Status::Failed
        return AuditAppendResult{AuditAppendResult::Status::Acked, ++sequence_};
    }

    AuditAppendResult append_rate_freeze(const RateLimitFreezePayload&,
                                          FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_compacted_freeze_snapshot(
        const RateLimitFreezePayload&, FrameTimeKind,
        const CompactionFreezeSnapshotProof&) noexcept override {
        return {};
    }
    AuditAppendResult append_compacted_wait_evidence(
        const CompactedFreezeWaitEvidencePayload&, FrameTimeKind,
        const CompactionWaitEvidenceProof&) noexcept override {
        return {};
    }
    AuditAppendResult append_weight_config(const EndpointWeightConfig&) noexcept override {
        return {};
    }
    AuditAppendResult append_usage_snapshot(
        const RateLimitUsageSnapshotPayload&) noexcept override {
        return {};
    }
    AuditAppendResult append_generation_bridge(const GenerationBridgePayload&) noexcept override {
        return {};
    }
    AuditAppendResult append_operator_override(const OperatorOverridePayload&) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_probe_attempt(const FreezeProbeAttemptPayload&,
                                                   FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_clear(const FreezeClearPayload&,
                                           FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_wait_arm(const FreezeWaitArmPayload&,
                                              FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_wait_satisfied(const FreezeWaitSatisfiedPayload&,
                                                    FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_freeze_epoch_watermark(std::uint32_t,
                                                     FrameTimeKind) noexcept override {
        return {};
    }
    AuditAppendResult append_seal_journal_apply(const SealJournalAppliedView&) noexcept override {
        return {};
    }
    RecoveryScanStatus recover_control_plane(
        RateLimitFreezePayload&, bool&, bool&, std::uint8_t&, EndpointWeightConfig&, bool&,
        RateLimitUsageSnapshotPayload&, bool&, GenerationBridgePayload&, bool&, std::uint32_t&,
        bool&, std::array<FreezeProbeAttemptPayload, 8>&, std::size_t&, FreezeClearPayload&,
        bool&, FreezeWaitSatisfiedPayload&, bool&, FreezeWaitArmPayload&,
        bool&) noexcept override {
        return RecoveryScanStatus::IoError;
    }

private:
    bool next_acked_{true};
    std::size_t call_count_{0};
    std::uint64_t sequence_{0};
    SymbolRegistrySnapshotPayload last_payload_{};
};

inline ParsedExchangeInfo make_single_symbol_parsed(std::int64_t server_time_ms,
                                                     std::uint8_t price_scale,
                                                     std::uint8_t qty_scale,
                                                     std::int64_t min_notional_ticks) {
    ParsedExchangeInfo parsed{};
    parsed.server_time_ms = server_time_ms;
    parsed.symbol_count = 1;
    std::strncpy(parsed.symbols[0].symbol, "BTCUSDT", sizeof(parsed.symbols[0].symbol) - 1);
    parsed.symbols[0].is_trading = true;
    parsed.symbols[0].price_scale = price_scale;
    parsed.symbols[0].qty_scale = qty_scale;
    parsed.symbols[0].min_notional_ticks = min_notional_ticks;
    return parsed;
}

}  // namespace hy
