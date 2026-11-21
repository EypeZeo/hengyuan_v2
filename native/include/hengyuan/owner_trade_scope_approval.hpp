// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/owner_p256_signature.hpp>
#include <hengyuan/trade_scope_grant.hpp>

#include <optional>
#include <span>

namespace hy {

struct TradeScopePolicy {
    TradeScopeLimits limits{};
    std::int64_t max_validity_ms{0};
};

// The caller supplies the enrolled scope key, the execution identity and the
// exchange time upper bound from the current clock token.
class VerifiedTradeScope {
public:
    [[nodiscard]] static std::optional<VerifiedTradeScope> verify(
        std::span<const std::byte> payload, std::span<const unsigned char> signature,
        std::span<const unsigned char> enrolled_public_key, const TradeScopeBinding& expected,
        const TradeScopePolicy& policy, std::optional<std::int64_t> exchange_upper_ms) noexcept {
        const auto grant = decode_trade_scope_grant(payload);
        if (!grant || grant->binding != expected || !exchange_upper_ms || *exchange_upper_ms <= 0 ||
            policy.max_validity_ms <= 0 || grant->not_after_ms <= *exchange_upper_ms ||
            grant->not_after_ms - *exchange_upper_ms > policy.max_validity_ms ||
            grant->limits.max_order_notional > policy.limits.max_order_notional ||
            grant->limits.budget_notional > policy.limits.budget_notional ||
            grant->limits.max_exposure_notional > policy.limits.max_exposure_notional ||
            grant->limits.max_price_deviation_bps > policy.limits.max_price_deviation_bps) return std::nullopt;

        const auto key = owner_signature::decode_p256(enrolled_public_key);
        if (!key || !owner_signature::verify_p256(*key, payload, signature)) return std::nullopt;
        return VerifiedTradeScope(*grant);
    }

    [[nodiscard]] const TradeScopeGrant& grant() const noexcept { return grant_; }

private:
    explicit VerifiedTradeScope(const TradeScopeGrant& grant) noexcept : grant_(grant) {}

    TradeScopeGrant grant_;
};

}  // namespace hy
