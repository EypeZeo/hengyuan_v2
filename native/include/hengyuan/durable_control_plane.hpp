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
// This file's second governance mode, added when it first gained an abstract
// class (DurableControlPlaneSink/ExternalAnchorClient/OperatorOverrideSidecar,
// below): interfaces get NO sizeof()/is_trivially_copyable_v/is_standard_layout_v
// assertions (meaningless for a type with a vtable) -- instead
// static_assert(std::is_abstract_v<T>) (this type cannot be instantiated
// because it has >=1 pure virtual -- the interface analog of "layout matches
// the spec") and static_assert(std::has_virtual_destructor_v<T>) (safe deletion
// through a base pointer -- the one property every caller of these three
// classes actually depends on). "No behavior" means every method body is `= 0`;
// no default implementations, no data members, no helper methods with bodies.
// tools/spec_enum_diff.py has no method-signature-diff equivalent -- it only
// parses `enum class` blocks -- so it says nothing at all about whether these
// interfaces match the spec byte-for-byte; the compensating check is a
// hand-written stub subclass per interface in the test file that overrides
// every pure virtual (a signature mismatch there is a compile error, the same
// proof-of-shape role is_trivially_copyable_v plays for a struct).
//
// Every type L4 §10 names is now transcribed -- the 3-round seal-journal
// split (Round A: id/journal-housekeeping cluster + SealJournalAppliedView's
// real definition; Round B: the Started quintet; Round C: the
// CompactionCandidateIntent GC family) is complete. See the three "Seal-
// journal Round A/B/C" banner comments below (above GenerationSeal's
// dependents, above SealJournalOriginKey, and above SealJournalOriginKey
// again respectively) and docs/SPEC_INVARIANTS.md's matching ledger entries.
//
// This is declaration-only completeness, not implementation completeness --
// see this file's SCOPE paragraph above and the "Seal-journal Round C" ledger
// entry's explicit scope boundary: no type in this file has encode/decode,
// filesystem publish, MAC verification, a recovery state machine, or a
// concurrency/ownership protocol. The one thing genuinely NOT transcribed
// here, and permanently so: the crash-window table and §10.1/10.2/10.3
// procedural prose (BINANCE_PRIVATE_REST_L4_SPEC.md:5080-5327) -- zero new
// named types, pure behavioral text, same treatment as
// DurableControlPlaneSink's un-implemented method bodies.

#pragma once

#include <hengyuan/account_truth.hpp>  // SymbolRules -- append_snapshot's payload
#include <hengyuan/order_lifecycle.hpp>
#include <hengyuan/spsc_ring.hpp>  // hy::kCacheLine
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
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
    // Phase 4 (docs/SPEC_INVARIANTS.md): deliberately NOT extended with a
    // KeyRotated value here. This enum is a closed, spec-transcribed,
    // persisted wire discriminator (docs/BINANCE_PRIVATE_REST_L4_SPEC.md:
    // 2511-2561, ending at SealJournalApplied=16) -- tools/spec_enum_diff.py
    // treats any code enumerator absent from that transcription as an
    // unconditional NAME_CONFLICT, with no delta/allowlist mechanism (unlike
    // e.g. OrchestratorGate's spec block, which the spec itself marks as a
    // "... existing N values unchanged ..." delta listing new proposed
    // values). KeyRotated has no spec basis anywhere, so it lives as its own
    // bespoke, non-DurableRecordType wire record instead -- see
    // control_plane_frame_codec.hpp's KeyRotatedRecord section.
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

// --- KeyRotationPayload ---
// Phase 4 (docs/SPEC_INVARIANTS.md) -- NOT a spec-transcribed struct (neither
// spec doc defines a runtime key-rotation record; this is the one place this
// round genuinely invents wire format, done deliberately and disclosed here
// rather than dressed up as a transcription). Durable evidence of a live
// active_key_id_ rotation on DurableAuditSink (own sidecar log) or
// ControlPlaneLogSink (own main log). Carried by control_plane_frame_codec.hpp's
// bespoke KeyRotatedRecord frame -- deliberately NOT a DurableRecordType
// member (that enum is closed/spec-transcribed; see KeyRotatedRecord's own
// header comment for why). The frame carrying this payload is
// always signed under new_key_id (already verified resolvable in KeyRing at
// the point this is written); old_key_id travels as authenticated payload
// data instead -- same convention durable_control_plane.hpp's own
// GenerationBridgePayload already established for prev_key_id/new_key_id, not
// a new rule invented for this struct.
struct KeyRotationPayload {
    std::uint32_t old_key_id{0};
    std::uint32_t new_key_id{0};
    // The rotated log's own next_sequence_-1 at the moment of rotation (the
    // tip sequence re-anchored under new_key_id). kNoPriorTipSequence
    // (durable_control_plane.hpp, defined alongside this struct) means the
    // rotation happened on a log that had never written a frame yet.
    std::uint64_t log_sequence_at_rotation{0};
};
static_assert(std::is_trivially_copyable_v<KeyRotationPayload>);
static_assert(std::is_standard_layout_v<KeyRotationPayload>);

inline constexpr std::uint64_t kNoPriorTipSequence = std::numeric_limits<std::uint64_t>::max();

// ===========================================================================
// DurableControlPlaneSink dependency structs (轨道 C) -- the 6 payload types
// the sink interface's method signatures require, transcribed together with
// (and immediately before) the interface itself below. Ordering follows spec
// line order among these six; EndpointWeightConfig is the one exception,
// grouped here with its five §10 siblings (all seven are exclusively consumed
// by the interfaces added in this same round) rather than placed at global
// spec-line position, which would be BINANCE_PRIVATE_REST_L4_SPEC.md:2339 --
// far ahead of AuditAppendResult at the top of this file and 500+ lines away
// from the group it belongs with for a reason.
// ===========================================================================

// --- EndpointWeightConfig ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2339
//
// §7.4 versioned runtime weight table -- starts as a pinned default; may be
// replaced only by an operator-loaded, checksummed config whose
// config_version is audited. Online Binance weight changes are NOT
// auto-scraped from undocumented fields.

struct EndpointWeightConfig {
    std::uint32_t config_version{0};
    std::uint32_t weights[6]{};
    std::uint32_t safety_pad{0};
};
static_assert(std::is_trivially_copyable_v<EndpointWeightConfig>);
static_assert(std::is_standard_layout_v<EndpointWeightConfig>);

// --- SymbolRegistrySnapshotPayload ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2880

struct SymbolRegistrySnapshotPayload {
    std::int64_t timestamp_ms{0};
    std::uint32_t rules_version{0};
    std::uint32_t symbol_count{0};  // <= kMaxSymbols; followed by symbol_count
                                     // SymbolRules entries in the same frame
};
static_assert(std::is_trivially_copyable_v<SymbolRegistrySnapshotPayload>);
static_assert(std::is_standard_layout_v<SymbolRegistrySnapshotPayload>);

// --- RateLimitUsageSnapshotPayload ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2889
//
// §7.5.1 -- per-tracker/per-interval usage at clean shutdown. Fixed capacity,
// no heap: at most kMaxUsageEntries (weight intervals + raw intervals) rows.

struct RateLimitUsageSnapshotPayload {
    std::int64_t recorded_utc_ms{0};
    std::uint32_t entry_count{0};   // <= kMaxUsageEntries (= 8)
    struct Entry {
        std::uint8_t tracker{0};    // 0=REQUEST_WEIGHT, 1=RAW_REQUESTS
        char interval_suffix[7]{};  // "1M", "5M", ...
        std::int64_t bucket_start_server_ms{0};
        std::uint32_t used{0};
    } entries[8]{};
};
static_assert(std::is_trivially_copyable_v<RateLimitUsageSnapshotPayload>);
static_assert(std::is_standard_layout_v<RateLimitUsageSnapshotPayload>);

// --- OperatorOverridePayload ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2903
//
// Round-12 P0 -- evidence that an operator authorized startup despite
// ExternalAnchorUnavailable. Written to the *sidecar* path (§10.2), not
// into a store that recovery has already refused.

struct OperatorOverridePayload {
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint64_t local_tip_seq{0};
    std::uint8_t local_tip_mac[32]{}; // binds the exact tip bytes, not seq alone
    std::uint32_t local_generation{0};
    // Round-19 P0 -- bind the durable export baseline this override authorizes
    // against. A new override MUST copy the on-disk LastRemoteAckedTip (or
    // zeros if never exported); it must NOT invent a "fresh empty backlog."
    std::uint32_t last_remote_acked_generation{0};
    std::uint64_t last_remote_acked_seq{0};
    std::uint8_t last_remote_acked_mac[32]{};
    std::uint8_t admit_mode{0};       // 0=AppendAllowedIfUnderHardLag,
                                        // 1=ReadOnlyDrain (forced when
                                        // inherited backlog already exceeds
                                        // hard-lag at issue time)
    std::int64_t wall_utc_ms{0};
    std::int64_t expires_utc_ms{0};  // short-lived, operator-selected
    std::uint64_t nonce{0};          // one-shot replay protection
    std::uint32_t kek_key_id{0};
    std::uint32_t reason_code{0};     // enumerated: ExternalAnchorDown=1, ...
    char operator_id[32]{};           // fixed, not heap
    std::uint8_t mac[32]{};           // HMAC under KEK over the above fields
};
static_assert(std::is_trivially_copyable_v<OperatorOverridePayload>);
static_assert(std::is_standard_layout_v<OperatorOverridePayload>);

// --- GenerationBridgePayload ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2940
//
// Round-12 P0 -- compaction predecessor seal bridging generation N -> N+1.
// CHANGED -- fixes that round's P0: bridge_mac was described as "under the
// tip key" with no field saying WHICH key -- the predecessor generation's
// last-used key_id and the new generation's key_id are not guaranteed to be
// the same. Both prev_key_id and new_key_id are explicit, MAC'd fields, and
// the MAC domain is pinned to new_key_id (the bridge frame lives IN the new
// generation) -- never "whichever key is current now."

struct GenerationBridgePayload {
    std::uint32_t prev_generation{0};
    std::uint64_t prev_tip_seq{0};
    std::uint8_t prev_tip_mac[32]{};
    std::uint32_t prev_key_id{0};       // the key_id prev_tip_mac was actually
                                          // computed under; verifying
                                          // prev_tip_mac uses THIS key, never
                                          // "whichever key is current now"
    std::uint32_t new_generation{0};
    std::uint64_t new_genesis_seq{0};  // usually 0
    std::uint32_t new_key_id{0};        // the key_id this bridge frame ITSELF
                                          // (and the new generation's
                                          // subsequent frames) is written under
    std::uint8_t bridge_mac[32]{};     // HMAC(new_key_id, "HY-GENBRIDGE-v1" ||
                                          // prev_generation || prev_tip_seq ||
                                          // prev_tip_mac || prev_key_id ||
                                          // new_generation || new_genesis_seq),
                                          // canonical declaration-order
                                          // little-endian encoding matching
                                          // §10.2's GenerationSeal convention.
                                          // Always keyed by new_key_id;
                                          // prev_key_id is carried as
                                          // authenticated DATA (covered by
                                          // bridge_mac) so a verifier who
                                          // already trusts new_key_id can look
                                          // up prev_key_id from the verified
                                          // payload itself, rather than
                                          // needing to already know it
                                          // out-of-band.
};
static_assert(std::is_trivially_copyable_v<GenerationBridgePayload>);
static_assert(std::is_standard_layout_v<GenerationBridgePayload>);

// --- GenerationSeal ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2973

struct GenerationSeal {
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    GenerationBridgePayload bridge{};
    std::uint64_t final_seq{0};
    std::uint8_t final_tip_mac[32]{};
    std::uint32_t key_id{0};
    std::uint8_t content_root[32]{}; // SHA-256 root -- see §10.2 point 4
                                       // content-root rule
    // Round-37/39 -- idempotency key. MUST equal SealExportStarted.request_id.
    // Allocated only via SealIdWatermark (§10.1); never a free-running RNG /
    // wall-clock / reused counter across restart.
    std::uint64_t request_id{0};
    // Round-38 -- normative MAC domain (little-endian field order, no
    // padding): HMAC(key_id, "HY-GENSEAL-v1" || store_uuid_lo ||
    //   store_uuid_hi || bridge_mac || final_seq || final_tip_mac || key_id ||
    //   content_root || request_id)
    // where bridge_mac is GenerationBridgePayload::bridge_mac (already covers
    // the prev/new bridge fields). key_id in the domain is this seal's key_id.
    std::uint8_t mac[32]{};
};
static_assert(std::is_trivially_copyable_v<GenerationSeal>);
static_assert(std::is_standard_layout_v<GenerationSeal>);

// ===========================================================================
// Seal-journal Round A (轨道 C, round 1 of 3) -- the id/journal-housekeeping
// cluster of the seal-journal/.xgc family, plus SealJournalAppliedView's real
// definition (previously forward-declared incomplete, see the comment that
// used to sit just above DurableControlPlaneSink -- deleted this round).
// Round B and Round C are both DONE -- see the "Seal-journal Round B" banner
// further below (right after SealJournalTombstoneWire) and the "Seal-journal
// Round C" banner further still (right after SealStartedAbandonWire). That
// completes the 3-round seal-journal split -- L4 §10's type-level surface is
// now fully transcribed. The crash-window table and §10.1/10.2/10.3
// procedural prose (BINANCE_PRIVATE_REST_L4_SPEC.md:5080-5327) contain zero
// new named types -- pure behavioral text, same treatment as
// DurableControlPlaneSink's un-implemented method bodies; not transcribed,
// not even as anchoring comments (would reference types that don't exist
// yet), and this exclusion is permanent, not a future round.
// ===========================================================================

// --- SealIdWatermark ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3009
//
// Durable high-water for seal ids (breadcrumb, outside store). next_* is the
// NEXT allocatable value (like FreezeEpochWatermark) -- never decreases,
// never reuses.

struct SealIdWatermark {
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint64_t next_candidate_id{1};
    std::uint64_t next_request_id{1};
    // HMAC(KEK, "HY-SEALIDWM-v1" || store_uuid_lo || store_uuid_hi ||
    //   next_candidate_id || next_request_id)
    std::uint8_t mac[32]{};
};
static_assert(std::is_trivially_copyable_v<SealIdWatermark>);
static_assert(std::is_standard_layout_v<SealIdWatermark>);

// --- SealJournalCommitWatermark ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3028
//
// Per-candidate journal commit high-water (breadcrumb directory). File:
// seal-journal/<store_uuid...>/<candidate_id_hex16>.jhw. Sole authoritative
// source for next journal_seq (= highest + 1). Advanced ONLY after the final
// .sj1 for that seq is durable; monotonic.

struct SealJournalCommitWatermark {
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint64_t candidate_id{0};
    std::uint64_t highest_committed_journal_seq{0};  // 0 = none yet
    std::uint32_t kek_key_id{0};
    // HMAC(KEK[kek_key_id], "HY-SEALJRNHW-v1" || store_uuid_lo ||
    //   store_uuid_hi || candidate_id || highest_committed_journal_seq ||
    //   kek_key_id)
    std::uint8_t mac[32]{};
};
static_assert(std::is_trivially_copyable_v<SealJournalCommitWatermark>);
static_assert(std::is_standard_layout_v<SealJournalCommitWatermark>);

// --- Seal-journal intake-close constants/alias ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3050
//
// kMaxSealHandoffProducers is the ARRAY CAPACITY (<=8), NOT the wait set (the
// wait set is SealJournalIntakeCloseControl::registered_producer_mask,
// below). mask is uint8_t => capacity MUST stay <=8 (round-54 P1).

constexpr std::size_t kMaxSealHandoffProducers = 8;
static_assert(kMaxSealHandoffProducers <= 8,
              "registered_producer_mask is uint8_t; raise mask width before kMax");
// Default close budget (owner may tighten); process-kill / hung producer must
// not busy-spin forever (round-53 P1).
constexpr std::uint64_t kSealJournalIntakeCloseDeadlineMs = 5'000;
constexpr std::uint32_t kSealJournalIntakeCloseMaxPollIters = 1'000'000;
using SealHandoffRingId = std::uint32_t;  // compile-time / config ring identity

// --- SealJournalIntakeCloseProducerSlot / SealJournalIntakeCloseControl ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3058 / 3065
//
// Linearizable PostSeal handoff intake-close (RAM control block; NOT a
// durable breadcrumb -- after process restart, close_epoch resets to 0 and
// the owner MUST re-run the full protocol before GC A; never trust a prior
// process's quiesce). One instance per active candidate_id. Fixed-size, NO
// heap; atomics cacheline-aligned (CLAUDE.md false-sharing rule).
//
// Second occurrence in this file of an atomic-bearing RAM-only type (first is
// ExportOutboxRing, above) -- NOT is_trivially_copyable_v. Governance mirrors
// ExportOutboxRing exactly: struct (not class -- spec declares zero method
// bodies for either type, unlike ExportOutboxRing's real try_push/
// peek_oldest/etc.), explicit `= default` constructor + four `= delete`
// special members (deleting the copy constructor alone would otherwise
// suppress the implicit default constructor -- the explicit `= default` is
// load-bearing, not decorative), and hy::kCacheLine in place of the spec's
// std::hardware_destructive_interference_size (see ExportOutboxRing's own
// deviation comment above for the GCC -Winterference-size/-Werror reason;
// not repeated here to avoid two copies drifting apart).

struct alignas(kCacheLine) SealJournalIntakeCloseProducerSlot {
    SealJournalIntakeCloseProducerSlot() = default;
    SealJournalIntakeCloseProducerSlot(const SealJournalIntakeCloseProducerSlot&) = delete;
    SealJournalIntakeCloseProducerSlot& operator=(const SealJournalIntakeCloseProducerSlot&) = delete;
    SealJournalIntakeCloseProducerSlot(SealJournalIntakeCloseProducerSlot&&) = delete;
    SealJournalIntakeCloseProducerSlot& operator=(SealJournalIntakeCloseProducerSlot&&) = delete;

    // Written by producer i with release after it has: observed close_epoch==E,
    // refused new try_push for E, and finished any admit that already incremented
    // in_flight_admit_guard. 0 = not quiesced for current epoch.
    std::atomic<std::uint64_t> quiesced_ack_epoch{0};
};

struct SealJournalIntakeCloseControl {
    SealJournalIntakeCloseControl() = default;
    SealJournalIntakeCloseControl(const SealJournalIntakeCloseControl&) = delete;
    SealJournalIntakeCloseControl& operator=(const SealJournalIntakeCloseControl&) = delete;
    SealJournalIntakeCloseControl(SealJournalIntakeCloseControl&&) = delete;
    SealJournalIntakeCloseControl& operator=(SealJournalIntakeCloseControl&&) = delete;

    // Frozen BEFORE SealExportStarted / PostSeal handoff start (single-writer
    // owner). Immutable until drain-complete. popcount(mask)==producer_count;
    // bit i set => producers[i] + ring_id[i] are in the close wait set.
    // Round-54: same tuple is MAC-bound into SealExportStarted (durable).
    std::uint64_t candidate_id{0};             // MUST match this candidate
    std::uint8_t registered_producer_mask{0};  // bits 0..kMax-1 only
    std::uint8_t producer_count{0};            // 1..kMax; 0 illegal after freeze
    SealHandoffRingId ring_id[kMaxSealHandoffProducers]{};  // per-slot ring bind
    bool topology_frozen{false};               // true after freeze; false=>no close

    // Owner stores non-zero close_epoch E with release to begin close.
    // Producers load with acquire; if close_epoch != 0 they must not start a
    // new try_push. 0 = intake open for Path B handoff admits.
    // After timeout: epoch stays armed; same-process retry MUST store E+1
    // (prior quiesced_ack_epoch==old E is void -- round-54 P1).
    alignas(kCacheLine) std::atomic<std::uint64_t> close_epoch{0};
    // Producer increments (acq_rel) BEFORE claiming a ring slot / publishing
    // tail; decrements after successful publish OR abandoned push.
    // ONLY producers whose bit is set in registered_producer_mask may touch
    // this guard; unset-bit fetch_add / try_push -> immediate hard fence.
    // Owner may treat rings as finally empty only when this is 0 (acquire).
    alignas(kCacheLine) std::atomic<std::uint32_t> in_flight_admit_guard{0};
    SealJournalIntakeCloseProducerSlot producers[kMaxSealHandoffProducers]{};

    // Owner-local (single-writer), set when close begins:
    // std::chrono::steady_clock::time_point close_deadline_steady;
    // (conceptual -- implement with steady_clock, not wall clock. Not a real
    // member -- the spec itself leaves this commented out; do not add it as
    // struct state.)
    // Owner-local: set true only after close protocol SUCCESS.
    bool path_b_prohibited{false};
};

// --- SealJournalTombstoneWire ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3116
//
// Per-seq clear receipt after Applied Ack (breadcrumb directory). File:
// seal-journal/<store_uuid...>/<candidate_id_hex16>-<journal_seq_hex16>.jts
// Conceptual; on-disk is packed LE, 108 bytes, no padding:
//   +0 u32 format_version(=1) / +4 u32 total_bytes(=108) / +8 u64 store_uuid_lo
//   / +16 u64 store_uuid_hi / +24 u32 kek_key_id / +28 u64 candidate_id /
//   +36 u64 journal_seq / +44 u8 entry_mac[32] (MUST equal the Applied /
//   journal entry_mac) / +76 u8 mac[32] (trailer)
// MAC domain (LE, no padding): HMAC(KEK[kek_key_id], "HY-SEALJRNTS-v1" ||
//   format_version || total_bytes || store_uuid_lo || store_uuid_hi ||
//   kek_key_id || candidate_id || journal_seq || entry_mac)

constexpr std::uint32_t kSealJournalTombstoneFormatVersion = 1;
constexpr std::size_t kSealJournalTombstoneBytes = 108;

struct SealJournalTombstoneWire {
    std::uint8_t mac[32]{};
};
static_assert(std::is_trivially_copyable_v<SealJournalTombstoneWire>);
static_assert(std::is_standard_layout_v<SealJournalTombstoneWire>);

// ===========================================================================
// Seal-journal Round B (轨道 C, round 2 of 3) -- the "Started quintet":
// SealExportStartedWire (L, alias SealExportStarted), SealExportStartedMigrationWire
// (M), SealStartedCleanupTombstoneWire (C), SealStartedAbandonWire (A). Depends
// on Round A's SealJournalIntakeCloseControl -- its frozen topology tuple
// (registered_producer_mask/producer_count/ring_id[]) is MAC-bound into
// SealExportStartedWire below.
//
// "V" (seal-export-started.v2) is NOT a separate type -- it is the identical
// SealExportStartedWire shape written to a second file (the migration
// companion), differing from L only in the topology tuple + kek_key_id per the
// spec's L<->V closed-field-bind rule (BINANCE_PRIVATE_REST_L4_SPEC.md:3900-
// 3914). Do not invent a second struct for it.
//
// Filenames (breadcrumb dir, outside the store root) -- round-56/57/64:
//   seal-export-started       -- greenfield v2 OR legacy 192B final (immutable)
//   seal-export-started.v2    -- migration companion (CREATE_NEW / no-replace)
//   seal-export-started.mig   -- migration commit (CREATE_NEW / no-replace)
//   seal-export-started.clr   -- cleanup tombstone (CREATE_NEW; phase via REPLACE)
//   seal-export-started.abd   -- ClrAbandoned proof (CREATE_NEW; phase via REPLACE)
// Forbidden for L/V/M: §10.3 REPLACE / replace-MoveFileExW / delete-then-recreate
// of a live final (power-cut -> lose PostSeal gate). `.clr`/`.abd` phase advances
// MAY use §10.3 REPLACE (watermark class) -- monotonic raise only.
//
// Round C (future, deferred): CompactionCandidateIntentWire,
// CompactionIntentTransitionWire, CompactionIntentGcAuthorizedWire (confirmed
// by research to not subdivide further).
//
// All four types below follow this file's established "Wire type has only
// mac[32]" rule -- the full packed on-disk layout is documented in a comment,
// never expanded into real struct members. phase/started_kind/abandon_reason
// are free constexpr std::uint8_t constants (not enum class), matching both
// the spec's own convention for this family and the already-transcribed
// RateLimitFreezePayload::source precedent -- they are not real struct fields
// at all, since the structs themselves are mac[32]-only.
// ===========================================================================

// --- SealExportStartedWire ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3185
//
// Durable breadcrumb OUTSIDE the store root (same class as LastRemoteAckedTip /
// OperatorOverrideSidecar). Written+fsynced BEFORE any seal network byte is
// sent. Presence forces PostSeal recovery even when no local seal Ack was
// observed. Conceptual layout (packed LE, NO padding), format_version == 2:
//   +0    u32 format_version (=2)
//   +4    u32 total_bytes    (= 238 == kSealExportStartedWireBytes)
//   +8    u64 store_uuid_lo
//   +16   u64 store_uuid_hi
//   +24   u64 candidate_id
//   +32   u32 source_generation
//   +36   u64 baseline_tip_seq
//   +44   u8  baseline_tip_mac[32]
//   +76   u32 baseline_key_id
//   +80   u32 new_generation
//   +84   u64 new_final_seq
//   +92   u8  new_final_tip_mac[32]
//   +124  u32 new_key_id
//   +128  u64 request_id
//   +136  u8  content_root[32]
//   +168  u32 kek_key_id          // round-57 P0 -- selects KEK[kek_key_id]
//   +172  u8  registered_producer_mask
//   +173  u8  producer_count
//   +174  u32 ring_id[8]          // kMaxSealHandoffProducers; unset -> 0
//   +206  u8  mac[32]             // trailer

constexpr std::uint32_t kSealExportStartedFormatVersion = 2;
constexpr std::size_t kSealExportStartedWireBytes = 238;
// Legacy pre-topology conceptual length (r37...r53, no format_version header):
// 192 = fields through content_root + mac under "HY-SEALSTART-v1" (no kek_key_id).
constexpr std::size_t kSealExportStartedLegacyV1Bytes = 192;
// Draft-only 234B (r55/r56 before kek_key_id) -- never admit; treat as Corrupt.
constexpr std::size_t kSealExportStartedDraft234Bytes = 234;
static_assert(kSealExportStartedWireBytes == 238);
static_assert(4 + 4 + 8 + 8 + 8 + 4 + 8 + 32 + 4 + 4 + 8 + 32 + 4 + 8 + 32 + 4 + 1 + 1
                  + (4 * kMaxSealHandoffProducers) + 32
              == kSealExportStartedWireBytes);

struct SealExportStartedWire {
    // Conceptual; on-disk packed as above. MAC domain (LE, no padding):
    // HMAC(KEK[kek_key_id], "HY-SEALSTART-v2" || format_version || total_bytes ||
    //   store_uuid_lo || store_uuid_hi || candidate_id || source_generation ||
    //   baseline_tip_seq || baseline_tip_mac || baseline_key_id ||
    //   new_generation || new_final_seq || new_final_tip_mac || new_key_id ||
    //   request_id || content_root || kek_key_id || registered_producer_mask ||
    //   producer_count || ring_id[0] || ... || ring_id[kMax-1])
    // Topology MUST match frozen SealJournalIntakeCloseControl (round-54/55).
    // Forbidden: write format_version!=2; Forbidden: total_bytes!=238;
    // Forbidden: invent mask=0 / empty topology; Forbidden: try-all / current-key
    // fallback when kek_key_id wrapper missing (L5 §6.1.1.2).
    std::uint8_t mac[32]{};
};
static_assert(std::is_trivially_copyable_v<SealExportStartedWire>);
static_assert(std::is_standard_layout_v<SealExportStartedWire>);
// Alias used in prose: SealExportStarted == SealExportStartedWire v2 fields.
using SealExportStarted = SealExportStartedWire;

// --- SealExportStartedMigrationWire ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3224
//
// Migration commit (companion strategy). File: seal-export-started.mig.
// Fields are SHA-256 digests + trailer MACs of the L and V files, not copies
// of their business fields. Packed LE, NO padding, format_version == 2:
//   +0   u32 format_version (=2)
//   +4   u32 total_bytes    (= 208)
//   +8   u64 store_uuid_lo
//   +16  u64 store_uuid_hi
//   +24  u64 candidate_id
//   +32  u64 request_id
//   +40  u32 legacy_kek_key_id  // sole KEK used to verify L (explicit; no try-all)
//   +44  u32 v2_kek_key_id      // == V.kek_key_id; selects KEK for V + M MACs
//   +48  u8  legacy_file_digest[32]  // SHA-256(entire L file bytes)
//   +80  u8  v2_file_digest[32]      // SHA-256(entire V file bytes)
//   +112 u8  legacy_mac[32]          // L trailer MAC
//   +144 u8  v2_mac[32]              // V trailer MAC
//   +176 u8  mac[32]

constexpr std::uint32_t kSealExportStartedMigrationFormatVersion = 2;
constexpr std::size_t kSealExportStartedMigrationWireBytes = 208;
// Draft-only r56 mig (macs+ids, no kek ids / no full-file digests) -- Corrupt.
constexpr std::size_t kSealExportStartedMigrationDraft136Bytes = 136;
static_assert(4 + 4 + 8 + 8 + 8 + 8 + 4 + 4 + 32 + 32 + 32 + 32 + 32
              == kSealExportStartedMigrationWireBytes);

struct SealExportStartedMigrationWire {
    // HMAC(KEK[v2_kek_key_id], "HY-SEALSTARTMIG-v2" || format_version ||
    //   total_bytes || store_uuid_lo || store_uuid_hi || candidate_id ||
    //   request_id || legacy_kek_key_id || v2_kek_key_id ||
    //   legacy_file_digest || v2_file_digest || legacy_mac || v2_mac)
    std::uint8_t mac[32]{};
};
static_assert(std::is_trivially_copyable_v<SealExportStartedMigrationWire>);
static_assert(std::is_standard_layout_v<SealExportStartedMigrationWire>);

// --- SealStartedCleanupTombstoneWire ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3278
//
// Cleanup tombstone (authorizes Started unlink ONLY after PostSeal committed:
// CURRENT flipped + tip persisted + bridge bound + journal drain-complete).
// File: seal-export-started.clr. Packed LE, NO padding, format_version == 2:
//   +0    u32 format_version (=2)
//   +4    u32 total_bytes    (= 304 == kSealStartedCleanupWireBytes)
//   +8    u64 store_uuid_lo
//   +16   u64 store_uuid_hi
//   +24   u64 candidate_id
//   +32   u64 request_id
//   +40   u32 kek_key_id
//   +44   u8  started_kind     // 1=NativeV2, 2=MigratedV2
//   +45   u8  present_mask     // bit0=L, bit1=V, bit2=M at authorize time
//   +46   u8  phase            // 0=Authorized ... see cleanup order below
//   +47   u8  reserved0 (=0)
//   // PostSealCommittedProof (round-58 P0) -- MAC-bound; all MUST hold at
//   // CREATE and at every CleanupInProgress resume:
//   +48   u32 source_generation      // == Started.source_generation == bridge.prev_generation
//   +52   u64 baseline_tip_seq       // == Started.baseline_tip_seq == bridge.prev_tip_seq
//   +60   u8  baseline_tip_mac[32]   // == Started / bridge.prev_tip_mac
//   +92   u32 baseline_key_id        // == Started / bridge.prev_key_id
//   +96   u32 new_generation         // == Started.new_generation; CURRENT MUST == this
//   +100  u64 new_final_seq          // == Started.new_final_seq
//   +108  u8  new_final_tip_mac[32]  // == Started.new_final_tip_mac
//   +140  u32 new_key_id             // == Started.new_key_id
//   +144  u8  content_root[32]       // == Started.content_root == GenerationSeal.content_root
//   +176  u8  digest_L[32]           // SHA-256(L) or zeros if bit0 clear
//   +208  u8  digest_V[32]
//   +240  u8  digest_M[32]
//   +272  u8  mac[32]

constexpr std::uint32_t kSealStartedCleanupFormatVersion = 2;
constexpr std::size_t kSealStartedCleanupWireBytes = 304;
// Draft-only r57 .clr (ids/digests only; no PostSealCommittedProof) -- Corrupt /
// never CleanupInProgress (never pad to 304).
constexpr std::size_t kSealStartedCleanupDraft176Bytes = 176;
constexpr std::uint8_t kSealStartedKindNativeV2 = 1;
constexpr std::uint8_t kSealStartedKindMigratedV2 = 2;
constexpr std::uint8_t kSealStartedCleanupPhaseAuthorized = 0;
constexpr std::uint8_t kSealStartedCleanupPhaseMGone = 1;
constexpr std::uint8_t kSealStartedCleanupPhaseVGone = 2;
constexpr std::uint8_t kSealStartedCleanupPhaseLGone = 3;
constexpr std::uint8_t kSealStartedCleanupPhaseClrPending = 4;  // only CLR left
static_assert(4 + 4 + 8 + 8 + 8 + 8 + 4 + 1 + 1 + 1 + 1
                  + 4 + 8 + 32 + 4 + 4 + 8 + 32 + 4 + 32
                  + 32 + 32 + 32 + 32
              == kSealStartedCleanupWireBytes);

struct SealStartedCleanupTombstoneWire {
    // HMAC(KEK[kek_key_id], "HY-SEALSTARTCLR-v2" || format_version ||
    //   total_bytes || store_uuid_lo || store_uuid_hi || candidate_id ||
    //   request_id || kek_key_id || started_kind || present_mask || phase ||
    //   reserved0 || source_generation || baseline_tip_seq ||
    //   baseline_tip_mac || baseline_key_id || new_generation ||
    //   new_final_seq || new_final_tip_mac || new_key_id || content_root ||
    //   digest_L || digest_V || digest_M)
    // phase advances are monotonic; REPLACE of .clr allowed only to raise phase
    // (proof fields immutable after Authorized CREATE_NEW).
    std::uint8_t mac[32]{};
};
static_assert(std::is_trivially_copyable_v<SealStartedCleanupTombstoneWire>);
static_assert(std::is_standard_layout_v<SealStartedCleanupTombstoneWire>);

// --- SealStartedAbandonWire ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3326
//
// ClrAbandoned proof (converges ClrUnauthorized + NotFound). File:
// seal-export-started.abd. Packed LE, NO padding, format_version == 1:
//   +0    u32 format_version (=1)
//   +4    u32 total_bytes    (= 192 == kSealStartedAbandonWireBytes)
//   +8    u64 store_uuid_lo
//   +16   u64 store_uuid_hi
//   +24   u64 candidate_id
//   +32   u64 request_id
//   +40   u32 kek_key_id
//   +44   u8  started_kind     // 1=NativeV2, 2=MigratedV2
//   +45   u8  abandon_reason   // 1=AuthenticatedNotFound
//   +46   u8  present_mask     // bit0=L, bit1=V, bit2=M, bit3=C at authorize
//   +47   u8  phase            // see ClrAbandoned order below
//   +48   u32 source_generation
//   +52   u64 baseline_tip_seq
//   +60   u8  baseline_tip_mac[32]
//   +92   u32 baseline_key_id
//   +96   u8  content_root[32]
//   +128  u8  digest_C[32]     // SHA-256(unauthorized C) or zeros if absent/torn
//   +160  u8  mac[32]

constexpr std::uint32_t kSealStartedAbandonFormatVersion = 1;
constexpr std::size_t kSealStartedAbandonWireBytes = 192;
constexpr std::uint8_t kSealStartedAbandonReasonNotFound = 1;
constexpr std::uint8_t kSealStartedAbandonPhaseAuthorized = 0;
constexpr std::uint8_t kSealStartedAbandonPhaseCGone = 1;
constexpr std::uint8_t kSealStartedAbandonPhaseMGone = 2;
constexpr std::uint8_t kSealStartedAbandonPhaseVGone = 3;
constexpr std::uint8_t kSealStartedAbandonPhaseLGone = 4;
constexpr std::uint8_t kSealStartedAbandonPhaseGenGone = 5;  // gen-N+1 abandoned
constexpr std::uint8_t kSealStartedAbandonPhaseResumeAuthorized = 6;  // tip ok; A kept; producer paused
constexpr std::uint8_t kSealStartedAbandonPhaseAbdPending = 7;  // unlink A next; resume after A gone
static_assert(4 + 4 + 8 + 8 + 8 + 8 + 4 + 1 + 1 + 1 + 1
                  + 4 + 8 + 32 + 4 + 32 + 32 + 32
              == kSealStartedAbandonWireBytes);

struct SealStartedAbandonWire {
    // HMAC(KEK[kek_key_id], "HY-SEALSTARTABD-v1" || format_version ||
    //   total_bytes || store_uuid_lo || store_uuid_hi || candidate_id ||
    //   request_id || kek_key_id || started_kind || abandon_reason ||
    //   present_mask || phase || source_generation || baseline_tip_seq ||
    //   baseline_tip_mac || baseline_key_id || content_root || digest_C)
    // phase advances monotonic; REPLACE .abd only to raise phase
    // (all other fields immutable after Authorized CREATE_NEW).
    std::uint8_t mac[32]{};
};
static_assert(std::is_trivially_copyable_v<SealStartedAbandonWire>);
static_assert(std::is_standard_layout_v<SealStartedAbandonWire>);

// ===========================================================================
// Seal-journal Round C (轨道 C, round 3 of 3 -- LAST round) -- the
// CompactionCandidateIntent GC family: CompactionCandidateIntentWire (Intent
// lifecycle), CompactionIntentTransitionWire (.x1, crash-verifiable path
// proof), CompactionIntentGcAuthorizedWire (.xgc, GC authorization receipt --
// the spec's own words: "the single densest individual type in the whole
// section").
//
// DECLARATION-ONLY, same governance as every Wire type in this file since
// round 1 -- see this file's top-of-file SCOPE paragraph ("not an
// implementation of the durable log... no wire (de)serialization"). After
// this round, every type L4 §10 names is transcribed; the only thing left
// out of this file is the crash-window table and §10.1/10.2/10.3 procedural
// prose (BINANCE_PRIVATE_REST_L4_SPEC.md:5080-5327), which contain zero named
// types and stay permanently excluded (same treatment as
// DurableControlPlaneSink's un-implemented method bodies). That means this
// round can claim "L4 §10's named types are all declared" -- it CANNOT claim
// ".x1/.xgc persistence, GC, or recovery are implemented," because (same as
// every other type here) these three structs are mac[32]-only: no encode/
// decode, no filesystem publish, no MAC verification, no recovery state
// machine, no concurrency/ownership protocol exists for them anywhere in this
// codebase. See docs/SPEC_INVARIANTS.md's "Seal-journal Round C" entry for
// the design input recorded for a future codec/recovery round that would
// actually implement this.
// ===========================================================================

// --- CompactionCandidateIntentWire ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3375
//
// Distinguishes a legal PreSeal-build remnant from a Started-cleared-without-
// A0 corruption. File: compaction-candidate-intent (breadcrumb dir, same
// durability class as SealExportStarted). Packed LE, NO padding,
// format_version == 1, wire size stays 140B (phase enum extended in r65 with
// no layout growth):
//   +0    u32 format_version (=1)
//   +4    u32 total_bytes    (= 140 == kCompactionCandidateIntentWireBytes)
//   +8    u64 store_uuid_lo
//   +16   u64 store_uuid_hi
//   +24   u32 kek_key_id
//   +28   u8  phase            // 0=Building ... 4=AbandonFinalizing
//   +29   u8  reserved0 (=0)   // MUST be 0; no completion_kind (phase is the
//                                live discriminator; path proof is .x1, r66)
//   +30   u16 reserved1 (=0)
//   +32   u32 source_generation
//   +36   u32 target_generation  // == source_generation + 1 (UINT32_MAX refuse)
//   +40   u64 baseline_tip_seq
//   +48   u8  baseline_tip_mac[32]
//   +80   u32 baseline_key_id
//   +84   u64 build_nonce        // unique per build attempt; never reused
//   +92   u64 candidate_id       // 0 while Building; bound at Reserved
//   +100  u64 request_id         // 0 while Building; bound at Reserved
//   +108  u8  mac[32]

constexpr std::uint32_t kCompactionCandidateIntentFormatVersion = 1;
constexpr std::size_t kCompactionCandidateIntentWireBytes = 140;
constexpr std::uint8_t kCompactionCandidateIntentPhaseBuilding = 0;
constexpr std::uint8_t kCompactionCandidateIntentPhaseReserved = 1;
constexpr std::uint8_t kCompactionCandidateIntentPhaseStartedPublished = 2;
constexpr std::uint8_t kCompactionCandidateIntentPhasePostSealFinalizing = 3;  // r65
constexpr std::uint8_t kCompactionCandidateIntentPhaseAbandonFinalizing = 4;   // r65
static_assert(4 + 4 + 8 + 8 + 4 + 1 + 1 + 2 + 4 + 4 + 8 + 32 + 4 + 8 + 8 + 8 + 32
              == kCompactionCandidateIntentWireBytes);

struct CompactionCandidateIntentWire {
    // HMAC(KEK[kek_key_id], "HY-COMPINTENT-v1" || format_version ||
    //   total_bytes || store_uuid_lo || store_uuid_hi || kek_key_id ||
    //   phase || reserved0 || reserved1 || source_generation ||
    //   target_generation || baseline_tip_seq || baseline_tip_mac ||
    //   baseline_key_id || build_nonce || candidate_id || request_id)
    // CREATE_NEW / no-replace for phase=Building (before any gen-N+1 write).
    // Genesis = this CREATE_NEW (no none->Building transition receipt).
    // REPLACE allowed ONLY to raise phase along ONE legal chain, and ONLY
    // AFTER a durable matching CompactionIntentTransitionWire `.x1` for
    // that edge exists (r66 -- Intent.phase alone is NOT crash-verifiable
    // path proof; monotonic REPLACE does NOT prove no-cross after reboot):
    //   Building->Reserved->StartedPublished->PostSealFinalizing
    //   OR Building->Reserved->StartedPublished->AbandonFinalizing
    // Forbidden: PostSealFinalizing<->AbandonFinalizing cross; Forbidden: jump
    // Building|Reserved->*Finalizing; Forbidden: any phase regression;
    // Forbidden: Intent REPLACE without prior durable matching `.x1`.
    // Proof fields (baseline 4-tuple, generations, build_nonce, store_uuid,
    // kek_key_id) immutable after CREATE_NEW. candidate_id/request_id may
    // change from 0->nonzero exactly once at Reserved (must match
    // SealIdWatermark reservation); nonzero->other nonzero -> Corrupt.
    // reserved0/reserved1 MUST stay 0 -- **no** parallel completion_kind field
    // (phase is the live terminal/non-terminal discriminator; crash-verifiable
    // path / no-cross proof is CompactionIntentTransitionWire `.x1`, r66).
    // Hot-path: preallocated fixed 140B buffer; no heap; no std::string.
    // Flush: same class as SealExportStarted (§10.3) -- file + parent.
    // Windows: complete MAC-valid Intent => durable Intent present
    // (parent-dir flush undecidable; do not invent "not durable" PreSeal
    // on a complete file -- same honesty rule as Started).
    // Clear (CAPTURE cleanup evidence -> unlink C/A -> CREATE `.xgc` ->
    // TipExportProducerResume idempotent -> GC matching `.x1` -> unlink Intent ->
    // unlink `.xgc` last; parent flush after each create/unlink) ONLY after
    // this candidate's final cleanup complete (r67/r70/r71 -- `.xgc` authorizes
    // partial `.x1` absence only when CREATE-time + Mode-B
    // PhysicalCleanupPreconditions hold; Mode B reads DurableCleanupAuthEvidence
    // from `.xgc`, never deleted `.clr`/`.abd`):
    //   (a) PreSeal-abandon (Building|Reserved): after G/T/journal/Started/A/C
    //       cleanup as applicable (GenGone rules for pre-Started) -> CREATE
    //       `.xgc` (PreSealAbandonClear; evidence zeros; Building-only
    //       terminal_transition_mac=0) -> TipExportProducerResume if pause
    //       armed (idempotent) -> GC `.x1` -> Intent -> `.xgc`;
    //   (b) PostSeal success: after Intent PostSealFinalizing + ordered unlink
    //       M->V->L -> CAPTURE from C -> unlink C -> CREATE `.xgc`
    //       (PostSealFinalizingClear; DurableCleanupAuthEvidence bound) ->
    //       TipExportProducerResume (idempotent) -> GC `.x1` -> Intent -> `.xgc`;
    //   (c) ClrAbandoned: after Intent AbandonFinalizing + GenGone +
    //       ResumeAuthorized -> CAPTURE from A -> unlink A -> CREATE `.xgc`
    //       (AbandonFinalizingClear; DurableCleanupAuthEvidence bound) ->
    //       TipExportProducerResume (idempotent) -> GC `.x1` -> Intent -> `.xgc` --
    //       Intent clear AFTER A unlink, never before GenGone; never leave
    //       Intent forever blocking new build.
    // Forbidden: CREATE Intent while prior Intent still present (dual /
    // leftover Intent incl. *Finalizing / leftover `.xgc` -> finish prior
    // cleanup first; never overwrite);
    // Forbidden: CREATE `.xgc` before the matching (a)/(b)/(c) authorization
    // moment / while Started|A|C (or PostSeal L/V/M) still gates cleanup /
    // wrong disposition / without DurableCleanupAuthEvidence capturable;
    // Forbidden: write G before durable Building Intent;
    // Forbidden: raise Reserved without durable SealIdWatermark advance
    // binding the same candidate_id/request_id;
    // Forbidden: raise StartedPublished before durable NativeV2/
    // MigratedV2 Started for those ids;
    // Forbidden: raise PostSealFinalizing before `.clr` Authorized +
    // PostSealCommittedProof;
    // Forbidden: raise AbandonFinalizing before ResumeAuthorized while A
    // present (or without GenGone done);
    // Forbidden: clear Intent while Started / `.abd` / live G for this
    // build still gates cleanup (except after *Finalizing proves the
    // terminal path and those names are already gone / finishable);
    // Forbidden: clear Intent without prior durable matching `.xgc`;
    // Forbidden: unlink any matching `.x1` without prior durable `.xgc`.
    std::uint8_t mac[32]{};
};
static_assert(std::is_trivially_copyable_v<CompactionCandidateIntentWire>);
static_assert(std::is_standard_layout_v<CompactionCandidateIntentWire>);

// --- CompactionIntentTransitionWire ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3494
//
// No-replace transition receipt. Intent.phase alone is REPLACE-overwriteable
// and CANNOT prove historical path (no-skip / no PostSeal<->Abandon cross)
// across reboot -- every phase RAISE must first publish a durable transition
// receipt; recovery verifies the full chain. Genesis: CREATE_NEW Intent
// phase=Building IS the genesis -- there is NO none->Building transition
// file. Transition chain starts at seq=1:
//   Building->Reserved->StartedPublished->PostSealFinalizing
//   OR Building->Reserved->StartedPublished->AbandonFinalizing
// File (breadcrumb dir; same durability class as .sj1 finals -- NEVER REPLACE):
//   compaction-intent-x-<build_nonce_hex16>-<transition_seq_hex16>.x1
// Publish recipe (align journal §10.1 / Windows): encode preallocated buffer ->
//   exclusive-create `*.x1.tmp` -> fsync(file) -> no-replace publish
//   (POSIX renameat2(RENAME_NOREPLACE)/linkat; Windows preferred
//   CreateHardLinkW -> parent FlushFileBuffers -> unlink tmp; CREATE_NEW copy
//   fallback only) -> parent dir flush. If final exists: MAC-verify + byte-equal
//   -> idempotent; any difference -> Corrupt, original unchanged.
// Forbidden: §10.3 REPLACE / replace-MoveFileExW / delete-then-recreate of a
// live `.x1` (power-cut would erase the only crash-verifiable edge).
// Packed LE, NO padding, format_version == 1, fixed 176B:
//   +0    u32 format_version (=1)
//   +4    u32 total_bytes    (= 176 == kCompactionIntentTransitionWireBytes)
//   +8    u64 store_uuid_lo
//   +16   u64 store_uuid_hi
//   +24   u32 kek_key_id
//   +28   u8  from_phase       // Intent phase BEFORE this raise
//   +29   u8  to_phase         // Intent phase AFTER this raise
//   +30   u16 reserved0 (=0)
//   +32   u32 transition_seq   // 1-based; first raise Building->Reserved = 1
//   +36   u32 source_generation
//   +40   u32 target_generation
//   +44   u64 baseline_tip_seq
//   +52   u8  baseline_tip_mac[32]
//   +84   u32 baseline_key_id
//   +88   u64 build_nonce      // MUST equal Intent.build_nonce
//   +96   u64 candidate_id     // post-raise bind (0 only illegal for seq>=1)
//   +104  u64 request_id       // post-raise bind (nonzero from seq=1 onward)
//   +112  u8  prev_transition_mac[32]  // all-zero iff transition_seq==1;
//                                      // else == trailer mac of seq-1 `.x1`
//   +144  u8  mac[32]

constexpr std::uint32_t kCompactionIntentTransitionFormatVersion = 1;
constexpr std::size_t kCompactionIntentTransitionWireBytes = 176;
static_assert(4 + 4 + 8 + 8 + 4 + 1 + 1 + 2 + 4 + 4 + 4 + 8 + 32 + 4
                  + 8 + 8 + 8 + 32 + 32
              == kCompactionIntentTransitionWireBytes);

struct CompactionIntentTransitionWire {
    // HMAC(KEK[kek_key_id], "HY-COMPINTENT-X-v1" || format_version ||
    //   total_bytes || store_uuid_lo || store_uuid_hi || kek_key_id ||
    //   from_phase || to_phase || reserved0 || transition_seq ||
    //   source_generation || target_generation || baseline_tip_seq ||
    //   baseline_tip_mac || baseline_key_id || build_nonce ||
    //   candidate_id || request_id || prev_transition_mac)
    // Legal edges ONLY (from_phase->to_phase):
    //   0->1 Building->Reserved                 (seq must be 1)
    //   1->2 Reserved->StartedPublished         (seq must be 2)
    //   2->3 StartedPublished->PostSealFinalizing (seq must be 3)
    //   2->4 StartedPublished->AbandonFinalizing  (seq must be 3)
    // Forbidden edges (always Corrupt if present): 3->4, 4->3, any skip
    // (0->2/0->3/0->4/1->3/1->4/...), any regression, duplicate seq, seq gap.
    // Bind MUST match Intent: store_uuid, kek_key_id, baseline 4-tuple,
    // generations, build_nonce. candidate_id/request_id MUST equal the
    // post-Reserved Intent ids (nonzero from seq=1). Transition for a
    // foreign build_nonce / wrong baseline while Intent exists -> Corrupt.
    // Hot-path: preallocated fixed 176B buffer; no heap; no std::string.
    // Control-plane I/O only.
    std::uint8_t mac[32]{};
};
static_assert(std::is_trivially_copyable_v<CompactionIntentTransitionWire>);
static_assert(std::is_standard_layout_v<CompactionIntentTransitionWire>);

// --- CompactionIntentGcAuthorizedWire ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3602
//
// No-replace GC authorization ("the single densest individual type in the
// whole section"). r66's `.x1` chain + "gap => Corrupt" + "GC all `.x1`
// before Intent" created a false-Corrupt power window: legal mid-GC (e.g.
// deleted x1[1] from {1,2,3}, Intent still present) looks like an
// unauthorized seq gap. Fixed by a durable GC-authorization receipt that
// must exist BEFORE any `.x1` unlink. r70 required Mode B to re-check
// physical cleanup preconditions, but those checks re-read `.clr`/`.abd`
// which normative order already unlinked -- legal mid-GC crash made Mode B
// non-constructible. r71 embeds DurableCleanupAuthEvidence into `.xgc` itself
// (one coherent artifact; no TerminalCleanupProof sibling) and verifies Mode
// B from that evidence + live gate absence.
// File (breadcrumb dir; same durability class as `.x1` / `.sj1` -- NEVER
// REPLACE): compaction-intent-gc-<build_nonce_hex16>.xgc
// Publish recipe: encode preallocated 316B -> exclusive-create `*.xgc.tmp` ->
//   fsync(file) -> no-replace publish (POSIX renameat2(RENAME_NOREPLACE)/linkat;
//   Windows preferred CreateHardLinkW -> parent FlushFileBuffers -> unlink tmp;
//   CREATE_NEW copy fallback only) -> parent dir flush. If final exists:
//   MAC-verify + byte-equal -> idempotent; any difference -> Corrupt, original
//   unchanged. Forbidden: §10.3 REPLACE / replace-MoveFileExW / delete-then-
//   recreate of a live `.xgc`.
// Packed LE, NO padding, format_version == 2, fixed 316B:
//   +0    u32 format_version (=2)
//   +4    u32 total_bytes    (= 316 == kCompactionIntentGcAuthorizedWireBytes)
//   +8    u64 store_uuid_lo
//   +16   u64 store_uuid_hi
//   +24   u32 kek_key_id
//   +28   u8  terminal_disposition
//         // 0 = PreSealAbandonClear
//         // 1 = PostSealFinalizingClear
//         // 2 = AbandonFinalizingClear
//   +29   u8  intent_phase_at_auth  // MUST == Intent.phase at CREATE time
//   +30   u16 reserved0 (=0)
//   +32   u32 source_generation
//   +36   u32 target_generation
//   +40   u64 baseline_tip_seq
//   +48   u8  baseline_tip_mac[32]
//   +80   u32 baseline_key_id
//   +84   u64 build_nonce          // MUST equal Intent.build_nonce
//   +92   u64 candidate_id
//   +100  u64 request_id
//   +108  u8  intent_mac[32]       // Intent trailer mac at CREATE time
//   +140  u8  terminal_transition_mac[32]
//         // trailer mac of last (highest-seq) matching `.x1` at CREATE;
//         // all-zero IFF no `.x1` exist AND disposition==PreSealAbandonClear
//         // AND intent_phase_at_auth==Building (Building-only clear)
//   // DurableCleanupAuthEvidence (r71) -- Mode B reconstructible w/o live C/A:
//   +172  u8  cleanup_auth_flags
//         // bit0 = journal_drain_complete_at_auth
//         // bit1 = post_seal_committed_bound   (PostSealFinalizingClear)
//         // bit2 = gen_gone_complete_at_auth   (AbandonFinalizingClear)
//         // bit3 = resume_authorized_bound     (AbandonFinalizingClear;
//         //         gate_trailer_mac is A.mac at phase>=ResumeAuthorized)
//         // bit4 = gate_absent_at_create       (1 only on recovery CREATE when
//         //         C/A already unlinked; live crash-free path MUST be 0)
//         // bit5..7 = 0
//   +173  u8  started_kind          // 0 PreSeal; else C/A.started_kind (1/2)
//   +174  u8  present_mask_at_auth  // 0 PreSeal; C mask (L/V/M) or A mask
//   +175  u8  reserved1 (=0)
//   +176  u64 proof_new_final_seq           // PostSeal: C.new_final_seq; else 0
//   +184  u8  proof_new_final_tip_mac[32]   // PostSeal: C.new_final_tip_mac
//   +216  u32 proof_new_key_id              // PostSeal: C.new_key_id; else 0
//   +220  u8  proof_content_root[32]        // PostSeal C / Abandon A; PreSeal 0
//   +252  u8  gate_trailer_mac[32]
//         // PostSeal: C.mac captured before unlink C (live path);
//         // Abandon: A.mac at phase>=ResumeAuthorized captured before unlink A;
//         // PreSeal: all-zero;
//         // gate_absent_at_create=1: MUST be all-zero (recovery reconstruction)
//   +284  u8  mac[32]
// Legacy v1 204B / domain HY-COMPINTENT-GC-v1 (r67...r70): fail-closed --
// never Mode B; never pad/truncate to 316; offline migration only.

constexpr std::uint32_t kCompactionIntentGcAuthorizedFormatVersion = 2;
constexpr std::size_t kCompactionIntentGcAuthorizedWireBytes = 316;
constexpr std::size_t kCompactionIntentGcAuthorizedLegacyV1Bytes = 204;
constexpr std::uint8_t kCompactionIntentGcDispositionPreSealAbandonClear = 0;
constexpr std::uint8_t kCompactionIntentGcDispositionPostSealFinalizingClear = 1;
constexpr std::uint8_t kCompactionIntentGcDispositionAbandonFinalizingClear = 2;
constexpr std::uint8_t kCompactionIntentGcAuthFlagJournalDrain = 1u << 0;
constexpr std::uint8_t kCompactionIntentGcAuthFlagPostSealBound = 1u << 1;
constexpr std::uint8_t kCompactionIntentGcAuthFlagGenGone = 1u << 2;
constexpr std::uint8_t kCompactionIntentGcAuthFlagResumeAuthorized = 1u << 3;
constexpr std::uint8_t kCompactionIntentGcAuthFlagGateAbsentAtCreate = 1u << 4;
static_assert(4 + 4 + 8 + 8 + 4 + 1 + 1 + 2 + 4 + 4 + 8 + 32 + 4
                  + 8 + 8 + 8 + 32 + 32
                  + 1 + 1 + 1 + 1 + 8 + 32 + 4 + 32 + 32 + 32
              == kCompactionIntentGcAuthorizedWireBytes);

struct CompactionIntentGcAuthorizedWire {
    // HMAC(KEK[kek_key_id], "HY-COMPINTENT-GC-v2" || format_version ||
    //   total_bytes || store_uuid_lo || store_uuid_hi || kek_key_id ||
    //   terminal_disposition || intent_phase_at_auth || reserved0 ||
    //   source_generation || target_generation || baseline_tip_seq ||
    //   baseline_tip_mac || baseline_key_id || build_nonce ||
    //   candidate_id || request_id || intent_mac || terminal_transition_mac ||
    //   cleanup_auth_flags || started_kind || present_mask_at_auth ||
    //   reserved1 || proof_new_final_seq || proof_new_final_tip_mac ||
    //   proof_new_key_id || proof_content_root || gate_trailer_mac)
    //
    // === CREATE-time PhysicalCleanupPreconditions (may read live C/A) ===
    // CREATE_NEW / no-replace ONLY when terminal cleanup already authorizes
    // Intent clear **AND** DurableCleanupAuthEvidence is captured into the
    // wire (r71 -- phase alone is insufficient; TipExportProducerResume is
    // NOT a CREATE/Mode-B authorization fact):
    //   PreSealAbandonClear (0): Intent.phase in {Building, Reserved};
    //     PreSeal cleanup complete for G/T/journal/Started/A/C as applicable
    //     (pre-Started GenGone rules); no admitted Started; A absent; C absent;
    //     cleanup_auth_flags bits1..4 == 0; started_kind/present_mask/
    //     proof_new_*/proof_content_root/gate_trailer_mac all zero;
    //     bit0 journal_drain as applicable (1 if candidate had journal work).
    //   PostSealFinalizingClear (1): Intent.phase == PostSealFinalizing;
    //     **L/V/M already cleared**; C was readable in the capture window
    //     (live path) OR gate_absent_at_create recovery reconstruction;
    //     PostSealCommittedProof held at capture (CURRENT==new_generation;
    //     tip equal-or-forward covers seal tip; bridge binds baseline +
    //     content_root; journal drain-complete); wire MUST set bit0+bit1;
    //     gate_trailer_mac = C.mac (live path, bit4=0) or 0 (bit4=1);
    //     proof_new_* / proof_content_root / started_kind / present_mask
    //     from C (or GenerationSeal/bridge/tip reconstruction when bit4=1).
    //     NOTE: Intent.phase / intent_phase_at_auth == PostSealFinalizing is
    //     **NOT** proof cleanup complete -- that phase is raised **before**
    //     Started/C unlink.
    //   AbandonFinalizingClear (2): Intent.phase == AbandonFinalizing;
    //     GenGone + ResumeAuthorized done; Started/C already cleared; A was
    //     readable in the capture window (live path) OR gate_absent_at_create
    //     recovery reconstruction; wire MUST set bit0+bit2+bit3;
    //     gate_trailer_mac = A.mac at phase>=ResumeAuthorized (live, bit4=0)
    //     or 0 (bit4=1); proof_content_root = A.content_root (or T bind when
    //     bit4=1); proof_new_* = 0; started_kind/present_mask from A.
    //
    // === Crash-safe capture -> unlink -> CREATE order (r71; mandatory) ===
    //   PostSeal (after Intent PostSealFinalizing + ordered unlink M->V->L,
    //   C still present at ClrPending):
    //     1) CAPTURE into preallocated buffer (same actor critical section;
    //        **no yield** / no schedule point): C.mac, C.started_kind,
    //        C.present_mask, C.new_final_seq/mac/key_id, C.content_root,
    //        journal_drain_complete, PostSealCommittedProof live-hold.
    //     2) unlink C -> parent flush.
    //     3) CREATE `.xgc` binding captured digests + Intent binds
    //        (bit4=0; gate_trailer_mac=captured C.mac).
    //     4) TipExportProducerResume (**idempotent**; not durable auth).
    //     5) GC matching `.x1` -> clear Intent -> unlink `.xgc` last.
    //   Abandon (after ResumeAuthorized + Intent AbandonFinalizing, A present):
    //     1) CAPTURE: A.mac (>=ResumeAuthorized), A.started_kind/present_mask/
    //        content_root, gen_gone + resume_authorized facts.
    //     2) unlink A -> parent flush.
    //     3) CREATE `.xgc` (bit4=0; gate_trailer_mac=captured A.mac).
    //     4) TipExportProducerResume (idempotent).
    //     5) GC `.x1` -> clear Intent -> unlink `.xgc` last.
    //   Crash after C/A unlink, before CREATE `.xgc`: nearly-finished path
    //     (Intent *Finalizing, no `.xgc`) reconstructs evidence with bit4=1
    //     (no live gate_trailer_mac) then CREATEs `.xgc` -- NOT Corrupt.
    //   Forbidden: yield between capture and CREATE on the live path;
    //   Forbidden: CREATE while L/V/M/Started still present (PostSeal) or
    //     while A/Started/C still present (Abandon) -- capture-then-unlink
    //     first; Forbidden: CREATE while Intent absent; Forbidden: separate
    //     TerminalCleanupProof sibling (dual-artifact); Forbidden: Mode B
    //     that re-reads deleted `.clr`/`.abd`.
    //
    // Bind MUST match Intent at CREATE: store_uuid, kek_key_id, baseline
    // 4-tuple, generations, build_nonce, candidate_id/request_id,
    // intent_mac == Intent.mac, intent_phase_at_auth == Intent.phase,
    // disposition<->phase pairing above. terminal_transition_mac MUST equal
    // last `.x1` trailer mac when any `.x1` exist; zeros only for Building-
    // only PreSeal clear. Forbidden: CREATE while StartedPublished mid-path;
    // Forbidden: CREATE before DurableCleanupAuthEvidence is capturable /
    // reconstructible; Forbidden: dual `.xgc` (same or foreign build_nonce)
    // while prior GC incomplete; Forbidden: REPLACE / overwrite live `.xgc`;
    // Forbidden: wrong disposition vs Intent.phase; Forbidden: intent_mac /
    // transition mac / baseline / build_nonce / cleanup-evidence mismatch
    // (forged -> Corrupt); Forbidden: format_version!=2 / total_bytes!=316 /
    // legacy v1 204B admitted as Mode B.
    // Hot-path: preallocated fixed 316B buffer; no heap; no std::string.
    // Control-plane I/O only.
    std::uint8_t mac[32]{};
};
static_assert(std::is_trivially_copyable_v<CompactionIntentGcAuthorizedWire>);
static_assert(std::is_standard_layout_v<CompactionIntentGcAuthorizedWire>);

// --- SealJournalOriginKey ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4363
//
// De-dup index key for seal-journal entries. entry_mac is deliberately NOT
// part of this key -- see SealJournalAppliedView below.

struct SealJournalOriginKey {
    std::uint64_t candidate_id{0};
    std::uint64_t journal_seq{0};
};
static_assert(std::is_trivially_copyable_v<SealJournalOriginKey>);
static_assert(std::is_standard_layout_v<SealJournalOriginKey>);

// --- is_seal_journal_embeddable_type ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4373
//
// Round-42 P1 -- closed allowlist for seal-journal / SealJournalApplied
// embed. Anything else (GenerationBridge, OperatorOverride, SealJournalApplied,
// compaction-only snapshots, registry/weight/usage, TransportFailover) is NOT
// embeddable -- journal admit and apply MUST Corrupt / refuse.

constexpr bool is_seal_journal_embeddable_type(DurableRecordType t) noexcept {
    switch (t) {
        case DurableRecordType::OrderEvent:
        case DurableRecordType::OrderCheckpoint:
        case DurableRecordType::RateLimitFreeze:
        case DurableRecordType::FreezeEpochWatermark:
        case DurableRecordType::FreezeProbeAttempt:
        case DurableRecordType::FreezeClear:
        case DurableRecordType::FreezeWaitArm:
        case DurableRecordType::FreezeWaitSatisfied:
            return true;
        default:
            return false;
    }
}

// --- Seal-journal entry-wire constants ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4448
//
// Derived constants for the SealJournalEntryWire on-disk layout (that wire
// format itself has no corresponding C++ struct -- documented byte layout
// only, same as SealJournalTombstoneWire above).

constexpr std::uint32_t kSealJournalFormatVersion = 1;
constexpr std::size_t kSealJournalFixedMetaBytes = 138;   // bytes excl. payload
constexpr std::size_t kSealJournalMaxEmbeddedBytes = 4096;
constexpr std::size_t kSealJournalMaxEntryBytes =
    kSealJournalFixedMetaBytes + kSealJournalMaxEmbeddedBytes;  // 4234

// --- SealJournalAppliedView ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4478
//
// In-memory call shape (zero heap; payload owned by caller until Ack) for
// DurableControlPlaneSink::append_seal_journal_apply, below. Previously a
// bare forward declaration (incomplete type only) sitting just above that
// class; now a real definition here, with the family it belongs to --
// append_seal_journal_apply is now genuinely callable (see that method's
// updated comment) and the sink-interface test now exercises it like every
// other method.

struct SealJournalAppliedView {
    std::uint64_t candidate_id{0};
    std::uint64_t journal_seq{0};
    std::uint8_t entry_mac[32]{};           // MUST equal recomputed HY-SEALJRN-v1
    std::uint32_t kek_key_id{0};            // selects KEK for recompute
    std::uint32_t source_generation{0};     // CompactionSourceBaseline.generation
    std::uint64_t baseline_tip_seq{0};
    std::uint8_t baseline_tip_mac[32]{};
    std::uint32_t baseline_key_id{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};        // MUST be 0 iff UnknownBootstrap
    DurableRecordType embedded_type{DurableRecordType::OrderEvent};
    std::span<const std::uint8_t> embedded_payload{};  // caller-owned; sink
                                                        // copies under Ack
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

// --- LastRemoteAckedTip ---
// SPEC-STRUCT: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4586
//
// Durable export baseline -- breadcrumb directory, outside store root. Updated
// on every remote Ack BEFORE ring pop, under monotonic (generation, sequence)
// CAS / single-writer (§10.2.1). Survives crash/restart; degraded-mode
// reconstruction baseline when remote down.
//
// Retroactive gap fix (轨道 C): this §10.2.1 type sits immediately beside
// ExportTuple/ExportOutboxRing below (both already transcribed in an earlier
// round) but was itself never transcribed, and wasn't even named in this
// file's own "not yet transcribed" list -- fixed this round.

struct LastRemoteAckedTip {
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint32_t generation{0};
    std::uint64_t sequence{0};
    std::array<std::uint8_t, 32> tip_mac{};
    std::uint32_t key_id{0};
    std::uint8_t mac[32]{};  // HMAC under KEK over the above
};
static_assert(std::is_trivially_copyable_v<LastRemoteAckedTip>);
static_assert(std::is_standard_layout_v<LastRemoteAckedTip>);

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

// ===========================================================================
// DurableControlPlaneSink interface family (轨道 C) -- the L4-owned interface
// surface of §10. Method bodies are `= 0` only: no fsync, no MAC verification,
// no network I/O, no recovery_scan() -- same "shape, not implementation"
// discipline as every struct above, extended to a vtable type. See this file's
// top-of-file comment for the is_abstract_v/has_virtual_destructor_v
// governance rule these three classes use in place of
// is_trivially_copyable_v/is_standard_layout_v.
// ===========================================================================

// --- ExternalAnchorClient ---
// SPEC-CLASS: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4659
//
// Pure remote-transport client -- fixes that round's P0 (ownership
// contradiction): holds NO queue, NO backlog, NO retry state. Every
// enqueue/dequeue/backpressure decision belongs to ExportOutboxRing above,
// owned by the sink -- never here.

class ExternalAnchorClient {
public:
    virtual ~ExternalAnchorClient() = default;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4673
    // Called by the export worker for exactly the tuple ExportOutboxRing::
    // peek_oldest() currently returns -- one outstanding export at a time
    // (single consumer, single in-flight call). This method has no notion
    // of "too far behind" and does no queueing of its own; that's the
    // ring's job (§10.2.1).
    // Round-40: remote MUST hard-reject any tip that is lexicographically
    // lagging vs the remote's last accepted (generation, sequence) for this
    // store_uuid (soft-accept / silent overwrite of a newer tip is forbidden).
    // Local client maps that reject to Failed (not Acked); the tuple stays at
    // ring head until policy resolves -- after a seal tip@N+1, PreSeal drain
    // guarantees no gen-N tuple remains to send (§10.1 / §10.2.1).
    virtual AuditAppendResult export_tip_and_wait_bounded(
        std::uint64_t store_uuid_lo, std::uint64_t store_uuid_hi,
        std::uint32_t generation, std::uint64_t sequence,
        std::span<const std::uint8_t, 32> tip_mac, std::uint32_t key_id) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4680
    // Round-37: seal MUST already have a durable SealExportStarted whose
    // request_id == seal.request_id BEFORE the first network write of this
    // call. Implementations MUST NOT send seal bytes if that breadcrumb is
    // missing.
    virtual AuditAppendResult export_and_wait_ack(const GenerationSeal&) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4685
    // Authenticated, monotonic read. Unavailable is distinct from malformed,
    // equivocal or stale data; only the latter three are Corrupt. Used both
    // for §10.2's rollback check and §10.2.1's durable-outbox reconstruction
    // at startup.
    virtual RecoveryScanStatus read_latest(GenerationSeal& out) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4686
    virtual RecoveryScanStatus read_latest_tip(
        std::uint64_t store_uuid_lo, std::uint64_t store_uuid_hi,
        std::uint32_t& out_generation, std::uint64_t& out_sequence,
        std::array<std::uint8_t, 32>& out_tip_mac, std::uint32_t& out_key_id) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4702
    // Round-37/39 -- recovery query by SealExportStarted.request_id.
    // Returns SealQueryStatus -- NEVER RecoveryScanStatus (no Found/NotFound
    // there).
    // Found: out filled; caller MUST field-bind to SealExportStarted + local
    //   N+1 including Started baseline 4-tuple == bridge.prev_* (full, incl.
    //   prev_generation / prev_key_id) -- see §10.1 recovery step 0.
    // NotFound: authenticated permanent-negative only -> may abandon IFF N
    //   tip still equals Started/bridge baseline 4-tuple; else Corrupt
    //   (§10.1). Abandon ONLY via ClrAbandoned (.abd A0 first) whether C
    //   present or not (§10 ABI round-59...63) -- never clear Started without
    //   durable A; never leave unauthorized C occupying the name.
    // TransportUnavailable: keep SealExportStarted; fence; re-query; never
    //   .abd.
    // Corrupt: MAC/equivocation/bind failure.
    virtual SealQueryStatus query_seal_by_request_id(
        std::uint64_t store_uuid_lo, std::uint64_t store_uuid_hi,
        std::uint64_t request_id, GenerationSeal& out) noexcept = 0;
};
static_assert(std::is_abstract_v<ExternalAnchorClient>);
static_assert(std::has_virtual_destructor_v<ExternalAnchorClient>);

// --- DurableControlPlaneSink ---
// SPEC-CLASS: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4707
//
// 14 pure-virtual append_* methods + 1 pure-virtual recover_control_plane =
// 15 pure-virtual declarations total (the destructor below is defaulted, not
// pure virtual, and does not add to that count). See docs/SPEC_INVARIANTS.md's
// durable-audit-log entry for why DurableAuditSink deliberately does NOT
// inherit from this class.

class DurableControlPlaneSink {
public:
    virtual ~DurableControlPlaneSink() = default;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4710
    // Round-31/32: sink MUST reject append_rate_freeze if
    //   freeze.conservative_wait_ms > 0 &&  // THIS frame's contribution
    //   (freeze.wait_generation == 0  // wait-bearing frames must be explicit
    //    || prior folded wait_generation == UINT32_MAX  // overflow fence
    //    || freeze.wait_generation != prior_folded_wait_generation + 1);
    // and if freeze.conservative_wait_ms == 0 &&
    //   freeze.wait_generation != prior_folded_wait_generation (must copy-forward).
    // New episode (no prior): wait_generation must be 1 if wait>0, else 0.
    // Legacy-wait migration (prior max==0, wait>0): first post-upgrade
    // wait-bearing frame MUST be wait_generation==1 (same rule as new episode
    // step from prior 0).
    // On Ack of a frame that advances wait_generation: any in-process
    // arm_ack_steady for a prior generation is immediately invalid (must re-Arm).
    // LIVE OWNER ONLY -- compaction MUST NOT call this for retained aggregates.
    virtual AuditAppendResult append_rate_freeze(
        const RateLimitFreezePayload& freeze,
        FrameTimeKind time_kind) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4759
    // Round-33/34 P0 -- compaction-only folded freeze into non-CURRENT gen-N+1.
    // Writes DurableRecordType::RateLimitFreezeSnapshot.
    // Sink MUST:
    //  - reject unless compaction session is active for target generation
    //    == CURRENT+1 (writing only into gen-N+1/ while CURRENT still names N);
    //  - reject if source_generation == UINT32_MAX (no target+1) -- fence +
    //    offline store migration required (round-34 P1);
    //  - reject if proof.target_generation != source_generation + 1;
    //  - reject if proof.source_* tip does not equal the session's
    //    CompactionSourceBaseline (and that baseline must still match live
    //    CURRENT tip -- caller re-verifies after yields; sink may re-read);
    //  - reject if proof.source_generation / tip MAC / folded fields do not
    //    match the compaction retain scan (bind fold evidence);
    //  - accept wait_generation == proof.folded_wait_generation (including
    //    values > 1 and legacy-seal 1) -- NO prior_folded+1 check;
    //  - treat payload.conservative_wait_ms / deadline_utc_ms / wait_generation
    //    as FOLDED snapshot values (not live event contributions);
    //  - NOT touch arm_ack_steady / reserved probe state;
    //  - reject if invoked by the live owner actor outside compaction.
    struct CompactionFreezeSnapshotProof {
        std::uint32_t source_generation{0};
        std::uint64_t source_tip_seq{0};        // CompactionSourceBaseline tip
        std::uint8_t source_tip_mac[32]{};
        std::uint32_t source_key_id{0};
        std::uint32_t target_generation{0};     // MUST be source+1; refuse at MAX
        std::uint32_t freeze_epoch{0};
        std::int64_t folded_deadline_utc_ms{0};
        std::int64_t folded_conservative_wait_ms{0};
        std::uint32_t folded_wait_generation{0};
        std::uint8_t folded_source{0};
    };
    virtual AuditAppendResult append_compacted_freeze_snapshot(
        const RateLimitFreezePayload& folded_snapshot,
        FrameTimeKind time_kind,
        const CompactionFreezeSnapshotProof& proof) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4789
    // Round-33/34/36 -- compaction-only SINGLE frame CompactedFreezeWaitEvidence.
    // Payload is CompactedFreezeWaitEvidencePayload ONLY -- never a pair of
    // live Arm/Satisfy record types. Sink MUST:
    //  - same compaction-session / non-CURRENT / baseline-tip gates as snapshot;
    //  - reject source_generation == UINT32_MAX / target != source+1;
    //  - require evidence.wait_generation > 0 and == folded snapshot gen;
    //  - require satisfy_arm_frame_seq == source_arm_seq;
    //  - require evidence.source_baseline_{tip_seq,tip_mac,key_id} ==
    //    CompactionSourceBaseline / proof.source_* (round-36 key_id on frame);
    //  - for legacy path-3: require source_arm_seq and source_satisfy_seq both
    //    strictly > every wait-bearing live RateLimitFreeze in source epoch
    //    at baseline scan time, bound matches folded wait;
    //  - for ordinary retain: require pair was current-gen evidence in scan;
    //  - NOT create/update arm_ack_steady; NOT re-validate steady elapsed;
    //  - reject live-owner calls outside compaction.
    struct CompactionWaitEvidenceProof {
        std::uint32_t source_generation{0};
        std::uint64_t source_tip_seq{0};
        std::uint8_t source_tip_mac[32]{};
        std::uint32_t source_key_id{0};
        std::uint32_t target_generation{0};
        std::uint32_t freeze_epoch{0};
        std::uint32_t wait_generation{0};       // explicit; > 0
        bool legacy_sequence_proven_rewrite{false};
    };
    virtual AuditAppendResult append_compacted_wait_evidence(
        const CompactedFreezeWaitEvidencePayload& evidence,
        FrameTimeKind time_kind,
        const CompactionWaitEvidenceProof& proof) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4794
    virtual AuditAppendResult append_snapshot(
        const SymbolRegistrySnapshotPayload& snap,
        std::span<const SymbolRules> entries) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4797
    virtual AuditAppendResult append_weight_config(
        const EndpointWeightConfig& cfg) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4799
    virtual AuditAppendResult append_usage_snapshot(
        const RateLimitUsageSnapshotPayload& usage) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4803
    // Appends GenerationBridge into the *new* generation before CURRENT flips
    // (§10.2 / L5 §6.1.3). Not usable as a general post-refuse recovery write.
    virtual AuditAppendResult append_generation_bridge(
        const GenerationBridgePayload& bridge) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4807
    // Post-admission audit mirror ONLY -- never the admission evidence itself
    // (that lives in OperatorOverrideSidecar). Called once the sink is open.
    virtual AuditAppendResult append_operator_override(
        const OperatorOverridePayload& ov) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4845
    // NEW -- fixes this round's P0: §7.3.1's FreezeProbeCredit claimed a
    // durable, recoverable attempt count backed by this write, but no
    // DurableControlPlaneSink method for it ever existed -- the only append
    // path available was L5's DurableAuditSink::append_durable(), which
    // would have reintroduced the exact L4-depends-on-L5 cycle round 9
    // closed (§7.3.1's probe runs on L4-only paths too -- account refresh,
    // startup baseline -- that have no L5 DurableAuditSink to call at all).
    // This method is the correct, L4-owned home for it, alongside every
    // other control-plane append above.
    // Round-26/27/28 -- FreezeProbeAttempt. Sink MUST:
    //  - reject attempt.cleared == true;
    //  - reject if freeze_epoch is not the single active uncleared epoch;
    //  - reject if attempt_ordinal != (max durable ordinal for that epoch) + 1
    //    (no gaps, no duplicates, no rewind);
    //  - reject if folded deadline_utc_ms > 0 and
    //    attempt.not_before_utc_ms < folded deadline_utc_ms;
    //  - reject if attempt_ordinal == 0 or > kMaxTotalAttempts;
    //  - NEW (round-30/31/32 P0): reject if folded conservative_wait_ms > 0
    //    and there is no already-Acked, sink-verified FreezeWaitSatisfied for
    //    this epoch with bound_conservative_wait_ms == folded wait AND
    //    wait_generation == folded wait_generation AND wait_generation > 0
    //    (stale-generation Satisfy after equal-wait rematch MUST reject --
    //    round-31; legacy gen 0/missing MUST reject -- round-32);
    //    also reject while legacy-wait (folded wait>0 && folded gen==0) --
    //    migration seal required before any probe attempt;
    //    try_reserve_probe(..., bool wait_ok) remains a FAST-PATH hint only;
    //  - accept purpose in {DeadlineOrVerify, ClockRepublishOrVerify}; both
    //    burn the same 8-cap (round-28/29 -- neither is a free lane);
    //  - reject ClockRepublishOrVerify if a trustworthy clock is already
    //    published (that purpose is only for now_utc-absent re-publish+verify);
    //  - on Ack: return AuditAppendResult with .sequence set; caller MUST
    //    pass that full result into confirm_attempt_acked before any /time
    //    send (round-27 P1). sequence==0 is legal (generation genesis).
    //  - Note: FreezeClear{ProbeVerified} after ClockRepublishOrVerify is
    //    legal when proof+deadline+WaitSatisfied(current gen) hold
    //    (round-29/31) -- append path is append_freeze_clear, not this method.
    virtual AuditAppendResult append_freeze_probe_attempt(
        const FreezeProbeAttemptPayload& attempt,
        FrameTimeKind time_kind) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4871
    // Round-22/25/26/31 -- terminal-clear path. Sink MUST:
    //  - reject attempt.cleared==true on append_freeze_probe_attempt;
    //  - reject ProbeVerified/ConservativeWaitCompleted if folded source==2;
    //  - ProbeVerified: verify FreezeTimeProbeProof (extended MAC domain,
    //      including proof.tls_verified_host in allowlist + MAC domain);
    //    if folded conservative_wait_ms > 0: REQUIRE an already-Acked
    //      sink-verified FreezeWaitSatisfied for this epoch with
    //      bound_conservative_wait_ms == folded wait AND
    //      wait_generation == folded wait_generation AND wait_generation > 0
    //      (required-present at clear Ack -- do NOT mark "consumed" before
    //      clear succeeds; a Failed clear must leave WaitSatisfied reusable
    //      for this gen); reject clear while legacy-wait (folded gen==0);
    //      equality of FreezeClearPayload.bound_conservative_wait_ms alone
    //      is NOT sufficient; stale-generation / legacy-gen Satisfy is NOT
    //      sufficient (round-25/26/31/32);
    //    clear.bound_wait_generation MUST equal folded wait_generation when
    //      wait>0; MUST be 0 when wait==0;
    //    if conservative_wait_ms == 0: WaitSatisfied not required;
    //  - ConservativeWaitCompleted ONLY when folded deadline_utc_ms == 0;
    //    if conservative_wait_ms > 0: SAME generation-bound WaitSatisfied
    //      requirement (round-26/31 -- not optional);
    //  - OperatorAuthorized: published clock + KEK MAC + tip bind + nonce;
    //  - Ack before in-memory lift / release_probe clear.
    virtual AuditAppendResult append_freeze_clear(
        const FreezeClearPayload& clear,
        FrameTimeKind time_kind) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4886
    // Round-26/31/32 P0 -- arm a conservative wait. Sink MUST:
    //  - reject if no active freeze for freeze_epoch;
    //  - reject if bound_conservative_wait_ms != folded wait;
    //  - reject if folded wait > 0 && wait_generation == 0 (gen 0 illegal
    //    whenever a wait is active -- blocks Arm-before-migration on legacy-wait);
    //  - reject if wait_generation != folded wait_generation;
    //    (legacy-wait folded gen==0: Arm impossible until migration seals gen>=1);
    //  - reject if source==2;
    //  - on Ack: record process-local arm_ack_steady[epoch] = steady_now,
    //    arm_frame_seq, arm_ordinal, bound, wait_generation (OVERWRITE prior
    //    incomplete arm when generation or arm_ordinal advances);
    //  - NOT clear the episode.
    virtual AuditAppendResult append_freeze_wait_arm(
        const FreezeWaitArmPayload& arm,
        FrameTimeKind time_kind) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4903
    // Round-25/26/31/32 P0 -- non-terminal wait evidence. Sink MUST:
    //  - reject if no active freeze for freeze_epoch;
    //  - reject if bound_conservative_wait_ms != folded wait;
    //  - reject if folded wait > 0 && wait_generation == 0;
    //  - reject if wait_generation != folded wait_generation (stale gen);
    //  - reject if source==2 (permanent has no timed wait to satisfy);
    //  - reject if no live-session Arm for (epoch, arm_ordinal, wait_generation)
    //    whose arm_frame_seq matches payload.arm_frame_seq;
    //  - reject unless (steady_now_at_satisfy_ack - arm_ack_steady) >= bound
    //    AND elapsed_steady_ms_claimed >= bound;
    //  - NOT clear the episode / NOT set out_has_freeze false;
    //  - after wait_generation advances, prior WaitSatisfied is insufficient
    //    even if bound ms is equal (round-31 equal-wait rematch);
    //  - legacy gen-0 Satisfy is never acceptable as current evidence (round-32).
    virtual AuditAppendResult append_freeze_wait_satisfied(
        const FreezeWaitSatisfiedPayload& wait,
        FrameTimeKind time_kind) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4912
    // NEW -- freeze_epoch durable high-water: next allocatable epoch.
    // Ack watermark with next=E+1 BEFORE using epoch E (round-21 P0).
    // Only for NEW episodes (round-22); merges skip this.
    // Never derive by scanning (compaction may drop resolved freeze/probe
    // history). time_kind REQUIRED (round-20) -- Phase-B episodes use
    // UnknownBootstrap matching the freeze that follows.
    virtual AuditAppendResult append_freeze_epoch_watermark(
        std::uint32_t next_freeze_epoch,
        FrameTimeKind time_kind) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4936
    // Round-40...46 -- exactly-once seal-journal APPLY as ONE durable frame.
    // Call shape: SealJournalAppliedView ONLY (time_* + baseline + kek_key_id).
    // FORBIDDEN: separate FrameTimeKind arg; FORBIDDEN: MAC-omitted baseline;
    // FORBIDDEN: opaque-only entry_mac compare without HY-SEALJRN-v1 recompute.
    // Ambient: CURRENT store_uuid + KEK table (same class as SealExportStarted).
    // Sink MUST on EVERY call (see SealJournalAppliedView's own definition,
    // above, for the full field list):
    //  - allowlist + size + schema + FrameTimeKind rules;
    //  - MUST recompute HY-SEALJRN-v1 over ambient store_uuid + view fields;
    //    view.entry_mac != expected -> Corrupt; zero business effects;
    //  - index {candidate_id, journal_seq} ONLY; present -> byte-equal stored
    //    Applied authenticated fields + same entry_mac -> idempotent Ack;
    //    else Corrupt; absent -> one framed apply + Ack;
    //  - store frame MAC is NOT a substitute for entry_mac recompute;
    //  - refuse naked append_* of embedded payload as journal apply;
    //  - span valid until return.
    // Caller / recovery: §10.1 baseline + wire MAC-valid before call.
    // Compaction: while any seal-journal entry for candidate_id remains,
    // retain every SealJournalApplied for that candidate (or refuse
    // compaction). Retained Applied MUST keep baseline 4-tuple + kek_key_id
    // + entry_mac intact until the frame is dropped entirely.
    //
    // SealJournalAppliedView is now a real, complete type (seal-journal Round
    // A, 轨道 C) -- this method is genuinely callable, and the sink-interface
    // test exercises it like every other method on this class.
    virtual AuditAppendResult append_seal_journal_apply(
        const SealJournalAppliedView& apply) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4972
    // Recovery (round-22/25/26/27/31/32):
    //  - out_has_freeze / out_active_freeze = folded active state (§7.3.1)
    //    including folded wait_generation (raw max; 0 means legacy/unspecified
    //    when wait>0 -- see legacy-wait migration, do NOT synthesize match)
    //  - out_permanent_latch / out_uncleared_epoch_count as before
    //  - out_has_wait_satisfied = true ONLY if FreezeWaitSatisfied OR
    //    CompactedFreezeWaitEvidence (single CompactedFreezeWaitEvidencePayload)
    //    matches binding epoch AND wait_generation == folded wait_generation
    //    AND wait_generation > 0 (legacy gen 0/missing -> always false)
    //  - CompactedFreezeWaitEvidence counts as Satisfy for recovery gates
    //    but does NOT restore arm_ack_steady (if compacted Satisfy present for
    //    current gen, skip re-wait -- same as live Satisfy Acked in prior process)
    //  - legacy-wait (wait>0 && folded gen==0): out_has_wait_satisfied=false;
    //    caller MUST migration/compaction-seal before probe/clear (§7.3.1)
    //  - out_has_wait_arm = latest FreezeWaitArm for binding epoch+generation
    //    WITHOUT a subsequent matching WaitSatisfied -- informational only;
    //    live process MUST re-Arm if no current-gen Satisfy (prior arm_ack_steady
    //    is gone across restart); legacy Arm never counts as live-session Arm
    //  - After return, caller MUST
    //      credit.restore_from_recovery(
    //          out_has_freeze ? out_active_freeze.freeze_epoch : 0,
    //          {out_freeze_probe_attempts.data(),
    //           out_freeze_probe_attempt_count},
    //          out_has_freeze ? out_active_freeze.deadline_utc_ms : 0,
    //          steady_now, now_utc_ms_or_nullopt);
    //    before any try_reserve_probe (round-25/27). folded deadline is
    //    REQUIRED so multi-day UTC bans survive crash without short-backoff
    //    probe storms.
    //  - After a NEW episode's watermark+freeze Ack:
    //      credit.bind_new_episode_after_durable_create(
    //          epoch, payload.deadline_utc_ms);
    //  - After legacy-wait migration freeze Ack (round-32): treat like a
    //    wait-generation advance -- no Satisfy yet; Arm under gen 1.
    virtual RecoveryScanStatus recover_control_plane(
        RateLimitFreezePayload& out_active_freeze,
        bool& out_has_freeze,
        bool& out_permanent_latch,
        std::uint8_t& out_uncleared_epoch_count,
        EndpointWeightConfig& out_weights,
        bool& out_has_weights,
        RateLimitUsageSnapshotPayload& out_usage,
        bool& out_has_usage,
        GenerationBridgePayload& out_bridge,
        bool& out_has_bridge,
        std::uint32_t& out_next_freeze_epoch,
        bool& out_has_freeze_epoch_watermark,
        std::array<FreezeProbeAttemptPayload, 8>& out_freeze_probe_attempts,
        std::size_t& out_freeze_probe_attempt_count,
        FreezeClearPayload& out_latest_clear,
        bool& out_has_clear,
        FreezeWaitSatisfiedPayload& out_wait_satisfied,
        bool& out_has_wait_satisfied,
        FreezeWaitArmPayload& out_wait_arm,       // NEW round-26
        bool& out_has_wait_arm) noexcept = 0;     // NEW round-26
};
static_assert(std::is_abstract_v<DurableControlPlaneSink>);
static_assert(std::has_virtual_destructor_v<DurableControlPlaneSink>);
static_assert(std::is_trivially_copyable_v<DurableControlPlaneSink::CompactionFreezeSnapshotProof>);
static_assert(std::is_standard_layout_v<DurableControlPlaneSink::CompactionFreezeSnapshotProof>);
static_assert(std::is_trivially_copyable_v<DurableControlPlaneSink::CompactionWaitEvidenceProof>);
static_assert(std::is_standard_layout_v<DurableControlPlaneSink::CompactionWaitEvidenceProof>);

// --- OperatorOverrideSidecar ---
// SPEC-CLASS: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4999
//
// Sidecar-only API (same header, separate class) -- round-12 P0. Lives beside
// the breadcrumb path, OUTSIDE the store root. Does not require the
// hash-chained log to be open for append. Offline operator tool + recovery
// admission both use this; the running fenced L5 process does not.

class OperatorOverrideSidecar {
public:
    virtual ~OperatorOverrideSidecar() = default;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:5006
    // Creates final nonce-named evidence with OS no-replace semantics
    // (POSIX linkat/renameat2(RENAME_NOREPLACE); Windows CREATE_NEW). A
    // replace-capable rename is forbidden. Canonical bytes + file and parent
    // durability barriers are required; an existing nonce is a hard failure.
    virtual AuditAppendResult write_override(
        const OperatorOverridePayload& ov) noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:5008
    virtual bool read_override(OperatorOverridePayload& out) const noexcept = 0;

    // SPEC-METHOD: docs/BINANCE_PRIVATE_REST_L4_SPEC.md:5012
    // Atomically creates an immutable nonce-named consumed tombstone outside
    // the store (CREATE_NEW/no-replace, MAC-protected, fsynced). It is never
    // deleted by compaction. Recovery rejects a nonce with any tombstone.
    virtual AuditAppendResult consume_after_successful_admission(
        std::uint64_t nonce) noexcept = 0;
};
static_assert(std::is_abstract_v<OperatorOverrideSidecar>);
static_assert(std::has_virtual_destructor_v<OperatorOverrideSidecar>);

}  // namespace hy
