// SPDX-License-Identifier: proprietary
// seal_started_migration_cleanup_loader.hpp — read-only L2 loaders for two
// "Round B" seal-journal breadcrumbs (docs/SPEC_INVARIANTS.md's
// "Seal-journal Round E breadcrumb L2 loaders" entry):
//   SealExportStartedMigrationLoader    -> "seal-export-started.mig"
//                                          (SealExportStartedMigrationWire)
//   SealStartedCleanupTombstoneLoader   -> "seal-export-started.clr"
//                                          (SealStartedCleanupTombstoneWire)
//
// Governance: L2 (real file I/O — through CandidateLease's friend-only read
// methods; this file itself never touches a filesystem). The load() bodies
// are a line-for-line structural copy of IntentStore::load_and_validate_intent()
// (compaction_intent_store.hpp:242-287) — the same six-round-review-derived
// discipline: read through the lease, map LeaseIoOutcome, map
// compaction_detail::ReadFixedStatus, peek the key id (unauthenticated),
// pin exactly that key, decode (which MAC-verifies), copy out. Every mapping
// decision and every comment below mirrors that function on purpose; if one
// of them changes, the other must be re-read for the same change.
//
// WHY THESE ARE TEMPLATES: the loaders are parameterized on the lease type
// so their tests can drive them against a duck-typed mock
// (MockCandidateLeaseForMigrationCleanup in the test file). The production
// instantiation is exactly one: SealExportStartedMigrationLoader<CandidateLease>
// (see seal_journal_breadcrumb_precondition_aggregate.hpp). compaction_lease.hpp's
// friend declarations name the primary template and therefore admit every
// specialization (including the CandidateLease one), so the "one friend per
// consumer, no generic accessor" boundary IntentStore established is unchanged.
//
// ABSOLUTE RULES (same set every previous review round enforced on
// IntentStore, enforced here the same way):
//   1. Only fields produced by a successful decode are ever trusted — the
//      pre-decode peek is used solely to select the pin key, never to
//      interpret content.
//   2. No handle type is exported, no reference/span into the caller's
//      buffer or key material outlives the load() call that created it.
//   3. Output is delivered by copy into a caller-owned object; load()
//      returns a plain enum value.
//   4. Everything is noexcept.
//   5. Zero heap allocation: fixed std::array stack buffers,
//      std::optional<Verified...> value semantics (no dynamic storage),
//      no strings, no containers.

#pragma once

#include <hengyuan/compaction_intent_store.hpp>  // hy::LoadStatus
#include <hengyuan/compaction_lease.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/seal_export_migration_cleanup_abandon_codec.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace hy {

// hy::LoadStatus (compaction_intent_store.hpp) already carries exactly the
// status set this loader needs — Ok/NotFound/Corrupt/LeaseNotHeld/
// CandidateFenced/DirectoryIdentityChanged/KeyNotFound/IoError — so it is
// reused rather than redefined: a second hy::LoadStatus would collide in any
// translation unit that includes both headers.

// Read-only loader for "seal-export-started.mig". Constructed with the
// lease, then load() performs read -> verify -> copy-out in one call.
// Does not mutate the lease (or anything else); safe to call repeatedly.
template <typename LeaseT>
class SealExportStartedMigrationLoader {
public:
    explicit SealExportStartedMigrationLoader(LeaseT& lease) noexcept : lease_(lease) {}
    LoadStatus load(KeyRing& key_ring, SealExportStartedMigrationWire& out) noexcept;

private:
    LeaseT& lease_;
};

// Read-only loader for "seal-export-started.clr" — same contract as the
// migration loader above, different wire type and read method.
template <typename LeaseT>
class SealStartedCleanupTombstoneLoader {
public:
    explicit SealStartedCleanupTombstoneLoader(LeaseT& lease) noexcept : lease_(lease) {}
    LoadStatus load(KeyRing& key_ring, SealStartedCleanupTombstoneWire& out) noexcept;

private:
    LeaseT& lease_;
};

template <typename LeaseT>
inline LoadStatus SealExportStartedMigrationLoader<LeaseT>::load(KeyRing& key_ring,
                                                                  SealExportStartedMigrationWire& out) noexcept {
    std::array<std::byte, kSealExportStartedMigrationWireBytes> buf{};
    // Peek the key id first (unauthenticated) so we know which key to pin
    // -- decode itself verifies the MAC against that specific key.
    const LeaseReadResult read_result = lease_.read_seal_export_started_migration(buf);
    switch (read_result.outcome) {
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
    switch (read_result.read.status) {
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
    if (!peek_seal_export_started_migration_v2_kek_key_id(buf, kek_key_id)) return LoadStatus::Corrupt;

    const PinResult pin = key_ring.pin_key(kek_key_id);
    if (pin.status != PinStatus::Pinned) return LoadStatus::KeyNotFound;

    std::optional<VerifiedSealExportStartedMigration> verified;
    if (decode_seal_export_started_migration_wire(buf, pin.handle->key_bytes(), verified) !=
        SealStartedWireDecodeStatus::Ok) {
        return LoadStatus::Corrupt;
    }
    out = verified->value();
    return LoadStatus::Ok;
}

template <typename LeaseT>
inline LoadStatus SealStartedCleanupTombstoneLoader<LeaseT>::load(KeyRing& key_ring,
                                                                   SealStartedCleanupTombstoneWire& out) noexcept {
    std::array<std::byte, kSealStartedCleanupWireBytes> buf{};
    const LeaseReadResult read_result = lease_.read_seal_started_cleanup_tombstone(buf);
    switch (read_result.outcome) {
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
    switch (read_result.read.status) {
        case compaction_detail::ReadFixedStatus::NotFound:
            return LoadStatus::NotFound;
        case compaction_detail::ReadFixedStatus::WrongSize:
        case compaction_detail::ReadFixedStatus::NotRegularFile:
            // Same Corrupt-not-IoError reasoning as the migration loader
            // above and IntentStore::load_and_validate_intent().
            return LoadStatus::Corrupt;
        case compaction_detail::ReadFixedStatus::IoError:
            return LoadStatus::IoError;
        case compaction_detail::ReadFixedStatus::Ok:
            break;
    }

    std::uint32_t kek_key_id = 0;
    if (!peek_seal_started_cleanup_kek_key_id(buf, kek_key_id)) return LoadStatus::Corrupt;

    const PinResult pin = key_ring.pin_key(kek_key_id);
    if (pin.status != PinStatus::Pinned) return LoadStatus::KeyNotFound;

    std::optional<VerifiedSealStartedCleanupTombstone> verified;
    if (decode_seal_started_cleanup_tombstone_wire(buf, pin.handle->key_bytes(), verified) !=
        SealStartedWireDecodeStatus::Ok) {
        return LoadStatus::Corrupt;
    }
    out = verified->value();
    return LoadStatus::Ok;
}

}  // namespace hy
