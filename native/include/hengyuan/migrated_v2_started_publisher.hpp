// SPDX-License-Identifier: proprietary
// migrated_v2_started_publisher.hpp -- MigratedV2StartedPublisher: the V+M
// companion write path for a candidate whose "L" (SealExportStartedWire,
// NativeV2Started/greenfield) is already durable (docs/SPEC_INVARIANTS.md's
// "Seal-journal Round E MigratedV2Started (V+M companion write path)"
// entry).
//
// Governance: L2 (real file I/O, via CandidateLease -- same governance class
// as intent_phase_advancer.hpp's IntentPhaseAdvancer).
//
// SCOPE, read this before trusting anything below:
//
// 1. This class is NOT a phase-advancer -- it never reads/writes/REPLACEs
//    Intent.phase, and is deliberately a separate class from
//    IntentPhaseAdvancer (folding it in would misleadingly imply this is
//    "one more phase raise" the way raise_intent_phase()/
//    raise_started_published()/raise_post_seal_finalizing()/
//    raise_abandon_finalizing() are). It only reads the Intent (read-only,
//    to confirm phase==StartedPublished as a precondition) and writes two
//    NEW breadcrumb files, "V" (seal-export-started.v2) and "M"
//    (seal-export-started.mig) -- "L" (seal-export-started) must already be
//    durable, written by a prior raise_started_published() call.
// 2. **This is a deliberate, documented reinterpretation of the L4 spec's
//    only concrete write-order procedure for MigratedV2Started**
//    (docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4002-4016), which is explicitly
//    labeled "Offline migration (operator tool only -- never hot-path
//    auto-upgrade)" and presupposes a pre-existing LEGACY 192-byte L file
//    (format_version==1, kSealExportStartedLegacyV1Bytes). This repo has
//    NEVER decoded or encoded that legacy 192-byte format anywhere -- only
//    the 238-byte v2 format (kSealExportStartedWireBytes) has a codec. This
//    class instead operates on the 238-byte v2 "L" that
//    raise_started_published() actually writes (the only "L" this repo can
//    produce), implementing spec steps 3-5 (write V, write M, done) against
//    that L -- NOT spec steps 0-2 (verify a pre-existing legacy L under an
//    operator-supplied legacy_kek_key_id). Do not read this class as a
//    faithful implementation of the spec's literal offline-migration
//    scenario; it is an honest, narrower adaptation of the same write shape
//    to the only "L" this repo has.
// 3. Test-only, same as every other write path in this class family: no
//    production call path constructs a MigratedV2StartedPublisher. No real
//    operator CLI/tool exists here either -- this is a callable library
//    function, not the literal "operator tool" the spec describes.
// 4. Thread safety: single owner-thread object, same discipline as
//    CandidateLease itself -- the two std::optional provenance-memory
//    members below are mutable state, never safe to call from more than
//    one thread concurrently. Deliberately no mutex/atomic added for this
//    cold path; that would add complexity without adding real safety for a
//    class that was never meant to be called concurrently in the first
//    place.
// 5. V's closed field set (everything except kek_key_id/
//    registered_producer_mask/producer_count/ring_id) is copied WHOLESALE
//    from the freshly-read-and-verified L, never accepted from the caller
//    -- the caller only supplies V's own kek_key_id and topology (the L4
//    spec's "explicit operator-supplied topology"). This closes off a
//    "caller lies about a closed field" attack surface entirely, rather
//    than checking-not-trusting it the way raise_started_published()'s
//    Step 2 does for `started`.
// 6. M's own HMAC is keyed by v2_kek_key_id (== the caller's v_kek_key_id
//    parameter), not legacy_kek_key_id -- confirmed via
//    peek_seal_export_started_migration_v2_kek_key_id()'s offset
//    (seal_export_migration_cleanup_abandon_codec.hpp). The key is pinned
//    exactly once (for V) and reused for M -- no separate key-not-found
//    status exists for M.
// 7. encode_seal_export_started_wire() (seal_journal_precondition_codec.hpp)
//    does NOT call validate_seal_export_started_shape() before writing --
//    that file's own header comment documents this as a known, deliberately
//    un-retrofitted gap relative to THIS file's own encode_*() functions
//    (which DO shape-validate first). This class closes that gap for its
//    own V construction by calling validate_seal_export_started_shape()
//    itself immediately after building V and before encoding it -- a
//    caller-supplied illegal topology must never reach disk with a valid
//    MAC.
// 8. MAC comparison never happens via memcmp/== in this class -- it only
//    ever happens inside decode_seal_export_started_wire()'s own
//    constant_time_equal() call. L's/V's `mac` fields are only ever *copied*
//    here (into M's legacy_mac/v2_mac), never compared.
//
// Explicitly NOT done: legacy 192-byte L format decode/validation (see
// point 2); any real operator interaction/CLI; wiring this into any real
// trigger flow; anything to do with `.clr`/`.abd`/`.xgc` (unrelated to this
// edge).

#pragma once

#include <hengyuan/compaction_intent_codec.hpp>
#include <hengyuan/compaction_lease.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/seal_export_migration_cleanup_abandon_codec.hpp>
#include <hengyuan/seal_journal_precondition_codec.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace hy {

enum class MigratedV2StartedPublishStatus : std::uint8_t {
    Published,            // V and M are both durably published (this call, or a byte-equal
                           // idempotent retry of an earlier successful call).
    PublishUncertain,     // at least one of V/M's writes returned PublishedNamespaceUncertain
                           // -- same platform-honest reporting as every other write path in
                           // this class family.
    LeaseNotHeld,
    CandidateFenced,
    DirectoryIdentityChanged,
    IntentNotFound,
    IntentCorrupt,
    IntentKeyNotFound,
    ForeignBuildNonce,
    IntentNotAtStartedPublished,  // Intent.phase != StartedPublished (exact equality, not >=;
                                   // this repo's write paths only guarantee L exists durably at
                                   // exactly that phase).
    LNotFound,             // "L" (seal-export-started) doesn't exist -- raise_started_published()
                            // never ran, or ran but L is somehow missing.
    LCorrupt,               // L exists but fails MAC verification/decode (includes a
                            // legacy-192-byte-sized L, which this class's decode path -- the
                            // same 238-byte-only decode_seal_export_started_wire() every other
                            // caller uses -- rejects as WrongSize, not a distinct status).
    LKeyNotFound,
    LForeignBinding,       // L's store_uuid/candidate_id/request_id/source_generation/
                            // baseline_*/kek_key_id (plus new_generation==target_generation)
                            // don't match this Intent's own fields.
    VShapeInvalid,         // the caller-supplied topology (registered_producer_mask/
                            // producer_count/ring_id) failed validate_seal_export_started_shape()
                            // -- see header comment's point 7. Neither V nor M is written.
    VKeyNotFound,           // v_kek_key_id isn't pinned in the KeyRing.
    VWriteFailed,           // V's CREATE_NEW returned NotPublished (a real, byte-differing
                            // conflict).
    MWriteFailed,           // M's CREATE_NEW returned NotPublished.
};

class MigratedV2StartedPublisher {
public:
    explicit MigratedV2StartedPublisher(CandidateLease& lease, KeyRing& key_ring) noexcept
        : lease_(lease), key_ring_(key_ring) {}

    // v_kek_key_id/registered_producer_mask/producer_count/ring_id are V's
    // own caller-supplied fields (L4 spec: "explicit operator-supplied
    // topology") -- everything else V needs is copied from the already-
    // durable L this call reads back itself (header comment's point 5).
    MigratedV2StartedPublishStatus publish(std::uint64_t build_nonce, std::uint32_t v_kek_key_id,
                                            std::uint8_t registered_producer_mask, std::uint8_t producer_count,
                                            std::span<const SealHandoffRingId, kMaxSealHandoffProducers> ring_id) noexcept;

private:
    // Same shape/reasoning as intent_phase_advancer.hpp's
    // StartedWriteProvenanceMemory -- CREATE_NEW's content is fully
    // deterministic per retry (once L is durable and the caller's inputs
    // are fixed), so only the *uncertain* outcome needs same-process
    // memory. V and M are tracked independently -- an uncertain V write
    // must never let an M write proceed on retry, so they cannot share one
    // struct (header comment's point 4 / design note 6).
    struct VWriteProvenanceMemory {
        std::uint64_t build_nonce{0};
        std::array<std::byte, kSealExportStartedWireBytes> encoded_v{};
        bool uncertain{false};
    };
    struct MWriteProvenanceMemory {
        std::uint64_t build_nonce{0};
        std::array<std::byte, kSealExportStartedMigrationWireBytes> encoded_m{};
        bool uncertain{false};
    };

    CandidateLease& lease_;
    KeyRing& key_ring_;
    std::optional<VWriteProvenanceMemory> v_provenance_memory_{};
    std::optional<MWriteProvenanceMemory> m_provenance_memory_{};
};

inline MigratedV2StartedPublishStatus MigratedV2StartedPublisher::publish(
    std::uint64_t build_nonce, std::uint32_t v_kek_key_id, std::uint8_t registered_producer_mask,
    std::uint8_t producer_count, std::span<const SealHandoffRingId, kMaxSealHandoffProducers> ring_id) noexcept {
    // Step 0: fresh read + full MAC verification of the current Intent --
    // same shape as raise_started_published()'s Step 0. Precondition is
    // phase == StartedPublished, exactly (not >=): that is the only phase
    // this repo's write paths guarantee L durably exists at.
    CompactionCandidateIntentWire before{};
    {
        std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
        const LeaseReadResult read_result = lease_.read_intent_genesis(buf);
        switch (read_result.outcome) {
            case LeaseIoOutcome::WrongOwner:
            case LeaseIoOutcome::NotHeld:
                return MigratedV2StartedPublishStatus::LeaseNotHeld;
            case LeaseIoOutcome::CandidateFenced:
                return MigratedV2StartedPublishStatus::CandidateFenced;
            case LeaseIoOutcome::DirectoryIdentityChanged:
                return MigratedV2StartedPublishStatus::DirectoryIdentityChanged;
            case LeaseIoOutcome::Ok:
                break;
        }
        switch (read_result.read.status) {
            case compaction_detail::ReadFixedStatus::NotFound:
                return MigratedV2StartedPublishStatus::IntentNotFound;
            case compaction_detail::ReadFixedStatus::WrongSize:
            case compaction_detail::ReadFixedStatus::NotRegularFile:
            case compaction_detail::ReadFixedStatus::IoError:
                return MigratedV2StartedPublishStatus::IntentCorrupt;
            case compaction_detail::ReadFixedStatus::Ok:
                break;
        }

        std::uint32_t kek_key_id_peek = 0;
        if (!peek_compaction_candidate_intent_kek_key_id(buf, kek_key_id_peek)) {
            return MigratedV2StartedPublishStatus::IntentCorrupt;
        }
        const PinResult read_pin = key_ring_.pin_key(kek_key_id_peek);
        if (read_pin.status != PinStatus::Pinned) return MigratedV2StartedPublishStatus::IntentKeyNotFound;

        std::optional<VerifiedCompactionCandidateIntent> verified;
        if (decode_compaction_candidate_intent_wire(buf, read_pin.handle->key_bytes(), verified) !=
            CompactionWireDecodeStatus::Ok) {
            return MigratedV2StartedPublishStatus::IntentCorrupt;
        }
        before = verified->value();
    }

    if (before.build_nonce != build_nonce) return MigratedV2StartedPublishStatus::ForeignBuildNonce;
    if (before.phase != kCompactionCandidateIntentPhaseStartedPublished) {
        return MigratedV2StartedPublishStatus::IntentNotAtStartedPublished;
    }

    // Step 1: read L's raw bytes (needed for Step 4's SHA-256 digest input,
    // which must be the real on-disk bytes, not a reconstruction) + decode
    // + MAC-verify it -- inline, same "no extra loader indirection" style
    // every raise_*() function in this class family already uses.
    std::array<std::byte, kSealExportStartedWireBytes> l_raw{};
    SealExportStartedWire l{};
    {
        const LeaseReadResult read_result = lease_.read_seal_export_started(/*legacy_or_greenfield=*/true, l_raw);
        switch (read_result.outcome) {
            case LeaseIoOutcome::WrongOwner:
            case LeaseIoOutcome::NotHeld:
                return MigratedV2StartedPublishStatus::LeaseNotHeld;
            case LeaseIoOutcome::CandidateFenced:
                return MigratedV2StartedPublishStatus::CandidateFenced;
            case LeaseIoOutcome::DirectoryIdentityChanged:
                return MigratedV2StartedPublishStatus::DirectoryIdentityChanged;
            case LeaseIoOutcome::Ok:
                break;
        }
        switch (read_result.read.status) {
            case compaction_detail::ReadFixedStatus::NotFound:
                return MigratedV2StartedPublishStatus::LNotFound;
            case compaction_detail::ReadFixedStatus::WrongSize:
            case compaction_detail::ReadFixedStatus::NotRegularFile:
            case compaction_detail::ReadFixedStatus::IoError:
                return MigratedV2StartedPublishStatus::LCorrupt;
            case compaction_detail::ReadFixedStatus::Ok:
                break;
        }

        std::uint32_t l_kek_key_id_peek = 0;
        if (!peek_seal_export_started_kek_key_id(l_raw, l_kek_key_id_peek)) {
            return MigratedV2StartedPublishStatus::LCorrupt;
        }
        const PinResult l_pin = key_ring_.pin_key(l_kek_key_id_peek);
        if (l_pin.status != PinStatus::Pinned) return MigratedV2StartedPublishStatus::LKeyNotFound;

        std::optional<VerifiedSealExportStarted> verified_l;
        if (decode_seal_export_started_wire(l_raw, l_pin.handle->key_bytes(), verified_l) !=
            SealJournalPreconditionDecodeStatus::Ok) {
            return MigratedV2StartedPublishStatus::LCorrupt;
        }
        l = verified_l->value();
    }

    // Step 2: L was already checked-not-trusted once, when raise_started_
    // published() wrote it -- but that was a different call, possibly a
    // different process. Re-verify it binds to THIS Intent's own fields
    // now, same discipline every other read-back-an-existing-breadcrumb
    // step in this class family uses (e.g. raise_post_seal_finalizing()'s
    // `.clr` binding check).
    if (l.store_uuid_lo != before.store_uuid_lo || l.store_uuid_hi != before.store_uuid_hi ||
        l.candidate_id != before.candidate_id || l.request_id != before.request_id ||
        l.source_generation != before.source_generation || l.new_generation != before.target_generation ||
        l.baseline_tip_seq != before.baseline_tip_seq ||
        std::memcmp(l.baseline_tip_mac, before.baseline_tip_mac, sizeof(before.baseline_tip_mac)) != 0 ||
        l.baseline_key_id != before.baseline_key_id || l.kek_key_id != before.kek_key_id) {
        return MigratedV2StartedPublishStatus::LForeignBinding;
    }

    // Step 3: construct V as a whole-struct copy of L (header comment's
    // point 5), overriding only the caller-supplied fields, then shape-
    // validate BEFORE encoding (header comment's point 7 -- encode_seal_
    // export_started_wire() itself does not shape-check).
    SealExportStartedWire v = l;
    v.kek_key_id = v_kek_key_id;
    v.registered_producer_mask = registered_producer_mask;
    v.producer_count = producer_count;
    for (std::size_t i = 0; i < kMaxSealHandoffProducers; ++i) v.ring_id[i] = ring_id[i];

    if (!validate_seal_export_started_shape(v)) {
        return MigratedV2StartedPublishStatus::VShapeInvalid;
    }

    const PinResult v_pin = key_ring_.pin_key(v_kek_key_id);
    if (v_pin.status != PinStatus::Pinned) return MigratedV2StartedPublishStatus::VKeyNotFound;

    std::array<std::byte, kSealExportStartedWireBytes> encoded_v{};
    encode_seal_export_started_wire(encoded_v, v, v_pin.handle->key_bytes());

    if (v_provenance_memory_.has_value() && v_provenance_memory_->uncertain &&
        v_provenance_memory_->build_nonce == build_nonce &&
        std::memcmp(v_provenance_memory_->encoded_v.data(), encoded_v.data(), encoded_v.size()) == 0) {
        // Same-process retry of an attempt already known to be uncertain --
        // do not re-derive/re-judge from a fresh read; a fresh byte-equal
        // collision would carry no information about whether the parent-
        // directory flush actually happened (same reasoning as every other
        // *WriteProvenanceMemory in this codebase).
        return MigratedV2StartedPublishStatus::PublishUncertain;
    }

    const LeaseWriteResult v_write =
        lease_.create_seal_export_started_no_replace(/*legacy_or_greenfield=*/false, encoded_v);
    switch (v_write.outcome) {
        case LeaseIoOutcome::WrongOwner:
        case LeaseIoOutcome::NotHeld:
            return MigratedV2StartedPublishStatus::LeaseNotHeld;
        case LeaseIoOutcome::CandidateFenced:
            return MigratedV2StartedPublishStatus::CandidateFenced;
        case LeaseIoOutcome::DirectoryIdentityChanged:
            return MigratedV2StartedPublishStatus::DirectoryIdentityChanged;
        case LeaseIoOutcome::Ok:
            break;
    }
    switch (v_write.publish.state) {
        case compaction_detail::PublishCommitState::DurablyPublished:
            v_provenance_memory_.reset();
            break;
        case compaction_detail::PublishCommitState::PublishedNamespaceUncertain:
            // V's uncertainty must never let M get written this call -- return
            // immediately (header comment's point 4 / design discipline).
            v_provenance_memory_ = VWriteProvenanceMemory{build_nonce, encoded_v, true};
            return MigratedV2StartedPublishStatus::PublishUncertain;
        case compaction_detail::PublishCommitState::NotPublished:
        default:
            return MigratedV2StartedPublishStatus::VWriteFailed;
    }

    // Step 4: only reached once V is confirmed DurablyPublished. Digests
    // are computed over the REAL on-disk byte sequences (L's raw read-back
    // bytes, V's just-encoded bytes) -- never reconstructed from the
    // decoded structs, which would not reflect the actual serialized form
    // if this codec's wire layout ever diverges from a naive re-encode.
    const auto legacy_file_digest = crypto::sha256(l_raw);
    const auto v2_file_digest = crypto::sha256(encoded_v);

    SealExportStartedMigrationWire m{};
    m.store_uuid_lo = l.store_uuid_lo;
    m.store_uuid_hi = l.store_uuid_hi;
    m.candidate_id = l.candidate_id;
    m.request_id = l.request_id;
    m.legacy_kek_key_id = l.kek_key_id;
    m.v2_kek_key_id = v_kek_key_id;
    std::memcpy(m.legacy_file_digest, legacy_file_digest.bytes.data(), sizeof(m.legacy_file_digest));
    std::memcpy(m.v2_file_digest, v2_file_digest.bytes.data(), sizeof(m.v2_file_digest));
    // legacy_mac is L's own already-MAC-verified trailer (decode already
    // constant-time-compared it, header comment's point 8) -- a plain copy
    // here, not a re-comparison. v2_mac is read from the just-encoded V
    // buffer's trailer, NOT from `v.mac` (which encode_seal_export_started_
    // wire() never populates -- it takes `v` by const reference and writes
    // the computed mac only into the output span).
    std::memcpy(m.legacy_mac, l.mac, sizeof(m.legacy_mac));
    std::memcpy(m.v2_mac, encoded_v.data() + (kSealExportStartedWireBytes - 32), sizeof(m.v2_mac));

    std::array<std::byte, kSealExportStartedMigrationWireBytes> encoded_m{};
    if (encode_seal_export_started_migration_wire(encoded_m, m, v_pin.handle->key_bytes()) == 0) {
        // Should be unreachable -- candidate_id/request_id are copied from
        // an already-verified L, which validate_seal_export_started_shape()
        // already guaranteed are nonzero, and that is the only shape rule
        // encode_seal_export_started_migration_wire() enforces. Checked
        // defensively rather than assumed (header comment's point 7's same
        // "never trust an encode silently" discipline, applied to M too).
        return MigratedV2StartedPublishStatus::MWriteFailed;
    }

    if (m_provenance_memory_.has_value() && m_provenance_memory_->uncertain &&
        m_provenance_memory_->build_nonce == build_nonce &&
        std::memcmp(m_provenance_memory_->encoded_m.data(), encoded_m.data(), encoded_m.size()) == 0) {
        return MigratedV2StartedPublishStatus::PublishUncertain;
    }

    const LeaseWriteResult m_write = lease_.create_seal_export_started_migration_no_replace(encoded_m);
    switch (m_write.outcome) {
        case LeaseIoOutcome::WrongOwner:
        case LeaseIoOutcome::NotHeld:
            return MigratedV2StartedPublishStatus::LeaseNotHeld;
        case LeaseIoOutcome::CandidateFenced:
            return MigratedV2StartedPublishStatus::CandidateFenced;
        case LeaseIoOutcome::DirectoryIdentityChanged:
            return MigratedV2StartedPublishStatus::DirectoryIdentityChanged;
        case LeaseIoOutcome::Ok:
            break;
    }
    switch (m_write.publish.state) {
        case compaction_detail::PublishCommitState::DurablyPublished:
            m_provenance_memory_.reset();
            return MigratedV2StartedPublishStatus::Published;
        case compaction_detail::PublishCommitState::PublishedNamespaceUncertain:
            m_provenance_memory_ = MWriteProvenanceMemory{build_nonce, encoded_m, true};
            return MigratedV2StartedPublishStatus::PublishUncertain;
        case compaction_detail::PublishCommitState::NotPublished:
        default:
            return MigratedV2StartedPublishStatus::MWriteFailed;
    }
}

}  // namespace hy
