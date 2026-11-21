// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/account_risk_policy.hpp>
#include <hengyuan/trade_scope_audit.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace hy::account_risk_audit {

inline constexpr std::int64_t kUtcDayMs = 86'400'000;

inline constexpr std::int64_t utc_day(std::int64_t utc_ms) noexcept {
    return utc_ms > 0 ? utc_ms / kUtcDayMs : -1;
}

inline bool is_account_prepared(AuditEventType type) noexcept {
    return type == AuditEventType::OrderAccountScopedPrepared || type == AuditEventType::OrderRestrictedExitPrepared;
}

struct Policy {
    AccountRiskLimits limits{};
    std::uint32_t symbol_id{0};

    bool operator==(const Policy&) const noexcept = default;
};

struct Debit {
    std::int64_t day{-1};
    std::int64_t quote{0};
    std::int64_t base{0};
};

inline AuditRecord policy_record(const Policy& policy, std::int64_t now_ms) noexcept {
    const auto& limits = policy.limits;
    auto record = trade_scope_audit::control_record(AuditEventType::AccountRiskPolicyInstalled, limits.version, now_ms);
    record.symbol_id = policy.symbol_id;
    record.price_ticks = limits.daily_quote_cap;
    record.qty_ticks = limits.exit_base_cap;
    record.filled_qty_ticks = limits.exit_order_quote_cap;
    record.avg_fill_price_ticks = limits.exit_price_deviation_bps;
    return record;
}

inline std::optional<Policy> decode_policy(const AuditRecord& record) noexcept {
    if (record.event_type != AuditEventType::AccountRiskPolicyInstalled || record.mode != ExecutionMode::Live ||
        record.timestamp_ms <= 0 || record.resulting_state != OrderState::Intent || record.side != OrderSide::Buy ||
        record.exchange_order_id != 0 || !trade_scope_audit::empty_bytes(record.client_order_id) ||
        !trade_scope_audit::empty_bytes(record.detail_msg) || record.avg_fill_price_ticks < 0 ||
        record.avg_fill_price_ticks > 10'000) return std::nullopt;
    const AccountRiskLimits limits{trade_scope_audit::serial_of(record), record.price_ticks, record.qty_ticks,
                                   record.filled_qty_ticks, static_cast<std::uint32_t>(record.avg_fill_price_ticks)};
    if (!valid_account_risk_limits(limits)) return std::nullopt;
    return Policy{limits, record.symbol_id};
}

inline void tag_prepared(AuditRecord& record, bool exit, std::uint64_t authorization, const SymbolRules& rules,
                         std::int64_t exchange_ms) noexcept {
    trade_scope_audit::tag_prepared(record, authorization, static_cast<int>(rules.price_scale) + rules.qty_scale);
    record.event_type = exit ? AuditEventType::OrderRestrictedExitPrepared : AuditEventType::OrderAccountScopedPrepared;
    record.timestamp_ms = exchange_ms;
    record.detail_msg[2] = static_cast<char>('0' + rules.qty_scale / 10);
    record.detail_msg[3] = static_cast<char>('0' + rules.qty_scale % 10);
}

class Ledger {
public:
    [[nodiscard]] const std::optional<Policy>& policy() const noexcept { return policy_; }
    [[nodiscard]] std::int64_t latest_day() const noexcept { return day_; }
    [[nodiscard]] std::int64_t day_used() const noexcept { return quote_; }
    [[nodiscard]] std::int64_t base_reserved() const noexcept { return base_; }

    [[nodiscard]] bool can_install(const Policy& policy) const noexcept {
        return !policy_ || (policy.limits.version > policy_->limits.version && policy.symbol_id == policy_->symbol_id);
    }

    void install(const Policy& policy) noexcept { policy_ = policy; }

    void note_unmetered(std::int64_t utc_ms) noexcept {
        unmetered_through_day_ = std::max(unmetered_through_day_, utc_day(utc_ms));
    }

    // Older scoped orders already store the quote scale. Their paid amount is
    // included if they belong to the current UTC day, including across upgrade.
    Debit charge_legacy(std::int64_t utc_ms, std::int64_t quote) noexcept {
        const auto day = utc_day(utc_ms);
        if (day < 0 || day < day_) return {};
        if (day == day_ && quote > std::numeric_limits<std::int64_t>::max() - quote_) {
            note_unmetered(utc_ms);
            return {};
        }
        const Debit debit{day, quote, 0};
        charge(debit);
        return debit;
    }

    [[nodiscard]] std::optional<Debit> debit_for(const AuditRecord& record, std::int64_t utc_ms) const noexcept {
        if (!policy_ || !is_account_prepared(record.event_type) || record.timestamp_ms != utc_ms ||
            record.mode != ExecutionMode::Live || record.resulting_state != OrderState::Submitting ||
            record.client_order_id[0] == '\0' || record.symbol_id != policy_->symbol_id ||
            record.exchange_order_id != 0 || record.filled_qty_ticks != 0 || record.avg_fill_price_ticks != 0 ||
            !trade_scope_audit::empty_bytes(std::span(record.detail_msg).subspan(4))) return std::nullopt;

        const auto digits = std::span(record.detail_msg).first(4);
        if (!std::all_of(digits.begin(), digits.end(), [](char digit) { return digit >= '0' && digit <= '9'; })) {
            return std::nullopt;
        }
        const int quote_scale = 10 * (digits[0] - '0') + (digits[1] - '0');
        const int qty_scale = 10 * (digits[2] - '0') + (digits[3] - '0');
        if (quote_scale > 19 || qty_scale > quote_scale) return std::nullopt;
        const auto quote = trade_scope_audit::quote_notional(record.price_ticks, record.qty_ticks, quote_scale);
        if (!quote) return std::nullopt;
        const auto day = utc_day(utc_ms);
        if (day < 0 || day < day_) return std::nullopt;
        std::int64_t base = 0;
        if (record.side == OrderSide::Sell &&
            (!rescale_notional_ceil(record.qty_ticks, qty_scale, kBalanceScale, base) ||
             base > std::numeric_limits<std::int64_t>::max() - base_)) return std::nullopt;

        const auto& limits = policy_->limits;
        if (record.event_type == AuditEventType::OrderRestrictedExitPrepared) {
            if (record.side != OrderSide::Sell || trade_scope_audit::serial_of(record) != limits.version ||
                *quote > limits.exit_order_quote_cap || base > limits.exit_base_cap - base_) return std::nullopt;
            return Debit{day, 0, base};
        }
        const auto used = day == day_ ? quote_ : 0;
        if (day <= unmetered_through_day_ || *quote > limits.daily_quote_cap - used) return std::nullopt;
        return Debit{day, *quote, base};
    }

    void charge(const Debit& debit) noexcept {
        if (debit.day > day_) {
            day_ = debit.day;
            quote_ = 0;
        }
        quote_ += debit.quote;
        base_ += debit.base;
    }

    [[nodiscard]] bool refund(const Debit& debit) noexcept {
        if (debit.day > day_ || (debit.day == day_ && debit.quote > quote_) || debit.base > base_) return false;
        if (debit.day == day_) quote_ -= debit.quote;
        base_ -= debit.base;
        return true;
    }

private:
    std::optional<Policy> policy_;
    std::int64_t day_{-1};
    std::int64_t unmetered_through_day_{-1};
    std::int64_t quote_{0};
    std::int64_t base_{0};
};

}  // namespace hy::account_risk_audit
