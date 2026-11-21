// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/durable_audit_export.hpp>
#include <hengyuan/kek_loader.hpp>
#include <hengyuan/owner_cancel_approval.hpp>

#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hy::spot_cancel_control {

inline constexpr std::size_t kMaxOwnerKeyBytes = 1024;
inline constexpr std::size_t kMaxOwnerSignatureBytes = 72;
inline constexpr std::int64_t kMaxGrantMs = 600'000;

inline std::optional<StoreEnvironment> parse_environment(std::string_view name) noexcept {
    if (name == "TESTNET") return StoreEnvironment::Testnet;
    if (name == "PRODUCTION") return StoreEnvironment::Production;
    return std::nullopt;
}

template <typename UInt>
bool parse_id(std::string_view text, UInt& id) noexcept {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), id);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && id != 0;
}

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

// The audit directory is owner-controlled. The grant and signature are separate from this trust record.
inline std::string enrollment_path(const std::string& audit_path) { return audit_path + ".owner-cancel.pub"; }

inline ExportStatus publish_file(const std::string& path, std::span<const std::byte> bytes) {
    ExclusiveOutputFile file;
    if (detail::open_archive_staging(file, path) != ExclusiveOutputFile::OpenStatus::Ok ||
        !file.write({reinterpret_cast<const char*>(bytes.data()), bytes.size()})) return ExportStatus::OutputError;
    return file.publish_as(path);
}

inline constexpr std::string_view kEnrollmentVersion = "HYCANPK1";
inline constexpr std::size_t kEnrollmentHeaderSize = kEnrollmentVersion.size() + 1 + 4 * sizeof(std::uint64_t);

inline ExportStatus enroll(const StoreFiles& files, StoreEnvironment environment, std::uint64_t owner_key_id,
                           const std::string& der_path) {
    auto store = open_existing_store(files, environment);
    if (!store.sink || owner_key_id == 0 || !is_regular_file(der_path)) return ExportStatus::OutputError;
    std::vector<std::byte> key;
    if (read_file_bounded(der_path, key, kMaxOwnerKeyBytes) != ReadFileStatus::Ok ||
        !OwnerCancelAuthority::valid_public_key(unsigned_bytes(key))) return ExportStatus::OutputError;

    const auto binding = store.sink->read_binding();
    if (binding.status != StoreBindingStatus::Bound || binding.binding.environment != environment) {
        return ExportStatus::OutputError;
    }
    std::vector<std::byte> record(kEnrollmentHeaderSize + key.size());
    std::memcpy(record.data(), kEnrollmentVersion.data(), kEnrollmentVersion.size());
    auto* p = record.data() + kEnrollmentVersion.size();
    detail::write_u8(p, static_cast<std::uint8_t>(environment));
    detail::write_u64_le(p, owner_key_id);
    detail::write_u64_le(p, binding.binding.account_uid);
    detail::write_u64_le(p, store.sink->store_uuid_lo());
    detail::write_u64_le(p, store.sink->store_uuid_hi());
    std::memcpy(p, key.data(), key.size());
    return publish_file(enrollment_path(files.log), record);
}

inline std::unique_ptr<OwnerCancelAuthority> load_authority(const std::string& audit_path,
                                                           const DurableAuditSink& sink) {
    const auto path = enrollment_path(audit_path);
    std::vector<std::byte> record;
    if (!is_regular_file(path) ||
        read_file_bounded(path, record, kEnrollmentHeaderSize + kMaxOwnerKeyBytes) != ReadFileStatus::Ok ||
        record.size() <= kEnrollmentHeaderSize ||
        std::memcmp(record.data(), kEnrollmentVersion.data(), kEnrollmentVersion.size()) != 0) return {};

    auto p = static_cast<const std::byte*>(record.data()) + kEnrollmentVersion.size();
    const auto environment = static_cast<StoreEnvironment>(detail::read_u8(p));
    const auto key_id = detail::read_u64_le(p);
    const auto uid = detail::read_u64_le(p);
    const auto uuid_lo = detail::read_u64_le(p);
    const auto uuid_hi = detail::read_u64_le(p);
    const auto binding = sink.read_binding();
    if (binding.status != StoreBindingStatus::Bound || binding.binding.environment != environment ||
        binding.binding.account_uid != uid || sink.store_uuid_lo() != uuid_lo || sink.store_uuid_hi() != uuid_hi) return {};
    return OwnerCancelAuthority::create(binding.binding, uuid_lo, uuid_hi, key_id,
                                        unsigned_bytes(std::span(record).subspan(kEnrollmentHeaderSize)), kMaxGrantMs);
}

inline std::optional<ClientOrderId> new_cancel_alias() noexcept {
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) return std::nullopt;
    ClientOrderId alias{};
    std::memcpy(alias.id, "HYC-", 4);
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t i = 0; i < random.size(); ++i) {
        alias.id[4 + 2 * i] = digits[random[i] >> 4];
        alias.id[5 + 2 * i] = digits[random[i] & 15];
    }
    return alias;
}

}  // namespace hy::spot_cancel_control
