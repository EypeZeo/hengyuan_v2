// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/durable_frame_codec.hpp>
#include <hengyuan/operator_reset.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace hy {

struct AccountRiskLimits {
    std::uint64_t version{0};
    std::int64_t daily_quote_cap{0};
    std::int64_t exit_base_cap{0};
    std::int64_t exit_order_quote_cap{0};
    std::uint32_t exit_price_deviation_bps{0};

    bool operator==(const AccountRiskLimits&) const noexcept = default;
};

struct AccountRiskPolicy {
    OperatorResetScope store{};
    std::uint64_t key_id{0};
    AccountRiskLimits limits{};

    bool operator==(const AccountRiskPolicy&) const noexcept = default;
};

inline constexpr char kAccountRiskDomain[] = "HY-SPOT-ACCOUNT-RISK";
inline constexpr char kAccountRiskSymbol[] = "BTCUSDT";
inline constexpr char kAccountRiskBaseAsset[] = "BTC";
inline constexpr char kAccountRiskQuoteAsset[] = "USDT";
inline constexpr std::uint8_t kAccountRiskFormat = 1;
using AccountRiskPolicyBytes = std::array<std::byte, sizeof(kAccountRiskDomain) + sizeof(kAccountRiskSymbol) +
    2 + 8 * sizeof(std::uint64_t) + sizeof(std::uint32_t)>;

inline bool valid_account_risk_limits(const AccountRiskLimits& limits) noexcept {
    return limits.version != 0 && limits.daily_quote_cap >= 0 && limits.exit_base_cap >= 0 &&
           limits.exit_order_quote_cap >= 0 && (limits.exit_base_cap == 0) == (limits.exit_order_quote_cap == 0) &&
           limits.exit_price_deviation_bps <= 10'000;
}

inline std::optional<AccountRiskPolicyBytes> encode_account_risk_policy(const AccountRiskPolicy& policy) noexcept {
    if (!policy.store.valid() || policy.key_id == 0 || !valid_account_risk_limits(policy.limits)) return std::nullopt;
    AccountRiskPolicyBytes bytes{};
    auto* p = bytes.data();
    detail::write_bytes(p, kAccountRiskDomain, sizeof(kAccountRiskDomain));
    detail::write_u8(p, kAccountRiskFormat);
    detail::write_u8(p, static_cast<std::uint8_t>(policy.store.environment));
    detail::write_bytes(p, kAccountRiskSymbol, sizeof(kAccountRiskSymbol));
    detail::write_u64_le(p, policy.store.account_uid);
    detail::write_u64_le(p, policy.store.store_uuid_lo);
    detail::write_u64_le(p, policy.store.store_uuid_hi);
    detail::write_u64_le(p, policy.key_id);
    detail::write_u64_le(p, policy.limits.version);
    detail::write_i64_le(p, policy.limits.daily_quote_cap);
    detail::write_i64_le(p, policy.limits.exit_base_cap);
    detail::write_i64_le(p, policy.limits.exit_order_quote_cap);
    detail::write_u32_le(p, policy.limits.exit_price_deviation_bps);
    return bytes;
}

inline std::optional<AccountRiskPolicy> decode_account_risk_policy(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != AccountRiskPolicyBytes{}.size() ||
        std::memcmp(bytes.data(), kAccountRiskDomain, sizeof(kAccountRiskDomain)) != 0) return std::nullopt;
    auto p = bytes.data() + sizeof(kAccountRiskDomain);
    if (detail::read_u8(p) != kAccountRiskFormat) return std::nullopt;
    AccountRiskPolicy policy;
    policy.store.environment = static_cast<StoreEnvironment>(detail::read_u8(p));
    if (std::memcmp(p, kAccountRiskSymbol, sizeof(kAccountRiskSymbol)) != 0) return std::nullopt;
    p += sizeof(kAccountRiskSymbol);
    policy.store.account_uid = detail::read_u64_le(p);
    policy.store.store_uuid_lo = detail::read_u64_le(p);
    policy.store.store_uuid_hi = detail::read_u64_le(p);
    policy.key_id = detail::read_u64_le(p);
    policy.limits.version = detail::read_u64_le(p);
    policy.limits.daily_quote_cap = detail::read_i64_le(p);
    policy.limits.exit_base_cap = detail::read_i64_le(p);
    policy.limits.exit_order_quote_cap = detail::read_i64_le(p);
    policy.limits.exit_price_deviation_bps = detail::read_u32_le(p);
    if (!policy.store.valid() || policy.key_id == 0 || !valid_account_risk_limits(policy.limits)) return std::nullopt;
    return policy;
}

class VerifiedAccountRiskPolicy {
public:
    [[nodiscard]] static std::optional<VerifiedAccountRiskPolicy> verify(
        std::span<const std::byte> payload, std::span<const unsigned char> signature,
        std::span<const unsigned char> public_key, const OperatorResetScope& expected_store,
        std::uint64_t expected_key_id, std::uint64_t expected_version) noexcept;

    [[nodiscard]] const AccountRiskPolicy& policy() const noexcept { return policy_; }

private:
    explicit VerifiedAccountRiskPolicy(AccountRiskPolicy policy) noexcept : policy_(policy) {}
    AccountRiskPolicy policy_;
};

}  // namespace hy
