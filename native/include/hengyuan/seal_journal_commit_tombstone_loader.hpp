// SPDX-License-Identifier: proprietary
// seal_journal_commit_tombstone_loader.hpp — read-only L2 loaders for
// per-store SealJournalCommitWatermark (.jhw) and
// SealJournalTombstoneWire (.jts) records.
//
// Absolute rules inherited from the Round D review closures:
//   1. No filesystem or key handle is exported.
//   2. The fixed input buffer, PinResult, and key_bytes() view are local to
//      one load() call; the caller receives only a copied wire value.
//   3. The peeked kek_key_id selects a key but is not trusted as record data.
//      Only a successful MAC-and-shape decode may populate `out`.
//   4. load() is noexcept and allocates no heap memory. The scan helper is a
//      diagnostic aggregation layer; only its result vector may allocate.
//   5. Tombstones are discovered only by enumerating the authenticated
//      1..highest_committed_journal_seq range. No directory listing is used.

#pragma once

#include <hengyuan/key_ring.hpp>
#include <hengyuan/seal_journal_commit_tombstone_codec.hpp>
#include <hengyuan/seal_journal_store_lease.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace hy {

// This store-level status must remain distinct from compaction_intent_store.hpp's
// existing hy::LoadStatus: that enum reports CandidateFenced, while this lease
// reports StoreDirFenced. Re-declaring another hy::LoadStatus would make the two
// otherwise-related loader headers impossible to include in one translation unit.
enum class SealJournalStoreLoadStatus : std::uint8_t {
    Ok,
    NotFound,
    Corrupt,
    LeaseNotHeld,
    StoreDirFenced,
    DirectoryIdentityChanged,
    KeyNotFound,
    IoError,
};

class SealJournalCommitWatermarkLoader {
public:
    explicit SealJournalCommitWatermarkLoader(SealJournalStoreLease& lease) noexcept
        : lease_(lease) {}

    SealJournalStoreLoadStatus load(std::uint64_t candidate_id, KeyRing& key_ring,
                    SealJournalCommitWatermark& out) noexcept;

private:
    SealJournalStoreLease& lease_;
};

class SealJournalTombstoneLoader {
public:
    explicit SealJournalTombstoneLoader(SealJournalStoreLease& lease) noexcept
        : lease_(lease) {}

    SealJournalStoreLoadStatus load(std::uint64_t candidate_id, std::uint64_t journal_seq,
                    KeyRing& key_ring, SealJournalTombstoneWire& out) noexcept;

    struct ScanResult {
        SealJournalStoreLoadStatus status{SealJournalStoreLoadStatus::Ok};
        std::vector<SealJournalTombstoneWire> found;
    };

    ScanResult scan_up_to_watermark(
        std::uint64_t candidate_id,
        std::uint64_t highest_committed_journal_seq,
        KeyRing& key_ring) noexcept;

private:
    SealJournalStoreLease& lease_;
};

namespace seal_journal_commit_tombstone_loader_detail {

// The reader callable keeps the friend-only lease access at the class-member
// call site while leaving the complete classification/verification state
// machine independently testable with a deterministic lease fake.
template <typename Reader>
inline SealJournalStoreLoadStatus load_commit_watermark_from_reader(
    Reader&& reader, KeyRing& key_ring,
    SealJournalCommitWatermark& out) noexcept {
    std::array<std::byte, kSealJournalCommitWatermarkWireBytes> buf{};
    // Peek the key_id first (unauthenticated) so we know which key to pin
    // -- decode itself verifies the MAC against that specific key.
    const auto probe_read = reader(buf);
    switch (probe_read.outcome) {
        case SealJournalLeaseIoOutcome::WrongOwner:
        case SealJournalLeaseIoOutcome::NotHeld:
            return SealJournalStoreLoadStatus::LeaseNotHeld;
        case SealJournalLeaseIoOutcome::StoreDirFenced:
            return SealJournalStoreLoadStatus::StoreDirFenced;
        case SealJournalLeaseIoOutcome::DirectoryIdentityChanged:
            return SealJournalStoreLoadStatus::DirectoryIdentityChanged;
        case SealJournalLeaseIoOutcome::Ok:
            break;
    }
    using ReadFixedStatus =
        std::remove_cvref_t<decltype(probe_read.read.status)>;
    switch (probe_read.read.status) {
        case ReadFixedStatus::NotFound:
            return SealJournalStoreLoadStatus::NotFound;
        case ReadFixedStatus::WrongSize:
        case ReadFixedStatus::NotRegularFile:
            // A wrong-size/non-regular record is malformed durable state,
            // not a transient read failure.
            return SealJournalStoreLoadStatus::Corrupt;
        case ReadFixedStatus::IoError:
            return SealJournalStoreLoadStatus::IoError;
        case ReadFixedStatus::Ok:
            break;
    }

    std::uint32_t kek_key_id{0};
    if (!peek_seal_journal_commit_watermark_kek_key_id(buf, kek_key_id)) {
        return SealJournalStoreLoadStatus::Corrupt;
    }

    const PinResult pin = key_ring.pin_key(kek_key_id);
    if (pin.status != PinStatus::Pinned) return SealJournalStoreLoadStatus::KeyNotFound;

    std::optional<VerifiedSealJournalCommitWatermark> verified;
    if (decode_seal_journal_commit_watermark_wire(
            buf, pin.handle->key_bytes(), verified) !=
        SealJournalCommitTombstoneDecodeStatus::Ok) {
        return SealJournalStoreLoadStatus::Corrupt;
    }
    out = verified->value();
    return SealJournalStoreLoadStatus::Ok;
}

template <typename Lease>
inline SealJournalStoreLoadStatus load_commit_watermark(
    Lease& lease, std::uint64_t candidate_id, KeyRing& key_ring,
    SealJournalCommitWatermark& out) noexcept {
    return load_commit_watermark_from_reader(
        [&](std::span<std::byte, kSealJournalCommitWatermarkWireBytes> buf)
            noexcept {
                return lease.read_seal_journal_commit_watermark(candidate_id,
                                                                 buf);
            },
        key_ring, out);
}

template <typename Reader>
inline SealJournalStoreLoadStatus load_tombstone_from_reader(
    Reader&& reader, KeyRing& key_ring,
    SealJournalTombstoneWire& out) noexcept {
    std::array<std::byte, kSealJournalTombstoneBytes> buf{};
    // Peek the key_id first (unauthenticated) so we know which key to pin
    // -- decode itself verifies the MAC against that specific key.
    const auto probe_read = reader(buf);
    switch (probe_read.outcome) {
        case SealJournalLeaseIoOutcome::WrongOwner:
        case SealJournalLeaseIoOutcome::NotHeld:
            return SealJournalStoreLoadStatus::LeaseNotHeld;
        case SealJournalLeaseIoOutcome::StoreDirFenced:
            return SealJournalStoreLoadStatus::StoreDirFenced;
        case SealJournalLeaseIoOutcome::DirectoryIdentityChanged:
            return SealJournalStoreLoadStatus::DirectoryIdentityChanged;
        case SealJournalLeaseIoOutcome::Ok:
            break;
    }
    using ReadFixedStatus =
        std::remove_cvref_t<decltype(probe_read.read.status)>;
    switch (probe_read.read.status) {
        case ReadFixedStatus::NotFound:
            return SealJournalStoreLoadStatus::NotFound;
        case ReadFixedStatus::WrongSize:
        case ReadFixedStatus::NotRegularFile:
            // A wrong-size/non-regular record is malformed durable state,
            // not a transient read failure.
            return SealJournalStoreLoadStatus::Corrupt;
        case ReadFixedStatus::IoError:
            return SealJournalStoreLoadStatus::IoError;
        case ReadFixedStatus::Ok:
            break;
    }

    std::uint32_t kek_key_id{0};
    if (!peek_seal_journal_tombstone_kek_key_id(buf, kek_key_id)) {
        return SealJournalStoreLoadStatus::Corrupt;
    }

    const PinResult pin = key_ring.pin_key(kek_key_id);
    if (pin.status != PinStatus::Pinned) return SealJournalStoreLoadStatus::KeyNotFound;

    std::optional<VerifiedSealJournalTombstoneWire> verified;
    if (decode_seal_journal_tombstone_wire(
            buf, pin.handle->key_bytes(), verified) !=
        SealJournalCommitTombstoneDecodeStatus::Ok) {
        return SealJournalStoreLoadStatus::Corrupt;
    }
    out = verified->value();
    return SealJournalStoreLoadStatus::Ok;
}

template <typename Lease>
inline SealJournalStoreLoadStatus load_tombstone(
    Lease& lease, std::uint64_t candidate_id, std::uint64_t journal_seq,
    KeyRing& key_ring, SealJournalTombstoneWire& out) noexcept {
    return load_tombstone_from_reader(
        [&](std::span<std::byte, kSealJournalTombstoneBytes> buf) noexcept {
            return lease.read_seal_journal_tombstone(candidate_id, journal_seq,
                                                      buf);
        },
        key_ring, out);
}

template <typename Loader>
inline SealJournalTombstoneLoader::ScanResult scan_from_loader(
    Loader&& loader,
    std::uint64_t highest_committed_journal_seq) noexcept {
    SealJournalTombstoneLoader::ScanResult result{};
    if (highest_committed_journal_seq == 0) return result;

    std::uint64_t journal_seq{1};
    for (;;) {
        SealJournalTombstoneWire tombstone{};
        const SealJournalStoreLoadStatus status = loader(journal_seq, tombstone);
        if (status == SealJournalStoreLoadStatus::Ok) {
            // ScanResult has no allocation-specific status. Preserve the
            // collected prefix and fail closed as IoError if vector growth
            // cannot complete; no exception may escape this noexcept API.
            try {
                result.found.push_back(tombstone);
            } catch (...) {
                result.status = SealJournalStoreLoadStatus::IoError;
                return result;
            }
        } else if (status != SealJournalStoreLoadStatus::NotFound) {
            result.status = status;
            return result;
        }

        // This form is intentional: it terminates correctly even when the
        // authenticated watermark is UINT64_MAX, without increment-wrap to 0.
        if (journal_seq == highest_committed_journal_seq) break;
        ++journal_seq;
    }
    return result;
}

template <typename Lease>
inline SealJournalTombstoneLoader::ScanResult scan_up_to_watermark(
    Lease& lease, std::uint64_t candidate_id,
    std::uint64_t highest_committed_journal_seq,
    KeyRing& key_ring) noexcept {
    return scan_from_loader(
        [&](std::uint64_t journal_seq,
            SealJournalTombstoneWire& out) noexcept {
            return load_tombstone(lease, candidate_id, journal_seq, key_ring,
                                  out);
        },
        highest_committed_journal_seq);
}

}  // namespace seal_journal_commit_tombstone_loader_detail

inline SealJournalStoreLoadStatus SealJournalCommitWatermarkLoader::load(
    std::uint64_t candidate_id, KeyRing& key_ring,
    SealJournalCommitWatermark& out) noexcept {
    return seal_journal_commit_tombstone_loader_detail::
        load_commit_watermark_from_reader(
            [&](std::span<std::byte,
                          kSealJournalCommitWatermarkWireBytes> buf) noexcept {
                return lease_.read_seal_journal_commit_watermark(candidate_id,
                                                                  buf);
            },
            key_ring, out);
}

inline SealJournalStoreLoadStatus SealJournalTombstoneLoader::load(
    std::uint64_t candidate_id, std::uint64_t journal_seq,
    KeyRing& key_ring, SealJournalTombstoneWire& out) noexcept {
    return seal_journal_commit_tombstone_loader_detail::
        load_tombstone_from_reader(
            [&](std::span<std::byte, kSealJournalTombstoneBytes> buf) noexcept {
                return lease_.read_seal_journal_tombstone(candidate_id,
                                                           journal_seq, buf);
            },
            key_ring, out);
}

inline SealJournalTombstoneLoader::ScanResult
SealJournalTombstoneLoader::scan_up_to_watermark(
    std::uint64_t candidate_id,
    std::uint64_t highest_committed_journal_seq,
    KeyRing& key_ring) noexcept {
    return seal_journal_commit_tombstone_loader_detail::scan_from_loader(
        [&](std::uint64_t journal_seq,
            SealJournalTombstoneWire& out) noexcept {
            return load(candidate_id, journal_seq, key_ring, out);
        },
        highest_committed_journal_seq);
}

}  // namespace hy
