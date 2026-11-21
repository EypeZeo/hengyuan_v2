// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/durable_audit_export.hpp>
#include <hengyuan/kek_loader.hpp>
#include <hengyuan/owner_p256_signature.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hy::spot_owner_key_store {

struct StoreFiles {
    std::string log;
    std::uint32_t audit_key_id{0};
    std::string kek;
    std::string wrapped_keys;
};

struct OpenStore {
    std::unique_ptr<KeyRing> keys;
    std::unique_ptr<DurableAuditSink> sink;
};

inline bool is_regular_file(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::symlink_status(path, ec)) && !ec;
}

inline OpenStore open_existing_store(const StoreFiles& files, StoreEnvironment environment) {
    if (!is_regular_file(files.log) || !is_regular_file(files.log + ".storeid.tip") ||
        !is_regular_file(files.log + ".binding.tip") ||
        !is_regular_file(files.kek) || !is_regular_file(files.wrapped_keys)) return {};
    KekLoader kek;
    if (kek.load(files.kek.c_str()) != KekLoadStatus::Ok) return {};
    auto keys = std::make_unique<KeyRing>(kek.get());
    kek.wipe();
    std::vector<std::byte> wrapped;
    if (read_file_bounded(files.wrapped_keys, wrapped, kAuditKeyFileMagic.size() + 4 + kMaxLiveKeys * kAuditKeyFileRecordSize) != ReadFileStatus::Ok ||
        load_audit_key_file(wrapped, *keys) != AuditKeyFileStatus::Ok) return {};
    {
        std::array<std::byte, kKeyBlockSize> key{};
        ScopedSecureWipe wipe(key);
        if (!keys->active_key(files.audit_key_id, key)) return {};
    }
    auto sink = std::make_unique<DurableAuditSink>(files.log, *keys, files.audit_key_id);
    const auto binding = sink->read_binding();
    if (!sink->is_open() || sink->fenced() || sink->store_identity_degraded() ||
        binding.status != StoreBindingStatus::Bound || binding.binding.environment != environment) return {};
    return {std::move(keys), std::move(sink)};
}

inline std::span<const unsigned char> unsigned_bytes(std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()};
}

inline ExportStatus publish_file(const std::string& path, std::span<const std::byte> bytes) {
    ExclusiveOutputFile file;
    if (detail::open_archive_staging(file, path) != ExclusiveOutputFile::OpenStatus::Ok ||
        !file.write({reinterpret_cast<const char*>(bytes.data()), bytes.size()})) return ExportStatus::OutputError;
    return file.publish_as(path);
}

struct EnrollmentRecord {
    StoreBinding binding;
    std::uint64_t key_id;
    std::uint64_t uuid_lo;
    std::uint64_t uuid_hi;
    std::vector<std::byte> der;
};

inline constexpr std::size_t kEnrollmentFieldsSize = 1 + 4 * sizeof(std::uint64_t);

// The audit directory is controlled by the owner. Each approval purpose uses its own path and format tag.
inline ExportStatus enroll(const StoreFiles& files, StoreEnvironment environment, std::uint64_t owner_key_id,
                           const std::string& der_path, const std::string& record_path, std::string_view format_tag) {
    auto store = open_existing_store(files, environment);
    if (!store.sink || owner_key_id == 0 || !is_regular_file(der_path)) return ExportStatus::OutputError;
    std::vector<std::byte> key;
    if (read_file_bounded(der_path, key, owner_signature::kMaxPublicKeyBytes) != ReadFileStatus::Ok ||
        !owner_signature::decode_p256(unsigned_bytes(key))) return ExportStatus::OutputError;

    const auto binding = store.sink->read_binding();
    if (binding.status != StoreBindingStatus::Bound || binding.binding.environment != environment) {
        return ExportStatus::OutputError;
    }
    std::vector<std::byte> record(format_tag.size() + kEnrollmentFieldsSize + key.size());
    std::memcpy(record.data(), format_tag.data(), format_tag.size());
    auto* p = record.data() + format_tag.size();
    detail::write_u8(p, static_cast<std::uint8_t>(environment));
    detail::write_u64_le(p, owner_key_id);
    detail::write_u64_le(p, binding.binding.account_uid);
    detail::write_u64_le(p, store.sink->store_uuid_lo());
    detail::write_u64_le(p, store.sink->store_uuid_hi());
    std::memcpy(p, key.data(), key.size());
    return publish_file(record_path, record);
}

inline std::optional<EnrollmentRecord> read_enrollment(const std::string& path, std::string_view format_tag,
                                                       const DurableAuditSink& sink) {
    const auto header_size = format_tag.size() + kEnrollmentFieldsSize;
    std::vector<std::byte> record;
    if (!sink.is_open() || sink.fenced() || sink.store_identity_degraded() || !is_regular_file(path) ||
        read_file_bounded(path, record, header_size + owner_signature::kMaxPublicKeyBytes) != ReadFileStatus::Ok ||
        record.size() <= header_size ||
        std::memcmp(record.data(), format_tag.data(), format_tag.size()) != 0) return std::nullopt;

    auto p = static_cast<const std::byte*>(record.data()) + format_tag.size();
    const auto environment = static_cast<StoreEnvironment>(detail::read_u8(p));
    const auto key_id = detail::read_u64_le(p);
    const auto uid = detail::read_u64_le(p);
    const auto uuid_lo = detail::read_u64_le(p);
    const auto uuid_hi = detail::read_u64_le(p);
    const auto binding = sink.read_binding();
    if (key_id == 0 || binding.status != StoreBindingStatus::Bound || binding.binding.environment != environment ||
        binding.binding.account_uid != uid || sink.store_uuid_lo() != uuid_lo || sink.store_uuid_hi() != uuid_hi) {
        return std::nullopt;
    }
    record.erase(record.begin(), record.begin() + static_cast<std::ptrdiff_t>(header_size));
    return EnrollmentRecord{binding.binding, key_id, uuid_lo, uuid_hi, std::move(record)};
}

}  // namespace hy::spot_owner_key_store
