// SPDX-License-Identifier: proprietary
// seal_started_abandon_loader.hpp — read-only L2 loader for
// SealStartedAbandonWire ("seal-export-started.abd").
//
// Governance: L2 (real file I/O, via CandidateLease's friend-only
// read_seal_started_abandon()). This class is intentionally read-only --
// it never publishes, replaces, or unlinks anything. The algorithm is a
// line-for-line structural copy of IntentStore::load_and_validate_intent()
// (compaction_intent_store.hpp), mapped onto the .abd codec
// (seal_export_migration_cleanup_abandon_codec.hpp).
//
// Absolute rules (docs/SPEC_INVARIANTS.md Round D review v4/v5 closures,
// still binding for every breadcrumb L2 loader):
//   1. Never export any handle type (no CandidateDirHandle, no
//      PinnedKeyHandle returned to the caller).
//   2. Never let buffer / key-material references outlive the load() call
//      that pinned them -- PinResult is a local, key_bytes() is consumed
//      only as a temporary decode argument, out is a by-value copy of the
//      verified wire fields.
//   3. Trust only fields that survived decode_seal_started_abandon_wire()
//      (MAC + shape). Peeked kek_key_id is used solely to select which key
//      to pin; it is not trusted as a semantic field on its own.
//   4. All paths noexcept; zero heap allocation on the load path
//      (fixed std::array buffer, stack PinResult / optional Verified*).

#pragma once

#include <hengyuan/compaction_intent_store.hpp>  // canonical hy::LoadStatus
#include <hengyuan/compaction_lease.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/seal_export_migration_cleanup_abandon_codec.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace hy {

class SealStartedAbandonLoader {
public:
    explicit SealStartedAbandonLoader(CandidateLease& lease) noexcept : lease_(lease) {}

    // Read-only: decode + fully verify whatever .abd currently exists under
    // the lease. Does not mutate anything. On any non-Ok return, `out` is
    // left untouched (caller-supplied bytes preserved).
    LoadStatus load(KeyRing& key_ring, SealStartedAbandonWire& out) noexcept {
        return load_from(lease_, key_ring, out);
    }

    // Same algorithm as load(). Templated so unit tests can bind
    // MockCandidateLeaseForAbandon without a filesystem; production
    // load() instantiates this with CandidateLease. Must remain a member
    // of this friend class -- a free-function detail would not be granted
    // access to CandidateLease's private read_seal_started_abandon().
    template <typename Lease>
    static LoadStatus load_from(Lease& lease, KeyRing& key_ring, SealStartedAbandonWire& out) noexcept {
        std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
        // Peek the key_id first (unauthenticated) so we know which key to pin
        // -- decode itself verifies the MAC against that specific key.
        const LeaseReadResult probe_read = lease.read_seal_started_abandon(buf);
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
        if (!peek_seal_started_abandon_kek_key_id(buf, kek_key_id)) return LoadStatus::Corrupt;

        const PinResult pin = key_ring.pin_key(kek_key_id);
        if (pin.status != PinStatus::Pinned) return LoadStatus::KeyNotFound;

        std::optional<VerifiedSealStartedAbandon> verified;
        if (decode_seal_started_abandon_wire(buf, pin.handle->key_bytes(), verified) !=
            SealStartedWireDecodeStatus::Ok) {
            return LoadStatus::Corrupt;
        }
        out = verified->value();
        return LoadStatus::Ok;
    }

private:
    CandidateLease& lease_;
};

}  // namespace hy
