// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/durable_order_audit_port.hpp>
#include <hengyuan/owner_trade_scope_approval.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace hy {

enum class TradeScopeBudgetEvent : std::uint8_t { Prepared, Refund };

struct TradeScopeBudgetRecord {
    TradeScopeBudgetEvent event;
    std::uint64_t scope_serial;
    std::string_view order_id;
    std::int64_t notional;
};

// Single owner, on the control or recovery path. Replay takes ordered events
// from validated storage. The caller supplies notionals in the grant's quote units.
class TradeScopeBudget {
public:
    TradeScopeBudget(const TradeScopeBudget&) = delete;
    TradeScopeBudget& operator=(const TradeScopeBudget&) = delete;
    TradeScopeBudget(TradeScopeBudget&& other) noexcept
        : serial_(other.serial_), limits_(other.limits_), entries_(std::move(other.entries_)),
          used_notional_(std::exchange(other.used_notional_, 0)),
          rebuild_required_(std::exchange(other.rebuild_required_, true)) {
        other.entries_.clear();
    }
    TradeScopeBudget& operator=(TradeScopeBudget&&) = delete;

    [[nodiscard]] static std::optional<TradeScopeBudget> rebuild(
        const VerifiedTradeScope& scope, std::span<const TradeScopeBudgetRecord> records) {
        TradeScopeBudget budget(scope.grant().serial, scope.grant().limits);
        for (const auto& record : records) {
            if (record.scope_serial != budget.serial_) return std::nullopt;
            if (record.event == TradeScopeBudgetEvent::Prepared) {
                if (!budget.reserve(record.order_id, record.notional)) return std::nullopt;
                budget.entries_.find(record.order_id)->second.phase = Phase::Prepared;
            } else if (record.event == TradeScopeBudgetEvent::Refund) {
                if (record.notional != 0 || !budget.refund(record.order_id)) return std::nullopt;
            } else {
                return std::nullopt;
            }
        }
        return budget;
    }

    [[nodiscard]] std::uint64_t serial() const noexcept { return serial_; }
    [[nodiscard]] std::int64_t used_notional() const noexcept { return used_notional_; }
    [[nodiscard]] std::int64_t available_notional() const noexcept {
        return rebuild_required_ ? 0 : limits_.budget_notional - used_notional_;
    }
    [[nodiscard]] bool rebuild_required() const noexcept { return rebuild_required_; }

    [[nodiscard]] bool reserve(std::string_view order_id, std::int64_t notional) {
        if (!valid_order_id(order_id) || notional <= 0 || notional > limits_.max_order_notional ||
            notional > available_notional() ||
            !entries_.try_emplace(std::string(order_id), Entry{notional, Phase::Reserved}).second) return false;
        used_notional_ += notional;
        return true;
    }

    [[nodiscard]] bool release_unprepared(std::string_view order_id) noexcept {
        const auto entry = entries_.find(order_id);
        if (entry == entries_.end() || entry->second.phase != Phase::Reserved) return false;
        used_notional_ -= entry->second.notional;
        entries_.erase(entry);
        return true;
    }

    [[nodiscard]] bool begin_preparation(std::string_view order_id) noexcept {
        const auto entry = entries_.find(order_id);
        if (rebuild_required_ || entry == entries_.end() || entry->second.phase != Phase::Reserved) return false;
        entry->second.phase = Phase::Preparing;
        return true;
    }

    [[nodiscard]] bool acknowledge_prepared(const AuditRecord& record, const AuditAppendResult& append) noexcept {
        if (record.event_type != AuditEventType::OrderSubmitPrepared ||
            record.resulting_state != OrderState::Submitting) return false;
        const auto entry = entries_.find(record_order_id(record));
        if (entry == entries_.end() || entry->second.phase != Phase::Preparing) return false;
        if (!append.acked()) {
            rebuild_required_ = true;
            return false;
        }
        entry->second.phase = Phase::Prepared;
        return true;
    }

    [[nodiscard]] bool acknowledge_never_sent(const AuditRecord& record, const ClosureAppendResult& closure) noexcept {
        if (record.event_type != AuditEventType::OrderNeverSent || record.resulting_state != OrderState::NeverSent ||
            record.exchange_order_id != 0 || record.filled_qty_ticks != 0 || record.avg_fill_price_ticks != 0) return false;
        const auto order_id = record_order_id(record);
        const auto entry = entries_.find(order_id);
        if (entry == entries_.end() || entry->second.phase != Phase::Prepared) return false;
        if (closure.status == ClosureAppendStatus::Appended && closure.append.acked()) return refund(order_id);
        if (closure.status == ClosureAppendStatus::Failed || closure.status == ClosureAppendStatus::Appended) {
            rebuild_required_ = true;
        }
        return false;
    }

private:
    enum class Phase : std::uint8_t { Reserved, Preparing, Prepared, Refunded };
    struct Entry {
        std::int64_t notional;
        Phase phase;
    };

    TradeScopeBudget(std::uint64_t serial, TradeScopeLimits limits) noexcept : serial_(serial), limits_(limits) {}

    static bool valid_order_id(std::string_view order_id) noexcept {
        return !order_id.empty() && order_id.size() <= kClientOrderIdLen &&
               std::all_of(order_id.begin(), order_id.end(), [](char c) { return c >= '!' && c <= '~'; });
    }

    static std::string_view record_order_id(const AuditRecord& record) noexcept {
        const std::span id(record.client_order_id);
        const auto end = std::find(id.begin(), id.end(), '\0');
        if (end == id.end()) return {};
        return {id.data(), static_cast<std::size_t>(end - id.begin())};
    }

    bool refund(std::string_view order_id) noexcept {
        const auto entry = entries_.find(order_id);
        if (entry == entries_.end() || entry->second.phase != Phase::Prepared) return false;
        used_notional_ -= entry->second.notional;
        entry->second.phase = Phase::Refunded;
        return true;
    }

    const std::uint64_t serial_;
    const TradeScopeLimits limits_;
    std::map<std::string, Entry, std::less<>> entries_;
    std::int64_t used_notional_{0};
    bool rebuild_required_{false};
};

}  // namespace hy
