// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/audit_trail.hpp>

#include <optional>

namespace hy::cancel_prepared {

inline bool working(OrderState state) noexcept {
    return state == OrderState::Accepted || state == OrderState::PartialFill;
}

inline bool valid_alias(std::string_view alias) noexcept {
    if (alias.empty() || alias.size() > kClientOrderIdLen) return false;
    for (const char c : alias) {
        if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') &&
            c != '-' && c != '_' && c != '.' && c != ':' && c != '/') return false;
    }
    return true;
}

inline bool valid_symbol(std::string_view symbol) noexcept {
    if (symbol.empty() || symbol.size() >= kSymbolNameLen) return false;
    for (const char c : symbol) {
        if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9')) return false;
    }
    return true;
}

// This event uses detail_msg for alias|symbol and detail_code for the three decimal scales.
inline std::optional<PendingSpotCancel> decode(const AuditRecord& record) noexcept {
    if (record.event_type != AuditEventType::OrderCancelPrepared ||
        record.resulting_state != OrderState::CancelRequested || record.exchange_order_id <= 0 ||
        record.mode != ExecutionMode::Live || record.client_order_id[kClientOrderIdLen] != '\0' ||
        record.detail_msg[sizeof(record.detail_msg) - 1] != '\0' ||
        record.detail_code < 0 || record.detail_code > 0xFFFFFF) return std::nullopt;

    const std::string_view message(record.detail_msg);
    const auto split = message.find('|');
    if (split == std::string_view::npos) return std::nullopt;
    const auto alias = message.substr(0, split);
    const auto symbol = message.substr(split + 1);
    if (!valid_alias(alias) || !valid_symbol(symbol) || record.client_order_id[0] == '\0' ||
        alias == std::string_view(record.client_order_id)) return std::nullopt;

    PendingSpotCancel pending{};
    const auto scales = static_cast<std::uint32_t>(record.detail_code);
    pending.price_scale = static_cast<std::uint8_t>(scales >> 16);
    pending.qty_scale = static_cast<std::uint8_t>((scales >> 8) & 0xFFu);
    pending.quote_scale = static_cast<std::uint8_t>(scales & 0xFFu);
    if (pending.price_scale > 18 || pending.qty_scale > 18 || pending.quote_scale > 18) return std::nullopt;
    std::memcpy(pending.alias.id, alias.data(), alias.size());
    std::memcpy(pending.symbol, symbol.data(), symbol.size());
    return pending;
}

inline bool matches_working_order(const AuditRecord& record, OrderState state, ExecutionMode mode,
                                  std::uint32_t symbol_id, OrderSide side, std::int64_t price,
                                  std::int64_t qty, std::int64_t exchange_id, std::int64_t filled,
                                  std::int64_t avg_price) noexcept {
    return working(state) && mode == ExecutionMode::Live && symbol_id == record.symbol_id &&
           side == record.side && price > 0 && price == record.price_ticks &&
           qty > 0 && qty == record.qty_ticks && exchange_id == record.exchange_order_id &&
           filled == record.filled_qty_ticks && avg_price == record.avg_fill_price_ticks &&
           ((state == OrderState::Accepted && filled == 0 && avg_price == 0) ||
            (state == OrderState::PartialFill && filled > 0 && filled < qty && avg_price > 0));
}

}  // namespace hy::cancel_prepared
