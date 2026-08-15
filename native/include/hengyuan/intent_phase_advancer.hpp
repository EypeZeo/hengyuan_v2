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
//    SealIdWatermark advance binding the same candidate_id/request_id."
//    SealIdWatermark's wire fields are `next_candidate_id`/`next_request_id`
//    -- "the next value to allocate," not "the value just allocated" -- so
//    this class treats "next - 1" as the id most recently, durably reserved
//    by whatever external actor advanced the watermark (that advance itself
//    is out of this round's scope -- see compaction_breadcrumb_io.hpp/
//    seal_journal_precondition_codec.hpp's read-only loaders; nothing in
//    this repo writes a SealIdWatermark yet). A production caller MUST have
//    already durably advanced the watermark for this exact candidate before
//    calling raise_intent_phase() -- this class only reads and binds, it
//    never performs that advance itself (matches the design docs' "明确不做"
//    boundary: `raise_intent_phase()` consumes already-durable watermark
//    state, it does not make that state durable).

#pragma once

#include <hengyuan/compaction_intent_codec.hpp>
#include <hengyuan/compaction_intent_store.hpp>
#include <hengyuan/compaction_lease.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/seal_id_watermark_export_started_loader.hpp>

#include <array>
#include <cstdint>
#include <cstring>
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

    CandidateLease& lease_;
    KeyRing& key_ring_;
    std::optional<X1WriteProvenanceMemory> x1_provenance_memory_{};
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

    // Step 1: the receipt -- fresh read + full MAC verification of
    // SealIdWatermark via the real loader/codec (SealIdWatermarkLoader,
    // Round E breadcrumb L2 loaders). kek_key_id is sourced from the
    // Intent's own field (intent_permanently_immutable_fields_match()
    // already keeps this field pinned for the Intent's whole lifetime, so
    // reusing it here is inherently consistent -- no separate parameter,
    // no caller has to know which key the watermark uses).
    SealIdWatermark watermark{};
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
                return PhaseAdvanceStatus::WatermarkNotFound;
            case LoadStatus::Corrupt:
            case LoadStatus::IoError:
                return PhaseAdvanceStatus::WatermarkCorrupt;
            case LoadStatus::KeyNotFound:
                return PhaseAdvanceStatus::WatermarkKeyNotFound;
            case LoadStatus::Ok:
                break;
        }
    }
    if (watermark.store_uuid_lo != before.store_uuid_lo || watermark.store_uuid_hi != before.store_uuid_hi) {
        return PhaseAdvanceStatus::WatermarkForeignStore;
    }
    // No next_candidate_id/next_request_id == 0 check here: SealIdWatermarkLoader::load() (via
    // decode_seal_id_watermark_wire()) already refuses to produce an Ok watermark with either
    // field 0 -- that case surfaced as WatermarkCorrupt above already, this point is unreachable
    // with a zero id.

    const SealIdWatermarkAdvanceReceipt receipt(watermark);
    const std::uint64_t candidate_id = receipt.value().next_candidate_id - 1;
    const std::uint64_t request_id = receipt.value().next_request_id - 1;

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
