// SPDX-License-Identifier: proprietary
// durable_control_plane.hpp — ABI surface for the durable audit / control-plane
// log described in docs/BINANCE_PRIVATE_REST_L4_SPEC.md §10 and
// docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md §6.
//
// Governance: L1 (data structures only — no file I/O, no network, no secret).
//
// ---------------------------------------------------------------------------
// THE ONE RULE FOR THIS FILE: TRANSCRIBE, NEVER INVENT.
// ---------------------------------------------------------------------------
// L4 §10's own header comment names this file "the ONLY definition site" for
// the types below. That makes this file a TRANSCRIPTION of the spec, not an
// interpretation of it. A plausible-looking type that does not match the spec
// byte-for-byte is worse than no type at all: it compiles, it passes tests, and
// it silently establishes a second, competing source of truth.
//
// This is not hypothetical. The first version of this header invented three
// types instead of transcribing them, and every one of them passed the full
// 381-test suite and the symbol-level cross-reference check:
//   * DurableRecordType   — 9 invented enumerators with invented numbering,
//                           against the spec's 17. This is a PERSISTED WIRE
//                           DISCRIMINATOR covered by the frame MAC; wrong
//                           numbering silently reinterprets every stored frame.
//   * RecoveryScanStatus  — 4 enumerators against the spec's 6, every value
//                           after Clean shifted.
//   * ExportTuple         — invented a payload-carrying frame; the spec's is a
//                           TIP-ANCHOR record. Near-zero field overlap. A
//                           bounds bug was even "found and fixed" in a payload
//                           buffer that should never have existed.
// tools/spec_enum_diff.py now diffs enumerator names AND values against the
// spec automatically. Every enum below carries a SPEC-ENUM provenance marker
// naming its source line so that tool can find it. When adding a type: copy the
// spec block, add the marker, run the differ. Do not paraphrase.
//
// SCOPE: this is an ABI surface, not an implementation of the durable log. No
// append(), no fsync, no MAC verification, no recovery_scan(), no wire
// (de)serialization. The spec encodes the on-disk format as explicit
// little-endian field-by-field serialization rather than a raw struct memcpy
// (SUBMITPORT rev8 §6.1.1.1), specifically so in-memory and wire layout may
// diverge — so this header deliberately does NOT static_assert sizeof() against
// any of the spec's pinned wire byte counts. Asserting that would pin a
// correspondence the design explicitly does not guarantee. What IS asserted are
// structural properties the spec and CLAUDE.md actually require: trivial
// copyability and standard layout for anything crossing a durable-write or
// cross-thread boundary.
//
// Types in L4 §10's canonical block that are NOT yet transcribed here (they are
// simply absent, which spec_enum_diff.py reports as NOT_PORTED rather than as a
// conflict): SealQueryStatus, FreezeClearKind, FreezeProbePurpose, and the
// payload/wire structs. Transcribe them when a consumer needs them.

#pragma once

#include <hengyuan/order_lifecycle.hpp>
#include <hengyuan/spsc_ring.hpp>  // hy::kCacheLine
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hy {

// --- AuditAppendResult ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2473
//
// Spec round 14's P0: a bare Acked/Failed enum gave the caller no way to learn
// which sequence its own just-written frame received, which several call sites
// require (§6.2's OrderOpenPollFailuresReset::reset_after_seq must equal the
// polled event's own frame sequence; §10.3's GenerationBridgePayload::
// prev_tip_seq is the predecessor's actual last-assigned sequence). Hence a
// struct with a nested Status enum — NOT a top-level enum beside it.

struct AuditAppendResult {
    // SPEC-ENUM: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2474
    enum class Status : std::uint8_t { Acked = 0, Failed = 1 };

    Status status{Status::Failed};
    std::uint64_t sequence{0};  // the frame-sequence number actually assigned
                                // to this write; valid ONLY if status == Acked.
                                // Monotonic within a generation (§10.2).

    bool acked() const noexcept { return status == Status::Acked; }
};
static_assert(std::is_trivially_copyable_v<AuditAppendResult>);

// --- RecoveryScanStatus ---
// SPEC-ENUM: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2486

enum class RecoveryScanStatus : std::uint8_t {
    Clean = 0,
    Recovered = 1,
    Corrupt = 2,
    CapacityExceeded = 3,
    IoError = 4,
    // Round-11 P0: previously named only in prose ("out-of-band"). Must be a
    // real enumerator so recovery can return it and the startup path can
    // require an explicit override (§10.2) rather than inventing a side channel.
    ExternalAnchorUnavailable = 5,
};

// --- DurableRecordType ---
// SPEC-ENUM: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2511
//
// PERSISTED WIRE DISCRIMINATOR, covered by the frame MAC. The numeric values
// are ABI in the strongest sense: renumbering silently reinterprets every frame
// already on disk. Transcribed verbatim; do not reorder, renumber, or "tidy".

enum class DurableRecordType : std::uint8_t {
    // Round-11 P0: OrderEvent MUST be a real enumerator (value 0).
    OrderEvent = 0,
    SymbolRegistrySnapshot = 1,
    RateLimitFreeze = 2,
    TransportFailover = 3,        // §1.1 allowlist failover audit
    EndpointWeightConfigSet = 4,  // §7.4 versioned weight table
    RateLimitUsageSnapshot = 5,   // §7.5.1 clean-shutdown usage baseline
    OperatorOverride = 6,         // round-12 P0 — external-anchor admission
    GenerationBridge = 7,         // round-12 P0 — compaction predecessor seal
    OrderCheckpoint = 8,          // revision-13 self-contained replay establish
    FreezeProbeAttempt = 9,       // round-16 P0 — FreezeProbeCredit's durable,
                                  // recoverable attempt count
    FreezeEpochWatermark = 10,    // round-17 P0 — freeze_epoch's durable,
                                  // monotonic, never-reset high-water mark;
                                  // survives compaction dropping old
                                  // RateLimitFreeze/FreezeProbeAttempt records
    FreezeClear = 11,             // round-22 P0 — typed terminal clear
                                  // (ProbeVerified / ConservativeWaitCompleted /
                                  // OperatorAuthorized); never overload probe
                                  // attempts to clear a permanent fence
    FreezeWaitSatisfied = 12,     // round-25/26 P0 — NON-terminal durable
                                  // evidence that conservative_wait_ms elapsed
                                  // for an epoch; does NOT clear the freeze
    FreezeWaitArm = 13,           // round-26 P0 — NON-terminal arm of a
                                  // conservative wait; WaitSatisfied is illegal
                                  // without a live-session Arm
    RateLimitFreezeSnapshot = 14,  // round-33 P0 — compaction-only FOLDED
                                   // aggregate for an uncleared epoch; must not
                                   // go through append_rate_freeze
    CompactedFreezeWaitEvidence = 15,  // round-33/34 — compaction-only SINGLE
                                       // frame; FORBIDDEN to substitute dual
                                       // FreezeWaitArm/Satisfy frames
    SealJournalApplied = 16,      // round-39…46 — SINGLE-frame seal-journal
                                  // apply record. Index key = {candidate_id,
                                  // journal_seq}; entry_mac MUST be
                                  // HY-SEALJRN-v1-recomputed (not opaque-only)
};

// --- FrameTimeKind ---
// SPEC-ENUM: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2565
// Also defined identically at SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md:1133;
// spec_enum_diff.py cross-checks the two spec copies against each other.
//
// Why it cannot be omitted: recorded_utc_ms alone is ambiguous. A Phase-B
// unknown_time_429 freeze has no trustworthy UTC to stamp, forging one is
// forbidden, and such frames are written with recorded_utc_ms = 0 and excluded
// from every age/TTL computation. Without a provenance tag, "the clock genuinely
// read 0" and "there was no trustworthy clock" are indistinguishable.

enum class FrameTimeKind : std::uint8_t {
    ServerCorrectedUtc = 0,  // recorded_utc_ms = local_utc + published offset
    UnknownBootstrap = 1,    // recorded_utc_ms MUST be 0
};

// --- OrderRecoveryCheckpoint ---
// SPEC-STRUCT: docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md:1289
//
// Introduced SUBMITPORT rev14 (round 13), then hit a self-inflicted circular
// dependency across rev14->rev16: an early version required this payload to
// carry its own not-yet-assigned durable write sequence, which is
// unconstructible under group commit. Fixed by removing that field — the
// checkpoint's sequence is read from the frame header at decode time, never
// from the payload. Do not re-add a self-referential sequence field; that is a
// known, already-reverted bug.

struct OrderRecoveryCheckpoint {
    ClientOrderId client_order_id{};
    OrderState resulting_state{OrderState::Intent};

    std::int64_t intended_price_ticks{0};
    std::int64_t intended_qty_ticks{0};
    std::int64_t filled_qty_ticks{0};
    std::int64_t avg_fill_price_ticks{0};

    std::uint32_t rules_version{0};
    std::uint8_t query_attempts{0};
    std::int64_t last_poll_completed_utc_ms{0};
};
static_assert(std::is_trivially_copyable_v<OrderRecoveryCheckpoint>);
static_assert(std::is_standard_layout_v<OrderRecoveryCheckpoint>);

// --- ExportTuple ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4611
//
// A TIP-ANCHOR record. The outbox exports the durable log's CHAIN TIP to an
// external anchor — it does not carry arbitrary frame payloads. (The first
// version of this header invented a payload[256] buffer here. There is no
// payload. Every field below is transcribed from the spec block.)
//
// `time_kind` is a raw std::uint8_t carrying a FrameTimeKind value, exactly as
// the spec writes it — it is a wire field copied from the frame header, not a
// typed enum member. Transcribed as-is rather than "improved" to FrameTimeKind.

struct ExportTuple {
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint32_t generation{0};
    std::uint64_t sequence{0};
    std::array<std::uint8_t, 32> tip_mac{};
    std::uint32_t key_id{0};
    std::int64_t enqueued_utc_ms{0};  // ServerCorrectedUtc only; 0 when frame
                                      // time_kind is UnknownBootstrap (must not
                                      // participate in age lag).
    std::uint8_t time_kind{0};        // FrameTimeKind; copied from header

    // A tuple stamped UnknownBootstrap carries enqueued_utc_ms == 0 and must be
    // excluded from hard-lag age arithmetic — never treated as "epoch, therefore
    // ancient". Callers doing lag math consult this rather than testing the
    // timestamp for zero.
    bool participates_in_age_computation() const noexcept {
        return time_kind == static_cast<std::uint8_t>(FrameTimeKind::ServerCorrectedUtc);
    }
};
static_assert(std::is_trivially_copyable_v<ExportTuple>,
              "ExportTuple crosses a producer/consumer ring boundary — must stay POD-like (CLAUDE.md zero-heap-allocation)");
static_assert(std::is_standard_layout_v<ExportTuple>);

// --- ExportOutboxRing ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4626
//
// NOT a plain SPSC ring, and deliberately NOT an alias of hy::SpscRing. The
// spec's contract is TWO-PHASE on the consumer side: peek_oldest() reads the
// head entry WITHOUT removing it, and pop_after_remote_ack() advances the head
// only once the external anchor has genuinely Acked that tuple AND
// LastRemoteAckedTip has been durably updated for it. A pop-on-read ring (which
// the first version of this header aliased it to) drops the frame before the
// remote confirms, which is precisely the data loss the two-phase design exists
// to prevent.
//
// Ownership (round 16 P0): the ring is owned SOLELY by the durable sink.
// ExternalAnchorClient is a pure, stateless remote-transport client holding no
// queue, no backlog, and no retry state of its own.
//
// Producer = owner actor thread ONLY. Consumer = export worker thread ONLY.
//
// DEVIATION, documented: the spec writes
// `alignas(std::hardware_destructive_interference_size)`. This uses hy::kCacheLine
// (spsc_ring.hpp, == 64) instead, because GCC 12+ emits -Winterference-size for
// the std constant (its value varies across compiler versions and is therefore
// ABI-fragile) and this project builds with -Werror. spsc_ring.hpp already
// established kCacheLine as the house equivalent. This is an alignment choice
// with no wire/ABI meaning — unlike the field layouts above, which are verbatim.

class ExportOutboxRing {
public:
    static constexpr std::size_t kCapacity = 256;  // = kExternalAnchorHardLagFrames

    ExportOutboxRing() = default;
    ExportOutboxRing(const ExportOutboxRing&) = delete;
    ExportOutboxRing& operator=(const ExportOutboxRing&) = delete;
    ExportOutboxRing(ExportOutboxRing&&) = delete;
    ExportOutboxRing& operator=(ExportOutboxRing&&) = delete;

    // Producer-side (owner actor thread ONLY). Returns false if the ring is at
    // capacity — the caller (append path) treats this as the hard-lag fence,
    // NOT as a reason to drop the tuple.
    bool try_push(const ExportTuple& t) noexcept {
        const std::size_t tail = tail_.value.load(std::memory_order_relaxed);
        const std::size_t head = head_.value.load(std::memory_order_acquire);
        if (tail - head >= kCapacity) return false;  // full → hard-lag fence
        slots_[tail % kCapacity] = t;
        tail_.value.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer-side (export worker thread ONLY). Returns false if empty.
    // Does NOT remove the entry — see pop_after_remote_ack().
    bool peek_oldest(ExportTuple& out) const noexcept {
        const std::size_t head = head_.value.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.value.load(std::memory_order_acquire);
        if (head == tail) return false;  // empty
        out = slots_[head % kCapacity];
        return true;
    }

    // Consumer-side ONLY, called after export_tip_and_wait_bounded() for the
    // peeked tuple returns genuinely Acked AND LastRemoteAckedTip has been
    // durably updated for that tuple. Advances head.
    //
    // Calling this without that remote Ack loses the tuple permanently — the
    // whole point of the peek/pop split. No-op when empty rather than
    // advancing past the producer.
    void pop_after_remote_ack() noexcept {
        const std::size_t head = head_.value.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.value.load(std::memory_order_acquire);
        if (head == tail) return;  // empty — never advance past the producer
        head_.value.store(head + 1, std::memory_order_release);
    }

    // Acquire-loads both indices. Load order matters: head_ is read FIRST so the
    // tail_ observed afterwards can only have grown, guaranteeing tail >= head
    // and therefore no unsigned underflow. Reading tail_ first would let the
    // consumer advance head_ past the observed tail_ between the two loads and
    // return a value near SIZE_MAX. (Producer advances tail_, consumer advances
    // head_ — the opposite of hy::SpscRing's index roles, so the safe load order
    // is the opposite there too.)
    std::size_t size() const noexcept {
        const std::size_t head = head_.value.load(std::memory_order_acquire);
        const std::size_t tail = tail_.value.load(std::memory_order_acquire);
        return tail - head;
    }

    bool empty() const noexcept { return size() == 0; }
    static constexpr std::size_t capacity() noexcept { return kCapacity; }

private:
    struct alignas(kCacheLine) Head {
        std::atomic<std::size_t> value{0};
    } head_;
    struct alignas(kCacheLine) Tail {
        std::atomic<std::size_t> value{0};
    } tail_;
    std::array<ExportTuple, kCapacity> slots_{};  // fixed, no heap
};

}  // namespace hy
