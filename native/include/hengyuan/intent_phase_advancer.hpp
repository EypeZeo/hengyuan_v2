// SPDX-License-Identifier: proprietary
// intent_phase_advancer.hpp -- IntentPhaseAdvancer: the receipt-gated
// Building->Reserved phase raise (docs/SPEC_INVARIANTS.md's "Seal-journal
// Round E receipt + raise_intent_phase() (Building->Reserved)" entry).
//
// Governance: L2 (real file I/O, via CandidateLease -- same governance class
// as compaction_intent_store.hpp's IntentStore).
//
// SCOPE, read this before trusting anything below (three rounds of
// adversarial review converged on this boundary -- see the ledger entry and
// the design scratch files it references for the full history):
//
// 1. This class implements ONLY the Building->Reserved edge (`.x1` seq=1).
//    Reserved->StartedPublished / StartedPublished->{PostSealFinalizing,
//    AbandonFinalizing} do not exist here -- their field-mapping tables were
//    found to have real bugs in round 1 of review and are deferred to a
//    future round that redesigns them against the real struct definitions,
//    not carried forward from a rejected draft.
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
//    call path. A candidate that reaches Reserved via this API today has NO
//    follow-on code path in this repository that can advance it further
//    (Reserved->StartedPublished does not exist) or safely clean it up
//    (.xgc / GC authorization is Round F, entirely absent from this repo) --
//    calling this in a real environment permanently strands the candidate
//    directory and its allocated ids (the same "crash after allocation
//    permanently burns the id" class of irreversibility the spec already
//    documents elsewhere, just reachable via a different door). Do not wire
//    this into any real compaction-trigger flow until Reserved->
//    StartedPublished and at least the shape of PreSeal-abandon cleanup
//    exist.
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
//    (the L4 spec's other Reserved-moment write, in a directory guarded by a
//    completely different lock class, SealJournalStoreLease) is deliberately
//    still out of scope this round -- folding it in requires a two-lock
//    coordination design this round does not attempt; see the ledger entry.

#pragma once

#include <hengyuan/compaction_intent_codec.hpp>
#include <hengyuan/compaction_intent_store.hpp>
#include <hengyuan/compaction_lease.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/seal_id_watermark_export_started_loader.hpp>

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
    IllegalTransition,           // validate_intent_transition() rejected the constructed
                                  // before/after/`.x1` triple.
    X1WriteFailed,
    IntentReplaceFailed,
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
    IntentPhaseAdvancer(CandidateLease& lease, KeyRing& key_ring) noexcept : lease_(lease), key_ring_(key_ring) {}

    // The only write operation this class has. See the header comment's
    // SCOPE section for the full boundary (Building->Reserved only, no
    // to_phase/id parameters, test-only this round).
    PhaseAdvanceStatus raise_intent_phase(std::uint64_t build_nonce) noexcept;

private:
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

    CandidateLease& lease_;
    KeyRing& key_ring_;
    std::optional<X1WriteProvenanceMemory> x1_provenance_memory_{};
    std::optional<WatermarkAdvanceProvenanceMemory> watermark_provenance_memory_{};
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

}  // namespace hy
