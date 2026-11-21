// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/cancel_prepared.hpp>
#include <hengyuan/durable_frame_codec.hpp>
#include <hengyuan/kill_switch.hpp>
#include <hengyuan/operator_reset.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace hy {

enum class TradeScopeSides : std::uint8_t { Buy = 1, Sell = 2, Both = 3 };

struct TradeScopeBinding {
    OperatorResetScope store{};
    std::array<std::byte, 16> activation{};
    TradeScopeEpoch scope_epoch{};
    std::uint64_t policy_version{0};
    std::array<std::byte, 32> strategy_version{};
    std::uint64_t key_id{0};

    bool operator==(const TradeScopeBinding&) const noexcept = default;
};

struct TradeScopeLimits {
    std::int64_t max_order_notional{0};
    std::int64_t budget_notional{0};
    std::int64_t max_exposure_notional{0};
    std::uint32_t max_price_deviation_bps{0};

    bool operator==(const TradeScopeLimits&) const noexcept = default;
};

struct TradeScopeGrant {
    TradeScopeBinding binding{};
    std::uint64_t serial{0};
    std::int64_t issued_at_ms{0};
    std::int64_t not_after_ms{0};
    std::array<char, kSymbolNameLen> symbol{};
    TradeScopeSides sides{};
    TradeScopeLimits limits{};

    bool operator==(const TradeScopeGrant&) const noexcept = default;
};

inline constexpr char kTradeScopeGrantDomain[] = "HY-SCOPE-GRANT";
inline constexpr std::uint8_t kTradeScopeGrantVersion = 1;
inline constexpr std::uint8_t kTradeScopeP256 = 1;
inline constexpr std::uint8_t kTradeScopeLimitOrder = 0;
using TradeScopeGrantBytes =
    std::array<std::byte, sizeof(kTradeScopeGrantDomain) + 5 + 12 * sizeof(std::uint64_t) +
                          16 + 32 + kSymbolNameLen + sizeof(std::uint32_t)>;

inline bool valid_trade_scope_grant(const TradeScopeGrant& grant) noexcept {
    const auto& binding = grant.binding;
    const auto& limits = grant.limits;
    const auto symbol_end = std::find(grant.symbol.begin(), grant.symbol.end(), '\0');
    if (symbol_end == grant.symbol.end()) return false;
    const std::string_view symbol(grant.symbol.data(), static_cast<std::size_t>(symbol_end - grant.symbol.begin()));
    const auto nonzero = [](std::byte value) { return value != std::byte{0}; };
    return binding.store.valid() && binding.key_id != 0 && binding.policy_version != 0 &&
           std::any_of(binding.activation.begin(), binding.activation.end(), nonzero) &&
           std::any_of(binding.strategy_version.begin(), binding.strategy_version.end(), nonzero) &&
           grant.serial != 0 && grant.issued_at_ms > 0 && grant.not_after_ms > 0 &&
           cancel_prepared::valid_symbol(symbol) &&
           std::all_of(symbol_end, grant.symbol.end(), [](char c) { return c == '\0'; }) &&
           (grant.sides == TradeScopeSides::Buy || grant.sides == TradeScopeSides::Sell ||
            grant.sides == TradeScopeSides::Both) &&
           limits.max_order_notional > 0 && limits.budget_notional >= limits.max_order_notional &&
           limits.max_exposure_notional >= limits.max_order_notional && limits.max_price_deviation_bps <= 10'000;
}

// Version 1 describes one spot symbol and limit orders. Notionals use the same
// quote units for the whole grant. The signer signs these complete bytes.
inline std::optional<TradeScopeGrantBytes> encode_trade_scope_grant(const TradeScopeGrant& grant) noexcept {
    if (!valid_trade_scope_grant(grant)) return std::nullopt;

    TradeScopeGrantBytes bytes{};
    auto* p = bytes.data();
    detail::write_bytes(p, kTradeScopeGrantDomain, sizeof(kTradeScopeGrantDomain));
    detail::write_u8(p, kTradeScopeGrantVersion);
    detail::write_u8(p, kTradeScopeP256);
    detail::write_u8(p, static_cast<std::uint8_t>(grant.binding.store.environment));
    detail::write_u64_le(p, grant.binding.store.account_uid);
    detail::write_u64_le(p, grant.binding.store.store_uuid_lo);
    detail::write_u64_le(p, grant.binding.store.store_uuid_hi);
    detail::write_bytes(p, grant.binding.activation.data(), grant.binding.activation.size());
    detail::write_u64_le(p, grant.binding.scope_epoch.value);
    detail::write_u64_le(p, grant.binding.policy_version);
    detail::write_bytes(p, grant.binding.strategy_version.data(), grant.binding.strategy_version.size());
    detail::write_u64_le(p, grant.binding.key_id);
    detail::write_u64_le(p, grant.serial);
    detail::write_i64_le(p, grant.issued_at_ms);
    detail::write_i64_le(p, grant.not_after_ms);
    detail::write_bytes(p, grant.symbol.data(), grant.symbol.size());
    detail::write_u8(p, static_cast<std::uint8_t>(grant.sides));
    detail::write_u8(p, kTradeScopeLimitOrder);
    detail::write_i64_le(p, grant.limits.max_order_notional);
    detail::write_i64_le(p, grant.limits.budget_notional);
    detail::write_i64_le(p, grant.limits.max_exposure_notional);
    detail::write_u32_le(p, grant.limits.max_price_deviation_bps);
    return bytes;
}

inline std::optional<TradeScopeGrant> decode_trade_scope_grant(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != TradeScopeGrantBytes{}.size() ||
        std::memcmp(bytes.data(), kTradeScopeGrantDomain, sizeof(kTradeScopeGrantDomain)) != 0) return std::nullopt;

    auto p = bytes.data() + sizeof(kTradeScopeGrantDomain);
    if (detail::read_u8(p) != kTradeScopeGrantVersion || detail::read_u8(p) != kTradeScopeP256) return std::nullopt;

    TradeScopeGrant grant;
    grant.binding.store.environment = static_cast<StoreEnvironment>(detail::read_u8(p));
    grant.binding.store.account_uid = detail::read_u64_le(p);
    grant.binding.store.store_uuid_lo = detail::read_u64_le(p);
    grant.binding.store.store_uuid_hi = detail::read_u64_le(p);
    detail::read_bytes(p, grant.binding.activation.data(), grant.binding.activation.size());
    grant.binding.scope_epoch.value = detail::read_u64_le(p);
    grant.binding.policy_version = detail::read_u64_le(p);
    detail::read_bytes(p, grant.binding.strategy_version.data(), grant.binding.strategy_version.size());
    grant.binding.key_id = detail::read_u64_le(p);
    grant.serial = detail::read_u64_le(p);
    grant.issued_at_ms = detail::read_i64_le(p);
    grant.not_after_ms = detail::read_i64_le(p);
    detail::read_bytes(p, grant.symbol.data(), grant.symbol.size());
    grant.sides = static_cast<TradeScopeSides>(detail::read_u8(p));
    if (detail::read_u8(p) != kTradeScopeLimitOrder) return std::nullopt;
    grant.limits.max_order_notional = detail::read_i64_le(p);
    grant.limits.budget_notional = detail::read_i64_le(p);
    grant.limits.max_exposure_notional = detail::read_i64_le(p);
    grant.limits.max_price_deviation_bps = detail::read_u32_le(p);
    if (!valid_trade_scope_grant(grant)) return std::nullopt;
    return grant;
}

}  // namespace hy
