// SPDX-License-Identifier: proprietary
// intent_phase_advancer.hpp -- IntentPhaseAdvancer: the receipt-gated
// Building->Reserved phase raise (docs/SPEC_INVARIANTS.md's "Seal-journal
// Round E receipt + raise_intent_phase() (Building->Reserved)" entry), the
// Reserved->StartedPublished raise (docs/SPEC_INVARIANTS.md's "Seal-journal
// Round E Reserved->StartedPublished (NativeV2Started)" entry),
// raise_started_published(), and the two terminal edges
// StartedPublished->{PostSealFinalizing, AbandonFinalizing}
// (docs/SPEC_INVARIANTS.md's "Seal-journal Round E StartedPublished->
// {PostSealFinalizing, AbandonFinalizing}" entry), raise_post_seal_
// finalizing() / raise_abandon_finalizing().
//
// Governance: L2 (real file I/O, via CandidateLease -- same governance class
// as compaction_intent_store.hpp's IntentStore).
//
// SCOPE, read this before trusting anything below (three rounds of
// adversarial review converged on this boundary -- see the ledger entry and
// the design scratch files it references for the full history):
//
// 1. This class implements Building->Reserved (`.x1` seq=1),
//    Reserved->StartedPublished (`.x1` seq=2, NativeV2Started/greenfield
//    path only -- raise_started_published()), and the two terminal edges
//    StartedPublished->PostSealFinalizing / StartedPublished->
//    AbandonFinalizing (`.x1` seq=3, NativeV2 path only --
//    raise_post_seal_finalizing() / raise_abandon_finalizing()).
//    MigratedV2Started (the V+M legacy-companion write path) does not exist
//    here. The two terminal edges' field-mapping tables were found to have
//    real bugs in round 1 of review (the Building->Reserved round) -- see
//    point 8 below for the corrected mapping and the three bugs it avoids
//    repeating; that finding is why this round exists as a dedicated,
//    re-derived design rather than carrying forward the rejected draft.
// 2. raise_intent_phase() takes ONLY build_nonce. It does not accept a bare
//    to_phase (forbidden by docs/SPEC_INVARIANTS.md's Round D/E boundary --
//    the whole reason `raise_intent_phase()` exists as a receipt-gated API
//    instead of a generic "REPLACE this Intent" call) and does not accept a
//    caller-supplied candidate_id/request_id either: since this round takes
//    no such parameter at all, candidate_id/request_id are derived, fresh,
//    every call, directly from the just-read SealIdWatermark receipt (see
//    "Where candidate_id/request_id come from" below) -- there is no
//    separately-obtained value for them to be compared against, so the
//    design docs' "watermark check must be strict == not >" property shows
//    up here as "always derive from the freshest read," not as an
//    independent cross-check of two separately-obtained numbers. This is a
//    deliberate, reviewed simplification of the design docs' original table
//    (which was written against an earlier draft where the ids WERE a
//    caller-supplied parameter) -- flagged explicitly rather than silently
//    reconciled, matching this codebase's documented preference for honest
//    scope statements over overclaiming (see point 4 below).
// 3. THIS ROUND IS TEST-ONLY. This class is not called from any production
//    call path. A candidate that reaches Reserved via raise_intent_phase()
//    today has NO follow-on code path in this repository that can clean it
//    up (.xgc / GC authorization is Round F, entirely absent from this
//    repo) -- calling this in a real environment permanently strands the
//    candidate directory and its allocated ids (the same "crash after
//    allocation permanently burns the id" class of irreversibility the spec
//    already documents elsewhere, just reachable via a different door).
//    raise_started_published() is a STRICTLY SHARPER version of the same
//    hazard, not just "one phase deeper": SealExportStartedWire's own
//    doc comment (durable_control_plane.hpp) states its "Presence forces
//    PostSeal recovery even when no local seal Ack was observed" -- a real
//    future PostSeal-recovery implementation (which does not exist in this
//    repo yet) encountering an L file this test-only call wrote would be
//    FORCED to treat it as proof a real seal attempt happened, even though
//    nothing real did. raise_post_seal_finalizing()/raise_abandon_
//    finalizing() are SHARPER STILL: a test-only call can now drive
//    Intent.phase all the way to a TERMINAL value (3 or 4), not just an
//    intermediate breadcrumb -- a future real PostSeal/Abandon recovery
//    implementation encountering that terminal phase would be forced to
//    treat it as proof the ENTIRE real cleanup procedure (ordered M/V/L or
//    A unlink, `.xgc` authorization, TipExportProducerResume) already ran to
//    completion, when in this repo nothing real ever did. Do not wire any of
//    these four functions into any real compaction-trigger flow until real
//    `.clr`/`.abd` authorization, `.xgc` CREATE/GC, and TipExportProducerResume
//    all exist.
// 4. Friend-isolation honesty (same finding as the breadcrumb-loader round,
//    repeated here because it applies to every new friend added since):
//    `friend class hy::IntentPhaseAdvancer;` in compaction_lease.hpp grants
//    this class compiler-level access to ALL of CandidateLease's private
//    members, not just the two write methods it actually calls
//    (write_x1_frame_no_replace / replace_intent_phase) -- C++ friendship is
//    class-scoped, not method-scoped. Keeping IntentPhaseAdvancer as its own
//    class (rather than folding it into IntentStore) is a code-review-
//    legibility choice ("reviewing this one class's method bodies tells you
//    everything it actually calls"), not a compiler-enforced isolation
//    boundary. IntentStore's own methods are entirely unaffected by this
//    class's existence -- see compaction_intent_store.hpp's own header
//    comment, which is still accurate.
// 5. Where candidate_id/request_id come from (Building->Reserved's whole
//    reason for existing): the L4 spec (quoted on CompactionCandidateIntentWire
//    in durable_control_plane.hpp) forbids raising Reserved "without durable
//    SealIdWatermark advance binding the same candidate_id/request_id," and
//    separately states the durable watermark advance is the SAME id-reserve
//    step as the `.x1`/Intent REPLACE that follows it (L4 spec: "Reserved:
//    after durable SealIdWatermark advance ... publish `.x1` seq=1 ... THEN
//    REPLACE Intent"), not a decoupled precondition satisfied by some other
//    actor beforehand. The first three landed rounds of this class (v1-v3,
//    docs/SPEC_INVARIANTS.md's "Seal-journal Round E receipt +
//    raise_intent_phase()" entry) assumed the opposite -- that watermark
//    advance was already-durable, external, out of scope -- and read
//    `next_candidate_id - 1` accordingly. That assumption was wrong (no code
//    path in this repo ever performed the advance, so the assumption was
//    never actually satisfiable) and is corrected here (docs/SPEC_INVARIANTS.md's
//    "Seal-journal Round E SealIdWatermark advance" entry): this class now
//    performs the watermark read-modify-write itself, as part of this same
//    call, and binds `candidate_id = next_candidate_id` / `request_id =
//    next_request_id` (the value AT the point of advance, not "minus one" --
//    the "-1" reading was only correct under the old, never-true
//    already-advanced assumption). `SealJournalCommitWatermark` CREATE_NEW
//    (the L4 spec's other Reserved-moment write, in a directory guarded by
//    the completely separate SealJournalStoreLease) is now folded in too
//    (docs/SPEC_INVARIANTS.md's "Seal-journal Round E SealJournalCommitWatermark
//    CREATE_NEW + two-lock coordination" entry) -- this class now requires
//    the caller to hold BOTH leases for the whole call. See
//    seal_journal_store_lease.hpp's header comment's LOCK ORDER section for
//    the acquisition-order rule this binds the caller (not this class) to.
// 6. raise_started_published()'s `started` parameter, and why this is a
//    genuinely different trust model than raise_intent_phase() (three
//    rounds of adversarial review converged on this boundary too --
//    docs/SPEC_INVARIANTS.md's "Seal-journal Round E Reserved->
//    StartedPublished (NativeV2Started)" entry): SealIdWatermark/`.jhw`'s
//    content is derived entirely INSIDE raise_intent_phase() from Intent
//    state this repo already has -- there is nothing for a caller to
//    supply. SealExportStartedWire's new_generation/new_final_seq/
//    new_final_tip_mac/content_root describe real, already-sealed
//    compaction output this repo has no execution engine to compute --
//    raise_started_published() cannot derive them, so `started` is a
//    caller-supplied parameter, checked (not trusted) against the Intent's
//    own baseline fields at Step 2 below, but never generated internally.
//    This is why this function's shape is closer to "commit already-
//    computed evidence" than "advance an internal counter" -- do not read
//    the two functions as the same pattern with different field names.
//    raise_post_seal_finalizing()/raise_abandon_finalizing() extend this
//    same "checked, not trusted, caller-must-already-have-produced-it"
//    trust model one step further, but with a twist: the evidence
//    (`SealStartedCleanupTombstoneWire`/`SealStartedAbandonWire`, `.clr`/
//    `.abd`) is not a call parameter at all -- these two functions read it
//    back off disk themselves (via the already-landed, read-only
//    `SealStartedCleanupTombstoneLoader`/`SealStartedAbandonLoader`), the
//    same way raise_started_published() reads back the seq=1 `.x1` frame a
//    prior call must have already published. Neither function ever writes
//    `.clr`/`.abd` -- this repo has no code path that can legitimately
//    produce a `.clr` already at phase==Authorized or an `.abd` already at
//    phase==ResumeAuthorized(6), because that requires the real (absent)
//    authorization procedure; test fixtures plant them directly via a raw
//    file write (see test_intent_phase_advancer.cpp), bypassing
//    CandidateLease entirely, same precedent as this file's own
//    plant_watermark() helper for SealIdWatermark.
// 7. walk_x1_chain_raw()'s TerminalBranchConflict check (compaction_intent_
//    codec.hpp) only fires when the supplied chain is exactly 3 frames --
//    for the 1-frame (Building->Reserved) and 2-frame (Reserved->
//    StartedPublished) chains, it is silently skipped, so `after.phase` is
//    NEVER cross-checked against the chain's actual terminal to_phase by
//    that shared function alone. This is NOT a bug to fix in
//    walk_x1_chain_raw itself -- IntentStore::inspect_x1_chain()
//    (compaction_intent_store.hpp) legitimately calls it with the REAL
//    on-disk Intent.phase against whatever `.x1` frames happen to exist,
//    and that phase can legitimately lag the chain during the documented
//    crash window between a `.x1` write landing and the following Intent
//    REPLACE (L4 spec: "recovery one-step lag completes REPLACE to
//    Reserved") -- forcing the check to run unconditionally would
//    misclassify that legitimate recovery window as corruption, with no
//    existing test to catch the regression. Every raise_*() function in
//    this class instead carries its OWN explicit `after.phase == <this
//    edge's target>` check, immediately after its own walk_x1_chain_raw
//    call -- compiler-unenforced discipline, not a shared-function
//    guarantee, same class of honest limitation as the LOCK ORDER comment
//    in seal_journal_store_lease.hpp. For the two 3-frame terminal edges
//    this round adds, walk_x1_chain_raw's own size()==3 TerminalBranchConflict
//    check DOES also fire (unlike the two shorter chains above), so the
//    explicit check in raise_post_seal_finalizing()/raise_abandon_
//    finalizing() is deliberately redundant with it -- kept for symmetry
//    with the other two functions' discipline, not because it is load-
//    bearing on its own for this particular edge.
// 8. walk_x1_chain_raw()'s ForeignBinding check (compaction_intent_codec.hpp)
//    compares store_uuid/kek_key_id/source_generation/target_generation/
//    baseline_tip_seq/baseline_tip_mac/baseline_key_id/build_nonce between
//    each `.x1` frame and the Intent it's validated against -- it NEVER
//    compares candidate_id/request_id. A `.x1` frame with a fully valid MAC
//    but a candidate_id/request_id that doesn't match the current Intent's
//    own (impossible under this class's own write paths, but not something
//    walk_x1_chain_raw itself rules out for an arbitrary on-disk frame) would
//    be silently accepted. raise_post_seal_finalizing()/raise_abandon_
//    finalizing() read back BOTH the seq=1 and seq=2 frames fresh (neither
//    was read by the call that's raising THIS edge), so this class closes
//    the gap explicitly for both, immediately after reading each frame back.
//    raise_started_published() reads back seq=1 the same way and carries the
//    identical exposure -- it is patched with the same explicit check as
//    part of this same round (two lines, no behavior change on any already-
//    passing test), rather than left as a latent gap in already-merged code
//    while only the new functions get the fix.

#pragma once

#include <hengyuan/compaction_intent_codec.hpp>
#include <hengyuan/compaction_intent_store.hpp>
#include <hengyuan/compaction_lease.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/seal_id_watermark_export_started_loader.hpp>
#include <hengyuan/seal_journal_commit_tombstone_codec.hpp>
#include <hengyuan/seal_journal_precondition_codec.hpp>
#include <hengyuan/seal_journal_store_lease.hpp>
#include <hengyuan/seal_started_abandon_loader.hpp>
#include <hengyuan/seal_started_migration_cleanup_loader.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace hy {

enum class PhaseAdvanceStatus : std::uint8_t {
    PhaseRaised,                 // this call performed both durable writes (`.x1` then Intent
                                  // REPLACE); Intent.phase is now Reserved.
    PhaseRaiseUncertain,         // at least one of the two writes returned
                                  // PublishedNamespaceUncertain (Windows parent-directory-flush-
                                  // undecidable case) -- same platform-honest reporting as
                                  // GenesisStatus::CreatedUncertain, never silently promoted by a
                                  // same-process retry (see X1WriteProvenanceMemory below).
    AlreadyAtOrPastTargetPhase,  // the Intent for this build_nonce is already at or past Reserved
                                  // with legally-bound (nonzero) ids -- safe idempotent-retry
                                  // outcome; no write was attempted this call.
    LeaseNotHeld,
    CandidateFenced,
    DirectoryIdentityChanged,
    IntentNotFound,
    IntentCorrupt,
    IntentKeyNotFound,
    ForeignBuildNonce,           // an Intent exists in this candidate directory, but for a
                                  // different build_nonce than the caller's.
    WatermarkNotFound,
    WatermarkCorrupt,            // includes next_candidate_id/next_request_id == 0 -- 0 is never a
                                  // legal next-allocatable id, and decode_seal_id_watermark_wire()
                                  // already refuses to decode a watermark whose fields are 0
                                  // (seal_journal_precondition_codec.hpp's allocator-safety check),
                                  // surfacing here as SealIdWatermarkLoader's LoadStatus::Corrupt --
                                  // this class does not re-check nonzero-ness itself (would be dead
                                  // code, unreachable past a successful load()).
    WatermarkKeyNotFound,
    WatermarkForeignStore,       // the watermark decoded and MAC-verified fine, but its store_uuid
                                  // doesn't match this Intent's -- there is no id this call can
                                  // legally bind against a watermark for a different store.
    WatermarkExhausted,          // next_candidate_id or next_request_id is already UINT64_MAX --
                                  // advancing would overflow to 0, which is never a legal
                                  // next-allocatable id (L4 spec: "At UINT64_MAX -> fence").
    WatermarkAdvanceFailed,      // the watermark CREATE_NEW/REPLACE returned NotPublished. Includes
                                  // the narrow bootstrap-race case (this call saw NotFound, but a
                                  // different process's watermark write raced in with DIFFERENT
                                  // content before this call's CREATE_NEW landed) -- the caller can
                                  // simply retry the whole raise_intent_phase() call; the retry's
                                  // fresh read will see the real, now-existing watermark and take
                                  // the normal (non-bootstrap) path.
    SealJournalStoreLeaseNotHeld,
    SealJournalStoreLeaseFenced,
    SealJournalStoreLeaseDirectoryIdentityChanged,
    JhwWriteFailed,               // the `.jhw` CREATE_NEW returned NotPublished (a real conflict --
                                  // see JhwWriteProvenanceMemory's comment for why this should be
                                  // unreachable given WatermarkAdvanceProvenanceMemory already
                                  // guarantees a deterministic candidate_id per build_nonce).
    IllegalTransition,           // validate_intent_transition() rejected the constructed
                                  // before/after/`.x1` triple.
    X1WriteFailed,
    IntentReplaceFailed,

    // raise_started_published()/raise_post_seal_finalizing()/
    // raise_abandon_finalizing() share these two: Reserved (and everything
    // past it) should always have a durable seq=1 frame; missing/corrupt
    // means the state machine was tampered with or corrupted out-of-band,
    // not a scenario any of these calls can silently route around.
    SeqOneX1NotFound,             // read_x1_frame(build_nonce, 1, ...) returned NotFound.
    SeqOneX1Corrupt,              // seq=1 frame exists but failed MAC verification/decode, or its
                                  // candidate_id/request_id don't match the Intent's own (header
                                  // comment's point 8).

    // raise_started_published() only.
    StartedForeignBinding,       // `started`'s store_uuid/candidate_id/request_id/source_generation/
                                  // target_generation/baseline_*/kek_key_id don't match the Intent's
                                  // own fields -- see the header comment's point 6 for why these are
                                  // checked (not derived) fields.
    StartedShapeInvalid,         // validate_seal_export_started_shape(started) rejected it (topology).
    StartedWriteFailed,          // the seal-export-started CREATE_NEW returned NotPublished.

    // raise_post_seal_finalizing()/raise_abandon_finalizing() share these
    // three: both need the seq=2 frame too (a 3-frame chain), and both can
    // legally see the Intent already at either terminal phase.
    SeqTwoX1NotFound,             // read_x1_frame(build_nonce, 2, ...) returned NotFound.
    SeqTwoX1Corrupt,              // seq=2 frame exists but failed MAC verification/decode, or its
                                  // candidate_id/request_id don't match the Intent's own.
    AlreadyAtOppositeTerminalPhase,  // the Intent is already at the OTHER terminal phase (this call
                                  // is raise_post_seal_finalizing() but Intent.phase is already
                                  // AbandonFinalizing, or vice versa) -- a permanent, non-idempotent
                                  // refusal, NOT a safe retry outcome: the spec forbids a
                                  // PostSealFinalizing<->AbandonFinalizing cross unconditionally.

    // raise_post_seal_finalizing() only.
    ClrNotFound,                  // SealStartedCleanupTombstoneLoader::load() returned NotFound --
                                  // no `.clr` exists yet for this candidate.
    ClrCorrupt,                   // the loader returned Corrupt or IoError (MAC/shape/decode failure
                                  // or a genuine read failure -- both surface as this one status,
                                  // same "Corrupt is the honest umbrella" choice as IntentCorrupt).
    ClrKeyNotFound,                // the loader returned KeyNotFound (`.clr`'s kek_key_id isn't pinned).
    ClrForeignBinding,             // `.clr`'s store_uuid/candidate_id/request_id/kek_key_id/
                                  // source_generation/baseline_*/new_generation/started_kind don't
                                  // match this Intent's own fields (or started_kind != NativeV2 --
                                  // MigratedV2Started is out of scope this round).
    ClrNotAuthorized,             // `.clr`'s phase != kSealStartedCleanupPhaseAuthorized(0) -- exact
                                  // equality, not `>=` (round-1 review bug #2: `>=0` is always true).

    // raise_abandon_finalizing() only.
    AbdNotFound,                   // SealStartedAbandonLoader::load() returned NotFound.
    AbdCorrupt,                    // the loader returned Corrupt or IoError.
    AbdKeyNotFound,                 // the loader returned KeyNotFound.
    AbdForeignBinding,              // `.abd`'s store_uuid/candidate_id/request_id/kek_key_id/
                                  // source_generation/baseline_*/started_kind don't match this
                                  // Intent's own fields (`.abd` has no new_generation/new_final_*
                                  // fields at all -- do not reuse `.clr`'s binding list here).
    AbdNotResumeAuthorized,        // `.abd`'s phase != kSealStartedAbandonPhaseResumeAuthorized(6) --
                                  // exact equality, not `>= GenGone(5)` (round-1 review bug #3:
                                  // GenGone alone does not prove the tip was re-verified against the
                                  // abandon baseline; only ResumeAuthorized does).
};

// A decoded-and-MAC-verified SealIdWatermark, obtainable only by
// IntentPhaseAdvancer's own raise_intent_phase() (via SealIdWatermarkLoader,
// a real codec-backed loader) -- same "type proves verification happened"
// idiom as VerifiedCompactionCandidateIntent/VerifiedTransition
// (compaction_intent_codec.hpp), with one addition: this type is never
// exposed to a caller and never persists across calls (see the class
// comment's point 2 above and the header comment's SCOPE section 5) --
// receipt production and consumption happen inside the same
// raise_intent_phase() call, closing the TOCTOU window a cross-call receipt
// object would open (design docs' explicit reasoning for this choice).
class SealIdWatermarkAdvanceReceipt {
public:
    const SealIdWatermark& value() const noexcept { return value_; }

private:
    friend class IntentPhaseAdvancer;
    explicit SealIdWatermarkAdvanceReceipt(const SealIdWatermark& v) noexcept : value_(v) {}
    SealIdWatermark value_;
};

class IntentPhaseAdvancer {
public:
    // Both leases must already be acquire()'d by the caller, held for the
    // whole call -- this class never acquires/releases either. See
    // seal_journal_store_lease.hpp's header comment's LOCK ORDER section:
    // candidate_lease must have been acquired before seal_journal_lease.
    IntentPhaseAdvancer(CandidateLease& candidate_lease, SealJournalStoreLease& seal_journal_lease,
                         KeyRing& key_ring) noexcept
        : lease_(candidate_lease), seal_journal_lease_(seal_journal_lease), key_ring_(key_ring) {}

    // Building->Reserved. See the header comment's SCOPE section for the
    // full boundary (no to_phase/id parameters, test-only this round).
    PhaseAdvanceStatus raise_intent_phase(std::uint64_t build_nonce) noexcept;

    // Reserved->StartedPublished, NativeV2Started (greenfield) path only.
    // `started` is caller-supplied, not derived -- see the header comment's
    // point 6 for why this function's trust model differs from
    // raise_intent_phase()'s. Test-only this round, same as above.
    PhaseAdvanceStatus raise_started_published(std::uint64_t build_nonce,
                                                const SealExportStartedWire& started) noexcept;

    // StartedPublished->PostSealFinalizing (`.x1` seq=3), NativeV2 path only.
    // Requires a durable `.clr` already at phase==Authorized -- read back via
    // SealStartedCleanupTombstoneLoader, never written by this function (see
    // the header comment's point 6). Test-only this round, same as above.
    PhaseAdvanceStatus raise_post_seal_finalizing(std::uint64_t build_nonce) noexcept;

    // StartedPublished->AbandonFinalizing (`.x1` seq=3), NativeV2 path only.
    // Requires a durable `.abd` already at phase==ResumeAuthorized(6) -- read
    // back via SealStartedAbandonLoader, never written by this function.
    // `.abd` itself is never unlinked by this call (spec: "while A is still
    // on disk"). Test-only this round, same as above.
    PhaseAdvanceStatus raise_abandon_finalizing(std::uint64_t build_nonce) noexcept;

private:
    // Shared by raise_post_seal_finalizing()/raise_abandon_finalizing():
    // reads back the durable seq=1 and seq=2 `.x1` frames a prior
    // raise_intent_phase()/raise_started_published() call must already have
    // published, MAC-verifies each, and checks each frame's candidate_id/
    // request_id against `before`'s own (header comment's point 8 --
    // walk_x1_chain_raw's ForeignBinding check never compares these two
    // fields). raise_started_published()'s own single-frame Step 1 is left
    // as its own inline block (not refactored to call this) to minimize the
    // diff on already-shipped, already-tested code; it gets the equivalent
    // two-line fix inline instead. Returns true (out_seq1/out_seq2 valid) on
    // success; on failure returns false and sets out_failure_status to the
    // real PhaseAdvanceStatus to propagate -- deliberately NOT expressed as
    // "return a PhaseAdvanceStatus and use some value as a success sentinel,"
    // since every enumerator here is a real, reachable terminal status and
    // reusing one as a sentinel would be a footgun for any future call site.
    bool read_verified_seq1_and_seq2(std::uint64_t build_nonce, const CompactionCandidateIntentWire& before,
                                      CompactionIntentTransitionWire& out_seq1,
                                      CompactionIntentTransitionWire& out_seq2,
                                      PhaseAdvanceStatus& out_failure_status) noexcept;

    // Same reasoning, same shape, as IntentStore::genesis_provenance_memory_*
    // (compaction_intent_store.hpp) -- a same-process retry after
    // PublishedNamespaceUncertain must not "heal" into certainty just
    // because a fresh byte-equal read-back looks fine; that new evidence
    // carries no information about whether the parent-directory flush
    // actually happened. Keyed on (build_nonce, transition_seq,
    // encoded `.x1` bytes) rather than GenesisRequest, since `.x1` identity
    // is build_nonce+seq (ValidatedArtifactName::for_x1()), not
    // candidate_id/request_id (which don't exist yet at the point this
    // write happens). Deliberately in-memory-only -- a fresh process has no
    // such memory, and this class does not try to resolve that harder case,
    // same "platform-honest recovery boundary" IntentStore already accepts.
    struct X1WriteProvenanceMemory {
        std::uint64_t build_nonce{0};
        std::uint32_t transition_seq{0};
        std::array<std::byte, kCompactionIntentTransitionWireBytes> encoded_transition{};
        bool uncertain{false};
    };

    // Different shape from X1WriteProvenanceMemory on purpose (design v5, see
    // docs/SPEC_INVARIANTS.md's "Seal-journal Round E SealIdWatermark
    // advance" entry for the full reasoning): X1's memory only needs to
    // trigger on the *uncertain* outcome, because a definite success or
    // definite failure of that write is each individually safe to re-derive
    // fresh on retry. The watermark advance is not symmetric that way -- the
    // moment the CREATE_NEW/REPLACE returns DurablyPublished, the id is
    // spent (L4 spec: never reclaimed), full stop, regardless of what
    // happens to `.x1`/Intent REPLACE afterward. So this memory must be set
    // as soon as DurablyPublished is observed (not gated on "uncertain"),
    // and a same-process retry for the same build_nonce must reuse the
    // bound ids rather than re-deriving from a fresh watermark read --
    // otherwise every retry after a downstream (`.x1`/Intent) failure would
    // burn one more id pair for the same build_nonce, forever.
    struct WatermarkAdvanceProvenanceMemory {
        std::uint64_t build_nonce{0};
        std::uint64_t bound_candidate_id{0};
        std::uint64_t bound_request_id{0};
        bool advanced{false};
    };

    // Same shape as X1WriteProvenanceMemory, NOT WatermarkAdvanceProvenanceMemory: `.jhw` is a
    // no-replace CREATE_NEW (same primitive class as `.x1`), not a REPLACE-advance -- only the
    // *uncertain* outcome needs same-process memory, because content is fully deterministic per
    // retry (candidate_id is already pinned via WatermarkAdvanceProvenanceMemory by the time this
    // write happens, so a definite success or definite failure is each safe to re-derive fresh).
    // Keyed on (build_nonce, candidate_id, encoded `.jhw` bytes) rather than just build_nonce,
    // matching X1WriteProvenanceMemory's "compare the exact bytes, not just the identity" caution.
    struct JhwWriteProvenanceMemory {
        std::uint64_t build_nonce{0};
        std::uint64_t candidate_id{0};
        std::array<std::byte, kSealJournalCommitWatermarkWireBytes> encoded_watermark{};
        bool uncertain{false};
    };

    // Same shape/reasoning as JhwWriteProvenanceMemory: seal-export-started is a no-replace
    // CREATE_NEW whose content is fully deterministic once `started` is supplied by the caller
    // (Step 2 below either accepts it whole or rejects the whole call before any write is
    // attempted), so only the *uncertain* outcome needs same-process memory -- a definite success
    // or definite failure is each safe to re-derive fresh on retry. Keyed on (build_nonce, encoded
    // `started` bytes) -- there is no candidate_id-vs-content split here the way `.jhw`'s memory
    // has, because `started` itself already carries candidate_id.
    struct StartedWriteProvenanceMemory {
        std::uint64_t build_nonce{0};
        std::array<std::byte, kSealExportStartedWireBytes> encoded_started{};
        bool uncertain{false};
    };

    CandidateLease& lease_;
    SealJournalStoreLease& seal_journal_lease_;
    KeyRing& key_ring_;
    std::optional<X1WriteProvenanceMemory> x1_provenance_memory_{};
    std::optional<WatermarkAdvanceProvenanceMemory> watermark_provenance_memory_{};
    std::optional<JhwWriteProvenanceMemory> jhw_provenance_memory_{};
    std::optional<StartedWriteProvenanceMemory> started_provenance_memory_{};
};

inline PhaseAdvanceStatus IntentPhaseAdvancer::raise_intent_phase(std::uint64_t build_nonce) noexcept {
    // Step 0: fresh read + full MAC verification of the current Intent --
    // never trust a cached/prior-call value (ledger's "reverify current file
    // state" requirement). Mirrors IntentStore::load_and_validate_intent()'s
    // exact shape.
    CompactionCandidateIntentWire before{};
    {
        std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
        const LeaseReadResult read_result = lease_.read_intent_genesis(buf);
        switch (read_result.outcome) {
            case LeaseIoOutcome::WrongOwner:
            case LeaseIoOutcome::NotHeld:
                return PhaseAdvanceStatus::LeaseNotHeld;
            case LeaseIoOutcome::CandidateFenced:
                return PhaseAdvanceStatus::CandidateFenced;
            case LeaseIoOutcome::DirectoryIdentityChanged:
                return PhaseAdvanceStatus::DirectoryIdentityChanged;
            case LeaseIoOutcome::Ok:
                break;
        }
        switch (read_result.read.status) {
            case compaction_detail::ReadFixedStatus::NotFound:
                return PhaseAdvanceStatus::IntentNotFound;
            case compaction_detail::ReadFixedStatus::WrongSize:
            case compaction_detail::ReadFixedStatus::NotRegularFile:
            case compaction_detail::ReadFixedStatus::IoError:
                return PhaseAdvanceStatus::IntentCorrupt;
            case compaction_detail::ReadFixedStatus::Ok:
                break;
        }

        std::uint32_t kek_key_id_peek = 0;
        if (!peek_compaction_candidate_intent_kek_key_id(buf, kek_key_id_peek)) {
            return PhaseAdvanceStatus::IntentCorrupt;
        }
        const PinResult read_pin = key_ring_.pin_key(kek_key_id_peek);
        if (read_pin.status != PinStatus::Pinned) return PhaseAdvanceStatus::IntentKeyNotFound;

        std::optional<VerifiedCompactionCandidateIntent> verified;
        if (decode_compaction_candidate_intent_wire(buf, read_pin.handle->key_bytes(), verified) !=
            CompactionWireDecodeStatus::Ok) {
            return PhaseAdvanceStatus::IntentCorrupt;
        }
        before = verified->value();
    }

    if (before.build_nonce != build_nonce) return PhaseAdvanceStatus::ForeignBuildNonce;

    // Scenario B (idempotent retry after the Intent REPLACE already
    // succeeded, whichever process/attempt did it): this build_nonce is
    // globally unique per build attempt and permanently immutable once
    // genesis is written, so "phase already past Building, with legally-
    // bound ids, for THIS build_nonce" is sufficient proof this exact
    // request already completed -- no caller-supplied value to compare
    // against (see header comment's SCOPE section 2).
    if (before.phase != kCompactionCandidateIntentPhaseBuilding) {
        if (is_legal_candidate_ids_for_phase(before.phase, before.candidate_id, before.request_id)) {
            return PhaseAdvanceStatus::AlreadyAtOrPastTargetPhase;
        }
        return PhaseAdvanceStatus::IllegalTransition;
    }

    // Step 1: the SealIdWatermark advance itself (design v5 -- see the
    // header comment's SCOPE section 5). Reserved's normative moment is
    // "durable watermark advance, THEN `.x1`, THEN Intent REPLACE" as one
    // bound sequence, not "assume some other actor already advanced it."
    std::uint64_t candidate_id = 0;
    std::uint64_t request_id = 0;
    if (watermark_provenance_memory_.has_value() && watermark_provenance_memory_->advanced &&
        watermark_provenance_memory_->build_nonce == build_nonce) {
        // Same-process retry for this exact build_nonce, after a prior call
        // already durably advanced the watermark -- reuse those bound ids.
        // See WatermarkAdvanceProvenanceMemory's comment for why re-deriving
        // from a fresh read here (instead) would burn a second id pair.
        candidate_id = watermark_provenance_memory_->bound_candidate_id;
        request_id = watermark_provenance_memory_->bound_request_id;
    } else {
        SealIdWatermark watermark{};
        bool bootstrapping = false;
        {
            SealIdWatermarkLoader loader(lease_);
            const LoadStatus wm_status = loader.load(before.kek_key_id, key_ring_, watermark);
            switch (wm_status) {
                case LoadStatus::LeaseNotHeld:
                    return PhaseAdvanceStatus::LeaseNotHeld;
                case LoadStatus::CandidateFenced:
                    return PhaseAdvanceStatus::CandidateFenced;
                case LoadStatus::DirectoryIdentityChanged:
                    return PhaseAdvanceStatus::DirectoryIdentityChanged;
                case LoadStatus::NotFound:
                    // This store's first-ever candidate: no seal-id-watermark on disk
                    // yet. Bootstrap with the spec's documented defaults
                    // (SealIdWatermark's next_candidate_id/next_request_id both
                    // default-initialize to 1) and CREATE_NEW it below, in the same
                    // call that consumes the first id pair.
                    bootstrapping = true;
                    watermark = SealIdWatermark{};
                    watermark.store_uuid_lo = before.store_uuid_lo;
                    watermark.store_uuid_hi = before.store_uuid_hi;
                    watermark.next_candidate_id = 1;
                    watermark.next_request_id = 1;
                    break;
                case LoadStatus::Corrupt:
                case LoadStatus::IoError:
                    return PhaseAdvanceStatus::WatermarkCorrupt;
                case LoadStatus::KeyNotFound:
                    return PhaseAdvanceStatus::WatermarkKeyNotFound;
                case LoadStatus::Ok:
                    break;
            }
        }
        if (!bootstrapping &&
            (watermark.store_uuid_lo != before.store_uuid_lo || watermark.store_uuid_hi != before.store_uuid_hi)) {
            return PhaseAdvanceStatus::WatermarkForeignStore;
        }
        // No next_candidate_id/next_request_id == 0 check on the loaded (non-bootstrap) path:
        // SealIdWatermarkLoader::load() (via decode_seal_id_watermark_wire()) already refuses to
        // produce an Ok watermark with either field 0 -- that case surfaced as WatermarkCorrupt
        // above already. The bootstrap path constructs next_*=1 directly, never 0.

        const SealIdWatermarkAdvanceReceipt receipt(watermark);
        candidate_id = receipt.value().next_candidate_id;
        request_id = receipt.value().next_request_id;
        if (candidate_id == std::numeric_limits<std::uint64_t>::max() ||
            request_id == std::numeric_limits<std::uint64_t>::max()) {
            return PhaseAdvanceStatus::WatermarkExhausted;
        }

        SealIdWatermark advanced = watermark;
        advanced.next_candidate_id = candidate_id + 1;
        advanced.next_request_id = request_id + 1;

        const PinResult watermark_pin = key_ring_.pin_key(before.kek_key_id);
        if (watermark_pin.status != PinStatus::Pinned) return PhaseAdvanceStatus::IntentKeyNotFound;

        std::array<std::byte, kSealIdWatermarkWireBytes> encoded_watermark{};
        encode_seal_id_watermark_wire(encoded_watermark, advanced, watermark_pin.handle->key_bytes());

        const LeaseWriteResult watermark_write =
            bootstrapping ? lease_.create_seal_id_watermark_no_replace(encoded_watermark)
                          : lease_.replace_seal_id_watermark(encoded_watermark);
        switch (watermark_write.outcome) {
            case LeaseIoOutcome::WrongOwner:
            case LeaseIoOutcome::NotHeld:
                return PhaseAdvanceStatus::LeaseNotHeld;
            case LeaseIoOutcome::CandidateFenced:
                return PhaseAdvanceStatus::CandidateFenced;
            case LeaseIoOutcome::DirectoryIdentityChanged:
                return PhaseAdvanceStatus::DirectoryIdentityChanged;
            case LeaseIoOutcome::Ok:
                break;
        }
        switch (watermark_write.publish.state) {
            case compaction_detail::PublishCommitState::DurablyPublished:
                // Spent, regardless of what happens next this call -- record it before
                // doing anything else (see WatermarkAdvanceProvenanceMemory's comment).
                watermark_provenance_memory_ =
                    WatermarkAdvanceProvenanceMemory{build_nonce, candidate_id, request_id, true};
                break;
            case compaction_detail::PublishCommitState::PublishedNamespaceUncertain:
                // Not recorded as advanced -- unconfirmed durability carries no proof the
                // id was actually spent; a retry re-attempts this same step fresh, same
                // "platform-honest recovery boundary" as every other uncertain outcome in
                // this class.
                return PhaseAdvanceStatus::PhaseRaiseUncertain;
            case compaction_detail::PublishCommitState::NotPublished:
            default:
                return PhaseAdvanceStatus::WatermarkAdvanceFailed;
        }
    }

    // Step 1b: CREATE_NEW SealJournalCommitWatermark (`.jhw`) for this
    // candidate_id -- same id-reserve step as the watermark advance above
    // (L4 spec: "Reserved: after durable SealIdWatermark advance + CREATE_NEW
    // SealJournalCommitWatermark for this candidate_id"), in the completely
    // separate directory SealJournalStoreLease guards. Runs unconditionally
    // on every call reaching this point, including the same-process-retry
    // branch above (candidate_id is pinned by then either way), because a
    // retry after a downstream (`.jhw`/`.x1`/Intent) failure must still
    // ensure `.jhw` exists before proceeding -- CREATE_NEW's own
    // byte-compare-on-collision makes a repeat call for the SAME
    // candidate_id/content safe on its own (no id is spent twice by this
    // step; only WatermarkAdvanceProvenanceMemory's step above accounts for
    // spent ids).
    {
        SealJournalCommitWatermark jhw{};
        jhw.store_uuid_lo = before.store_uuid_lo;
        jhw.store_uuid_hi = before.store_uuid_hi;
        jhw.candidate_id = candidate_id;
        jhw.highest_committed_journal_seq = 0;
        jhw.kek_key_id = before.kek_key_id;

        const PinResult jhw_pin = key_ring_.pin_key(before.kek_key_id);
        if (jhw_pin.status != PinStatus::Pinned) return PhaseAdvanceStatus::IntentKeyNotFound;

        std::array<std::byte, kSealJournalCommitWatermarkWireBytes> encoded_jhw{};
        encode_seal_journal_commit_watermark_wire(encoded_jhw, jhw, jhw_pin.handle->key_bytes());

        if (jhw_provenance_memory_.has_value() && jhw_provenance_memory_->uncertain &&
            jhw_provenance_memory_->build_nonce == build_nonce &&
            jhw_provenance_memory_->candidate_id == candidate_id &&
            std::memcmp(jhw_provenance_memory_->encoded_watermark.data(), encoded_jhw.data(),
                         encoded_jhw.size()) == 0) {
            // Same-process retry of an attempt already known to be uncertain -- do not
            // re-derive/re-judge from a fresh read; a fresh byte-equal read-back would
            // carry no information about whether the parent-directory flush actually
            // happened (same reasoning as X1WriteProvenanceMemory).
            return PhaseAdvanceStatus::PhaseRaiseUncertain;
        }

        const SealJournalLeaseWriteResult jhw_write =
            seal_journal_lease_.create_seal_journal_commit_watermark_no_replace(candidate_id, encoded_jhw);
        switch (jhw_write.outcome) {
            case SealJournalLeaseIoOutcome::WrongOwner:
            case SealJournalLeaseIoOutcome::NotHeld:
                return PhaseAdvanceStatus::SealJournalStoreLeaseNotHeld;
            case SealJournalLeaseIoOutcome::StoreDirFenced:
                return PhaseAdvanceStatus::SealJournalStoreLeaseFenced;
            case SealJournalLeaseIoOutcome::DirectoryIdentityChanged:
                return PhaseAdvanceStatus::SealJournalStoreLeaseDirectoryIdentityChanged;
            case SealJournalLeaseIoOutcome::Ok:
                break;
        }
        switch (jhw_write.publish.state) {
            case seal_journal_store_detail::PublishCommitState::DurablyPublished:
                jhw_provenance_memory_.reset();
                break;
            case seal_journal_store_detail::PublishCommitState::PublishedNamespaceUncertain:
                jhw_provenance_memory_ = JhwWriteProvenanceMemory{build_nonce, candidate_id, encoded_jhw, true};
                return PhaseAdvanceStatus::PhaseRaiseUncertain;
            case seal_journal_store_detail::PublishCommitState::NotPublished:
            default:
                return PhaseAdvanceStatus::JhwWriteFailed;
        }
    }

    // Step 2: construct the proposed after-state + the `.x1` seq=1 evidence
    // that must be durable before the Intent REPLACE is allowed to happen.
    CompactionCandidateIntentWire after = before;
    after.phase = kCompactionCandidateIntentPhaseReserved;
    after.candidate_id = candidate_id;
    after.request_id = request_id;

    CompactionIntentTransitionWire transition{};
    transition.format_version = kCompactionIntentTransitionFormatVersion;
    transition.total_bytes = kCompactionIntentTransitionWireBytes;
    transition.store_uuid_lo = before.store_uuid_lo;
    transition.store_uuid_hi = before.store_uuid_hi;
    transition.kek_key_id = before.kek_key_id;
    transition.from_phase = kCompactionCandidateIntentPhaseBuilding;
    transition.to_phase = kCompactionCandidateIntentPhaseReserved;
    transition.reserved0 = 0;
    transition.transition_seq = 1;
    transition.source_generation = before.source_generation;
    transition.target_generation = before.target_generation;
    transition.baseline_tip_seq = before.baseline_tip_seq;
    std::memcpy(transition.baseline_tip_mac, before.baseline_tip_mac, sizeof(before.baseline_tip_mac));
    transition.baseline_key_id = before.baseline_key_id;
    transition.build_nonce = before.build_nonce;
    transition.candidate_id = candidate_id;
    transition.request_id = request_id;
    // prev_transition_mac stays all-zero (default-constructed) -- required
    // for transition_seq == 1 (walk_x1_chain_raw's MacChainBroken check).

    // Step 3: pure-function semantic validation, reusing the existing,
    // already-tested rule engine (compaction_intent_codec.hpp) rather than
    // reimplementing the ID-lifecycle/immutable-field/chain-shape rules
    // here. validate_intent_transition() hardcodes before.phase == Building,
    // which is exactly this edge's precondition (already checked above),
    // not a limitation this call needs to work around.
    const std::array<CompactionIntentTransitionWire, 1> chain{transition};
    if (!validate_intent_transition(before, after, chain)) {
        return PhaseAdvanceStatus::IllegalTransition;
    }

    // Step 4: publish `.x1` (no-replace), with the idempotent-retry guard
    // for the PublishedNamespaceUncertain case (v3 design fix -- see
    // X1WriteProvenanceMemory's comment).
    const PinResult write_pin = key_ring_.pin_key(before.kek_key_id);
    if (write_pin.status != PinStatus::Pinned) return PhaseAdvanceStatus::IntentKeyNotFound;

    std::array<std::byte, kCompactionIntentTransitionWireBytes> encoded_transition{};
    encode_compaction_intent_transition_wire(encoded_transition, transition, write_pin.handle->key_bytes());

    if (x1_provenance_memory_.has_value() && x1_provenance_memory_->uncertain &&
        x1_provenance_memory_->build_nonce == build_nonce &&
        x1_provenance_memory_->transition_seq == transition.transition_seq &&
        std::memcmp(x1_provenance_memory_->encoded_transition.data(), encoded_transition.data(),
                     encoded_transition.size()) == 0) {
        // Same-process retry of an attempt already known to be uncertain --
        // do not re-derive/re-judge from a fresh read; a fresh byte-equal
        // read-back would carry no information about whether the parent-
        // directory flush actually happened (v3's fix).
        return PhaseAdvanceStatus::PhaseRaiseUncertain;
    }

    const LeaseWriteResult x1_write =
        lease_.write_x1_frame_no_replace(build_nonce, transition.transition_seq, encoded_transition);
    switch (x1_write.outcome) {
        case LeaseIoOutcome::WrongOwner:
        case LeaseIoOutcome::NotHeld:
            return PhaseAdvanceStatus::LeaseNotHeld;
        case LeaseIoOutcome::CandidateFenced:
            return PhaseAdvanceStatus::CandidateFenced;
        case LeaseIoOutcome::DirectoryIdentityChanged:
            return PhaseAdvanceStatus::DirectoryIdentityChanged;
        case LeaseIoOutcome::Ok:
            break;
    }
    switch (x1_write.publish.state) {
        case compaction_detail::PublishCommitState::DurablyPublished:
            x1_provenance_memory_.reset();
            break;
        case compaction_detail::PublishCommitState::PublishedNamespaceUncertain:
            x1_provenance_memory_ = X1WriteProvenanceMemory{build_nonce, transition.transition_seq,
                                                              encoded_transition, true};
            return PhaseAdvanceStatus::PhaseRaiseUncertain;
        case compaction_detail::PublishCommitState::NotPublished:
        default:
            return PhaseAdvanceStatus::X1WriteFailed;
    }

    // Step 5: only now (the `.x1` is confirmed durable) REPLACE the Intent
    // to phase=Reserved. Never allowed to happen before step 4 confirms
    // durability -- see CompactionCandidateIntentWire's "Forbidden: Intent
    // REPLACE without prior durable matching `.x1`" invariant.
    std::array<std::byte, kCompactionCandidateIntentWireBytes> encoded_intent{};
    encode_compaction_candidate_intent_wire(encoded_intent, after, write_pin.handle->key_bytes());

    const LeaseWriteResult intent_write = lease_.replace_intent_phase(encoded_intent);
    switch (intent_write.outcome) {
        case LeaseIoOutcome::WrongOwner:
        case LeaseIoOutcome::NotHeld:
            return PhaseAdvanceStatus::LeaseNotHeld;
        case LeaseIoOutcome::CandidateFenced:
            return PhaseAdvanceStatus::CandidateFenced;
        case LeaseIoOutcome::DirectoryIdentityChanged:
            return PhaseAdvanceStatus::DirectoryIdentityChanged;
        case LeaseIoOutcome::Ok:
            break;
    }
    switch (intent_write.publish.state) {
        case compaction_detail::PublishCommitState::DurablyPublished:
            return PhaseAdvanceStatus::PhaseRaised;
        case compaction_detail::PublishCommitState::PublishedNamespaceUncertain:
            // No separate provenance-memory struct needed for this step
            // (unlike step 4): a retry re-reads the Intent fresh at the top
            // of the next call (scenario B above), and REPLACE has no
            // byte-compare-on-collision path that could silently upgrade
            // Uncertain into Durable the way the no-replace primitive can
            // -- see the header comment's point 2 discussion and the
            // ledger entry for why this asymmetry is safe.
            return PhaseAdvanceStatus::PhaseRaiseUncertain;
        case compaction_detail::PublishCommitState::NotPublished:
        default:
            return PhaseAdvanceStatus::IntentReplaceFailed;
    }
}

inline PhaseAdvanceStatus IntentPhaseAdvancer::raise_started_published(
    std::uint64_t build_nonce, const SealExportStartedWire& started) noexcept {
    // Step 0: fresh read + full MAC verification of the current Intent --
    // same shape as raise_intent_phase()'s Step 0, except the precondition
    // phase is Reserved, not Building.
    CompactionCandidateIntentWire before{};
    {
        std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
        const LeaseReadResult read_result = lease_.read_intent_genesis(buf);
        switch (read_result.outcome) {
            case LeaseIoOutcome::WrongOwner:
            case LeaseIoOutcome::NotHeld:
                return PhaseAdvanceStatus::LeaseNotHeld;
            case LeaseIoOutcome::CandidateFenced:
                return PhaseAdvanceStatus::CandidateFenced;
            case LeaseIoOutcome::DirectoryIdentityChanged:
                return PhaseAdvanceStatus::DirectoryIdentityChanged;
            case LeaseIoOutcome::Ok:
                break;
        }
        switch (read_result.read.status) {
            case compaction_detail::ReadFixedStatus::NotFound:
                return PhaseAdvanceStatus::IntentNotFound;
            case compaction_detail::ReadFixedStatus::WrongSize:
            case compaction_detail::ReadFixedStatus::NotRegularFile:
            case compaction_detail::ReadFixedStatus::IoError:
                return PhaseAdvanceStatus::IntentCorrupt;
            case compaction_detail::ReadFixedStatus::Ok:
                break;
        }

        std::uint32_t kek_key_id_peek = 0;
        if (!peek_compaction_candidate_intent_kek_key_id(buf, kek_key_id_peek)) {
            return PhaseAdvanceStatus::IntentCorrupt;
        }
        const PinResult read_pin = key_ring_.pin_key(kek_key_id_peek);
        if (read_pin.status != PinStatus::Pinned) return PhaseAdvanceStatus::IntentKeyNotFound;

        std::optional<VerifiedCompactionCandidateIntent> verified;
        if (decode_compaction_candidate_intent_wire(buf, read_pin.handle->key_bytes(), verified) !=
            CompactionWireDecodeStatus::Ok) {
            return PhaseAdvanceStatus::IntentCorrupt;
        }
        before = verified->value();
    }

    if (before.build_nonce != build_nonce) return PhaseAdvanceStatus::ForeignBuildNonce;

    if (before.phase != kCompactionCandidateIntentPhaseReserved) {
        // Past Reserved with legally-bound ids for this exact build_nonce is
        // a safe idempotent-retry outcome (same reasoning as
        // raise_intent_phase()'s Scenario B); Building (not yet Reserved) or
        // an illegal id/phase combination is not.
        if (before.phase > kCompactionCandidateIntentPhaseReserved &&
            is_legal_candidate_ids_for_phase(before.phase, before.candidate_id, before.request_id)) {
            return PhaseAdvanceStatus::AlreadyAtOrPastTargetPhase;
        }
        return PhaseAdvanceStatus::IllegalTransition;
    }

    // Step 1: read back the durable seq=1 `.x1` frame this Intent's own
    // Reserved raise must have already published -- required to construct
    // the 2-frame chain walk_x1_chain_raw needs (prev_transition_mac must
    // chain from the real seq=1 frame's mac, not be re-derived).
    CompactionIntentTransitionWire seq1{};
    {
        std::array<std::byte, kCompactionIntentTransitionWireBytes> buf{};
        const LeaseReadResult read_result = lease_.read_x1_frame(build_nonce, 1, buf);
        switch (read_result.outcome) {
            case LeaseIoOutcome::WrongOwner:
            case LeaseIoOutcome::NotHeld:
                return PhaseAdvanceStatus::LeaseNotHeld;
            case LeaseIoOutcome::CandidateFenced:
                return PhaseAdvanceStatus::CandidateFenced;
            case LeaseIoOutcome::DirectoryIdentityChanged:
                return PhaseAdvanceStatus::DirectoryIdentityChanged;
            case LeaseIoOutcome::Ok:
                break;
        }
        switch (read_result.read.status) {
            case compaction_detail::ReadFixedStatus::NotFound:
                return PhaseAdvanceStatus::SeqOneX1NotFound;
            case compaction_detail::ReadFixedStatus::WrongSize:
            case compaction_detail::ReadFixedStatus::NotRegularFile:
            case compaction_detail::ReadFixedStatus::IoError:
                return PhaseAdvanceStatus::SeqOneX1Corrupt;
            case compaction_detail::ReadFixedStatus::Ok:
                break;
        }

        const PinResult seq1_pin = key_ring_.pin_key(before.kek_key_id);
        if (seq1_pin.status != PinStatus::Pinned) return PhaseAdvanceStatus::IntentKeyNotFound;

        std::optional<VerifiedTransition> verified_seq1;
        if (decode_compaction_intent_transition_wire(buf, seq1_pin.handle->key_bytes(), verified_seq1) !=
            CompactionWireDecodeStatus::Ok) {
            return PhaseAdvanceStatus::SeqOneX1Corrupt;
        }
        seq1 = verified_seq1->value();

        // walk_x1_chain_raw()'s ForeignBinding check never compares
        // candidate_id/request_id (header comment's point 8) -- check them
        // explicitly here rather than trust a MAC-valid-but-foreign-ids frame.
        if (seq1.candidate_id != before.candidate_id || seq1.request_id != before.request_id) {
            return PhaseAdvanceStatus::SeqOneX1Corrupt;
        }
    }

    // Step 2: `started` is caller-supplied (header comment's point 6) --
    // checked, not trusted. First: every field this edge requires to bind to
    // THIS Intent's own baseline/id/generation identity. Second: topology
    // self-consistency (validate_seal_export_started_shape), independent of
    // any particular Intent.
    if (started.store_uuid_lo != before.store_uuid_lo || started.store_uuid_hi != before.store_uuid_hi ||
        started.candidate_id != before.candidate_id || started.request_id != before.request_id ||
        started.source_generation != before.source_generation ||
        started.new_generation != before.target_generation ||
        started.baseline_tip_seq != before.baseline_tip_seq ||
        std::memcmp(started.baseline_tip_mac, before.baseline_tip_mac, sizeof(before.baseline_tip_mac)) != 0 ||
        started.baseline_key_id != before.baseline_key_id || started.kek_key_id != before.kek_key_id) {
        return PhaseAdvanceStatus::StartedForeignBinding;
    }
    if (!validate_seal_export_started_shape(started)) {
        return PhaseAdvanceStatus::StartedShapeInvalid;
    }

    // Step 3: construct the proposed after-state (ids stay byte-unchanged --
    // this edge is not Building->Reserved) + the `.x1` seq=2 evidence.
    CompactionCandidateIntentWire after = before;
    after.phase = kCompactionCandidateIntentPhaseStartedPublished;

    CompactionIntentTransitionWire transition{};
    transition.format_version = kCompactionIntentTransitionFormatVersion;
    transition.total_bytes = kCompactionIntentTransitionWireBytes;
    transition.store_uuid_lo = before.store_uuid_lo;
    transition.store_uuid_hi = before.store_uuid_hi;
    transition.kek_key_id = before.kek_key_id;
    transition.from_phase = kCompactionCandidateIntentPhaseReserved;
    transition.to_phase = kCompactionCandidateIntentPhaseStartedPublished;
    transition.reserved0 = 0;
    transition.transition_seq = 2;
    transition.source_generation = before.source_generation;
    transition.target_generation = before.target_generation;
    transition.baseline_tip_seq = before.baseline_tip_seq;
    std::memcpy(transition.baseline_tip_mac, before.baseline_tip_mac, sizeof(before.baseline_tip_mac));
    transition.baseline_key_id = before.baseline_key_id;
    transition.build_nonce = before.build_nonce;
    transition.candidate_id = before.candidate_id;
    transition.request_id = before.request_id;
    std::memcpy(transition.prev_transition_mac, seq1.mac, sizeof(seq1.mac));

    // Step 4: pure-function semantic validation via the shared chain-walker,
    // called directly (not through validate_intent_transition(), which
    // hardcodes before.phase == Building and is Building->Reserved-only) --
    // plus this edge's own explicit terminal-phase check (header comment's
    // point 7: walk_x1_chain_raw only cross-checks the terminal to_phase
    // against Intent.phase for 3-frame chains; this is a 2-frame chain).
    {
        const std::array<CompactionIntentTransitionWire, 2> chain{seq1, transition};
        std::array<std::uint8_t, 32> terminal_mac{};
        if (compaction_codec_detail::walk_x1_chain_raw(after, chain, terminal_mac) != X1ChainStatus::Valid) {
            return PhaseAdvanceStatus::IllegalTransition;
        }
        if (after.phase != kCompactionCandidateIntentPhaseStartedPublished) {
            return PhaseAdvanceStatus::IllegalTransition;
        }
    }

    const PinResult write_pin = key_ring_.pin_key(before.kek_key_id);
    if (write_pin.status != PinStatus::Pinned) return PhaseAdvanceStatus::IntentKeyNotFound;

    // Step 5: publish the seal-export-started L file (no-replace), with the
    // idempotent-retry guard for the PublishedNamespaceUncertain case (same
    // pattern as JhwWriteProvenanceMemory -- see StartedWriteProvenanceMemory's
    // comment for why only the uncertain outcome needs memory here).
    std::array<std::byte, kSealExportStartedWireBytes> encoded_started{};
    encode_seal_export_started_wire(encoded_started, started, write_pin.handle->key_bytes());

    if (started_provenance_memory_.has_value() && started_provenance_memory_->uncertain &&
        started_provenance_memory_->build_nonce == build_nonce &&
        std::memcmp(started_provenance_memory_->encoded_started.data(), encoded_started.data(),
                     encoded_started.size()) == 0) {
        return PhaseAdvanceStatus::PhaseRaiseUncertain;
    }

    const LeaseWriteResult started_write =
        lease_.create_seal_export_started_no_replace(/*legacy_or_greenfield=*/true, encoded_started);
    switch (started_write.outcome) {
        case LeaseIoOutcome::WrongOwner:
        case LeaseIoOutcome::NotHeld:
            return PhaseAdvanceStatus::LeaseNotHeld;
        case LeaseIoOutcome::CandidateFenced:
            return PhaseAdvanceStatus::CandidateFenced;
        case LeaseIoOutcome::DirectoryIdentityChanged:
            return PhaseAdvanceStatus::DirectoryIdentityChanged;
        case LeaseIoOutcome::Ok:
            break;
    }
    switch (started_write.publish.state) {
        case compaction_detail::PublishCommitState::DurablyPublished:
            started_provenance_memory_.reset();
            break;
        case compaction_detail::PublishCommitState::PublishedNamespaceUncertain:
            started_provenance_memory_ = StartedWriteProvenanceMemory{build_nonce, encoded_started, true};
            return PhaseAdvanceStatus::PhaseRaiseUncertain;
        case compaction_detail::PublishCommitState::NotPublished:
        default:
            return PhaseAdvanceStatus::StartedWriteFailed;
    }

    // Step 6: publish `.x1` seq=2 (no-replace) -- only now that the L file
    // is confirmed durable. Reuses the existing write_x1_frame_no_replace /
    // X1WriteProvenanceMemory machinery unchanged (it is already generically
    // keyed on transition_seq, not hardcoded to seq=1).
    std::array<std::byte, kCompactionIntentTransitionWireBytes> encoded_transition{};
    encode_compaction_intent_transition_wire(encoded_transition, transition, write_pin.handle->key_bytes());

    if (x1_provenance_memory_.has_value() && x1_provenance_memory_->uncertain &&
        x1_provenance_memory_->build_nonce == build_nonce &&
        x1_provenance_memory_->transition_seq == transition.transition_seq &&
        std::memcmp(x1_provenance_memory_->encoded_transition.data(), encoded_transition.data(),
                     encoded_transition.size()) == 0) {
        return PhaseAdvanceStatus::PhaseRaiseUncertain;
    }

    const LeaseWriteResult x1_write =
        lease_.write_x1_frame_no_replace(build_nonce, transition.transition_seq, encoded_transition);
    switch (x1_write.outcome) {
        case LeaseIoOutcome::WrongOwner:
        case LeaseIoOutcome::NotHeld:
            return PhaseAdvanceStatus::LeaseNotHeld;
        case LeaseIoOutcome::CandidateFenced:
            return PhaseAdvanceStatus::CandidateFenced;
        case LeaseIoOutcome::DirectoryIdentityChanged:
            return PhaseAdvanceStatus::DirectoryIdentityChanged;
        case LeaseIoOutcome::Ok:
            break;
    }
    switch (x1_write.publish.state) {
        case compaction_detail::PublishCommitState::DurablyPublished:
            x1_provenance_memory_.reset();
            break;
        case compaction_detail::PublishCommitState::PublishedNamespaceUncertain:
            x1_provenance_memory_ = X1WriteProvenanceMemory{build_nonce, transition.transition_seq,
                                                              encoded_transition, true};
            return PhaseAdvanceStatus::PhaseRaiseUncertain;
        case compaction_detail::PublishCommitState::NotPublished:
        default:
            return PhaseAdvanceStatus::X1WriteFailed;
    }

    // Step 7: only now (the `.x1` is confirmed durable) REPLACE the Intent
    // to phase=StartedPublished.
    std::array<std::byte, kCompactionCandidateIntentWireBytes> encoded_intent{};
    encode_compaction_candidate_intent_wire(encoded_intent, after, write_pin.handle->key_bytes());

    const LeaseWriteResult intent_write = lease_.replace_intent_phase(encoded_intent);
    switch (intent_write.outcome) {
        case LeaseIoOutcome::WrongOwner:
        case LeaseIoOutcome::NotHeld:
            return PhaseAdvanceStatus::LeaseNotHeld;
        case LeaseIoOutcome::CandidateFenced:
            return PhaseAdvanceStatus::CandidateFenced;
        case LeaseIoOutcome::DirectoryIdentityChanged:
            return PhaseAdvanceStatus::DirectoryIdentityChanged;
        case LeaseIoOutcome::Ok:
            break;
    }
    switch (intent_write.publish.state) {
        case compaction_detail::PublishCommitState::DurablyPublished:
            return PhaseAdvanceStatus::PhaseRaised;
        case compaction_detail::PublishCommitState::PublishedNamespaceUncertain:
            return PhaseAdvanceStatus::PhaseRaiseUncertain;
        case compaction_detail::PublishCommitState::NotPublished:
        default:
            return PhaseAdvanceStatus::IntentReplaceFailed;
    }
}

inline bool IntentPhaseAdvancer::read_verified_seq1_and_seq2(
    std::uint64_t build_nonce, const CompactionCandidateIntentWire& before,
    CompactionIntentTransitionWire& out_seq1, CompactionIntentTransitionWire& out_seq2,
    PhaseAdvanceStatus& out_failure_status) noexcept {
    {
        std::array<std::byte, kCompactionIntentTransitionWireBytes> buf{};
        const LeaseReadResult read_result = lease_.read_x1_frame(build_nonce, 1, buf);
        switch (read_result.outcome) {
            case LeaseIoOutcome::WrongOwner:
            case LeaseIoOutcome::NotHeld:
                out_failure_status = PhaseAdvanceStatus::LeaseNotHeld;
                return false;
            case LeaseIoOutcome::CandidateFenced:
                out_failure_status = PhaseAdvanceStatus::CandidateFenced;
                return false;
            case LeaseIoOutcome::DirectoryIdentityChanged:
                out_failure_status = PhaseAdvanceStatus::DirectoryIdentityChanged;
                return false;
            case LeaseIoOutcome::Ok:
                break;
        }
        switch (read_result.read.status) {
            case compaction_detail::ReadFixedStatus::NotFound:
                out_failure_status = PhaseAdvanceStatus::SeqOneX1NotFound;
                return false;
            case compaction_detail::ReadFixedStatus::WrongSize:
            case compaction_detail::ReadFixedStatus::NotRegularFile:
            case compaction_detail::ReadFixedStatus::IoError:
                out_failure_status = PhaseAdvanceStatus::SeqOneX1Corrupt;
                return false;
            case compaction_detail::ReadFixedStatus::Ok:
                break;
        }

        const PinResult pin = key_ring_.pin_key(before.kek_key_id);
        if (pin.status != PinStatus::Pinned) {
            out_failure_status = PhaseAdvanceStatus::IntentKeyNotFound;
            return false;
        }

        std::optional<VerifiedTransition> verified;
        if (decode_compaction_intent_transition_wire(buf, pin.handle->key_bytes(), verified) !=
            CompactionWireDecodeStatus::Ok) {
            out_failure_status = PhaseAdvanceStatus::SeqOneX1Corrupt;
            return false;
        }
        out_seq1 = verified->value();
        // header comment's point 8: walk_x1_chain_raw never compares
        // candidate_id/request_id -- check explicitly here.
        if (out_seq1.candidate_id != before.candidate_id || out_seq1.request_id != before.request_id) {
            out_failure_status = PhaseAdvanceStatus::SeqOneX1Corrupt;
            return false;
        }
    }
    {
        std::array<std::byte, kCompactionIntentTransitionWireBytes> buf{};
        const LeaseReadResult read_result = lease_.read_x1_frame(build_nonce, 2, buf);
        switch (read_result.outcome) {
            case LeaseIoOutcome::WrongOwner:
            case LeaseIoOutcome::NotHeld:
                out_failure_status = PhaseAdvanceStatus::LeaseNotHeld;
                return false;
            case LeaseIoOutcome::CandidateFenced:
                out_failure_status = PhaseAdvanceStatus::CandidateFenced;
                return false;
            case LeaseIoOutcome::DirectoryIdentityChanged:
                out_failure_status = PhaseAdvanceStatus::DirectoryIdentityChanged;
                return false;
            case LeaseIoOutcome::Ok:
                break;
        }
        switch (read_result.read.status) {
            case compaction_detail::ReadFixedStatus::NotFound:
                out_failure_status = PhaseAdvanceStatus::SeqTwoX1NotFound;
                return false;
            case compaction_detail::ReadFixedStatus::WrongSize:
            case compaction_detail::ReadFixedStatus::NotRegularFile:
            case compaction_detail::ReadFixedStatus::IoError:
                out_failure_status = PhaseAdvanceStatus::SeqTwoX1Corrupt;
                return false;
            case compaction_detail::ReadFixedStatus::Ok:
                break;
        }

        const PinResult pin = key_ring_.pin_key(before.kek_key_id);
        if (pin.status != PinStatus::Pinned) {
            out_failure_status = PhaseAdvanceStatus::IntentKeyNotFound;
            return false;
        }

        std::optional<VerifiedTransition> verified;
        if (decode_compaction_intent_transition_wire(buf, pin.handle->key_bytes(), verified) !=
            CompactionWireDecodeStatus::Ok) {
            out_failure_status = PhaseAdvanceStatus::SeqTwoX1Corrupt;
            return false;
        }
        out_seq2 = verified->value();
        if (out_seq2.candidate_id != before.candidate_id || out_seq2.request_id != before.request_id) {
            out_failure_status = PhaseAdvanceStatus::SeqTwoX1Corrupt;
            return false;
        }
    }
    return true;
}

inline PhaseAdvanceStatus IntentPhaseAdvancer::raise_post_seal_finalizing(std::uint64_t build_nonce) noexcept {
    // Step 0: fresh read + full MAC verification of the current Intent.
    // Precondition phase is StartedPublished; two possible "already past"
    // terminal phases must be told apart (this edge's own target vs the
    // opposite, mutually-exclusive terminal edge -- spec forbids a
    // PostSealFinalizing<->AbandonFinalizing cross unconditionally).
    CompactionCandidateIntentWire before{};
    {
        std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
        const LeaseReadResult read_result = lease_.read_intent_genesis(buf);
        switch (read_result.outcome) {
            case LeaseIoOutcome::WrongOwner:
            case LeaseIoOutcome::NotHeld:
                return PhaseAdvanceStatus::LeaseNotHeld;
            case LeaseIoOutcome::CandidateFenced:
                return PhaseAdvanceStatus::CandidateFenced;
            case LeaseIoOutcome::DirectoryIdentityChanged:
                return PhaseAdvanceStatus::DirectoryIdentityChanged;
            case LeaseIoOutcome::Ok:
                break;
        }
        switch (read_result.read.status) {
            case compaction_detail::ReadFixedStatus::NotFound:
                return PhaseAdvanceStatus::IntentNotFound;
            case compaction_detail::ReadFixedStatus::WrongSize:
            case compaction_detail::ReadFixedStatus::NotRegularFile:
            case compaction_detail::ReadFixedStatus::IoError:
                return PhaseAdvanceStatus::IntentCorrupt;
            case compaction_detail::ReadFixedStatus::Ok:
                break;
        }

        std::uint32_t kek_key_id_peek = 0;
        if (!peek_compaction_candidate_intent_kek_key_id(buf, kek_key_id_peek)) {
            return PhaseAdvanceStatus::IntentCorrupt;
        }
        const PinResult read_pin = key_ring_.pin_key(kek_key_id_peek);
        if (read_pin.status != PinStatus::Pinned) return PhaseAdvanceStatus::IntentKeyNotFound;

        std::optional<VerifiedCompactionCandidateIntent> verified;
        if (decode_compaction_candidate_intent_wire(buf, read_pin.handle->key_bytes(), verified) !=
            CompactionWireDecodeStatus::Ok) {
            return PhaseAdvanceStatus::IntentCorrupt;
        }
        before = verified->value();
    }

    if (before.build_nonce != build_nonce) return PhaseAdvanceStatus::ForeignBuildNonce;

    if (before.phase != kCompactionCandidateIntentPhaseStartedPublished) {
        if (before.phase == kCompactionCandidateIntentPhasePostSealFinalizing &&
            is_legal_candidate_ids_for_phase(before.phase, before.candidate_id, before.request_id)) {
            return PhaseAdvanceStatus::AlreadyAtOrPastTargetPhase;
        }
        if (before.phase == kCompactionCandidateIntentPhaseAbandonFinalizing &&
            is_legal_candidate_ids_for_phase(before.phase, before.candidate_id, before.request_id)) {
            return PhaseAdvanceStatus::AlreadyAtOppositeTerminalPhase;
        }
        return PhaseAdvanceStatus::IllegalTransition;
    }

    // Step 1: read back the durable seq=1 and seq=2 `.x1` frames.
    CompactionIntentTransitionWire seq1{};
    CompactionIntentTransitionWire seq2{};
    {
        PhaseAdvanceStatus failure_status{};
        if (!read_verified_seq1_and_seq2(build_nonce, before, seq1, seq2, failure_status)) {
            return failure_status;
        }
    }

    // Step 2: read `.clr` back (never written by this function -- header
    // comment's point 6), checked not trusted.
    SealStartedCleanupTombstoneWire clr{};
    {
        SealStartedCleanupTombstoneLoader<CandidateLease> loader(lease_);
        const LoadStatus load_status = loader.load(key_ring_, clr);
        switch (load_status) {
            case LoadStatus::LeaseNotHeld:
                return PhaseAdvanceStatus::LeaseNotHeld;
            case LoadStatus::CandidateFenced:
                return PhaseAdvanceStatus::CandidateFenced;
            case LoadStatus::DirectoryIdentityChanged:
                return PhaseAdvanceStatus::DirectoryIdentityChanged;
            case LoadStatus::NotFound:
                return PhaseAdvanceStatus::ClrNotFound;
            case LoadStatus::Corrupt:
            case LoadStatus::IoError:
                return PhaseAdvanceStatus::ClrCorrupt;
            case LoadStatus::KeyNotFound:
                return PhaseAdvanceStatus::ClrKeyNotFound;
            case LoadStatus::Ok:
                break;
        }
    }
    // Note on clr.new_generation == before.target_generation: given
    // clr.source_generation == before.source_generation is ALSO checked
    // here, and validate_seal_started_cleanup_shape()'s own internal algebra
    // already forces clr.new_generation == clr.source_generation + 1 (any
    // record that doesn't satisfy this fails to decode at all), this
    // particular equality can never independently fail once the
    // source_generation check above already passed -- it is real defense-
    // in-depth against either upstream invariant being loosened later, not
    // something reachable as its own distinct failure mode today.
    if (clr.store_uuid_lo != before.store_uuid_lo || clr.store_uuid_hi != before.store_uuid_hi ||
        clr.candidate_id != before.candidate_id || clr.request_id != before.request_id ||
        clr.kek_key_id != before.kek_key_id || clr.source_generation != before.source_generation ||
        clr.baseline_tip_seq != before.baseline_tip_seq ||
        std::memcmp(clr.baseline_tip_mac, before.baseline_tip_mac, sizeof(before.baseline_tip_mac)) != 0 ||
        clr.baseline_key_id != before.baseline_key_id || clr.new_generation != before.target_generation ||
        clr.started_kind != kSealStartedKindNativeV2) {
        return PhaseAdvanceStatus::ClrForeignBinding;
    }
    if (clr.phase != kSealStartedCleanupPhaseAuthorized) {
        return PhaseAdvanceStatus::ClrNotAuthorized;
    }
    // Not re-calling validate_seal_started_cleanup_shape() here -- the
    // loader's decode path already ran it before the MAC check; a second
    // call would be dead code (same reasoning as raise_started_published()'s
    // Step 2 for validate_seal_export_started_shape()).

    // Step 3: construct the proposed after-state (ids stay byte-unchanged) +
    // the `.x1` seq=3 evidence.
    CompactionCandidateIntentWire after = before;
    after.phase = kCompactionCandidateIntentPhasePostSealFinalizing;

    CompactionIntentTransitionWire transition{};
    transition.format_version = kCompactionIntentTransitionFormatVersion;
    transition.total_bytes = kCompactionIntentTransitionWireBytes;
    transition.store_uuid_lo = before.store_uuid_lo;
    transition.store_uuid_hi = before.store_uuid_hi;
    transition.kek_key_id = before.kek_key_id;
    transition.from_phase = kCompactionCandidateIntentPhaseStartedPublished;
    transition.to_phase = kCompactionCandidateIntentPhasePostSealFinalizing;
    transition.reserved0 = 0;
    transition.transition_seq = 3;
    transition.source_generation = before.source_generation;
    transition.target_generation = before.target_generation;
    transition.baseline_tip_seq = before.baseline_tip_seq;
    std::memcpy(transition.baseline_tip_mac, before.baseline_tip_mac, sizeof(before.baseline_tip_mac));
    transition.baseline_key_id = before.baseline_key_id;
    transition.build_nonce = before.build_nonce;
    transition.candidate_id = before.candidate_id;
    transition.request_id = before.request_id;
    std::memcpy(transition.prev_transition_mac, seq2.mac, sizeof(seq2.mac));

    // Step 4: pure-function semantic validation via the shared chain-walker.
    // walk_x1_chain_raw's own TerminalBranchConflict check DOES fire for
    // this 3-frame chain (header comment's point 7) -- the explicit check
    // below is deliberately redundant with it, kept for symmetry with the
    // other functions in this class.
    {
        const std::array<CompactionIntentTransitionWire, 3> chain{seq1, seq2, transition};
        std::array<std::uint8_t, 32> terminal_mac{};
        if (compaction_codec_detail::walk_x1_chain_raw(after, chain, terminal_mac) != X1ChainStatus::Valid) {
            return PhaseAdvanceStatus::IllegalTransition;
        }
        if (after.phase != kCompactionCandidateIntentPhasePostSealFinalizing) {
            return PhaseAdvanceStatus::IllegalTransition;
        }
    }

    const PinResult write_pin = key_ring_.pin_key(before.kek_key_id);
    if (write_pin.status != PinStatus::Pinned) return PhaseAdvanceStatus::IntentKeyNotFound;

    // Step 5: publish `.x1` seq=3 (no-replace), with the idempotent-retry
    // guard for the PublishedNamespaceUncertain case.
    std::array<std::byte, kCompactionIntentTransitionWireBytes> encoded_transition{};
    encode_compaction_intent_transition_wire(encoded_transition, transition, write_pin.handle->key_bytes());

    if (x1_provenance_memory_.has_value() && x1_provenance_memory_->uncertain &&
        x1_provenance_memory_->build_nonce == build_nonce &&
        x1_provenance_memory_->transition_seq == transition.transition_seq &&
        std::memcmp(x1_provenance_memory_->encoded_transition.data(), encoded_transition.data(),
                     encoded_transition.size()) == 0) {
        return PhaseAdvanceStatus::PhaseRaiseUncertain;
    }

    const LeaseWriteResult x1_write =
        lease_.write_x1_frame_no_replace(build_nonce, transition.transition_seq, encoded_transition);
    switch (x1_write.outcome) {
        case LeaseIoOutcome::WrongOwner:
        case LeaseIoOutcome::NotHeld:
            return PhaseAdvanceStatus::LeaseNotHeld;
        case LeaseIoOutcome::CandidateFenced:
            return PhaseAdvanceStatus::CandidateFenced;
        case LeaseIoOutcome::DirectoryIdentityChanged:
            return PhaseAdvanceStatus::DirectoryIdentityChanged;
        case LeaseIoOutcome::Ok:
            break;
    }
    switch (x1_write.publish.state) {
        case compaction_detail::PublishCommitState::DurablyPublished:
            x1_provenance_memory_.reset();
            break;
        case compaction_detail::PublishCommitState::PublishedNamespaceUncertain:
            x1_provenance_memory_ = X1WriteProvenanceMemory{build_nonce, transition.transition_seq,
                                                              encoded_transition, true};
            return PhaseAdvanceStatus::PhaseRaiseUncertain;
        case compaction_detail::PublishCommitState::NotPublished:
        default:
            return PhaseAdvanceStatus::X1WriteFailed;
    }

    // Step 6: only now REPLACE the Intent to phase=PostSealFinalizing.
    std::array<std::byte, kCompactionCandidateIntentWireBytes> encoded_intent{};
    encode_compaction_candidate_intent_wire(encoded_intent, after, write_pin.handle->key_bytes());

    const LeaseWriteResult intent_write = lease_.replace_intent_phase(encoded_intent);
    switch (intent_write.outcome) {
        case LeaseIoOutcome::WrongOwner:
        case LeaseIoOutcome::NotHeld:
            return PhaseAdvanceStatus::LeaseNotHeld;
        case LeaseIoOutcome::CandidateFenced:
            return PhaseAdvanceStatus::CandidateFenced;
        case LeaseIoOutcome::DirectoryIdentityChanged:
            return PhaseAdvanceStatus::DirectoryIdentityChanged;
        case LeaseIoOutcome::Ok:
            break;
    }
    switch (intent_write.publish.state) {
        case compaction_detail::PublishCommitState::DurablyPublished:
            return PhaseAdvanceStatus::PhaseRaised;
        case compaction_detail::PublishCommitState::PublishedNamespaceUncertain:
            return PhaseAdvanceStatus::PhaseRaiseUncertain;
        case compaction_detail::PublishCommitState::NotPublished:
        default:
            return PhaseAdvanceStatus::IntentReplaceFailed;
    }
}

inline PhaseAdvanceStatus IntentPhaseAdvancer::raise_abandon_finalizing(std::uint64_t build_nonce) noexcept {
    // Step 0: same shape as raise_post_seal_finalizing()'s Step 0, with the
    // two terminal-phase branches swapped.
    CompactionCandidateIntentWire before{};
    {
        std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
        const LeaseReadResult read_result = lease_.read_intent_genesis(buf);
        switch (read_result.outcome) {
            case LeaseIoOutcome::WrongOwner:
            case LeaseIoOutcome::NotHeld:
                return PhaseAdvanceStatus::LeaseNotHeld;
            case LeaseIoOutcome::CandidateFenced:
                return PhaseAdvanceStatus::CandidateFenced;
            case LeaseIoOutcome::DirectoryIdentityChanged:
                return PhaseAdvanceStatus::DirectoryIdentityChanged;
            case LeaseIoOutcome::Ok:
                break;
        }
        switch (read_result.read.status) {
            case compaction_detail::ReadFixedStatus::NotFound:
                return PhaseAdvanceStatus::IntentNotFound;
            case compaction_detail::ReadFixedStatus::WrongSize:
            case compaction_detail::ReadFixedStatus::NotRegularFile:
            case compaction_detail::ReadFixedStatus::IoError:
                return PhaseAdvanceStatus::IntentCorrupt;
            case compaction_detail::ReadFixedStatus::Ok:
                break;
        }

        std::uint32_t kek_key_id_peek = 0;
        if (!peek_compaction_candidate_intent_kek_key_id(buf, kek_key_id_peek)) {
            return PhaseAdvanceStatus::IntentCorrupt;
        }
        const PinResult read_pin = key_ring_.pin_key(kek_key_id_peek);
        if (read_pin.status != PinStatus::Pinned) return PhaseAdvanceStatus::IntentKeyNotFound;

        std::optional<VerifiedCompactionCandidateIntent> verified;
        if (decode_compaction_candidate_intent_wire(buf, read_pin.handle->key_bytes(), verified) !=
            CompactionWireDecodeStatus::Ok) {
            return PhaseAdvanceStatus::IntentCorrupt;
        }
        before = verified->value();
    }

    if (before.build_nonce != build_nonce) return PhaseAdvanceStatus::ForeignBuildNonce;

    if (before.phase != kCompactionCandidateIntentPhaseStartedPublished) {
        if (before.phase == kCompactionCandidateIntentPhaseAbandonFinalizing &&
            is_legal_candidate_ids_for_phase(before.phase, before.candidate_id, before.request_id)) {
            return PhaseAdvanceStatus::AlreadyAtOrPastTargetPhase;
        }
        if (before.phase == kCompactionCandidateIntentPhasePostSealFinalizing &&
            is_legal_candidate_ids_for_phase(before.phase, before.candidate_id, before.request_id)) {
            return PhaseAdvanceStatus::AlreadyAtOppositeTerminalPhase;
        }
        return PhaseAdvanceStatus::IllegalTransition;
    }

    // Step 1: read back the durable seq=1 and seq=2 `.x1` frames.
    CompactionIntentTransitionWire seq1{};
    CompactionIntentTransitionWire seq2{};
    {
        PhaseAdvanceStatus failure_status{};
        if (!read_verified_seq1_and_seq2(build_nonce, before, seq1, seq2, failure_status)) {
            return failure_status;
        }
    }

    // Step 2: read `.abd` back (never written by this function -- header
    // comment's point 6), checked not trusted. `.abd` has no new_generation/
    // new_final_* fields at all -- this binding list must NOT reuse `.clr`'s.
    SealStartedAbandonWire abd{};
    {
        SealStartedAbandonLoader loader(lease_);
        const LoadStatus load_status = loader.load(key_ring_, abd);
        switch (load_status) {
            case LoadStatus::LeaseNotHeld:
                return PhaseAdvanceStatus::LeaseNotHeld;
            case LoadStatus::CandidateFenced:
                return PhaseAdvanceStatus::CandidateFenced;
            case LoadStatus::DirectoryIdentityChanged:
                return PhaseAdvanceStatus::DirectoryIdentityChanged;
            case LoadStatus::NotFound:
                return PhaseAdvanceStatus::AbdNotFound;
            case LoadStatus::Corrupt:
            case LoadStatus::IoError:
                return PhaseAdvanceStatus::AbdCorrupt;
            case LoadStatus::KeyNotFound:
                return PhaseAdvanceStatus::AbdKeyNotFound;
            case LoadStatus::Ok:
                break;
        }
    }
    if (abd.store_uuid_lo != before.store_uuid_lo || abd.store_uuid_hi != before.store_uuid_hi ||
        abd.candidate_id != before.candidate_id || abd.request_id != before.request_id ||
        abd.kek_key_id != before.kek_key_id || abd.source_generation != before.source_generation ||
        abd.baseline_tip_seq != before.baseline_tip_seq ||
        std::memcmp(abd.baseline_tip_mac, before.baseline_tip_mac, sizeof(before.baseline_tip_mac)) != 0 ||
        abd.baseline_key_id != before.baseline_key_id || abd.started_kind != kSealStartedKindNativeV2) {
        return PhaseAdvanceStatus::AbdForeignBinding;
    }
    if (abd.phase != kSealStartedAbandonPhaseResumeAuthorized) {
        return PhaseAdvanceStatus::AbdNotResumeAuthorized;
    }
    // Not re-calling validate_seal_started_abandon_shape() here -- same
    // dead-code reasoning as raise_post_seal_finalizing()'s Step 2.

    // Step 3: construct the proposed after-state (ids stay byte-unchanged) +
    // the `.x1` seq=3 evidence.
    CompactionCandidateIntentWire after = before;
    after.phase = kCompactionCandidateIntentPhaseAbandonFinalizing;

    CompactionIntentTransitionWire transition{};
    transition.format_version = kCompactionIntentTransitionFormatVersion;
    transition.total_bytes = kCompactionIntentTransitionWireBytes;
    transition.store_uuid_lo = before.store_uuid_lo;
    transition.store_uuid_hi = before.store_uuid_hi;
    transition.kek_key_id = before.kek_key_id;
    transition.from_phase = kCompactionCandidateIntentPhaseStartedPublished;
    transition.to_phase = kCompactionCandidateIntentPhaseAbandonFinalizing;
    transition.reserved0 = 0;
    transition.transition_seq = 3;
    transition.source_generation = before.source_generation;
    transition.target_generation = before.target_generation;
    transition.baseline_tip_seq = before.baseline_tip_seq;
    std::memcpy(transition.baseline_tip_mac, before.baseline_tip_mac, sizeof(before.baseline_tip_mac));
    transition.baseline_key_id = before.baseline_key_id;
    transition.build_nonce = before.build_nonce;
    transition.candidate_id = before.candidate_id;
    transition.request_id = before.request_id;
    std::memcpy(transition.prev_transition_mac, seq2.mac, sizeof(seq2.mac));

    // Step 4: same discipline as raise_post_seal_finalizing()'s Step 4.
    {
        const std::array<CompactionIntentTransitionWire, 3> chain{seq1, seq2, transition};
        std::array<std::uint8_t, 32> terminal_mac{};
        if (compaction_codec_detail::walk_x1_chain_raw(after, chain, terminal_mac) != X1ChainStatus::Valid) {
            return PhaseAdvanceStatus::IllegalTransition;
        }
        if (after.phase != kCompactionCandidateIntentPhaseAbandonFinalizing) {
            return PhaseAdvanceStatus::IllegalTransition;
        }
    }

    const PinResult write_pin = key_ring_.pin_key(before.kek_key_id);
    if (write_pin.status != PinStatus::Pinned) return PhaseAdvanceStatus::IntentKeyNotFound;

    // Step 5: publish `.x1` seq=3 (no-replace).
    std::array<std::byte, kCompactionIntentTransitionWireBytes> encoded_transition{};
    encode_compaction_intent_transition_wire(encoded_transition, transition, write_pin.handle->key_bytes());

    if (x1_provenance_memory_.has_value() && x1_provenance_memory_->uncertain &&
        x1_provenance_memory_->build_nonce == build_nonce &&
        x1_provenance_memory_->transition_seq == transition.transition_seq &&
        std::memcmp(x1_provenance_memory_->encoded_transition.data(), encoded_transition.data(),
                     encoded_transition.size()) == 0) {
        return PhaseAdvanceStatus::PhaseRaiseUncertain;
    }

    const LeaseWriteResult x1_write =
        lease_.write_x1_frame_no_replace(build_nonce, transition.transition_seq, encoded_transition);
    switch (x1_write.outcome) {
        case LeaseIoOutcome::WrongOwner:
        case LeaseIoOutcome::NotHeld:
            return PhaseAdvanceStatus::LeaseNotHeld;
        case LeaseIoOutcome::CandidateFenced:
            return PhaseAdvanceStatus::CandidateFenced;
        case LeaseIoOutcome::DirectoryIdentityChanged:
            return PhaseAdvanceStatus::DirectoryIdentityChanged;
        case LeaseIoOutcome::Ok:
            break;
    }
    switch (x1_write.publish.state) {
        case compaction_detail::PublishCommitState::DurablyPublished:
            x1_provenance_memory_.reset();
            break;
        case compaction_detail::PublishCommitState::PublishedNamespaceUncertain:
            x1_provenance_memory_ = X1WriteProvenanceMemory{build_nonce, transition.transition_seq,
                                                              encoded_transition, true};
            return PhaseAdvanceStatus::PhaseRaiseUncertain;
        case compaction_detail::PublishCommitState::NotPublished:
        default:
            return PhaseAdvanceStatus::X1WriteFailed;
    }

    // Step 6: only now REPLACE the Intent to phase=AbandonFinalizing. `.abd`
    // itself is never unlinked here (spec: "while A is still on disk").
    std::array<std::byte, kCompactionCandidateIntentWireBytes> encoded_intent{};
    encode_compaction_candidate_intent_wire(encoded_intent, after, write_pin.handle->key_bytes());

    const LeaseWriteResult intent_write = lease_.replace_intent_phase(encoded_intent);
    switch (intent_write.outcome) {
        case LeaseIoOutcome::WrongOwner:
        case LeaseIoOutcome::NotHeld:
            return PhaseAdvanceStatus::LeaseNotHeld;
        case LeaseIoOutcome::CandidateFenced:
            return PhaseAdvanceStatus::CandidateFenced;
        case LeaseIoOutcome::DirectoryIdentityChanged:
            return PhaseAdvanceStatus::DirectoryIdentityChanged;
        case LeaseIoOutcome::Ok:
            break;
    }
    switch (intent_write.publish.state) {
        case compaction_detail::PublishCommitState::DurablyPublished:
            return PhaseAdvanceStatus::PhaseRaised;
        case compaction_detail::PublishCommitState::PublishedNamespaceUncertain:
            return PhaseAdvanceStatus::PhaseRaiseUncertain;
        case compaction_detail::PublishCommitState::NotPublished:
        default:
            return PhaseAdvanceStatus::IntentReplaceFailed;
    }
}

}  // namespace hy
