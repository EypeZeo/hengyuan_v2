// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/binance_clock_sync.hpp>
#include <hengyuan/cancel_prepared.hpp>
#include <hengyuan/kill_switch.hpp>
#include <hengyuan/order_fill_context.hpp>
#include <hengyuan/store_binding.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace hy {

struct CancelSubject {
    StoreEnvironment environment{};
    std::uint64_t account_uid{0};
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint64_t activation_id{0};

    bool operator==(const CancelSubject&) const noexcept = default;

    bool valid() const noexcept {
        return is_valid_store_environment(environment) && account_uid != 0 &&
               (store_uuid_lo != 0 || store_uuid_hi != 0) && activation_id != 0;
    }
};

struct CancelApproval {
    CancelSubject subject{};
    char symbol[kSymbolNameLen]{};
    std::uint64_t serial{0};
    std::int64_t not_after_ms{0};
};

// Reads the current cancellation-only approval from a trusted, preverified local source.
// A missing source or inactive approval supplies no authority. The callback runs on the hot owner.
struct CancelApprovalPort {
    using Fn = std::optional<CancelApproval> (*)(void*, std::string_view) noexcept;
    Fn fn{nullptr};
    void* user_data{nullptr};

    std::optional<CancelApproval> read(std::string_view symbol) const noexcept {
        return fn ? fn(user_data, symbol) : std::nullopt;
    }
};

struct AutoCancelConfig {
    std::uint64_t activation_id{0};
    CancelApprovalPort approvals{};
};

struct AutomaticCancelRequest {
    InFlightHandle handle{};
    ClientOrderId client_order_id{};
    std::int64_t exchange_order_id{0};
    std::uint32_t symbol_id{0};
    std::uint64_t kill_revision{0};
    std::uint64_t approval_serial{0};
    CancelSubject subject{};
    char symbol[kSymbolNameLen]{};
    std::int64_t approval_not_after_ms{0};
};

inline bool emergency_cancel_requested(KillState state) noexcept {
    return state == KillState::Triggered || state == KillState::Latched;
}

inline bool is_confirmed_cancel_target(const OrderFillContextEntry& order) noexcept {
    if (order.cancel_symbol[0] == '\0' || order.exchange_order_id <= 0 || order.intended_qty_ticks <= 0) return false;
    const auto filled = order.shadow.filled_qty_ticks;
    return (order.shadow.state == OrderState::Accepted && filled == 0) ||
           (order.shadow.state == OrderState::PartialFill && filled > 0 && filled < order.intended_qty_ticks);
}

inline std::optional<std::int64_t> cancel_server_time_upper_bound(const ClockOffsetPublisher& clock,
                                                                ClockPairSample now) noexcept {
    const auto snapshot = clock.load();
    std::int64_t upper = 0;
    if (!is_snapshot_fresh(snapshot, now) || snapshot.error_bound_ms < 0 ||
        !server_now_ms_for_signing(snapshot, now.system_ms(), upper) ||
        !checked_add_i64(upper, snapshot.error_bound_ms, upper) ||
        !checked_add_i64(upper, kMaxClockDriftMs, upper) || upper <= 0) return std::nullopt;
    return upper;
}

inline bool permits_automatic_cancel(const CancelApproval& approval, const CancelSubject& subject,
                                      std::string_view symbol, std::int64_t server_upper_ms) noexcept {
    return subject.valid() && approval.subject == subject && approval.serial != 0 &&
           approval.symbol[kSymbolNameLen - 1] == '\0' && cancel_prepared::valid_symbol(symbol) &&
           std::string_view(approval.symbol) == symbol && server_upper_ms > 0 &&
           approval.not_after_ms >= server_upper_ms;
}

}  // namespace hy
