// SPDX-License-Identifier: proprietary
// seal_id_watermark_export_started_loader.hpp -- fixed-name, read-only
// SealIdWatermark / SealExportStarted loaders. CandidateLease keeps the
// directory handle private; these two narrowly-scoped friends are the only
// consumers of their corresponding typed reads.

#pragma once

#include <hengyuan/compaction_intent_store.hpp>  // canonical hy::LoadStatus
#include <hengyuan/compaction_lease.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/seal_journal_precondition_codec.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace hy {

class SealIdWatermarkLoader {
public:
    explicit SealIdWatermarkLoader(CandidateLease& lease) noexcept : lease_(lease) {}

    LoadStatus load(std::uint32_t kek_key_id, KeyRing& key_ring, SealIdWatermark& out) noexcept {
        std::array<std::byte, kSealIdWatermarkWireBytes> buf{};
        const LeaseReadResult read_result = lease_.read_seal_id_watermark(buf);
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
                return LoadStatus::Corrupt;
            case compaction_detail::ReadFixedStatus::IoError:
                return LoadStatus::IoError;
            case compaction_detail::ReadFixedStatus::Ok:
                break;
        }

        const PinResult pin = key_ring.pin_key(kek_key_id);
        if (pin.status != PinStatus::Pinned) return LoadStatus::KeyNotFound;

        std::optional<VerifiedSealIdWatermark> verified;
        if (decode_seal_id_watermark_wire(buf, pin.handle->key_bytes(), verified) !=
            SealJournalPreconditionDecodeStatus::Ok) {
            return LoadStatus::Corrupt;
        }
        out = verified->value();
        return LoadStatus::Ok;
    }

private:
    CandidateLease& lease_;
};

class SealExportStartedLoader {
public:
    explicit SealExportStartedLoader(CandidateLease& lease) noexcept : lease_(lease) {}

    LoadStatus load(bool legacy_or_greenfield, KeyRing& key_ring, SealExportStartedWire& out) noexcept {
        std::array<std::byte, kSealExportStartedWireBytes> buf{};
        const LeaseReadResult read_result = lease_.read_seal_export_started(legacy_or_greenfield, buf);
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
                return LoadStatus::Corrupt;
            case compaction_detail::ReadFixedStatus::IoError:
                return LoadStatus::IoError;
            case compaction_detail::ReadFixedStatus::Ok:
                break;
        }

        std::uint32_t kek_key_id{0};
        if (!peek_seal_export_started_kek_key_id(buf, kek_key_id)) return LoadStatus::Corrupt;

        const PinResult pin = key_ring.pin_key(kek_key_id);
        if (pin.status != PinStatus::Pinned) return LoadStatus::KeyNotFound;

        std::optional<VerifiedSealExportStarted> verified;
        if (decode_seal_export_started_wire(buf, pin.handle->key_bytes(), verified) !=
            SealJournalPreconditionDecodeStatus::Ok) {
            return LoadStatus::Corrupt;
        }
        out = verified->value();
        return LoadStatus::Ok;
    }

private:
    CandidateLease& lease_;
};

}  // namespace hy
