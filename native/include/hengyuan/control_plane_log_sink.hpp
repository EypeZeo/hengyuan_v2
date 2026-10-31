// SPDX-License-Identifier: proprietary
// control_plane_log_sink.hpp — ControlPlaneLogSink: the first real
// DurableControlPlaneSink implementation.
//
// Governance: L2 (real file I/O). Builds on Phase 0's KeyRing/keyed-frame
// substrate and this round's shared DurableLogStore (durable_log_store.hpp)
// -- see docs/SPEC_INVARIANTS.md's "Phase 1" entry for the full scope
// decision and its evolution (an external review the user forwarded
// mid-round led to two additions beyond the original "mechanics only" plan:
// two safety-critical recovery fold rules, below, and sharing DurableLogStore
// with DurableAuditSink instead of duplicating its platform code).
//
// SCOPE -- MECHANICS ONLY, EXPLICITLY NOT BUSINESS RULES:
//   Every append_* method here durably persists whatever valid-shaped
//   payload it is given (fsync, hash-chained v4 frame, tip-anchor, a real
//   per-frame KeyRing key lookup) and Acks -- it does NOT implement the
//   elaborate cross-frame "MUST reject if..." validation documented in each
//   method's own SPEC-METHOD comment in durable_control_plane.hpp:
//     - append_freeze_probe_attempt: attempt_ordinal no-gap enforcement,
//       purpose-vs-clock-state gating, folded-wait-vs-Satisfied cross-check
//     - append_freeze_clear/append_freeze_wait_satisfied: wait_generation
//       sequencing at APPEND time (recover_control_plane below DOES gate
//       wait_generation, but only at recovery, not at append)
//     - append_compacted_*: compaction-session/baseline verification (moot
//       here -- see supports_compaction() below, this class never runs them)
//   This is a deliberate, user-confirmed gap -- not silently done. A future
//   round implements the append-time causal checks; this round is the
//   durable-mechanics foundation they will sit on top of.
//
// TWO SAFETY-CRITICAL RECOVERY FOLD RULES (the one piece of "business logic"
// this round DOES implement, because leaving it out was judged a real
// security hole, not a scope simplification -- see recover_control_plane()):
//   1. A FreezeClear frame retracts out_has_freeze for the RateLimitFreeze
//      epoch it clears -- a pure latest-frame-wins fold would otherwise
//      report an already-cleared freeze as still active after a restart.
//   2. out_has_wait_satisfied requires wait_generation to match the folded
//      freeze's wait_generation (and be > 0) -- otherwise a stale, prior-
//      generation WaitSatisfied frame would be misread as current evidence.
//   Everything else recover_control_plane folds is naive latest-frame-wins
//   with NO other cross-frame reasoning (out_has_wait_arm, the 0/1
//   out_uncleared_epoch_count simplification, etc.) -- see the method's own
//   comment for the complete, explicitly disclosed list of what's NOT done.
//
// KEY SELECTION: active_key_id is a constructor parameter; rotate_active_key()
// (Phase 4, docs/SPEC_INVARIANTS.md) is the ONE place it is ever reassigned,
// always after durably confirming the rotation (see that method's own doc
// comment for the exact sequencing -- no writer-quiesce protocol was needed
// in the end: this class already has no internal synchronization and
// rotate_active_key() simply joins append_*()/recover_control_plane() in
// requiring the same single owning thread). Every append_* does a LIVE
// per-call KeyRing::active_key(active_key_id_, ...) lookup (never cached) so
// this class is "really" KeyRing-backed rather than snapshotting a key at
// construction -- an unknown/retired active_key_id at append time returns
// Failed WITHOUT fencing (a correctable configuration mistake); the SAME
// failure during recovery (a frame's own key_id cannot be resolved) returns
// Corrupt instead, because at that point it means the current process's
// KeyRing does not contain a key that was actually used to sign durable
// on-disk data -- an environment mismatch that must fail closed, not silently
// skip the frame. Precondition: the caller must load every key_id still
// referenced by this log into `key_ring` (via KeyRing::load_wrapped_key())
// BEFORE constructing this sink.
//
// FENCING DISCIPLINE: fencing is reserved for genuine I/O-layer failure
// (append_and_fsync / write_tip_anchor returning false) -- a pre-I/O
// input-shape rejection (append_snapshot's entries-vs-symbol_count mismatch,
// an unresolvable active_key_id) returns Failed without touching fenced_,
// since nothing was ever durably written and the sink's on-disk state is
// unaffected. This is a deliberately narrower fencing trigger than
// DurableAuditSink's "any failure fences" rule -- that class's single fixed-
// size AuditRecord payload never hits a legitimate pre-I/O encode rejection
// in practice, but this class's variable-length append_snapshot can.
//
// ZERO HEAP ALLOCATION ON THE APPEND PATH: every append_* uses a
// stack-allocated std::array sized to its own (or, for the 10 fixed-size
// payload types, the largest fixed) frame -- kMaxFixedFrameBytes below is
// sized to FreezeClearPayload's frame (the largest of the 10), and
// append_snapshot uses its own kMaxControlPlaneFrameBytes-sized buffer
// (control_plane_frame_codec.hpp's kMaxSnapshotFrameSize). Construction and
// recover_control_plane's one-time recovery scan (streamed via
// DurableLogStore::read_chunk, not a whole-file read -- see that class's own
// header comment for why this differs from DurableAuditSink) are the only
// places any per-instance state beyond these stack buffers exists, and even
// those need no heap containers (a handful of fixed struct+bool fold slots
// plus one 8-slot ring for FreezeProbeAttemptPayload).

#pragma once

#include <hengyuan/control_plane_frame_codec.hpp>
#include <hengyuan/durable_audit_sink.hpp>  // reused: detail::encode_tip_anchor/decode_tip_anchor,
                                              // kTipAnchorSize -- generic tip-anchor MAC codec, not
                                              // OrderEvent-specific, no need for a third copy
#include <hengyuan/durable_control_plane.hpp>
#include <hengyuan/durable_log_store.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/secure_wipe.hpp>

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace hy {

// D5 / R3-0 (docs/OPERATOR_RESOLUTION_DESIGN.md 6): what the control-plane log says `symbol_id` stands for. The audit
// log records only the numeric id of an order's symbol; the exchange symbol ("BTCUSDT") lives in the symbol-registry
// snapshots this log persists, one entry per id, keyed by POSITION (symbol_registry.hpp: parsed.symbols[i] becomes the
// rules for symbol_id == i, and the caller keeps that order stable across refreshes). The operator tool needs the name to
// ask the exchange about an order, and a WRONG name is the dangerous failure: a query for the wrong symbol answers "no
// such order" for an order that exists. So the answer is only ever Known when EVERY snapshot in the log agrees.
//
// D5 / R3b-3 (blueprint L-50): "every snapshot agrees" says nothing about WHICH log this is. If the control-plane log was
// deleted and recreated after the orders were placed, an old id can have been given a different symbol and still be Known,
// so the lookup also reports since when the log has named the id (SymbolNameLookup::named_since_ms), and the operator tool
// refuses a name that is not provably older than the order (operator_resolution_flow.hpp: symbol_for_order()).
enum class SymbolNameStatus : std::uint8_t {
    Unknown = 0,   // no snapshot mentioned this id, or the log could not be recovered: no answer
    Known = 1,     // every snapshot that mentioned the id gave the same, well-formed name
    Conflict = 2,  // two snapshots disagreed, or one gave a name that is not a symbol: refuse, do not guess
    // The log names the id, but not provably before the order was sent (the first snapshot that named it is later than the
    // order, or carries no usable time): refuse. The sink never answers this itself -- it does not know the order; only
    // symbol_for_order() produces it, from a Known lookup and the order's send time.
    NotNamedAtOrderTime = 3,
};

struct SymbolNameLookup {
    SymbolNameStatus status{SymbolNameStatus::Unknown};
    std::string_view name{};  // valid only when status == Known; views storage inside the sink
    // Meaningful only when status is Known (and NotNamedAtOrderTime): the wall time, in milliseconds, of the FIRST snapshot
    // frame -- first in the log -- that gave the id its name, as its MAC-protected header records it (frame_wall_time_ms():
    // the header time of a ServerCorrectedUtc frame, which for the registry's own snapshots is the exchange's serverTime of
    // the exchangeInfo response). 0 when that frame carried no wall time; whatever else is here (negative, or not a
    // plausible date) is what the frame said, and judging it is the caller's job. 0 for every other status.
    std::int64_t named_since_ms{0};
};

class ControlPlaneLogSink final : public DurableControlPlaneSink {
public:
    // path: base path (e.g. "state/control_plane.log"); lock/tip sidecars use
    // ".cp.lock"/".cp.tip" suffixes (distinct from DurableAuditSink's own
    // ".lock"/".tip" so the two sinks can coexist over sibling base paths).
    // key_ring: NOT owned, must outlive this sink. active_key_id: fixed for
    // this instance's lifetime -- see class header's "KEY SELECTION" note.
    ControlPlaneLogSink(const std::string& path, KeyRing& key_ring, std::uint32_t active_key_id) noexcept
        : log_store_(path, path + ".cp.lock", path + ".cp.tip"),
          key_ring_(key_ring),
          active_key_id_(active_key_id) {
        if (!log_store_.acquire_lock()) {
            recovery_status_ = RecoveryScanStatus::IoError;
            return;
        }
        if (!log_store_.open_log()) {
            recovery_status_ = RecoveryScanStatus::IoError;
            log_store_.release_lock();
            return;
        }
        recovery_status_ = run_recovery_scan();
        if (recovery_status_ != RecoveryScanStatus::Clean && recovery_status_ != RecoveryScanStatus::Recovered) {
            fenced_ = true;
        }
    }

    ~ControlPlaneLogSink() override {
        log_store_.close_log();
        log_store_.release_lock();
    }

    ControlPlaneLogSink(const ControlPlaneLogSink&) = delete;
    ControlPlaneLogSink& operator=(const ControlPlaneLogSink&) = delete;
    ControlPlaneLogSink(ControlPlaneLogSink&&) = delete;
    ControlPlaneLogSink& operator=(ControlPlaneLogSink&&) = delete;

    bool is_open() const noexcept { return log_store_.is_log_open(); }
    bool fenced() const noexcept { return fenced_; }
    RecoveryScanStatus recovery_status() const noexcept { return recovery_status_; }
    // SAFE-03 crash-window slice (blueprint L-36): how many bytes of a torn tail -- the partial frame a crash in the
    // middle of an append left behind -- recovery cut off the end of the log before the first new append (see
    // DurableAuditSink::recovered_torn_tail_bytes() for why). 0 = the file ended on a frame boundary.
    std::uint64_t recovered_torn_tail_bytes() const noexcept { return torn_tail_bytes_; }

    // D5 / R3-0: the exchange symbol that `symbol_id` has meant in EVERY symbol-registry snapshot of this log (see
    // SymbolNameStatus for why "every"). Unknown for an id no snapshot mentioned, for an id at or past kMaxSymbols, and for
    // every id when this sink's recovery did not come back Clean/Recovered: a half-read log answers nothing. The view is
    // into this sink and lives as long as it does. Read-only: the append path and the 20-argument recover_control_plane()
    // interface are untouched. (A Known answer also says since when: SymbolNameLookup::named_since_ms.)
    SymbolNameLookup recovered_symbol_name(std::uint32_t symbol_id) const noexcept {
        if (recovery_status_ != RecoveryScanStatus::Clean && recovery_status_ != RecoveryScanStatus::Recovered) return {};
        if (symbol_id >= kMaxSymbols) return {};
        const SymbolNameSlot& slot = fold_.symbol_names[symbol_id];
        if (slot.status != SymbolNameStatus::Known) return {slot.status, {}, 0};
        return {SymbolNameStatus::Known, std::string_view(slot.name, slot.length), slot.first_named_ms};
    }
    // How many symbol-registry snapshot frames the recovery scan read.
    std::uint64_t recovered_snapshot_count() const noexcept { return fold_.snapshot_count; }

    // Capability gate for the 2 compaction-only methods below -- callers
    // MUST check this before calling them. Always false: there is no
    // CompactionSourceBaseline/compaction-session machinery anywhere in this
    // codebase yet.
    bool supports_compaction() const noexcept { return false; }

    std::uint32_t active_key_id() const noexcept { return active_key_id_; }

    // Phase 4 (docs/SPEC_INVARIANTS.md): safely switches active_key_id_ to
    // new_key_id without a process restart. Caller must have already loaded
    // new_key_id into key_ring (the "prepare" step). Must be called from the
    // same owning thread as every append_*/recover_control_plane() call --
    // see class header's "KEY SELECTION" note.
    //
    // Unlike DurableAuditSink (which needs a separate sidecar log -- its main
    // log format is hard-limited to OrderEvent frames only), this class
    // already supports multiple record types in one stream, so the
    // KeyRotated record is written directly to THIS sink's own main log --
    // as control_plane_frame_codec.hpp's bespoke, non-DurableRecordType
    // KeyRotatedRecord frame (see that struct's own header comment for why
    // it isn't a real DurableRecordType member), not as an 18th enumerator.
    // The scan below peeks for its marker byte before ever consulting
    // DurableRecordType/the switch below. Durable-before-flip: the frame
    // (and its tip anchor) is written and Acked BEFORE active_key_id_ itself
    // changes; on any failure active_key_id_ is left untouched. This does
    // NOT go through append_generic() -- that helper resolves its signing
    // key from the (still old, not yet flipped) active_key_id_ member, but
    // this frame must be signed under new_key_id specifically (same
    // reasoning as DurableAuditSink::rotate_active_key(), and the same
    // reason finish_append() above takes an explicit key_id_for_anchor
    // parameter instead of reading the member).
    bool rotate_active_key(std::uint32_t new_key_id, std::int64_t now_ms) noexcept {
        if (fenced_ || !log_store_.is_log_open()) return false;

        std::array<std::byte, kKeyBlockSize> new_key_block{};
        if (!key_ring_.active_key(new_key_id, new_key_block)) return false;  // "prepare" not done yet
        std::span<const std::byte> new_hmac_key(new_key_block.data(), new_key_block.size());

        const std::uint32_t old_key_id = active_key_id_;
        const std::uint64_t seq_at_rotation = (next_sequence_ == 0) ? kNoPriorTipSequence : next_sequence_ - 1;
        KeyRotationPayload payload{old_key_id, new_key_id, seq_at_rotation};

        std::array<std::byte, kKeyRotatedFrameSize> buf{};
        const auto n = encode_key_rotated_frame(buf, new_key_id, next_sequence_, FrameTimeKind::ServerCorrectedUtc,
                                                 now_ms, payload, tip_mac_, new_hmac_key);
        if (n != kKeyRotatedFrameSize) {
            fenced_ = true;  // encode failure at a fixed, always-sufficient buffer size is a real bug, not input shape
            secure_wipe(new_key_block.data(), new_key_block.size());
            return false;
        }
        auto result = finish_append(std::span<const std::byte>(buf.data(), n), new_key_block,
                                     /*key_id_for_anchor=*/new_key_id);
        secure_wipe(new_key_block.data(), new_key_block.size());
        if (!result.acked()) return false;  // finish_append() already fenced on genuine I/O failure

        active_key_id_ = new_key_id;
        return true;
    }

    AuditAppendResult append_rate_freeze(const RateLimitFreezePayload& freeze,
                                         FrameTimeKind time_kind) noexcept override {
        const std::int64_t recorded_utc_ms = time_kind == FrameTimeKind::UnknownBootstrap ? 0 : freeze.recorded_utc_ms;
        return append_generic(kRateLimitFreezeFrameSize, [&](std::span<std::byte> out, std::uint32_t key_id,
                                                               std::uint64_t seq,
                                                               std::span<const std::byte, kMacLen> prev,
                                                               std::span<const std::byte> key) {
            return encode_rate_limit_freeze_frame(out, key_id, seq, time_kind, recorded_utc_ms, freeze, prev, key);
        });
    }

    AuditAppendResult append_compacted_freeze_snapshot(const RateLimitFreezePayload&, FrameTimeKind,
                                                        const CompactionFreezeSnapshotProof&) noexcept override {
        return failed_result();
    }

    AuditAppendResult append_compacted_wait_evidence(const CompactedFreezeWaitEvidencePayload&, FrameTimeKind,
                                                      const CompactionWaitEvidenceProof&) noexcept override {
        return failed_result();
    }

    AuditAppendResult append_snapshot(const SymbolRegistrySnapshotPayload& snap,
                                       std::span<const SymbolRules> entries) noexcept override {
        if (fenced_ || !log_store_.is_log_open()) return failed_result();

        std::array<std::byte, kKeyBlockSize> key_block{};
        if (!key_ring_.active_key(active_key_id_, key_block)) return failed_result();

        std::array<std::byte, kMaxSnapshotFrameSize> buf{};
        const auto n = encode_snapshot_frame(buf, active_key_id_, next_sequence_, FrameTimeKind::ServerCorrectedUtc,
                                             snap.timestamp_ms, snap, entries, tip_mac_,
                                             std::span<const std::byte>(key_block.data(), key_block.size()));
        if (n == 0) return failed_result();  // pre-I/O shape rejection, nothing written -- not fenced

        return finish_append(std::span<const std::byte>(buf.data(), n), key_block, active_key_id_);
    }

    AuditAppendResult append_weight_config(const EndpointWeightConfig& cfg) noexcept override {
        return append_generic(kEndpointWeightConfigFrameSize, [&](std::span<std::byte> out, std::uint32_t key_id,
                                                                   std::uint64_t seq,
                                                                   std::span<const std::byte, kMacLen> prev,
                                                                   std::span<const std::byte> key) {
            return encode_endpoint_weight_config_frame(out, key_id, seq, FrameTimeKind::ServerCorrectedUtc, 0, cfg,
                                                        prev, key);
        });
    }

    AuditAppendResult append_usage_snapshot(const RateLimitUsageSnapshotPayload& usage) noexcept override {
        return append_generic(kRateLimitUsageSnapshotFrameSize, [&](std::span<std::byte> out, std::uint32_t key_id,
                                                                     std::uint64_t seq,
                                                                     std::span<const std::byte, kMacLen> prev,
                                                                     std::span<const std::byte> key) {
            return encode_rate_limit_usage_snapshot_frame(out, key_id, seq, FrameTimeKind::ServerCorrectedUtc,
                                                           usage.recorded_utc_ms, usage, prev, key);
        });
    }

    AuditAppendResult append_generation_bridge(const GenerationBridgePayload& bridge) noexcept override {
        return append_generic(kGenerationBridgeFrameSize, [&](std::span<std::byte> out, std::uint32_t key_id,
                                                               std::uint64_t seq,
                                                               std::span<const std::byte, kMacLen> prev,
                                                               std::span<const std::byte> key) {
            return encode_generation_bridge_frame(out, key_id, seq, FrameTimeKind::ServerCorrectedUtc, 0, bridge,
                                                   prev, key);
        });
    }

    AuditAppendResult append_operator_override(const OperatorOverridePayload& ov) noexcept override {
        return append_generic(kOperatorOverrideFrameSize, [&](std::span<std::byte> out, std::uint32_t key_id,
                                                               std::uint64_t seq,
                                                               std::span<const std::byte, kMacLen> prev,
                                                               std::span<const std::byte> key) {
            return encode_operator_override_frame(out, key_id, seq, FrameTimeKind::ServerCorrectedUtc, ov.wall_utc_ms,
                                                   ov, prev, key);
        });
    }

    AuditAppendResult append_freeze_probe_attempt(const FreezeProbeAttemptPayload& attempt,
                                                  FrameTimeKind time_kind) noexcept override {
        const std::int64_t recorded_utc_ms = 0;
        return append_generic(kFreezeProbeAttemptFrameSize, [&](std::span<std::byte> out, std::uint32_t key_id,
                                                                 std::uint64_t seq,
                                                                 std::span<const std::byte, kMacLen> prev,
                                                                 std::span<const std::byte> key) {
            return encode_freeze_probe_attempt_frame(out, key_id, seq, time_kind, recorded_utc_ms, attempt, prev,
                                                      key);
        });
    }

    AuditAppendResult append_freeze_clear(const FreezeClearPayload& clear, FrameTimeKind time_kind) noexcept override {
        const std::int64_t recorded_utc_ms = 0;
        return append_generic(kFreezeClearFrameSize, [&](std::span<std::byte> out, std::uint32_t key_id,
                                                          std::uint64_t seq,
                                                          std::span<const std::byte, kMacLen> prev,
                                                          std::span<const std::byte> key) {
            return encode_freeze_clear_frame(out, key_id, seq, time_kind, recorded_utc_ms, clear, prev, key);
        });
    }

    AuditAppendResult append_freeze_wait_arm(const FreezeWaitArmPayload& arm, FrameTimeKind time_kind) noexcept override {
        const std::int64_t recorded_utc_ms = 0;
        return append_generic(kFreezeWaitArmFrameSize, [&](std::span<std::byte> out, std::uint32_t key_id,
                                                            std::uint64_t seq,
                                                            std::span<const std::byte, kMacLen> prev,
                                                            std::span<const std::byte> key) {
            return encode_freeze_wait_arm_frame(out, key_id, seq, time_kind, recorded_utc_ms, arm, prev, key);
        });
    }

    AuditAppendResult append_freeze_wait_satisfied(const FreezeWaitSatisfiedPayload& wait,
                                                    FrameTimeKind time_kind) noexcept override {
        const std::int64_t recorded_utc_ms = 0;
        return append_generic(kFreezeWaitSatisfiedFrameSize, [&](std::span<std::byte> out, std::uint32_t key_id,
                                                                  std::uint64_t seq,
                                                                  std::span<const std::byte, kMacLen> prev,
                                                                  std::span<const std::byte> key) {
            return encode_freeze_wait_satisfied_frame(out, key_id, seq, time_kind, recorded_utc_ms, wait, prev, key);
        });
    }

    AuditAppendResult append_freeze_epoch_watermark(std::uint32_t next_freeze_epoch,
                                                     FrameTimeKind time_kind) noexcept override {
        FreezeEpochWatermarkPayload payload{};
        payload.next_freeze_epoch = next_freeze_epoch;
        const std::int64_t recorded_utc_ms = 0;
        return append_generic(kFreezeEpochWatermarkFrameSize, [&](std::span<std::byte> out, std::uint32_t key_id,
                                                                   std::uint64_t seq,
                                                                   std::span<const std::byte, kMacLen> prev,
                                                                   std::span<const std::byte> key) {
            return encode_freeze_epoch_watermark_frame(out, key_id, seq, time_kind, recorded_utc_ms, payload, prev,
                                                        key);
        });
    }

    AuditAppendResult append_seal_journal_apply(const SealJournalAppliedView&) noexcept override {
        return failed_result();
    }

    // See class header for the full disclosure of what this fold does and
    // does not implement. Naive latest-frame-wins for most categories, plus
    // the two safety-critical rules (freeze/clear epoch cross-check,
    // wait_generation gating) added after the external review.
    RecoveryScanStatus recover_control_plane(RateLimitFreezePayload& out_active_freeze, bool& out_has_freeze,
                                              bool& out_permanent_latch, std::uint8_t& out_uncleared_epoch_count,
                                              EndpointWeightConfig& out_weights, bool& out_has_weights,
                                              RateLimitUsageSnapshotPayload& out_usage, bool& out_has_usage,
                                              GenerationBridgePayload& out_bridge, bool& out_has_bridge,
                                              std::uint32_t& out_next_freeze_epoch,
                                              bool& out_has_freeze_epoch_watermark,
                                              std::array<FreezeProbeAttemptPayload, 8>& out_freeze_probe_attempts,
                                              std::size_t& out_freeze_probe_attempt_count,
                                              FreezeClearPayload& out_latest_clear, bool& out_has_clear,
                                              FreezeWaitSatisfiedPayload& out_wait_satisfied,
                                              bool& out_has_wait_satisfied, FreezeWaitArmPayload& out_wait_arm,
                                              bool& out_has_wait_arm) noexcept override {
        out_active_freeze = fold_.active_freeze;
        out_has_freeze = fold_.has_freeze;
        out_permanent_latch = fold_.has_freeze && fold_.active_freeze.source == 2;
        out_uncleared_epoch_count = fold_.has_freeze ? 1 : 0;
        out_weights = fold_.weights;
        out_has_weights = fold_.has_weights;
        out_usage = fold_.usage;
        out_has_usage = fold_.has_usage;
        out_bridge = fold_.bridge;
        out_has_bridge = fold_.has_bridge;
        out_next_freeze_epoch = fold_.next_freeze_epoch;
        out_has_freeze_epoch_watermark = fold_.has_freeze_epoch_watermark;
        out_freeze_probe_attempts = fold_.probe_ring;
        out_freeze_probe_attempt_count = fold_.probe_count;
        out_latest_clear = fold_.latest_clear;
        out_has_clear = fold_.has_clear;
        out_wait_arm = fold_.wait_arm;
        out_has_wait_arm = fold_.has_wait_arm;

        // Safety-critical rule 2 (wait_generation gating): only true when the
        // latest WaitSatisfied/CompactedFreezeWaitEvidence frame's
        // wait_generation matches the CURRENT folded freeze's wait_generation
        // and is > 0 -- a stale cross-generation frame must never read as
        // current evidence.
        out_wait_satisfied = fold_.wait_satisfied;
        out_has_wait_satisfied = fold_.has_wait_satisfied_raw && out_has_freeze &&
                                  fold_.wait_satisfied.wait_generation == out_active_freeze.wait_generation &&
                                  fold_.wait_satisfied.wait_generation > 0;

        return recovery_status_;
    }

private:
    static constexpr std::size_t kKeyBlockSize = hy::kKeyBlockSize;

    // Largest of the 10 fixed-size frames this class writes (FreezeClear's,
    // 91 + 304 = 395 bytes) -- append_snapshot uses its own
    // kMaxSnapshotFrameSize buffer separately (control_plane_frame_codec.hpp).
    static constexpr std::size_t kMaxFixedFrameBytes = kFreezeClearFrameSize;
    static_assert(kMaxFixedFrameBytes >= kRateLimitFreezeFrameSize);
    static_assert(kMaxFixedFrameBytes >= kFreezeProbeAttemptFrameSize);
    static_assert(kMaxFixedFrameBytes >= kFreezeWaitArmFrameSize);
    static_assert(kMaxFixedFrameBytes >= kFreezeWaitSatisfiedFrameSize);
    static_assert(kMaxFixedFrameBytes >= kFreezeEpochWatermarkFrameSize);
    static_assert(kMaxFixedFrameBytes >= kEndpointWeightConfigFrameSize);
    static_assert(kMaxFixedFrameBytes >= kRateLimitUsageSnapshotFrameSize);
    static_assert(kMaxFixedFrameBytes >= kOperatorOverrideFrameSize);
    static_assert(kMaxFixedFrameBytes >= kGenerationBridgeFrameSize);

    static AuditAppendResult failed_result() noexcept {
        AuditAppendResult r{};
        r.status = AuditAppendResult::Status::Failed;
        return r;
    }

    // Shared plumbing for all 10 fixed-size append_* methods: resolve the
    // active key live (never cached), encode into a stack buffer via
    // `encode_fn`, append+fsync, write the tip anchor, bump the sequence.
    // Fences ONLY on a real I/O failure (append_and_fsync/write_tip_anchor)
    // -- an unresolvable key or an encode_fn returning a size mismatch
    // (dead in practice for these fixed sizes, since the buffer is always
    // exactly frame_size) returns Failed without fencing, since nothing was
    // ever durably written in either case.
    template <typename EncodeFn>
    AuditAppendResult append_generic(std::size_t frame_size, EncodeFn&& encode_fn) noexcept {
        if (fenced_ || !log_store_.is_log_open()) return failed_result();

        std::array<std::byte, kKeyBlockSize> key_block{};
        if (!key_ring_.active_key(active_key_id_, key_block)) return failed_result();

        std::array<std::byte, kMaxFixedFrameBytes> buf{};
        const auto n = encode_fn(std::span<std::byte>(buf.data(), frame_size), active_key_id_, next_sequence_,
                                  std::span<const std::byte, kMacLen>(tip_mac_.data(), tip_mac_.size()),
                                  std::span<const std::byte>(key_block.data(), key_block.size()));
        if (n != frame_size) return failed_result();

        return finish_append(std::span<const std::byte>(buf.data(), n), key_block, active_key_id_);
    }

    // Common tail shared by every append path (fixed-size and
    // append_snapshot's variable-size path): append+fsync the already-
    // encoded frame, write the tip anchor, and advance sequence/tip_mac_ on
    // success. `frame_bytes` must be the complete, already-MAC'd frame.
    // key_id_for_anchor is explicit (not read from the active_key_id_ member)
    // so rotate_active_key() (Phase 4) can write its own KeyRotated frame's
    // tip anchor under new_key_id BEFORE active_key_id_ itself is flipped --
    // see that method's own doc comment. Every existing call site passes
    // active_key_id_ explicitly -- a zero-behavior-change signature extension.
    AuditAppendResult finish_append(std::span<const std::byte> frame_bytes,
                                     const std::array<std::byte, kKeyBlockSize>& key_block,
                                     std::uint32_t key_id_for_anchor) noexcept {
        if (!log_store_.append_and_fsync(frame_bytes)) {
            fenced_ = true;
            return failed_result();
        }

        std::array<std::byte, kMacLen> this_mac{};
        std::memcpy(this_mac.data(), frame_bytes.data() + frame_bytes.size() - kMacLen, kMacLen);

        std::array<std::byte, kTipAnchorSize> anchor_buf{};
        detail::encode_tip_anchor(anchor_buf, next_sequence_, this_mac, key_id_for_anchor,
                                   std::span<const std::byte>(key_block.data(), key_block.size()));
        if (!log_store_.write_tip_anchor(anchor_buf)) {
            fenced_ = true;
            return failed_result();
        }

        AuditAppendResult result{};
        result.status = AuditAppendResult::Status::Acked;
        result.sequence = next_sequence_;
        tip_mac_ = this_mac;
        ++next_sequence_;
        return result;
    }

    // --- Recovery scan: naive fold + the two safety-critical rules ---

    // D5 / R3-0: one symbol_id's name as the snapshots of the log have given it.
    struct SymbolNameSlot {
        SymbolNameStatus status{SymbolNameStatus::Unknown};
        std::uint8_t length{0};
        char name[kSymbolNameLen]{};
        std::int64_t first_named_ms{0};  // D5 / R3b-3: wall time of the frame of the first well-formed mention; see fold_symbol_names()
    };

    struct FoldState {
        std::array<SymbolNameSlot, kMaxSymbols> symbol_names{};  // D5 / R3-0, see fold_symbol_names()
        std::uint64_t snapshot_count{0};
        RateLimitFreezePayload active_freeze{};
        bool has_freeze{false};
        EndpointWeightConfig weights{};
        bool has_weights{false};
        RateLimitUsageSnapshotPayload usage{};
        bool has_usage{false};
        GenerationBridgePayload bridge{};
        bool has_bridge{false};
        std::uint32_t next_freeze_epoch{0};
        bool has_freeze_epoch_watermark{false};
        std::array<FreezeProbeAttemptPayload, 8> probe_ring{};
        std::size_t probe_count{0};
        FreezeClearPayload latest_clear{};
        bool has_clear{false};
        FreezeWaitSatisfiedPayload wait_satisfied{};
        bool has_wait_satisfied_raw{false};  // existence-only; generation-gated at read time (see recover_control_plane)
        FreezeWaitArmPayload wait_arm{};
        bool has_wait_arm{false};
    };

    // A symbol name as the exchange spells it: non-empty, terminated INSIDE the field, upper-case letters and digits only.
    // The snapshot decoder copies the 20 name bytes as they are on the wire, so none of that is checked before this point.
    static std::optional<std::string_view> well_formed_symbol_name(const SymbolRules& rules) noexcept {
        std::size_t n = 0;
        while (n < kSymbolNameLen && rules.symbol[n] != '\0') ++n;
        if (n == 0 || n == kSymbolNameLen) return std::nullopt;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = rules.symbol[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return std::nullopt;
        }
        return std::string_view(rules.symbol, n);
    }

    // One snapshot into the name table. An id's name becomes Known by its first well-formed mention, stays Known while
    // every later mention agrees, and becomes Conflict -- for good -- the first time one does not (a rename, a swap of two
    // ids, or a name that is not a symbol). A snapshot that mentions fewer ids than an earlier one says nothing about the
    // ids it leaves out: an order that used such an id was made under an earlier snapshot, which named it.
    //
    // D5 / R3b-3: the slot also keeps `frame_wall_ms`, the wall time of the frame that made the id Known (see
    // SymbolNameLookup::named_since_ms). It is set by that first mention and never moved: not by a later snapshot that agrees
    // (the name has been known since the first), and not forward to the first frame that happens to carry a usable time
    // when the first one did not (that would be the less cautious reading, and the sink's own writer always stamps a wall time).
    void fold_symbol_names(std::span<const SymbolRules> entries, std::int64_t frame_wall_ms) noexcept {
        ++fold_.snapshot_count;
        for (std::size_t i = 0; i < entries.size() && i < kMaxSymbols; ++i) {
            SymbolNameSlot& slot = fold_.symbol_names[i];
            if (slot.status == SymbolNameStatus::Conflict) continue;
            const std::optional<std::string_view> name = well_formed_symbol_name(entries[i]);
            if (!name) {
                slot.status = SymbolNameStatus::Conflict;
            } else if (slot.status == SymbolNameStatus::Unknown) {
                std::memcpy(slot.name, name->data(), name->size());
                slot.length = static_cast<std::uint8_t>(name->size());
                slot.first_named_ms = frame_wall_ms;
                slot.status = SymbolNameStatus::Known;
            } else if (*name != std::string_view(slot.name, slot.length)) {
                slot.status = SymbolNameStatus::Conflict;
            }
        }
    }

    void push_probe_attempt(const FreezeProbeAttemptPayload& attempt) noexcept {
        if (fold_.probe_count < fold_.probe_ring.size()) {
            fold_.probe_ring[fold_.probe_count++] = attempt;
        } else {
            for (std::size_t i = 1; i < fold_.probe_ring.size(); ++i) fold_.probe_ring[i - 1] = fold_.probe_ring[i];
            fold_.probe_ring.back() = attempt;
        }
    }

    RecoveryScanStatus run_recovery_scan() noexcept {
        fold_ = FoldState{};

        const std::uint64_t total_size = log_store_.log_size();
        if (total_size == 0) {
            return finalize_scan_with_anchor_check(/*log_tip_valid=*/false, 0, {});
        }

        std::uint64_t offset = 0;
        std::uint64_t expected_seq = 0;
        std::array<std::byte, kMacLen> running_prev_mac{};
        std::array<std::byte, kMacLen> last_mac{};
        std::array<std::byte, kMacLen> prev_of_last_mac{};  // the MAC of the frame before the last one
        bool any_frame_seen = false;

        // Read-ahead buffer sized to the largest frame this sink can ever
        // produce -- bounded, not proportional to file size (see class
        // header's zero-heap-allocation note and DurableLogStore's own
        // streaming-vs-whole-file-read rationale).
        std::array<std::byte, kMaxSnapshotFrameSize> chunk{};

        while (offset < total_size) {
            std::size_t want = chunk.size();
            const std::uint64_t remaining = total_size - offset;
            if (remaining < want) want = static_cast<std::size_t>(remaining);
            std::size_t got = 0;
            if (!log_store_.read_chunk(offset, chunk, want, got)) return RecoveryScanStatus::IoError;
            if (got == 0) break;  // nothing left to read -- treat as end of usable data

            std::span<const std::byte> view(chunk.data(), got);

            // Phase 4: KeyRotatedRecord frames are deliberately NOT
            // DurableRecordType-typed (see that struct's own header comment)
            // -- peek the marker byte BEFORE assuming this is a normal
            // control-plane frame, since read_control_plane_header would
            // otherwise reject it as UnknownVersion.
            if (peek_is_key_rotated_record(view)) {
                std::uint32_t rot_key_id = 0;
                std::array<std::byte, kKeyBlockSize> rot_key_block{};
                if (peek_key_rotated_record_key_id(view, rot_key_id)) {
                    if (!key_ring_.active_key(rot_key_id, rot_key_block)) return RecoveryScanStatus::Corrupt;
                }
                DecodedKeyRotatedFrame f{};
                std::size_t rot_frame_size = 0;
                const auto st = decode_key_rotated_frame(
                    view, std::span<const std::byte>(rot_key_block.data(), rot_key_block.size()), f, rot_frame_size);
                if (st == FrameDecodeStatus::Truncated) break;  // torn tail, stop scanning
                if (st != FrameDecodeStatus::Ok) return RecoveryScanStatus::Corrupt;
                if (f.sequence_number != expected_seq || f.prev_mac != running_prev_mac) {
                    return RecoveryScanStatus::Corrupt;
                }
                // Not folded into any out-param -- every rotation is a
                // permanent historical fact, not "unresolved state to
                // recover" the way the freeze/weights/usage/bridge folds are.
                running_prev_mac = f.mac;
                prev_of_last_mac = last_mac;
                last_mac = f.mac;
                any_frame_seen = true;
                ++expected_seq;
                offset += rot_frame_size;
                continue;
            }

            detail::ControlPlaneFrameHeader hdr{};
            const auto hdr_status = detail::read_control_plane_header(view, hdr);
            if (hdr_status == FrameDecodeStatus::Truncated) break;  // torn tail, stop scanning
            if (hdr_status != FrameDecodeStatus::Ok) return RecoveryScanStatus::Corrupt;

            std::array<std::byte, kKeyBlockSize> key_block{};
            if (!key_ring_.active_key(hdr.key_id, key_block)) return RecoveryScanStatus::Corrupt;
            std::span<const std::byte> key(key_block.data(), key_block.size());

            std::size_t frame_size = 0;
            std::array<std::byte, kMacLen> frame_mac{};
            bool dispatched_ok = false;

            switch (hdr.record_type) {
                case DurableRecordType::RateLimitFreeze: {
                    DecodedRateLimitFreezeFrame f{};
                    const auto st = decode_rate_limit_freeze_frame(view, key, f, frame_size);
                    if (st == FrameDecodeStatus::Truncated) goto torn_tail;
                    if (st != FrameDecodeStatus::Ok) return RecoveryScanStatus::Corrupt;
                    if (f.sequence_number != expected_seq || f.prev_mac != running_prev_mac) {
                        return RecoveryScanStatus::Corrupt;
                    }
                    fold_.active_freeze = f.payload;
                    fold_.has_freeze = true;
                    frame_mac = f.mac;
                    dispatched_ok = true;
                    break;
                }
                case DurableRecordType::FreezeProbeAttempt: {
                    DecodedFreezeProbeAttemptFrame f{};
                    const auto st = decode_freeze_probe_attempt_frame(view, key, f, frame_size);
                    if (st == FrameDecodeStatus::Truncated) goto torn_tail;
                    if (st != FrameDecodeStatus::Ok) return RecoveryScanStatus::Corrupt;
                    if (f.sequence_number != expected_seq || f.prev_mac != running_prev_mac) {
                        return RecoveryScanStatus::Corrupt;
                    }
                    push_probe_attempt(f.payload);
                    frame_mac = f.mac;
                    dispatched_ok = true;
                    break;
                }
                case DurableRecordType::FreezeClear: {
                    DecodedFreezeClearFrame f{};
                    const auto st = decode_freeze_clear_frame(view, key, f, frame_size);
                    if (st == FrameDecodeStatus::Truncated) goto torn_tail;
                    if (st != FrameDecodeStatus::Ok) return RecoveryScanStatus::Corrupt;
                    if (f.sequence_number != expected_seq || f.prev_mac != running_prev_mac) {
                        return RecoveryScanStatus::Corrupt;
                    }
                    fold_.latest_clear = f.payload;
                    fold_.has_clear = true;
                    // Safety-critical rule 1: retract out_has_freeze for the
                    // epoch this clear frame terminates.
                    if (fold_.has_freeze && f.payload.freeze_epoch == fold_.active_freeze.freeze_epoch) {
                        fold_.has_freeze = false;
                    }
                    frame_mac = f.mac;
                    dispatched_ok = true;
                    break;
                }
                case DurableRecordType::FreezeWaitArm: {
                    DecodedFreezeWaitArmFrame f{};
                    const auto st = decode_freeze_wait_arm_frame(view, key, f, frame_size);
                    if (st == FrameDecodeStatus::Truncated) goto torn_tail;
                    if (st != FrameDecodeStatus::Ok) return RecoveryScanStatus::Corrupt;
                    if (f.sequence_number != expected_seq || f.prev_mac != running_prev_mac) {
                        return RecoveryScanStatus::Corrupt;
                    }
                    fold_.wait_arm = f.payload;
                    fold_.has_wait_arm = true;
                    frame_mac = f.mac;
                    dispatched_ok = true;
                    break;
                }
                case DurableRecordType::FreezeWaitSatisfied: {
                    DecodedFreezeWaitSatisfiedFrame f{};
                    const auto st = decode_freeze_wait_satisfied_frame(view, key, f, frame_size);
                    if (st == FrameDecodeStatus::Truncated) goto torn_tail;
                    if (st != FrameDecodeStatus::Ok) return RecoveryScanStatus::Corrupt;
                    if (f.sequence_number != expected_seq || f.prev_mac != running_prev_mac) {
                        return RecoveryScanStatus::Corrupt;
                    }
                    fold_.wait_satisfied = f.payload;
                    fold_.has_wait_satisfied_raw = true;
                    frame_mac = f.mac;
                    dispatched_ok = true;
                    break;
                }
                case DurableRecordType::FreezeEpochWatermark: {
                    DecodedFreezeEpochWatermarkFrame f{};
                    const auto st = decode_freeze_epoch_watermark_frame(view, key, f, frame_size);
                    if (st == FrameDecodeStatus::Truncated) goto torn_tail;
                    if (st != FrameDecodeStatus::Ok) return RecoveryScanStatus::Corrupt;
                    if (f.sequence_number != expected_seq || f.prev_mac != running_prev_mac) {
                        return RecoveryScanStatus::Corrupt;
                    }
                    fold_.next_freeze_epoch = f.payload.next_freeze_epoch;
                    fold_.has_freeze_epoch_watermark = true;
                    frame_mac = f.mac;
                    dispatched_ok = true;
                    break;
                }
                case DurableRecordType::EndpointWeightConfigSet: {
                    DecodedEndpointWeightConfigFrame f{};
                    const auto st = decode_endpoint_weight_config_frame(view, key, f, frame_size);
                    if (st == FrameDecodeStatus::Truncated) goto torn_tail;
                    if (st != FrameDecodeStatus::Ok) return RecoveryScanStatus::Corrupt;
                    if (f.sequence_number != expected_seq || f.prev_mac != running_prev_mac) {
                        return RecoveryScanStatus::Corrupt;
                    }
                    fold_.weights = f.payload;
                    fold_.has_weights = true;
                    frame_mac = f.mac;
                    dispatched_ok = true;
                    break;
                }
                case DurableRecordType::RateLimitUsageSnapshot: {
                    DecodedRateLimitUsageSnapshotFrame f{};
                    const auto st = decode_rate_limit_usage_snapshot_frame(view, key, f, frame_size);
                    if (st == FrameDecodeStatus::Truncated) goto torn_tail;
                    if (st != FrameDecodeStatus::Ok) return RecoveryScanStatus::Corrupt;
                    if (f.sequence_number != expected_seq || f.prev_mac != running_prev_mac) {
                        return RecoveryScanStatus::Corrupt;
                    }
                    fold_.usage = f.payload;
                    fold_.has_usage = true;
                    frame_mac = f.mac;
                    dispatched_ok = true;
                    break;
                }
                case DurableRecordType::GenerationBridge: {
                    DecodedGenerationBridgeFrame f{};
                    const auto st = decode_generation_bridge_frame(view, key, f, frame_size);
                    if (st == FrameDecodeStatus::Truncated) goto torn_tail;
                    if (st != FrameDecodeStatus::Ok) return RecoveryScanStatus::Corrupt;
                    if (f.sequence_number != expected_seq || f.prev_mac != running_prev_mac) {
                        return RecoveryScanStatus::Corrupt;
                    }
                    fold_.bridge = f.payload;
                    fold_.has_bridge = true;
                    frame_mac = f.mac;
                    dispatched_ok = true;
                    break;
                }
                case DurableRecordType::OperatorOverride: {
                    DecodedOperatorOverrideFrame f{};
                    const auto st = decode_operator_override_frame(view, key, f, frame_size);
                    if (st == FrameDecodeStatus::Truncated) goto torn_tail;
                    if (st != FrameDecodeStatus::Ok) return RecoveryScanStatus::Corrupt;
                    if (f.sequence_number != expected_seq || f.prev_mac != running_prev_mac) {
                        return RecoveryScanStatus::Corrupt;
                    }
                    // Not folded into any out-param -- the 20-arg interface
                    // has no slot for it (see class header / ledger).
                    frame_mac = f.mac;
                    dispatched_ok = true;
                    break;
                }
                case DurableRecordType::SymbolRegistrySnapshot: {
                    DecodedSnapshotFrame f{};
                    std::array<SymbolRules, kMaxSnapshotSymbols> entries{};
                    std::size_t entry_count = 0;
                    const auto st = decode_snapshot_frame(view, key, f, entries, entry_count, frame_size);
                    if (st == FrameDecodeStatus::Truncated) goto torn_tail;
                    if (st != FrameDecodeStatus::Ok) return RecoveryScanStatus::Corrupt;
                    if (f.sequence_number != expected_seq || f.prev_mac != running_prev_mac) {
                        return RecoveryScanStatus::Corrupt;
                    }
                    // Not folded into any out-param of recover_control_plane() (its interface has no slot) -- but, since
                    // D5 / R3-0, into the per-symbol_id name table behind recovered_symbol_name() -- with (R3b-3) the frame's
                    // time read as a wall time, which is 0 for a frame that says there was no trustworthy clock.
                    fold_symbol_names(std::span<const SymbolRules>(entries.data(), entry_count),
                                      frame_wall_time_ms(f.time_kind, f.recorded_utc_ms));
                    frame_mac = f.mac;
                    dispatched_ok = true;
                    break;
                }
                // Never produced by this sink -- seeing one means a future-
                // format log or tampering, same precedent DurableAuditSink
                // uses for record types it never writes. CompactedFreezeWait-
                // Evidence forward-compat decode/fold is deferred (dead code
                // for this concrete class either way, since it never writes
                // one) -- disclosed simplification vs. the original plan.
                case DurableRecordType::TransportFailover:
                case DurableRecordType::RateLimitFreezeSnapshot:
                case DurableRecordType::CompactedFreezeWaitEvidence:
                case DurableRecordType::SealJournalApplied:
                case DurableRecordType::OrderEvent:
                case DurableRecordType::OrderCheckpoint:
                default:
                    return RecoveryScanStatus::Corrupt;
            }

            if (dispatched_ok) {
                running_prev_mac = frame_mac;
                prev_of_last_mac = last_mac;
                last_mac = frame_mac;
                any_frame_seen = true;
                ++expected_seq;
                offset += frame_size;
                continue;
            }

        torn_tail:
            break;
        }

        if (!any_frame_seen) {
            const auto first = finalize_scan_with_anchor_check(/*log_tip_valid=*/false, 0, {});
            if (first != RecoveryScanStatus::Clean && first != RecoveryScanStatus::Recovered) return first;
            // A first frame that never completed leaves no anchor and nothing to lose: cut it, so that the first
            // append starts the file instead of following debris.
            const auto first_cut =
                detail::cut_torn_tail(log_store_, offset, total_size, kMaxSnapshotFrameSize, torn_tail_bytes_);
            return first_cut == RecoveryScanStatus::Clean ? first : first_cut;
        }

        const auto status =
            finalize_scan_with_anchor_check(/*log_tip_valid=*/true, expected_seq - 1, last_mac, prev_of_last_mac);
        if (status != RecoveryScanStatus::Clean && status != RecoveryScanStatus::Recovered) return status;

        // SAFE-03 crash-window slice (blueprint L-36): the scan stopped at `offset`, the end of the last complete
        // frame; anything after it is the partial frame a crash left behind (or nothing). The anchor check above
        // has passed, so the log up to `offset` is proven -- cut the rest, because append_and_fsync() appends at the
        // PHYSICAL end of the file. A log that failed the check, or holds a complete-but-wrong frame, never gets
        // here and is left byte-for-byte as it was.
        const auto cut = detail::cut_torn_tail(log_store_, offset, total_size, kMaxSnapshotFrameSize, torn_tail_bytes_);
        if (cut != RecoveryScanStatus::Clean) return cut;

        next_sequence_ = expected_seq;
        tip_mac_ = last_mac;
        return RecoveryScanStatus::Recovered;
    }

    // Tip-anchor cross-check -- the same decision as DurableAuditSink's own finalize_scan_with_anchor_check
    // (durable_audit_sink.hpp), reused here as duplicated logic (not shared code -- that method is private,
    // non-exported) since it is small and the two classes' recovery loops otherwise differ (this one is streaming,
    // that one is whole-file). The comment used to claim "structurally identical" while this copy lacked the
    // late-fsync window below; the crash-window slice (blueprint L-36) added it, with a test on each side.
    //
    // finish_append() fsyncs the frame and THEN writes the anchor, so a crash between the two leaves the anchor
    // exactly one frame behind a perfectly good frame. That window is legitimate and is accepted -- iff the MAC at
    // the anchor's sequence (the frame before the last, `log_prev_tip_mac`) is what the anchor claims. Any other
    // lag, or a MAC that does not match, stays Corrupt: an anchor further behind is a rolled-back anchor, not a
    // crash.
    RecoveryScanStatus finalize_scan_with_anchor_check(bool log_tip_valid, std::uint64_t log_tip_sequence,
                                                        std::array<std::byte, kMacLen> log_tip_mac,
                                                        std::array<std::byte, kMacLen> log_prev_tip_mac = {}) noexcept {
        std::vector<std::byte> anchor_bytes;
        const bool anchor_present = log_store_.read_tip_anchor(anchor_bytes);

        if (!anchor_present) {
            return log_tip_valid ? RecoveryScanStatus::IoError : RecoveryScanStatus::Clean;
        }
        if (!log_tip_valid) return RecoveryScanStatus::Corrupt;

        std::array<std::byte, kKeyBlockSize> key_block{};
        if (!key_ring_.active_key(active_key_id_, key_block)) return RecoveryScanStatus::Corrupt;

        detail::DecodedTipAnchor anchor{};
        if (!detail::decode_tip_anchor(anchor_bytes, std::span<const std::byte>(key_block.data(), key_block.size()),
                                        anchor)) {
            return RecoveryScanStatus::Corrupt;
        }

        if (anchor.sequence_number > log_tip_sequence) return RecoveryScanStatus::Corrupt;
        if (anchor.sequence_number == log_tip_sequence) {
            return anchor.mac == log_tip_mac ? RecoveryScanStatus::Clean : RecoveryScanStatus::Corrupt;
        }
        if (anchor.sequence_number + 1 == log_tip_sequence && anchor.mac == log_prev_tip_mac) {
            return RecoveryScanStatus::Clean;
        }
        return RecoveryScanStatus::Corrupt;
    }

    DurableLogStore log_store_;
    KeyRing& key_ring_;
    // Phase 4: no longer const -- rotate_active_key() is the one and only
    // place this is ever reassigned, always after durably confirming the
    // rotation (see that method's doc comment for the exact ordering).
    std::uint32_t active_key_id_;

    bool fenced_{false};
    RecoveryScanStatus recovery_status_{RecoveryScanStatus::IoError};
    std::uint64_t next_sequence_{0};
    std::array<std::byte, kMacLen> tip_mac_{};
    std::uint64_t torn_tail_bytes_{0};

    FoldState fold_{};
};

}  // namespace hy
