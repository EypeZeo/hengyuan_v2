// SPDX-License-Identifier: proprietary
// compaction_intent_store.hpp — IntentStore: Round D's genesis-only public
// entry point (docs/SPEC_INVARIANTS.md's "Seal-journal Round D" entry).
//
// Governance: L2 (real file I/O, via CandidateLease). IntentStore is the
// ONLY friend CandidateLease declares -- this file's create_building_intent()
// is the ONLY place in the whole codebase that calls
// CandidateLease::create_intent_genesis_no_replace(). There is no
// raise_intent_phase(), no `.x1` write method, no `.xgc` write method
// anywhere in this class -- see compaction_lease.hpp's and this file's own
// ledger entry for the six design revisions that converged on this
// boundary. Genesis is the one write Round D can prove safe: it asserts
// nothing about durable facts this round has no way to verify (no
// watermark reservation, no started-publish, no gate cleanup) -- Building
// is the starting point, there is no "this should have already happened"
// claim baked into creating it.

#pragma once

#include <hengyuan/compaction_intent_codec.hpp>
#include <hengyuan/compaction_lease.hpp>
#include <hengyuan/key_ring.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>

namespace hy {

enum class GenesisStatus : std::uint8_t {
    Created,             // this call created it, DurablyPublished
    CreatedUncertain,     // this call created it, PublishedNamespaceUncertain (Windows)
    AlreadyExists,        // target existed before this call, OR this same IntentStore
                            // instance already returned CreatedUncertain for an identical
                            // request and is reporting the same uncertain outcome again
                            // (never silently promoted to Created by a same-process retry
                            // -- see the class comment on why)
    LeaseNotHeld,
    DirectoryIdentityChanged,   // first detection this call
    CandidateFenced,            // already fenced by an earlier call
    KeyNotFound,                 // KeyRing had no key for the requested kek_key_id
    InvalidRequest,              // is_legal_candidate_ids_for_phase/is_legal_generation_transition
                                   // rejected the request before any I/O was attempted
    IoError,
};

enum class LoadStatus : std::uint8_t {
    Ok,
    NotFound,
    Corrupt,   // decode failed (MAC mismatch, malformed field, etc.) -- fail-closed,
               // does not distinguish which specific reason, same discipline as
               // durable_frame_codec.hpp's ChecksumMismatch handling
    LeaseNotHeld,
    DirectoryIdentityChanged,
    CandidateFenced,
    KeyNotFound,
    IoError,
};

struct GenesisRequest {
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint32_t kek_key_id{0};
    std::uint32_t source_generation{0};
    std::uint32_t target_generation{0};
    std::uint64_t baseline_tip_seq{0};
    std::array<std::uint8_t, 32> baseline_tip_mac{};
    std::uint32_t baseline_key_id{0};
    std::uint64_t build_nonce{0};
};

enum class GenesisMatchStatus : std::uint8_t {
    MatchesRequestedGenesis,
    ExistingDifferent,
    ExistingNonBuilding,
    Corrupt,
    NotFound,
    LeaseNotHeld,
    DirectoryIdentityChanged,
    CandidateFenced,
    KeyNotFound,
    IoError,
};

class IntentStore {
public:
    IntentStore(std::filesystem::path candidate_dir, KeyRing& key_ring)
        : lease_(std::move(candidate_dir)), key_ring_(key_ring) {}

    LeaseAcquireStatus acquire_lease() noexcept { return lease_.acquire(); }
    ReleaseStatus release_lease() noexcept { return lease_.release(); }
    bool holds_lease() const noexcept { return lease_.held(); }

    // Round D's ONLY write operation. Requires the lease to be held.
    // Refuses (AlreadyExists) if a genesis already exists on disk, in any
    // phase -- this is genesis, it never overwrites, never "upgrades," and
    // never races a second attempt into producing two different Building
    // records. Same-instance retries after a CreatedUncertain outcome
    // continue to report CreatedUncertain (never silently promoted to
    // Created by a byte-equal re-check) -- see genesis_provenance_memory_
    // below.
    GenesisStatus create_building_intent(const GenesisRequest& req) noexcept;

    // Read-only: decode + fully verify whatever genesis currently exists on
    // disk (any phase -- Round D doesn't try to interpret meaning beyond
    // "decode succeeded and the MAC checks out," it just reports it
    // honestly; Round D itself only ever WRITES Building, but a real
    // recovery scenario might find something else there from a future
    // round). Does not mutate anything.
    LoadStatus load_and_validate_intent(CompactionCandidateIntentWire& out) noexcept;

    // Read-only diagnostic: walks `.x1` seq 1..3 by fixed name (never a
    // directory scan), decoding and MAC-verifying each, and runs
    // validate_x1_chain() over whatever it finds. Round D never WRITES a
    // `.x1` -- this exists purely to honestly report what's on disk (e.g.
    // for an operator inspecting a candidate directory, or a future
    // round's recovery flow), not to drive any Round D behavior off the
    // result.
    X1ChainStatus inspect_x1_chain(std::uint64_t build_nonce,
                                    std::array<std::uint8_t, 32>& out_terminal_transition_mac) noexcept;

    // Compares whatever genesis exists on disk against `req`, reusing the
    // codec's own permanent-immutable-fields comparator and the Building
    // id==0 rule rather than re-implementing field-by-field comparison
    // here.
    GenesisMatchStatus load_and_match_building_genesis(const GenesisRequest& req,
                                                        CompactionCandidateIntentWire& out) noexcept;

private:
    static CompactionCandidateIntentWire to_wire(const GenesisRequest& req) noexcept {
        CompactionCandidateIntentWire v{};
        v.format_version = kCompactionCandidateIntentFormatVersion;
        v.total_bytes = kCompactionCandidateIntentWireBytes;
        v.store_uuid_lo = req.store_uuid_lo;
        v.store_uuid_hi = req.store_uuid_hi;
        v.kek_key_id = req.kek_key_id;
        v.phase = kCompactionCandidateIntentPhaseBuilding;
        v.source_generation = req.source_generation;
        v.target_generation = req.target_generation;
        v.baseline_tip_seq = req.baseline_tip_seq;
        std::memcpy(v.baseline_tip_mac, req.baseline_tip_mac.data(), req.baseline_tip_mac.size());
        v.baseline_key_id = req.baseline_key_id;
        v.build_nonce = req.build_nonce;
        v.candidate_id = 0;
        v.request_id = 0;
        return v;
    }

    static bool requests_equal(const GenesisRequest& a, const GenesisRequest& b) noexcept {
        return a.store_uuid_lo == b.store_uuid_lo && a.store_uuid_hi == b.store_uuid_hi &&
               a.kek_key_id == b.kek_key_id && a.source_generation == b.source_generation &&
               a.target_generation == b.target_generation && a.baseline_tip_seq == b.baseline_tip_seq &&
               a.baseline_tip_mac == b.baseline_tip_mac && a.baseline_key_id == b.baseline_key_id &&
               a.build_nonce == b.build_nonce;
    }

    CandidateLease lease_;
    KeyRing& key_ring_;

    // Round D review P1-1: a same-process retry after CreatedUncertain must
    // not be allowed to "heal" into Created just because a subsequent
    // byte-equal read-back looks fine -- that new evidence adds no
    // information about whether the parent-directory flush actually
    // happened. This is deliberately in-memory-only (a fresh process has
    // no such memory, and Round D does not try to resolve that harder
    // case -- see the ledger entry's "platform-honest recovery boundary"
    // note).
    std::optional<GenesisRequest> genesis_provenance_memory_request_{};
    bool genesis_provenance_memory_uncertain_{false};
};

inline GenesisStatus IntentStore::create_building_intent(const GenesisRequest& req) noexcept {
    if (!is_legal_generation_transition(req.source_generation, req.target_generation)) {
        return GenesisStatus::InvalidRequest;
    }
    if (!is_legal_candidate_ids_for_phase(kCompactionCandidateIntentPhaseBuilding, 0, 0)) {
        return GenesisStatus::InvalidRequest;  // always true in practice (0,0 for Building is
                                                 // legal by definition) -- kept as an explicit,
                                                 // visible check rather than an implicit assumption
    }

    // Same-instance uncertain-provenance memory: if we already created this
    // exact genesis and got an uncertain outcome, keep reporting that --
    // never silently promote via a fresh byte-equal read.
    if (genesis_provenance_memory_uncertain_ && genesis_provenance_memory_request_.has_value() &&
        requests_equal(*genesis_provenance_memory_request_, req)) {
        return GenesisStatus::CreatedUncertain;
    }

    const PinResult pin = key_ring_.pin_key(req.kek_key_id);
    if (pin.status != PinStatus::Pinned) return GenesisStatus::KeyNotFound;

    const CompactionCandidateIntentWire v = to_wire(req);
    std::array<std::byte, kCompactionCandidateIntentWireBytes> encoded{};
    encode_compaction_candidate_intent_wire(encoded, v, pin.handle->key_bytes());

    const LeaseWriteResult write_result = lease_.create_intent_genesis_no_replace(encoded);
    switch (write_result.outcome) {
        case LeaseIoOutcome::WrongOwner:
        case LeaseIoOutcome::NotHeld:
            return GenesisStatus::LeaseNotHeld;
        case LeaseIoOutcome::CandidateFenced:
            return GenesisStatus::CandidateFenced;
        case LeaseIoOutcome::DirectoryIdentityChanged:
            return GenesisStatus::DirectoryIdentityChanged;
        case LeaseIoOutcome::Ok:
            break;
    }

    switch (write_result.publish.state) {
        case compaction_detail::PublishCommitState::DurablyPublished:
            if (write_result.publish.provenance == compaction_detail::PublishProvenance::FoundPreExisting) {
                return GenesisStatus::AlreadyExists;
            }
            genesis_provenance_memory_request_.reset();
            genesis_provenance_memory_uncertain_ = false;
            return GenesisStatus::Created;
        case compaction_detail::PublishCommitState::PublishedNamespaceUncertain:
            genesis_provenance_memory_request_ = req;
            genesis_provenance_memory_uncertain_ = true;
            return GenesisStatus::CreatedUncertain;
        case compaction_detail::PublishCommitState::NotPublished:
        default:
            return GenesisStatus::IoError;
    }
}

inline LoadStatus IntentStore::load_and_validate_intent(CompactionCandidateIntentWire& out) noexcept {
    std::array<std::byte, kCompactionCandidateIntentWireBytes> buf{};
    // Peek the key_id first (unauthenticated) so we know which key to pin
    // -- decode itself verifies the MAC against that specific key.
    const LeaseReadResult probe_read = lease_.read_intent_genesis(buf);
    switch (probe_read.outcome) {
        case LeaseIoOutcome::WrongOwner:
        case LeaseIoOutcome::NotHeld:
            return LoadStatus::LeaseNotHeld;
        case LeaseIoOutcome::CandidateFenced:
            return LoadStatus::CandidateFenced;
        case LeaseIoOutcome::DirectoryIdentityChanged:
            return LoadStatus::DirectoryIdentityChanged;
        case LeaseIoOutcome::Ok:
            break;
    }
    switch (probe_read.read.status) {
        case compaction_detail::ReadFixedStatus::NotFound:
            return LoadStatus::NotFound;
        case compaction_detail::ReadFixedStatus::WrongSize:
        case compaction_detail::ReadFixedStatus::NotRegularFile:
            // Wrong size / not-a-regular-file is evidence the on-disk record
            // itself is malformed, not a transient I/O condition -- report
            // it the same way a MAC failure would be (Corrupt), not as
            // IoError, which this class reserves for genuine read failures.
            return LoadStatus::Corrupt;
        case compaction_detail::ReadFixedStatus::IoError:
            return LoadStatus::IoError;
        case compaction_detail::ReadFixedStatus::Ok:
            break;
    }

    std::uint32_t kek_key_id = 0;
    if (!peek_compaction_candidate_intent_kek_key_id(buf, kek_key_id)) return LoadStatus::Corrupt;

    const PinResult pin = key_ring_.pin_key(kek_key_id);
    if (pin.status != PinStatus::Pinned) return LoadStatus::KeyNotFound;

    std::optional<VerifiedCompactionCandidateIntent> verified;
    if (decode_compaction_candidate_intent_wire(buf, pin.handle->key_bytes(), verified) !=
        CompactionWireDecodeStatus::Ok) {
        return LoadStatus::Corrupt;
    }
    out = verified->value();
    return LoadStatus::Ok;
}

inline X1ChainStatus IntentStore::inspect_x1_chain(
    std::uint64_t build_nonce, std::array<std::uint8_t, 32>& out_terminal_transition_mac) noexcept {
    CompactionCandidateIntentWire intent{};
    if (load_and_validate_intent(intent) != LoadStatus::Ok) return X1ChainStatus::Empty;

    std::array<CompactionIntentTransitionWire, 3> frames{};
    std::size_t count = 0;
    for (std::uint32_t seq = 1; seq <= 3; ++seq) {
        std::array<std::byte, kCompactionIntentTransitionWireBytes> buf{};
        const LeaseReadResult read_result = lease_.read_x1_frame(build_nonce, seq, buf);
        if (read_result.outcome != LeaseIoOutcome::Ok) return X1ChainStatus::Empty;
        if (read_result.read.status == compaction_detail::ReadFixedStatus::NotFound) break;
        if (read_result.read.status != compaction_detail::ReadFixedStatus::Ok) return X1ChainStatus::Empty;

        std::uint32_t kek_key_id = intent.kek_key_id;
        const PinResult pin = key_ring_.pin_key(kek_key_id);
        if (pin.status != PinStatus::Pinned) return X1ChainStatus::Empty;

        std::optional<VerifiedTransition> verified;
        if (decode_compaction_intent_transition_wire(buf, pin.handle->key_bytes(), verified) !=
            CompactionWireDecodeStatus::Ok) {
            return X1ChainStatus::Empty;  // undecodable/tampered -- treat as no usable chain
        }
        frames[count] = verified->value();
        ++count;
    }

    if (count == 0) return X1ChainStatus::Empty;
    std::array<std::uint8_t, 32> terminal{};
    const X1ChainStatus status = compaction_codec_detail::walk_x1_chain_raw(
        intent, std::span<const CompactionIntentTransitionWire>(frames.data(), count), terminal);
    if (status == X1ChainStatus::Valid) out_terminal_transition_mac = terminal;
    return status;
}

inline GenesisMatchStatus IntentStore::load_and_match_building_genesis(const GenesisRequest& req,
                                                                          CompactionCandidateIntentWire& out) noexcept {
    const LoadStatus load_status = load_and_validate_intent(out);
    switch (load_status) {
        case LoadStatus::NotFound:
            return GenesisMatchStatus::NotFound;
        case LoadStatus::Corrupt:
            return GenesisMatchStatus::Corrupt;
        case LoadStatus::LeaseNotHeld:
            return GenesisMatchStatus::LeaseNotHeld;
        case LoadStatus::DirectoryIdentityChanged:
            return GenesisMatchStatus::DirectoryIdentityChanged;
        case LoadStatus::CandidateFenced:
            return GenesisMatchStatus::CandidateFenced;
        case LoadStatus::KeyNotFound:
            return GenesisMatchStatus::KeyNotFound;
        case LoadStatus::IoError:
            return GenesisMatchStatus::IoError;
        case LoadStatus::Ok:
            break;
    }

    if (out.phase != kCompactionCandidateIntentPhaseBuilding) return GenesisMatchStatus::ExistingNonBuilding;

    const CompactionCandidateIntentWire expected = to_wire(req);
    if (!intent_permanently_immutable_fields_match(expected, out)) return GenesisMatchStatus::ExistingDifferent;
    if (out.candidate_id != 0 || out.request_id != 0) return GenesisMatchStatus::ExistingDifferent;
    return GenesisMatchStatus::MatchesRequestedGenesis;
}

}  // namespace hy
