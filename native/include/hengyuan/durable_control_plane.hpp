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
// conflict): DurableControlPlaneSink itself (the 14-pure-virtual-method
// interface -- see docs/SPEC_INVARIANTS.md's durable-audit-log entry for why
// DurableAuditSink deliberately does NOT inherit from it), and everything on
// the compaction/generation-switch/seal-journal side: SymbolRegistrySnapshotPayload,
// RateLimitUsageSnapshotPayload, OperatorOverridePayload, GenerationBridgePayload,
// GenerationSeal, SealIdWatermark, SealJournalCommitWatermark, the whole
// Seal*Wire/CompactionCandidateIntentWire/.xgc family, ExternalAnchorClient,
// OperatorOverrideSidecar. Transcribe them when a consumer needs them.

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

// --- SealQueryStatus ---
// SPEC-ENUM: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2501
//
// Round-39 P0: MUST NOT reuse RecoveryScanStatus for seal-by-request-id
// queries -- that enum has no Found and conflates transport unavailability
// with "record genuinely absent." Abandon (giving up and treating a seal as
// never-completed) is legal only on the authenticated NotFound below, never
// on TransportUnavailable.

enum class SealQueryStatus : std::uint8_t {
    Found = 0,                 // remote durably holds this request_id; out filled
    NotFound = 1,               // authenticated negative: remote asserts this
                                 // request_id will never become durable (signed /
                                 // quorum-negative). NOT a transport miss.
    TransportUnavailable = 2,   // cannot confirm presence (lag, not indexed,
                                 // timeout, unreachable) -- keep SealExportStarted
    Corrupt = 3,                // MAC fail / equivocation / bind mismatch
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

// =============================================================================
// Freeze-episode payload types (docs/SPEC_INVARIANTS.md's "durable 审计日志" /
// "崩溃恢复 / Freeze 子系统" entries). These are the durable ABI backing the
// three already-documented behavioral invariants FreezeProbeCredit/wait_ok/
// wait_generation -- this block adds no new behavioral claim, only gives those
// invariants a type to point at. Deliberately excludes DurableControlPlaneSink
// itself and everything on the compaction/generation-switch/seal-journal side
// (SymbolRegistrySnapshotPayload, RateLimitUsageSnapshotPayload,
// OperatorOverridePayload, GenerationBridgePayload, GenerationSeal, and the
// whole .xgc/Seal*Wire family) -- see this round's SPEC_INVARIANTS.md entry
// for the scope decision.
// =============================================================================

// --- FreezeClearKind ---
// SPEC-ENUM: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2570

enum class FreezeClearKind : std::uint8_t {
    ProbeVerified = 0,              // /time past UTC deadline; not for source=2
    ConservativeWaitCompleted = 1,  // unknown-time wait done; not for source=2
    OperatorAuthorized = 2,         // ONLY legal clear for source=2
};

// --- RateLimitFreezePayload ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2576
//
// Round-23/31: BOTH deadline_utc_ms and conservative_wait_ms may be non-zero
// across an episode after merges (UTC copy-forward + wait contributions).
// Recovery / the live owner fold max(deadline), max(wait), max(wait_generation)
// across ALL frames of the epoch -- never latest frame alone. Per-frame
// conservative_wait_ms is THIS EVENT's wait contribution (0 if none), not a
// restated running max. Permanent source=2: timed fields ignored for latching.
// Never wall-clock-invent deadline_utc_ms (spec §7.0).
//
// wait_generation (round-31 P0): advanced by +1 iff THIS frame's
// conservative_wait_ms contribution > 0 (incoming event wait, not a restated
// max), even when the folded max duration is unchanged; 0 contribution copies
// forward the prior folded generation. Arm/WaitSatisfied/probe/clear all bind
// this value; stale-generation evidence is rejected (this is the durable
// backing for docs/SPEC_INVARIANTS.md's wait_generation entry, round 31/32).
//
// pad[3] is transcribed verbatim from the spec text (BINANCE:2591), not a
// padding byte this header invented -- this file otherwise never hand-computes
// alignment (see the file-header note on why sizeof() is never asserted here).

struct RateLimitFreezePayload {
    std::int64_t recorded_utc_ms{0};       // legacy payload mirror; prefer header
    std::int64_t deadline_utc_ms{0};       // absolute UTC; 0 = absent (not "now")
    std::int64_t conservative_wait_ms{0};  // THIS frame's wait contribution
    std::uint8_t source{0};                // 0=429 timed, 1=418 timed, 2=permanent,
                                            // 3=unknown-time-429 (missing Retry-After)
    std::uint8_t pad[3]{};
    std::uint32_t freeze_epoch{0};
    std::uint32_t wait_generation{0};
};
static_assert(std::is_trivially_copyable_v<RateLimitFreezePayload>);
static_assert(std::is_standard_layout_v<RateLimitFreezePayload>);

// --- FreezeProbePurpose ---
// SPEC-ENUM: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2616
//
// Round-28/29: EVERY freeze-period /time probe uses this type -- no separate
// TimeResyncCredit while frozen. ClockRepublishOrVerify may ProbeVerified-clear
// on the same response when serverTime >= deadline (round-29 P0 withdrew an
// earlier "republish never clears" rule).

enum class FreezeProbePurpose : std::uint8_t {
    DeadlineOrVerify = 0,        // now_utc known; probing toward/past not_before
    ClockRepublishOrVerify = 1,  // now_utc absent at reserve; burns the same
                                  // 8-attempt cap; MAY ProbeVerified-clear if
                                  // serverTime >= deadline (+ WaitSatisfied
                                  // when required)
};

// --- FreezeProbeAttemptPayload ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2623
//
// Attempt tracking only (round-22); terminal clears use FreezeClearPayload.
// Round-26 P0: Ack of this record is the linearization point that consumes
// probe budget -- the /time send is illegal until Acked (this is
// SendImpliesDurablePrepared's own pattern applied to the freeze subsystem).
// This is the durable backing for docs/SPEC_INVARIANTS.md's FreezeProbeCredit
// entry (8-attempt cap, exponential backoff capped at 5 minutes).

struct FreezeProbeAttemptPayload {
    std::uint32_t freeze_epoch{0};
    std::uint32_t attempt_ordinal{0};  // 1-based within this freeze_epoch;
                                        // sink requires == durable_max + 1
    bool cleared{false};               // MUST be false on append -- sink
                                        // rejects true. A legacy true does not
                                        // clear source=2; see FreezeClearPayload.
    FreezeProbePurpose purpose{FreezeProbePurpose::DeadlineOrVerify};
    std::int64_t not_before_utc_ms{0}; // durable not-before for the NEXT probe
                                        // send; round-27 P0 -- sink rejects a
                                        // not_before strictly below the folded
                                        // deadline when deadline > 0 (no early
                                        // schedule)
};
static_assert(std::is_trivially_copyable_v<FreezeProbeAttemptPayload>);
static_assert(std::is_standard_layout_v<FreezeProbeAttemptPayload>);

// --- FreezeTimeProbeProof ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2694
//
// Round-23 P0: MAC-covered /time proof required for ProbeVerified clears.
// This is a LOCAL, tamper-evident attestation, not third-party proof of
// Binance's identity (Binance does not sign /time responses) -- its actual
// guarantee is "this record cannot be forged or altered after being durably
// written without also controlling the tip/KEK key," same as every other
// durable frame in this design. Round-26 P1: the MAC domain is extended
// beyond the raw response body to bind the full request/response context
// (freeze_epoch + bound_deadline_utc_ms bind the proof to the exact episode
// it clears; request_nonce is fresh per probe attempt and prevents replaying
// the same response's proof twice; tls_verified_host ties the proof to the
// already-verified transport-level host identity, spec §8) -- canonical MAC
// input: "HY-FREEZE-TIME-PROOF-v1" || freeze_epoch || bound_deadline_utc_ms ||
// request_nonce || server_time_ms || clock_snapshot_seq || clock_offset_ms ||
// tls_verified_host. clock_snapshot_seq/clock_offset_ms are captured inline,
// synchronously, at proof-build time -- the sink's verification is INTERNAL
// CONSISTENCY only (recomputes server_time_ms against clock_offset_ms), never
// a lookup against a retained history of past snapshots.

struct FreezeTimeProbeProof {
    std::int64_t server_time_ms{0};         // Binance serverTime from /time
    std::int64_t bound_deadline_utc_ms{0};  // MUST equal folded active
                                             // deadline_utc_ms; clear iff
                                             // server_time_ms >= this
    std::uint32_t clock_snapshot_seq{0};    // audit-only; not independently
                                             // re-verified against a registry
    std::int64_t clock_offset_ms{0};        // used for the sink's internal
                                             // consistency recomputation
    std::uint64_t request_nonce{0};         // fresh per probe attempt; binds
                                             // the proof to this ONE attempt
    static constexpr std::size_t kTlsVerifiedHostMax = 64;  // includes NUL
    char tls_verified_host[kTlsVerifiedHostMax]{};  // required (round-26 P1);
                                                     // must be non-empty and
                                                     // allowlisted
    std::uint8_t time_response_mac[32]{};   // covers the extended canonical
                                             // input above, not just the
                                             // response body
};
static_assert(std::is_trivially_copyable_v<FreezeTimeProbeProof>);
static_assert(std::is_standard_layout_v<FreezeTimeProbeProof>);

// --- FreezeClearPayload ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2715
//
// Round-22/23: the sole terminal-clear record (3 kinds via FreezeClearKind).
// For ProbeVerified / ConservativeWaitCompleted with wait>0,
// bound_conservative_wait_ms is INFORMATIONAL ONLY -- the sink requires a
// prior sink-verified FreezeWaitSatisfiedPayload for the current
// wait_generation (round-25/26/31), never this field's equality alone. This
// is the durable backing for docs/SPEC_INVARIANTS.md's wait_ok entry (round
// 30: sink must independently re-verify, never trust a caller-supplied bool).

struct FreezeClearPayload {
    std::uint32_t freeze_epoch{0};
    FreezeClearKind clear_kind{FreezeClearKind::ProbeVerified};
    std::uint8_t pad[3]{};
    FreezeTimeProbeProof time_proof{};  // ProbeVerified: required, zeros illegal
    std::int64_t bound_conservative_wait_ms{0};  // ConservativeWaitCompleted:
                                                  // required bind, zeros illegal
    std::uint32_t bound_wait_generation{0};  // round-31: when wait>0 MUST equal
                                              // folded wait_generation; sink
                                              // rejects mismatch. 0 when wait==0.
    // OperatorAuthorized fields (ignored/zero for the other two kinds):
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint32_t bound_generation{0};
    std::uint64_t bound_tip_seq{0};
    std::uint8_t bound_tip_mac[32]{};
    std::int64_t wall_utc_ms{0};
    std::int64_t expires_utc_ms{0};
    std::uint64_t nonce{0};
    std::uint32_t kek_key_id{0};
    char operator_id[32]{};
    std::uint8_t mac[32]{};
};
static_assert(std::is_trivially_copyable_v<FreezeClearPayload>);
static_assert(std::is_standard_layout_v<FreezeClearPayload>);

// --- FreezeWaitArmPayload ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2747
//
// Round-26/31 P0: NON-terminal arm of a conservative wait for an epoch,
// written BEFORE waiting begins. The sink records arm_ack_steady
// (process-local) at Ack; this does NOT lift can_send*. Abandoned across a
// process restart if no matching FreezeWaitSatisfiedPayload was Acked (a
// live sink has no arm_ack_steady for a prior session's Arm).

struct FreezeWaitArmPayload {
    std::uint32_t freeze_epoch{0};
    std::int64_t bound_conservative_wait_ms{0};  // == folded wait at arm time
    std::uint32_t wait_generation{0};            // MUST == folded wait_generation
    std::uint32_t arm_ordinal{1};                // 1-based within this generation
};
static_assert(std::is_trivially_copyable_v<FreezeWaitArmPayload>);
static_assert(std::is_standard_layout_v<FreezeWaitArmPayload>);

// --- FreezeWaitSatisfiedPayload ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2760
//
// Round-25/26/31 P0: NON-terminal wait-completion evidence for an epoch; does
// NOT lift can_send*. A ProbeVerified or ConservativeWaitCompleted (wait>0)
// clear must find a matching Acked record for the CURRENT wait_generation.
// The sink MUST independently verify elapsed steady time since the cited
// Arm's Ack in THIS process session -- duration equality alone is never
// sufficient (an equal-wait rematch must re-Arm under a new generation). This
// is the durable backing for docs/SPEC_INVARIANTS.md's wait_generation entry
// (round 31/32: cross-generation replay of a stale WaitSatisfied is illegal).

struct FreezeWaitSatisfiedPayload {
    std::uint32_t freeze_epoch{0};
    std::int64_t bound_conservative_wait_ms{0};  // == folded wait at write time
    std::uint32_t wait_generation{0};            // MUST == Arm and folded gen
    std::uint32_t satisfaction_ordinal{1};       // 1-based within this generation
    std::uint32_t arm_ordinal{0};                // MUST match an Acked Arm
    std::uint64_t arm_frame_seq{0};              // AuditAppendResult::sequence
                                                  // of the matching Arm
    std::int64_t elapsed_steady_ms_claimed{0};   // MUST be >= bound; sink
                                                  // checks its own elapsed
                                                  // independently, never trusts
                                                  // this value alone
};
static_assert(std::is_trivially_copyable_v<FreezeWaitSatisfiedPayload>);
static_assert(std::is_standard_layout_v<FreezeWaitSatisfiedPayload>);

// --- CompactedFreezeWaitEvidencePayload ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2779
//
// Round-34/36 P1: the SOLE on-disk shape for
// DurableRecordType::CompactedFreezeWaitEvidence -- exactly one frame;
// implementations MUST NOT emit a separate FreezeWaitArmPayload +
// FreezeWaitSatisfiedPayload pair as a compaction substitute (recovery /
// content-root would diverge). Does NOT create arm_ack_steady; does NOT
// re-check live elapsed time.

struct CompactedFreezeWaitEvidencePayload {
    std::uint32_t freeze_epoch{0};
    std::uint32_t wait_generation{0};            // explicit; MUST be > 0
    std::int64_t bound_conservative_wait_ms{0};  // == folded wait
    // Embedded Arm (rewritten/retained values -- not a second record type):
    std::uint32_t arm_ordinal{1};
    std::uint64_t source_arm_seq{0};   // Arm's sequence in source generation N
    // Embedded Satisfy:
    std::uint32_t satisfaction_ordinal{1};
    std::uint64_t source_satisfy_seq{0};      // Satisfy's sequence in source gen N
    std::uint64_t satisfy_arm_frame_seq{0};   // MUST == source_arm_seq
    std::int64_t elapsed_steady_ms_claimed{0};  // copied; not re-validated live
    // Source-generation bind (must match CompactionSourceBaseline / proof):
    std::uint32_t source_generation{0};
    std::uint64_t source_baseline_tip_seq{0};
    std::uint8_t source_baseline_tip_mac[32]{};
    std::uint32_t source_baseline_key_id{0};   // round-36: key that signs
                                                // source_baseline_tip_mac; MUST
                                                // == CompactionSourceBaseline.key_id
                                                // == GenerationBridge.prev_key_id
    std::uint8_t legacy_sequence_proven_rewrite{0};  // 1 if path-3 rewrite
};
static_assert(std::is_trivially_copyable_v<CompactedFreezeWaitEvidencePayload>);
static_assert(std::is_standard_layout_v<CompactedFreezeWaitEvidencePayload>);

// --- FreezeEpochWatermarkPayload ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2810
//
// Durable high-water mark for freeze_epoch. Stores the NEXT allocatable
// epoch: using epoch E is legal only after Ack of a watermark with
// next_freeze_epoch == E+1 (round-21 P0 -- the field names the next UNUSED
// value, not the epoch being consumed). Never derived by scanning
// RateLimitFreeze/FreezeProbeAttempt records (compaction may drop resolved
// episodes). Round-22: the watermark advances ONLY when starting a new
// episode (no active uncleared epoch); merges reuse the active epoch and do
// not touch the watermark.

struct FreezeEpochWatermarkPayload {
    std::uint32_t next_freeze_epoch{1};  // next allocatable epoch; starts at 1
                                          // (0 reserved "never used"); monotonic;
                                          // never decreases/resets across
                                          // restart or compaction
};
static_assert(std::is_trivially_copyable_v<FreezeEpochWatermarkPayload>);
static_assert(std::is_standard_layout_v<FreezeEpochWatermarkPayload>);

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

    // Additive beyond the spec's current flat shape at the line cited above
    // (docs/SPEC_INVARIANTS.md's "durable 审计日志" entry): AuditRecord already
    // carries both, and OrderRecord needs both restored for correctness (order
    // book/exposure accounting needs symbol_id; a resolved order's exchange
    // identity needs exchange_order_id), not just diagnostics. Deliberately NOT
    // chasing the spec text's other proposed fields (side, order_type, a
    // rules_snapshot-at-submit SymbolRules join, an escalated-ledger array) --
    // those pull in symbol-registry-snapshot recovery, out of scope for the
    // minimal durable-log slice this struct currently serves.
    std::uint32_t symbol_id{0};
    std::int64_t exchange_order_id{0};
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
