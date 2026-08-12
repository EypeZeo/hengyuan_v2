// SPDX-License-Identifier: proprietary
// seal_journal_breadcrumb_precondition_aggregate.hpp — coordinator-only
// composition of Modules 1/3/4's independently-authored breadcrumb-
// directory loaders (docs/SPEC_INVARIANTS.md's "Seal-journal Round E
// breadcrumb L2 loaders" entry) into a single call. Deliberately written by
// the coordinator AFTER all three modules landed, not delegated to any of
// them -- this function needs to know all three modules' return types at
// once, which would have broken the "zero shared context between Modules
// 1-4" property if any one of them had been asked to write it.
//
// Scope: ONLY the five breadcrumb-DIRECTORY types (SealIdWatermark,
// SealExportStartedWire x2 filenames, SealExportStartedMigrationWire,
// SealStartedCleanupTombstoneWire, SealStartedAbandonWire) -- all read
// through one CandidateLease. Module 2's SealJournalCommitWatermark/
// SealJournalTombstoneWire live in a genuinely different directory
// (seal-journal/<store_uuid...>/, via SealJournalStoreLease) and are
// deliberately NOT folded into this aggregate; mixing two different
// directory scopes and two different lease objects into one "load
// everything" call would blur a distinction this round's ledger entry
// went out of its way to keep clear.
//
// Purely additive diagnostic composition: each field's status is
// independent. A NotFound/Corrupt/etc. on one file does not stop the
// others from being attempted -- there is no early return anywhere in this
// function. No cross-file consistency is checked here (e.g. this does NOT
// verify SealExportStartedWire.candidate_id matches SealIdWatermark's
// allocated range, or that the migration/cleanup/abandon records agree with
// each other) -- that is explicitly out of scope for this round (receipt
// concept / raise_intent_phase() / manager wiring, still not done).

#pragma once

#include <hengyuan/compaction_lease.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/seal_id_watermark_export_started_loader.hpp>
#include <hengyuan/seal_started_abandon_loader.hpp>
#include <hengyuan/seal_started_migration_cleanup_loader.hpp>

#include <cstdint>

namespace hy {

struct BreadcrumbPreconditionsResult {
    LoadStatus seal_id_watermark_status{LoadStatus::IoError};
    SealIdWatermark seal_id_watermark{};

    // "L" -- greenfield v2 or legacy 192B final, file "seal-export-started".
    LoadStatus seal_export_started_legacy_status{LoadStatus::IoError};
    SealExportStartedWire seal_export_started_legacy{};
    // "V" -- migration companion, file "seal-export-started.v2". Same wire
    // shape as L, different file.
    LoadStatus seal_export_started_migration_companion_status{LoadStatus::IoError};
    SealExportStartedWire seal_export_started_migration_companion{};

    LoadStatus seal_export_started_migration_status{LoadStatus::IoError};
    SealExportStartedMigrationWire seal_export_started_migration{};

    LoadStatus seal_started_cleanup_tombstone_status{LoadStatus::IoError};
    SealStartedCleanupTombstoneWire seal_started_cleanup_tombstone{};

    LoadStatus seal_started_abandon_status{LoadStatus::IoError};
    SealStartedAbandonWire seal_started_abandon{};
};

// seal_id_watermark_kek_key_id is required (not peeked) because
// SealIdWatermark's wire has no kek_key_id field -- see
// SealIdWatermarkLoader::load()'s own comment and this round's ledger entry
// for why no "current active key" fallback exists or should ever be added.
inline BreadcrumbPreconditionsResult load_all_breadcrumb_preconditions(
    CandidateLease& lease, KeyRing& key_ring, std::uint32_t seal_id_watermark_kek_key_id) noexcept {
    BreadcrumbPreconditionsResult result{};

    SealIdWatermarkLoader id_watermark_loader(lease);
    result.seal_id_watermark_status =
        id_watermark_loader.load(seal_id_watermark_kek_key_id, key_ring, result.seal_id_watermark);

    SealExportStartedLoader started_loader(lease);
    result.seal_export_started_legacy_status =
        started_loader.load(/*legacy_or_greenfield=*/true, key_ring, result.seal_export_started_legacy);
    result.seal_export_started_migration_companion_status = started_loader.load(
        /*legacy_or_greenfield=*/false, key_ring, result.seal_export_started_migration_companion);

    SealExportStartedMigrationLoader<CandidateLease> migration_loader(lease);
    result.seal_export_started_migration_status =
        migration_loader.load(key_ring, result.seal_export_started_migration);

    SealStartedCleanupTombstoneLoader<CandidateLease> cleanup_loader(lease);
    result.seal_started_cleanup_tombstone_status =
        cleanup_loader.load(key_ring, result.seal_started_cleanup_tombstone);

    SealStartedAbandonLoader abandon_loader(lease);
    result.seal_started_abandon_status = abandon_loader.load(key_ring, result.seal_started_abandon);

    return result;
}

}  // namespace hy
