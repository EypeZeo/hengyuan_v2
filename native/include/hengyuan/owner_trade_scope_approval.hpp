// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/account_risk_policy.hpp>
#include <hengyuan/owner_p256_signature.hpp>
#include <hengyuan/trade_scope_grant.hpp>

namespace hy {

inline std::optional<VerifiedAccountRiskPolicy> VerifiedAccountRiskPolicy::verify(
    std::span<const std::byte> payload, std::span<const unsigned char> signature,
    std::span<const unsigned char> public_key, const OperatorResetScope& expected_store,
    std::uint64_t expected_key_id, std::uint64_t expected_version) noexcept {
    const auto policy = decode_account_risk_policy(payload);
    if (!policy || policy->store != expected_store || policy->key_id != expected_key_id ||
        policy->limits.version != expected_version) return std::nullopt;
    const auto key = owner_signature::decode_p256(public_key);
    if (!key || !owner_signature::verify_p256(*key, payload, signature)) return std::nullopt;
    return VerifiedAccountRiskPolicy(*policy);
}

inline std::optional<VerifiedTradeScope> VerifiedTradeScope::verify(
    std::span<const std::byte> payload, std::span<const unsigned char> signature,
    std::span<const unsigned char> enrolled_public_key, const TradeScopeBinding& expected,
    const TradeScopePolicy& policy, std::optional<std::int64_t> exchange_upper_ms) noexcept {
    const auto grant = decode_trade_scope_grant(payload);
    if (!grant || grant->binding != expected || !exchange_upper_ms || *exchange_upper_ms <= 0 ||
        policy.max_validity_ms <= 0 || grant->issued_at_ms > *exchange_upper_ms ||
        grant->not_after_ms <= *exchange_upper_ms || grant->not_after_ms - grant->issued_at_ms > policy.max_validity_ms ||
        grant->limits.max_order_notional > policy.limits.max_order_notional ||
        grant->limits.budget_notional > policy.limits.budget_notional ||
        grant->limits.max_exposure_notional > policy.limits.max_exposure_notional ||
        grant->limits.max_price_deviation_bps > policy.limits.max_price_deviation_bps) return std::nullopt;

    const auto key = owner_signature::decode_p256(enrolled_public_key);
    if (!key || !owner_signature::verify_p256(*key, payload, signature)) return std::nullopt;
    return VerifiedTradeScope(*grant);
}

}  // namespace hy
