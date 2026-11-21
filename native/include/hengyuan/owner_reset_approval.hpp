// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/binance_clock_sync.hpp>
#include <hengyuan/durable_frame_codec.hpp>
#include <hengyuan/operator_reset.hpp>
#include <hengyuan/owner_p256_signature.hpp>

#include <openssl/rand.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace hy {

inline constexpr char kOwnerResetDomain[] = "HY-OWNER-KILL-RESET";
inline constexpr std::int64_t kOwnerResetValidityMs = 300'000;
using OwnerResetActivation = std::array<unsigned char, 16>;
using OwnerResetChallengeBytes =
    std::array<std::byte, sizeof(kOwnerResetDomain) + 2 + 5 * sizeof(std::uint64_t) +
                               OwnerResetActivation{}.size()>;

// One active challenge at a time. Each challenge has a fresh random identifier.
class OwnerResetAuthority {
public:
    static std::unique_ptr<OwnerResetAuthority> create(std::span<const unsigned char> public_key_der) {
        return create_for_scope(public_key_der, std::nullopt);
    }

    OwnerResetAuthority(const OwnerResetAuthority&) = delete;
    OwnerResetAuthority& operator=(const OwnerResetAuthority&) = delete;

    [[nodiscard]] const OwnerResetActivation& activation() const noexcept { return activation_; }

    [[nodiscard]] std::optional<OperatorResetApproval> pending_approval() const noexcept {
        if (!pending_) return std::nullopt;
        return pending_->approval;
    }

    [[nodiscard]] bool renew_request() noexcept {
        if (!pending_) return false;
        OwnerResetActivation next{};
        if (RAND_bytes(next.data(), static_cast<int>(next.size())) != 1 || next == activation_) return false;
        pending_.reset();
        activation_ = next;
        return true;
    }

    [[nodiscard]] OperatorResetApprovalPort approval_port() noexcept {
        return {&verify_current, this};
    }

    [[nodiscard]] std::optional<OwnerResetChallengeBytes> prepare(const OperatorResetTarget& target,
                                                                  std::int64_t server_upper_ms) noexcept {
        std::int64_t deadline = 0;
        const bool matches_scope = enrolled_scope_ ? target.scope == *enrolled_scope_
                                                   : target.scope.environment == StoreEnvironment::Testnet;
        if (pending_ || !target.scope.valid() || !matches_scope || server_upper_ms <= 0 ||
            !checked_add_i64(server_upper_ms, kOwnerResetValidityMs, deadline)) return std::nullopt;

        Challenge challenge{};
        challenge.approval = OperatorResetApproval{target, deadline};
        std::memcpy(challenge.bytes.data(), kOwnerResetDomain, sizeof(kOwnerResetDomain));
        auto* p = challenge.bytes.data() + sizeof(kOwnerResetDomain);
        detail::write_u8(p, 1);
        detail::write_u8(p, static_cast<std::uint8_t>(target.scope.environment));
        detail::write_u64_le(p, target.scope.account_uid);
        detail::write_u64_le(p, target.scope.store_uuid_lo);
        detail::write_u64_le(p, target.scope.store_uuid_hi);
        detail::write_u64_le(p, target.revision);
        detail::write_i64_le(p, deadline);
        std::memcpy(p, activation_.data(), activation_.size());
        pending_ = challenge;
        return challenge.bytes;
    }

private:
    friend class ProductionResetKey;

    struct Challenge {
        OperatorResetApproval approval;
        OwnerResetChallengeBytes bytes;
    };

    static std::unique_ptr<OwnerResetAuthority> create_for_scope(std::span<const unsigned char> public_key_der,
                                                                std::optional<OperatorResetScope> scope) {
        auto key = owner_signature::decode_p256(public_key_der);
        if (!key) return {};
        OwnerResetActivation activation{};
        if (RAND_bytes(activation.data(), static_cast<int>(activation.size())) != 1) return {};
        return std::unique_ptr<OwnerResetAuthority>(new OwnerResetAuthority(std::move(key), activation, scope));
    }

    OwnerResetAuthority(owner_signature::PublicKey key, OwnerResetActivation activation,
                        std::optional<OperatorResetScope> scope) noexcept
        : key_(std::move(key)), activation_(activation), enrolled_scope_(scope) {}

    static std::optional<OperatorResetApproval> verify_current(void* context,
                                                              std::span<const unsigned char> signature) noexcept {
        const auto& authority = *static_cast<const OwnerResetAuthority*>(context);
        if (!authority.pending_ ||
            !owner_signature::verify_p256(*authority.key_, authority.pending_->bytes, signature)) return std::nullopt;
        return authority.pending_->approval;
    }

    owner_signature::PublicKey key_;
    OwnerResetActivation activation_;
    const std::optional<OperatorResetScope> enrolled_scope_;
    std::optional<Challenge> pending_;
};

}  // namespace hy
