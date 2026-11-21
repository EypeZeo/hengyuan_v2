// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/binance_clock_sync.hpp>
#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/submit_authority.hpp>
#include <hengyuan/trade_scope_grant.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace hy {

enum class TradeScopeChange : std::uint8_t { Applied, Refused, WriteUnconfirmed };

// The execution owner handles control requests and order submissions on the
// same thread. Asynchronous stream revocations use its existing SubmitAuthority.
class TradeScopeExecution {
public:
    TradeScopeExecution(DurableAuditSink& sink, const KillSwitch& kill, SubmitAuthority& authority,
                        const ClockOffsetPublisher& clock, TradeScopeBinding binding) noexcept
        : sink_(sink), kill_(kill), authority_(authority), clock_(clock), binding_(binding) {}
    TradeScopeExecution(const TradeScopeExecution&) = delete;
    TradeScopeExecution& operator=(const TradeScopeExecution&) = delete;

    [[nodiscard]] std::optional<TradeScopeBinding> current_binding() const noexcept {
        if (failed_ || !sink_.trade_scope_writes_available()) return std::nullopt;
        if (kill_.persistence_health() != KillPersistenceHealth::Ready) return std::nullopt;
        auto binding = binding_;
        binding.scope_epoch = kill_.snapshot().scope_epoch;
        return binding;
    }

    [[nodiscard]] std::optional<std::int64_t> exchange_upper() const noexcept {
        return approval_server_time_upper_bound(clock_, fetch_clock_pair());
    }

    [[nodiscard]] TradeScopeChange install(const VerifiedTradeScope& verified, const SymbolRules& rules,
                                          std::uint32_t symbol_id, std::int64_t now_ms) noexcept {
        const auto binding = current_binding();
        const auto upper = exchange_upper();
        const auto& grant = verified.grant();
        if (!binding || grant.binding != *binding || !upper || *upper >= grant.not_after_ms ||
            grant.serial <= sink_.trade_scope_high_water() || rules.symbol[kSymbolNameLen - 1] != '\0' ||
            rules.symbol_name() != std::string_view(grant.symbol.data())) return TradeScopeChange::Refused;
        const auto append = sink_.append_trade_scope_install(grant, symbol_id, now_ms);
        if (!append.acked()) return write_unconfirmed();
        active_ = grant;
        authority_.revoke();
        return TradeScopeChange::Applied;
    }

    [[nodiscard]] TradeScopeChange revoke(std::uint64_t serial, std::int64_t now_ms) noexcept {
        if (serial == 0 || failed_ || !sink_.trade_scope_writes_available()) return TradeScopeChange::Refused;
        if (active_ && active_->serial == serial) active_.reset();
        authority_.revoke();
        if (!sink_.append_trade_scope_revocation(serial, now_ms).acked()) return write_unconfirmed();
        return TradeScopeChange::Applied;
    }

    [[nodiscard]] bool matches(const SubmitAuthority& authority, const void* sink) const noexcept {
        return &authority_ == &authority && &sink_ == sink;
    }

    [[nodiscard]] const TradeScopeGrant* active() const noexcept {
        return active_ && !failed_ ? &*active_ : nullptr;
    }

    [[nodiscard]] bool permits_claim(std::uint64_t serial) const noexcept {
        const auto upper = exchange_upper();
        return active_ && !failed_ && active_->serial == serial && upper &&
               *upper < active_->not_after_ms && sink_.active_trade_scope_serial() == serial &&
               sink_.trade_scope_writes_available();
    }

    [[nodiscard]] bool authorizes(std::uint64_t serial, const SymbolRules& rules, OrderSide side, OrderType type,
                                  std::int64_t price, std::int64_t qty, std::int64_t reference_price,
                                  const ExposureLimits& exposure) const noexcept {
        if (!permits_claim(serial) || type != OrderType::Limit ||
            rules.symbol[kSymbolNameLen - 1] != '\0' || rules.symbol_name() != std::string_view(active_->symbol.data()) ||
            !trade_scope_audit::permits_side(active_->sides, side) || price <= 0 || reference_price <= 0 ||
            exposure.total_exposure_notional_cap <= 0) return false;
        const auto native_scale = static_cast<int>(rules.price_scale) + static_cast<int>(rules.qty_scale);
        const auto amount = trade_scope_audit::quote_notional(price, qty, native_scale);
        std::int64_t projected = 0;
        if (!amount || *amount > active_->limits.max_order_notional ||
            !checked_add(exposure.current_exposure_notional, *amount, projected) ||
            projected > active_->limits.max_exposure_notional) return false;
        const auto difference = price > reference_price ? price - reference_price : reference_price - price;
        const auto bps = static_cast<std::int64_t>(active_->limits.max_price_deviation_bps);
        const auto permitted_difference = (reference_price / 10'000) * bps + (reference_price % 10'000) * bps / 10'000;
        return difference <= permitted_difference;
    }

    [[nodiscard]] bool can_reserve(std::uint64_t serial, const SymbolRules& rules,
                                   std::int64_t price, std::int64_t quantity) const noexcept {
        const auto* balance = sink_.trade_scope_balance(serial);
        const auto native_scale = static_cast<int>(rules.price_scale) + static_cast<int>(rules.qty_scale);
        const auto amount = trade_scope_audit::quote_notional(price, quantity, native_scale);
        return balance && amount && balance->usage.can_charge(*amount);
    }

private:
    TradeScopeChange write_unconfirmed() noexcept {
        failed_ = true;
        active_.reset();
        authority_.revoke();
        return TradeScopeChange::WriteUnconfirmed;
    }

    DurableAuditSink& sink_;
    const KillSwitch& kill_;
    SubmitAuthority& authority_;
    const ClockOffsetPublisher& clock_;
    const TradeScopeBinding binding_;
    std::optional<TradeScopeGrant> active_;
    bool failed_{false};
};

}  // namespace hy
