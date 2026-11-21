// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/automatic_cancel.hpp>
#include <hengyuan/durable_frame_codec.hpp>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <utility>

namespace hy {

inline constexpr char kOwnerCancelGrantDomain[] = "HY-OWNER-SPOT-CANCEL";
inline constexpr std::uint8_t kOwnerCancelGrantVersion = 1;
using OwnerCancelGrantBytes =
    std::array<std::byte, sizeof(kOwnerCancelGrantDomain) + 2 + 7 * sizeof(std::uint64_t) + kSymbolNameLen>;

inline std::optional<OwnerCancelGrantBytes> encode_owner_cancel_grant(const CancelApproval& grant,
                                                                    std::uint64_t key_id) noexcept {
    if (!grant.subject.valid() || key_id == 0 || grant.serial == 0 || grant.not_after_ms <= 0 ||
        grant.symbol[kSymbolNameLen - 1] != '\0') return std::nullopt;
    const std::string_view symbol(grant.symbol);
    if (!cancel_prepared::valid_symbol(symbol)) return std::nullopt;

    OwnerCancelGrantBytes bytes{};
    std::copy_n(reinterpret_cast<const std::byte*>(kOwnerCancelGrantDomain), sizeof(kOwnerCancelGrantDomain),
                bytes.begin());
    auto* p = bytes.data() + sizeof(kOwnerCancelGrantDomain);
    detail::write_u8(p, kOwnerCancelGrantVersion);
    detail::write_u8(p, static_cast<std::uint8_t>(grant.subject.environment));
    detail::write_u64_le(p, key_id);
    detail::write_u64_le(p, grant.subject.account_uid);
    detail::write_u64_le(p, grant.subject.store_uuid_lo);
    detail::write_u64_le(p, grant.subject.store_uuid_hi);
    detail::write_u64_le(p, grant.subject.activation_id);
    detail::write_u64_le(p, grant.serial);
    detail::write_i64_le(p, grant.not_after_ms);
    std::memcpy(p, symbol.data(), symbol.size());
    return bytes;
}

enum class OwnerCancelInstallResult {
    Installed,
    MalformedGrant,
    WrongScope,
    InvalidSignature,
    StaleSerial,
    ClockUnavailable,
    InvalidDeadline,
};

// The control owner provisions the public key and serializes installations, reads and sends.
class OwnerCancelAuthority {
public:
    static std::unique_ptr<OwnerCancelAuthority> create(
        StoreBinding binding, std::uint64_t uuid_lo, std::uint64_t uuid_hi, std::uint64_t key_id,
        std::span<const unsigned char> public_key_der, std::int64_t max_validity_ms) {
        CancelSubject subject{binding.environment, binding.account_uid, uuid_lo, uuid_hi, 0};
        if (key_id == 0 || public_key_der.empty() || public_key_der.size() > 1024 || max_validity_ms <= 0 ||
            RAND_bytes(reinterpret_cast<unsigned char*>(&subject.activation_id),
                       static_cast<int>(sizeof(subject.activation_id))) != 1 || !subject.valid()) return {};

        auto key = decode_public_key(public_key_der);
        if (!key) return {};

        return std::unique_ptr<OwnerCancelAuthority>(
            new OwnerCancelAuthority(subject, key_id, std::move(key), max_validity_ms));
    }

    static bool valid_public_key(std::span<const unsigned char> der) noexcept {
        return static_cast<bool>(decode_public_key(der));
    }

    OwnerCancelAuthority(const OwnerCancelAuthority&) = delete;
    OwnerCancelAuthority& operator=(const OwnerCancelAuthority&) = delete;

    const CancelSubject& subject() const noexcept { return subject_; }

    std::optional<OwnerCancelGrantBytes> signing_payload(std::string_view symbol, std::uint64_t serial,
                                                       std::int64_t not_after_ms) const noexcept {
        if (!cancel_prepared::valid_symbol(symbol)) return std::nullopt;
        CancelApproval grant{subject_, {}, serial, not_after_ms};
        std::memcpy(grant.symbol, symbol.data(), symbol.size());
        return encode_owner_cancel_grant(grant, key_id_);
    }

    // The authority must outlive the pipeline configured with this borrowed port.
    AutoCancelConfig config() noexcept {
        return {subject_.activation_id, {&read_adapter, this}};
    }

    OwnerCancelInstallResult install(std::span<const std::byte> payload,
                                     std::span<const unsigned char> signature,
                                     const ClockOffsetPublisher& clock, ClockPairSample now) noexcept {
        using Result = OwnerCancelInstallResult;
        if (payload.size() != OwnerCancelGrantBytes{}.size()) return Result::MalformedGrant;
        auto p = payload.data() + sizeof(kOwnerCancelGrantDomain) + 1;
        CancelApproval grant{};
        grant.subject.environment = static_cast<StoreEnvironment>(detail::read_u8(p));
        const auto key_id = detail::read_u64_le(p);
        grant.subject.account_uid = detail::read_u64_le(p);
        grant.subject.store_uuid_lo = detail::read_u64_le(p);
        grant.subject.store_uuid_hi = detail::read_u64_le(p);
        grant.subject.activation_id = detail::read_u64_le(p);
        grant.serial = detail::read_u64_le(p);
        grant.not_after_ms = std::bit_cast<std::int64_t>(detail::read_u64_le(p));
        std::memcpy(grant.symbol, p, sizeof(grant.symbol));
        const auto encoded = encode_owner_cancel_grant(grant, key_id);
        if (!encoded || !std::equal(encoded->begin(), encoded->end(), payload.begin())) {
            return Result::MalformedGrant;
        }
        if (key_id != key_id_ || grant.subject != subject_) return Result::WrongScope;
        if (!verify(payload, signature)) return Result::InvalidSignature;
        if (grant.serial <= highest_serial_) return Result::StaleSerial;
        const auto upper = cancel_server_time_upper_bound(clock, now);
        if (!upper) return Result::ClockUnavailable;
        if (grant.not_after_ms <= *upper || grant.not_after_ms - *upper > max_validity_ms_) {
            return Result::InvalidDeadline;
        }
        highest_serial_ = grant.serial;
        active_ = grant;
        return Result::Installed;
    }

    void revoke(std::uint64_t through_serial) noexcept {
        highest_serial_ = std::max(highest_serial_, through_serial);
        if (active_ && active_->serial <= through_serial) active_.reset();
    }

    bool permits(const AutomaticCancelRequest& request, const ClockOffsetPublisher& clock,
                 ClockPairSample now) const noexcept {
        if (!active_ || request.symbol[kSymbolNameLen - 1] != '\0' ||
            request.approval_serial != active_->serial ||
            request.approval_not_after_ms != active_->not_after_ms) return false;
        const auto upper = cancel_server_time_upper_bound(clock, now);
        return upper && permits_automatic_cancel(*active_, request.subject, request.symbol, *upper);
    }

private:
    using PublicKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

    static PublicKey decode_public_key(std::span<const unsigned char> public_key_der) noexcept {
        if (public_key_der.empty() || public_key_der.size() > 1024) return PublicKey(nullptr, EVP_PKEY_free);
        const auto* p = public_key_der.data();
        PublicKey key(d2i_PUBKEY(nullptr, &p, static_cast<long>(public_key_der.size())), EVP_PKEY_free);
        if (!key || p != public_key_der.data() + public_key_der.size() ||
            EVP_PKEY_is_a(key.get(), "EC") != 1) return PublicKey(nullptr, EVP_PKEY_free);
        std::array<char, 32> group{};
        if (EVP_PKEY_get_group_name(key.get(), group.data(), group.size(), nullptr) != 1 ||
            std::string_view(group.data()) != "prime256v1") return PublicKey(nullptr, EVP_PKEY_free);
        const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> check(
            EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr), EVP_PKEY_CTX_free);
        if (!check || EVP_PKEY_public_check(check.get()) != 1) return PublicKey(nullptr, EVP_PKEY_free);
        return key;
    }

    OwnerCancelAuthority(CancelSubject subject, std::uint64_t key_id, PublicKey key,
                         std::int64_t max_validity_ms) noexcept
        : subject_(subject), key_id_(key_id), key_(std::move(key)), max_validity_ms_(max_validity_ms) {}

    static std::optional<CancelApproval> read_adapter(void* context, std::string_view symbol) noexcept {
        const auto& active = static_cast<const OwnerCancelAuthority*>(context)->active_;
        if (active && std::string_view(active->symbol) == symbol) return active;
        return std::nullopt;
    }

    bool verify(std::span<const std::byte> payload, std::span<const unsigned char> signature) const noexcept {
        if (signature.empty() || signature.size() > 72) return false;
        const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        return ctx && EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key_.get()) == 1 &&
               EVP_DigestVerify(ctx.get(), signature.data(), signature.size(),
                                reinterpret_cast<const unsigned char*>(payload.data()), payload.size()) == 1;
    }

    CancelSubject subject_;
    std::uint64_t key_id_;
    PublicKey key_;
    std::int64_t max_validity_ms_;
    std::uint64_t highest_serial_{0};
    std::optional<CancelApproval> active_;
};

}  // namespace hy
