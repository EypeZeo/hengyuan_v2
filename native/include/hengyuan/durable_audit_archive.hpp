// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/audit_archive_format.hpp>
#include <hengyuan/durable_audit_export.hpp>

namespace hy {

namespace detail {

inline constexpr std::size_t kArchivePrefixSize = audit_archive::kHeaderSize + kTipAnchorSize + kStoreIdentitySize;

// Held for the entire snapshot and publication. The lock sidecar already belongs to the source store.
class ArchiveSourceLock {
public:
    ArchiveSourceLock() = default;
    ArchiveSourceLock(const ArchiveSourceLock&) = delete;
    ArchiveSourceLock& operator=(const ArchiveSourceLock&) = delete;

    ~ArchiveSourceLock() {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (fd_ >= 0) ::close(fd_);
#endif
    }

    ExportStatus acquire(const std::string& lock_path) noexcept {
#if defined(_WIN32)
        handle_ = CreateFileA(lock_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ != INVALID_HANDLE_VALUE) {
            return GetFileType(handle_) == FILE_TYPE_DISK ? ExportStatus::Ok : ExportStatus::WriterUnknown;
        }
        const auto error = GetLastError();
        return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION
            ? ExportStatus::WriterLive : ExportStatus::WriterUnknown;
#else
        fd_ = ::open(lock_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd_ < 0) return ExportStatus::WriterUnknown;
        struct stat status{};
        if (::fstat(fd_, &status) != 0 || !S_ISREG(status.st_mode)) return ExportStatus::WriterUnknown;
        if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) return ExportStatus::Ok;
        return errno == EWOULDBLOCK || errno == EAGAIN ? ExportStatus::WriterLive : ExportStatus::WriterUnknown;
#endif
    }

private:
#if defined(_WIN32)
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int fd_{-1};
#endif
};

inline bool matches_existing_migration(const std::string& path, const AuditExportInput& input) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error) return false;
    std::ifstream archive(path, std::ios::binary);
    std::array<std::byte, audit_archive::kHeaderSize> bytes;
    if (!audit_archive::read_exact(archive, bytes)) return false;
    const auto header = audit_archive::decode_header(bytes);
    return header && header->kind == audit_archive::Kind::S7Migration && header->log_bytes == input.log.size() &&
           audit_archive::matches_bytes(archive, input.anchor) && audit_archive::matches_bytes(archive, input.identity) &&
           audit_archive::matches_bytes(archive, input.log) && audit_archive::at_end(archive);
}

inline ExportStatus existing_migration_status(const std::string& path, const AuditExportInput& input) {
    if (!matches_existing_migration(path, input)) return ExportStatus::OutputExists;
    return audit_archive::sync_directory(path) ? ExportStatus::Ok : ExportStatus::OutputSyncUncertain;
}

inline ExportFileResult publish_audit_archive(const std::string& log_path, const std::string& out_path,
                                               const KeyRing& ring, std::int64_t created_utc_ms,
                                               audit_archive::Kind kind) {
    ExportFileResult result;
    try {
        if (kind == audit_archive::Kind::Segment && is_reserved_migration_output(out_path)) {
            result.status = ExportStatus::ReservedMigrationPath;
            return result;
        }
        if (created_utc_ms <= 0) {
            result.status = ExportStatus::MalformedArchive;
            return result;
        }
        ArchiveSourceLock source_lock;
        result.status = source_lock.acquire(log_path + ".lock");
        if (result.status != ExportStatus::Ok) return result;

        AuditExportInput input;
        result.status = read_audit_export_input(log_path, input);
        if (result.status != ExportStatus::Ok) return result;
        const ExportOptions opts{.require_complete_source = true};
        result.verification = verify_audit_log(input.log, ring, input.anchor_present, input.anchor,
                                               input.identity_present, input.identity, opts);
        result.status = result.verification.status;
        if (result.status != ExportStatus::Ok) return result;
        if (kind == audit_archive::Kind::S7Migration && result.verification.first_v6_sequence) {
            result.status = ExportStatus::MigrationSourceVersion;
            return result;
        }

        std::error_code output_error;
        if (std::filesystem::exists(out_path, output_error)) {
            result.status = kind == audit_archive::Kind::S7Migration
                ? existing_migration_status(out_path, input) : ExportStatus::OutputExists;
            return result;
        }

        const auto header = audit_archive::encode_header({kind, created_utc_ms, input.log.size()});
        ExclusiveOutputFile stage;
        if (output_error || open_archive_staging(stage, out_path) != ExclusiveOutputFile::OpenStatus::Ok) {
            result.status = ExportStatus::OutputError;
            return result;
        }
        const auto write_bytes = [&stage](std::span<const std::byte> bytes) {
            return stage.write({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        };
        if (!write_bytes(header) || !write_bytes(input.anchor) || !write_bytes(input.identity) || !write_bytes(input.log)) {
            result.status = ExportStatus::OutputError;
            return result;
        }
        result.status = stage.publish_as(out_path);
        if (result.status == ExportStatus::OutputExists && kind == audit_archive::Kind::S7Migration) {
            result.status = existing_migration_status(out_path, input);
        }
    } catch (...) {
        result.status = ExportStatus::IoError;
    }
    return result;
}

}  // namespace detail

inline ExportFileResult archive_audit_log_file(const std::string& log, const std::string& out, const KeyRing& ring,
                                               std::int64_t created_utc_ms) {
    return detail::publish_audit_archive(log, out, ring, created_utc_ms, audit_archive::Kind::Segment);
}

inline ExportFileResult prepare_audit_migration(const std::string& log, const KeyRing& ring,
                                                std::int64_t created_utc_ms) {
    return detail::publish_audit_archive(log, log + std::string(audit_archive::kMigrationSuffix), ring, created_utc_ms,
                                         audit_archive::Kind::S7Migration);
}

inline ExportFileResult verify_audit_archive_file(const std::string& path, const KeyRing& ring) {
    ExportFileResult result;
    try {
        std::vector<std::byte> file;
        switch (read_file_bounded(path, file, kMaxDurableLogBytes + detail::kArchivePrefixSize)) {
            case ReadFileStatus::Ok: break;
            case ReadFileStatus::TooLarge: result.status = ExportStatus::LogTooLarge; return result;
            case ReadFileStatus::NotFound:
            case ReadFileStatus::Error: result.status = ExportStatus::IoError; return result;
        }
        const std::span<const std::byte> bytes(file);
        const auto header = audit_archive::decode_header(bytes);
        if (!header || bytes.size() < detail::kArchivePrefixSize ||
            bytes.size() - detail::kArchivePrefixSize != header->log_bytes) {
            result.status = ExportStatus::MalformedArchive;
            return result;
        }
        const auto anchor = bytes.subspan(audit_archive::kHeaderSize, kTipAnchorSize);
        const auto identity = bytes.subspan(audit_archive::kHeaderSize + kTipAnchorSize, kStoreIdentitySize);
        const ExportOptions opts{.require_complete_source = true};
        result.verification = verify_audit_log(bytes.subspan(detail::kArchivePrefixSize), ring, true, anchor, true,
                                               identity, opts);
        result.status = result.verification.status;
        if (result.status == ExportStatus::Ok && header->kind == audit_archive::Kind::S7Migration &&
            result.verification.first_v6_sequence) {
            result.status = ExportStatus::MigrationSourceVersion;
        }
    } catch (...) {
        result.status = ExportStatus::IoError;
    }
    return result;
}

}  // namespace hy
