// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/owner_cancel_approval.hpp>
#include <hengyuan/spot_owner_key_store.hpp>

#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace hy::spot_cancel_control {

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

using spot_owner_key_store::OpenStore;
using spot_owner_key_store::StoreFiles;
using spot_owner_key_store::is_regular_file;
using spot_owner_key_store::open_existing_store;
using spot_owner_key_store::publish_file;
using spot_owner_key_store::unsigned_bytes;

inline constexpr std::string_view kEnrollmentVersion = "HYCANPK1";

inline std::string enrollment_path(const std::string& audit_path) { return audit_path + ".owner-cancel.pub"; }

inline ExportStatus enroll(const StoreFiles& files, StoreEnvironment environment, std::uint64_t owner_key_id,
                           const std::string& der_path) {
    return spot_owner_key_store::enroll(files, environment, owner_key_id, der_path,
                                        enrollment_path(files.log), kEnrollmentVersion);
}

inline std::unique_ptr<OwnerCancelAuthority> load_authority(const std::string& audit_path,
                                                           const DurableAuditSink& sink) {
    const auto record = spot_owner_key_store::read_enrollment(enrollment_path(audit_path), kEnrollmentVersion, sink);
    if (!record) return {};
    return OwnerCancelAuthority::create(record->binding, record->uuid_lo, record->uuid_hi, record->key_id,
                                        unsigned_bytes(record->der), kMaxGrantMs);
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
