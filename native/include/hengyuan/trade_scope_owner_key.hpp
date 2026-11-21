// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/owner_trade_scope_approval.hpp>
#include <hengyuan/spot_owner_key_store.hpp>

namespace hy {

inline constexpr char kScopeFirstOrderDomain[] = "HY-SPOT-FIRST-ORDER";
using ScopeFirstOrderBytes = std::array<std::byte, sizeof(kScopeFirstOrderDomain) + 1 + 4 * sizeof(std::uint64_t)>;

inline ScopeFirstOrderBytes scope_first_order_declaration(const OperatorResetScope& store, std::uint64_t key_id) noexcept {
    ScopeFirstOrderBytes bytes{};
    auto* p = bytes.data();
    detail::write_bytes(p, kScopeFirstOrderDomain, sizeof(kScopeFirstOrderDomain));
    detail::write_u8(p, static_cast<std::uint8_t>(store.environment));
    detail::write_u64_le(p, store.account_uid);
    detail::write_u64_le(p, store.store_uuid_lo);
    detail::write_u64_le(p, store.store_uuid_hi);
    detail::write_u64_le(p, key_id);
    return bytes;
}

class TradeScopeOwnerKey {
public:
    static ExportStatus enroll(const spot_owner_key_store::StoreFiles& files, StoreEnvironment environment,
                               std::uint64_t key_id, const std::string& public_der) {
        return spot_owner_key_store::enroll(files, environment, key_id, public_der, key_path(files.log), kFormatTag);
    }

    static ExportStatus publish_first_order_draft(const spot_owner_key_store::StoreFiles& files, const std::string& output) {
        auto store = spot_owner_key_store::open_existing_store(files, StoreEnvironment::Production);
        if (!store.sink) return ExportStatus::OutputError;
        const auto registration = spot_owner_key_store::read_enrollment(key_path(files.log), kFormatTag, *store.sink);
        if (!registration) return ExportStatus::OutputError;
        const OperatorResetScope subject{StoreEnvironment::Production, registration->binding.account_uid,
                                        registration->uuid_lo, registration->uuid_hi};
        return spot_owner_key_store::publish_file(output, scope_first_order_declaration(subject, registration->key_id));
    }

    static std::optional<TradeScopeOwnerKey> load(const std::string& log, const DurableAuditSink& sink) {
        auto registration = spot_owner_key_store::read_enrollment(key_path(log), kFormatTag, sink);
        if (!registration) return std::nullopt;
        const OperatorResetScope subject{registration->binding.environment, registration->binding.account_uid,
                                        registration->uuid_lo, registration->uuid_hi};
        const auto key = owner_signature::decode_p256(spot_owner_key_store::unsigned_bytes(registration->der));
        if (!key) return std::nullopt;
        if (subject.environment == StoreEnvironment::Production) {
            const auto signature_path = log + ".first-order.sig";
            std::vector<std::byte> signature;
            const auto declaration = scope_first_order_declaration(subject, registration->key_id);
            if (!spot_owner_key_store::is_regular_file(signature_path) ||
                read_file_bounded(signature_path, signature, owner_signature::kMaxSignatureBytes) != ReadFileStatus::Ok ||
                !owner_signature::verify_p256(*key, declaration, spot_owner_key_store::unsigned_bytes(signature))) {
                return std::nullopt;
            }
        }
        return TradeScopeOwnerKey(subject, registration->key_id, std::move(registration->der));
    }

    [[nodiscard]] std::optional<VerifiedAccountRiskPolicy> load_account_policy(
        const std::string& path, std::uint64_t version, bool& missing) const {
        std::vector<std::byte> payload, signature;
        const auto grant_path = path + ".account.grant";
        const auto signature_path = path + ".account.sig";
        const auto grant = read_file_bounded(grant_path, payload, AccountRiskPolicyBytes{}.size());
        const auto sig = read_file_bounded(signature_path, signature, owner_signature::kMaxSignatureBytes);
        missing = grant == ReadFileStatus::NotFound && sig == ReadFileStatus::NotFound;
        if (grant != ReadFileStatus::Ok || sig != ReadFileStatus::Ok ||
            !spot_owner_key_store::is_regular_file(grant_path) || !spot_owner_key_store::is_regular_file(signature_path))
            return std::nullopt;
        return VerifiedAccountRiskPolicy::verify(payload, spot_owner_key_store::unsigned_bytes(signature),
                                                 public_key(), subject_, key_id_, version);
    }

    [[nodiscard]] const OperatorResetScope& subject() const noexcept { return subject_; }
    [[nodiscard]] std::uint64_t key_id() const noexcept { return key_id_; }
    [[nodiscard]] std::span<const unsigned char> public_key() const noexcept {
        return spot_owner_key_store::unsigned_bytes(der_);
    }

private:
    TradeScopeOwnerKey(OperatorResetScope subject, std::uint64_t key_id, std::vector<std::byte> der) noexcept
        : subject_(subject), key_id_(key_id), der_(std::move(der)) {}

    static std::string key_path(const std::string& log) { return log + ".owner-scope.pub"; }
    static constexpr std::string_view kFormatTag = "HYSCOPK1";

    OperatorResetScope subject_{};
    std::uint64_t key_id_{0};
    std::vector<std::byte> der_;
};

}  // namespace hy
