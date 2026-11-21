// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/account_truth.hpp>
#include <hengyuan/audit_trail.hpp>
#include <hengyuan/trade_scope_grant.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hy {

// Quote amounts are in the same units as the account risk limits.
struct TradeScopeUsage {
    std::int64_t max_order_notional{0};
    std::int64_t budget_notional{0};
    std::int64_t used_notional{0};

    [[nodiscard]] bool can_charge(std::int64_t amount) const noexcept {
        return amount > 0 && amount <= max_order_notional && amount <= available();
    }

    [[nodiscard]] std::int64_t available() const noexcept {
        return budget_notional - used_notional;
    }

    [[nodiscard]] bool charge(std::int64_t amount) noexcept {
        if (!can_charge(amount)) return false;
        used_notional += amount;
        return true;
    }

    [[nodiscard]] bool refund(std::int64_t amount) noexcept {
        if (amount <= 0 || amount > used_notional) return false;
        used_notional -= amount;
        return true;
    }
};

namespace trade_scope_audit {

struct Balance {
    std::uint64_t serial{0};
    std::uint32_t symbol_id{0};
    TradeScopeSides sides{};
    TradeScopeUsage usage{};
};

inline std::optional<std::int64_t> quote_notional(std::int64_t price, std::int64_t qty, int native_scale) noexcept {
    std::int64_t native = 0;
    std::int64_t quote = 0;
    if (price <= 0 || qty <= 0 || !checked_notional(price, qty, native) ||
        !rescale_notional_ceil(native, native_scale, kBalanceScale, quote)) return std::nullopt;
    return quote;
}

inline bool permits_side(TradeScopeSides sides, OrderSide side) noexcept {
    return (side == OrderSide::Buy && (sides == TradeScopeSides::Buy || sides == TradeScopeSides::Both)) ||
           (side == OrderSide::Sell && (sides == TradeScopeSides::Sell || sides == TradeScopeSides::Both));
}

inline bool is_scoped_prepared(AuditEventType event) noexcept {
    return event == AuditEventType::OrderScopedPrepared || event == AuditEventType::OrderAccountScopedPrepared;
}

inline bool is_control_event(AuditEventType event) noexcept {
    return event == AuditEventType::TradeScopeInstalled || event == AuditEventType::TradeScopeRevoked;
}

inline std::uint64_t serial_of(const AuditRecord& record) noexcept {
    return std::bit_cast<std::uint64_t>(record.detail_code);
}

// The two control records have an empty COID. Their detail_code preserves all
// 64 bits of the serial. Install price and quantity fields carry quote limits.
inline AuditRecord control_record(AuditEventType event, std::uint64_t serial, std::int64_t now_ms) noexcept {
    AuditRecord record{};
    record.event_type = event;
    record.mode = ExecutionMode::Live;
    record.timestamp_ms = now_ms;
    record.detail_code = std::bit_cast<std::int64_t>(serial);
    return record;
}

inline AuditRecord install_record(const TradeScopeGrant& grant, std::uint32_t symbol_id, std::int64_t now_ms) noexcept {
    auto record = control_record(AuditEventType::TradeScopeInstalled, grant.serial, now_ms);
    record.symbol_id = symbol_id;
    record.price_ticks = grant.limits.max_order_notional;
    record.qty_ticks = grant.limits.budget_notional;
    record.detail_msg[0] = static_cast<char>('0' + static_cast<unsigned>(grant.sides));
    return record;
}

// The scoped variant of Prepared reserves detail_code for the serial and the
// first two detail characters for the price-plus-quantity decimal scale.
inline void tag_prepared(AuditRecord& record, std::uint64_t serial, int native_scale) noexcept {
    record.event_type = AuditEventType::OrderScopedPrepared;
    record.detail_code = std::bit_cast<std::int64_t>(serial);
    record.detail_msg[0] = static_cast<char>('0' + native_scale / 10);
    record.detail_msg[1] = static_cast<char>('0' + native_scale % 10);
}

inline bool empty_bytes(std::span<const char> bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](char value) { return value == '\0'; });
}

class Ledger {
public:
    [[nodiscard]] std::uint64_t high_water() const noexcept { return high_water_; }
    [[nodiscard]] std::uint64_t active_serial() const noexcept { return active_serial_; }
    [[nodiscard]] std::span<const Balance> balances() const noexcept { return balances_; }

    [[nodiscard]] const Balance* find(std::uint64_t serial) const noexcept {
        const auto it = find_entry(balances_, serial);
        return it == balances_.end() || it->serial != serial ? nullptr : &*it;
    }

    [[nodiscard]] bool stage_control(const AuditRecord& record) {
        if (!is_control_event(record.event_type) || record.mode != ExecutionMode::Live ||
            record.resulting_state != OrderState::Intent || record.timestamp_ms <= 0 ||
            record.side != OrderSide::Buy || serial_of(record) == 0 || !empty_bytes(record.client_order_id) ||
            record.exchange_order_id != 0 || record.filled_qty_ticks != 0 || record.avg_fill_price_ticks != 0) {
            return false;
        }
        if (record.event_type == AuditEventType::TradeScopeRevoked) {
            return record.symbol_id == 0 && record.price_ticks == 0 && record.qty_ticks == 0 &&
                   empty_bytes(record.detail_msg);
        }
        const char side = record.detail_msg[0];
        const auto serial = serial_of(record);
        if (serial <= high_water_ || record.price_ticks <= 0 || record.qty_ticks < record.price_ticks ||
            side < '1' || side > '3' || !empty_bytes(std::span(record.detail_msg).subspan(1))) return false;
        balances_.push_back({serial, record.symbol_id, static_cast<TradeScopeSides>(side - '0'),
                             {record.price_ticks, record.qty_ticks, 0}});
        return true;
    }

    void finish_control(const AuditRecord& record, bool acked) noexcept {
        if (!acked) {
            if (record.event_type == AuditEventType::TradeScopeInstalled) balances_.pop_back();
            return;
        }
        const auto serial = serial_of(record);
        high_water_ = std::max(high_water_, serial);
        if (record.event_type == AuditEventType::TradeScopeInstalled) {
            active_serial_ = serial;
        } else if (active_serial_ == serial) {
            active_serial_ = 0;
        }
    }

    [[nodiscard]] std::optional<std::int64_t> charge_for(const AuditRecord& record) const noexcept {
        const auto serial = serial_of(record);
        const auto* scope = serial != 0 && serial == active_serial_ ? find(serial) : nullptr;
        if (!scope || !is_scoped_prepared(record.event_type) ||
            record.mode != ExecutionMode::Live || record.resulting_state != OrderState::Submitting ||
            record.client_order_id[0] == '\0' || record.symbol_id != scope->symbol_id ||
            !permits_side(scope->sides, record.side) || record.exchange_order_id != 0 || record.filled_qty_ticks != 0 ||
            record.avg_fill_price_ticks != 0 || record.detail_msg[0] < '0' || record.detail_msg[0] > '1' ||
            record.detail_msg[1] < '0' || record.detail_msg[1] > '9' ||
            !empty_bytes(std::span(record.detail_msg).subspan(
                record.event_type == AuditEventType::OrderAccountScopedPrepared ? 4 : 2))) return std::nullopt;
        const int scale = 10 * (record.detail_msg[0] - '0') + (record.detail_msg[1] - '0');
        const auto amount = quote_notional(record.price_ticks, record.qty_ticks, scale);
        if (!amount || !scope->usage.can_charge(*amount)) return std::nullopt;
        return amount;
    }

    [[nodiscard]] bool charge(std::uint64_t serial, std::int64_t amount) noexcept {
        auto it = find_entry(balances_, serial);
        return it != balances_.end() && it->serial == serial && it->usage.charge(amount);
    }

    [[nodiscard]] bool refund(std::uint64_t serial, std::int64_t amount) noexcept {
        auto it = find_entry(balances_, serial);
        return it != balances_.end() && it->serial == serial && it->usage.refund(amount);
    }

    void retire_active() noexcept { active_serial_ = 0; }

private:
    template <typename Balances>
    static auto find_entry(Balances& balances, std::uint64_t serial) noexcept -> decltype(balances.begin()) {
        return std::lower_bound(balances.begin(), balances.end(), serial,
                                [](const Balance& balance, std::uint64_t value) { return balance.serial < value; });
    }

    std::vector<Balance> balances_;
    std::uint64_t high_water_{0};
    std::uint64_t active_serial_{0};
};

}  // namespace trade_scope_audit
}  // namespace hy
