// SPDX-License-Identifier: proprietary
// compaction_intent_gc_authorized_loader.hpp — read-only L2 loader for
// CompactionIntentGcAuthorizedWire ("compaction-intent-gc-<nonce_hex16>.xgc",
// docs/SPEC_INVARIANTS.md's "Seal-journal Round E `.xgc` decode + loader"
// entry).
//
// Governance: L2 (real file I/O, via CandidateLease's friend-only
// read_compaction_intent_gc_authorized()). This class is intentionally
// read-only -- it never publishes, replaces, or unlinks anything. The
// algorithm is a line-for-line structural copy of SealStartedAbandonLoader
// (seal_started_abandon_loader.hpp), mapped onto the `.xgc` codec
// (compaction_intent_codec.hpp) -- with one difference: `.xgc`'s filename
// carries build_nonce (ValidatedArtifactName::for_xgc(build_nonce)), not a
// fixed literal, so load() takes build_nonce as a parameter the other six
// breadcrumb loaders don't need.
//
// Absolute rules (same set every breadcrumb L2 loader enforces):
//   1. Never export any handle type (no CandidateDirHandle, no
//      PinnedKeyHandle returned to the caller).
//   2. Never let buffer / key-material references outlive the load() call
//      that pinned them -- PinResult is a local, key_bytes() is consumed
//      only as a temporary decode argument, out is a by-value copy of the
//      verified wire fields.
//   3. Trust only fields that survived decode_compaction_intent_gc_
//      authorized_wire() (MAC + structural checks -- see that function's own
//      comment for why it does NOT also apply is_legal_cleanup_auth_flags()'s
//      business-legality rules; this loader inherits that same boundary and
//      does not call it either). Peeked kek_key_id is used solely to select
//      which key to pin; it is not trusted as a semantic field on its own.
//   4. All paths noexcept; zero heap allocation on the load path (fixed
//      std::array buffer, stack PinResult / optional Verified*).
//
// SCOPE: read-only, decode-only, matching CompactionIntentGcAuthorizedWire's
// own doc comment (durable_control_plane.hpp) -- "Round D's codec can
// decode/verify this type... but no production code path ever constructs/
// publishes one." This loader does not change that: no write/CREATE_NEW
// method exists on CandidateLease for `.xgc`, and none is added here. Real
// `.xgc` CREATE requires a real crash-safe capture-then-unlink-then-CREATE
// write path plus real journal drain-completeness checking, neither of
// which exists in this repo -- see docs/SPEC_INVARIANTS.md's ledger entry
// for the full reasoning.

#pragma once

#include <hengyuan/compaction_intent_codec.hpp>
#include <hengyuan/compaction_intent_store.hpp>  // canonical hy::LoadStatus
#include <hengyuan/compaction_lease.hpp>
#include <hengyuan/key_ring.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace hy {

class CompactionIntentGcAuthorizedLoader {
public:
    explicit CompactionIntentGcAuthorizedLoader(CandidateLease& lease) noexcept : lease_(lease) {}

    // Read-only: decode + fully verify whatever `.xgc` currently exists for
    // this build_nonce under the lease. Does not mutate anything. On any
    // non-Ok return, `out` is left untouched (caller-supplied bytes preserved).
    LoadStatus load(std::uint64_t build_nonce, KeyRing& key_ring, CompactionIntentGcAuthorizedWire& out) noexcept {
        return load_from(lease_, build_nonce, key_ring, out);
    }

    // Same algorithm as load(). Templated so unit tests can bind a mock
    // lease without a filesystem; production load() instantiates this with
    // CandidateLease. Must remain a member of this friend class -- a
    // free-function detail would not be granted access to CandidateLease's
    // private read_compaction_intent_gc_authorized().
    template <typename Lease>
    static LoadStatus load_from(Lease& lease, std::uint64_t build_nonce, KeyRing& key_ring,
                                 CompactionIntentGcAuthorizedWire& out) noexcept {
        std::array<std::byte, kCompactionIntentGcAuthorizedWireBytes> buf{};
        // Peek the key_id first (unauthenticated) so we know which key to pin
        // -- decode itself verifies the MAC against that specific key.
        const LeaseReadResult probe_read = lease.read_compaction_intent_gc_authorized(build_nonce, buf);
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
        if (!peek_compaction_intent_gc_authorized_kek_key_id(buf, kek_key_id)) return LoadStatus::Corrupt;

        const PinResult pin = key_ring.pin_key(kek_key_id);
        if (pin.status != PinStatus::Pinned) return LoadStatus::KeyNotFound;

        std::optional<VerifiedGcAuthorized> verified;
        if (decode_compaction_intent_gc_authorized_wire(buf, pin.handle->key_bytes(), verified) !=
            CompactionWireDecodeStatus::Ok) {
            return LoadStatus::Corrupt;
        }
        out = verified->value();
        return LoadStatus::Ok;
    }

private:
    CandidateLease& lease_;
};

}  // namespace hy
