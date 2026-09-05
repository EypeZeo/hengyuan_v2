// SPDX-License-Identifier: proprietary
// binance_submit_adapter.hpp — Batch H, H5: composite SubmitPort wiring that closes
// SymbolRegistry::current_rules_version() into live_submit_orchestrator.hpp's Gate 1
// (symbol_registry.hpp's own comment names this exact gap: "the natural injection point
// for SubmitPort::CurrentRulesVersionFn... a follow-up integration task, not done here").
//
// Lives in its own header, not inside binance_private_rest.hpp: symbol_registry.hpp
// #includes durable_control_plane.hpp (the whole persistence control-plane dependency
// chain), while binance_private_rest.hpp has never included symbol_registry.hpp and is
// kept deliberately lightweight (Boost/Beast/OpenSSL only, no persistence dependency).
// Folding SubmitAdapterContext into binance_private_rest.hpp would force that file to
// also pull in the persistence chain for every caller, not just the ones that need Gate 1
// wired to a real SymbolRegistry. The existing submit_order_adapter()/query_order_adapter()
// (binance_private_rest.hpp) are left untouched so callers that don't need SymbolRegistry
// keep their current, lighter dependency footprint -- this file adds a second, distinctly
// named pair of adapters rather than overloading the existing names (illegal in C++ besides:
// overload resolution can't dispatch on what a void* actually points to).

#pragma once

#include <hengyuan/binance_private_rest.hpp>
#include <hengyuan/live_submit_orchestrator.hpp>
#include <hengyuan/symbol_registry.hpp>

#include <type_traits>

namespace hy {

// Non-owning: both pointers must outlive every SubmitPort::call()/current_rules_version()
// built from this context. Caller contract (mirrors BoundHmacCredentials' single-owner
// posture): construct `client`/`registry` first and this context second, in the same
// scope, so C++'s reverse-destruction order keeps them alive for the context's entire
// lifetime -- see H6's main() declaration-order requirement (SymbolRegistry, then
// BinancePrivateRestClient, then this context, then the SubmitPort built from it).
struct SubmitAdapterContext {
    BinancePrivateRestClient* client{nullptr};
    const SymbolRegistry* registry{nullptr};
};

static_assert(std::is_trivially_copyable_v<SubmitAdapterContext>);
static_assert(std::is_standard_layout_v<SubmitAdapterContext>);

// Bridges SubmitAdapterContext to SubmitPort::SubmitFn. Double null-guard: `user_data`
// itself, then the destructured `client` pointer -- a null client fails exactly the same
// way binance_private_rest.hpp's own submit_order_adapter() does.
inline SubmitResponse composite_submit_order_adapter(const char* client_order_id,
                                                      std::uint32_t symbol_id, OrderSide side,
                                                      OrderType type, std::int64_t price_ticks,
                                                      std::int64_t qty_ticks,
                                                      const SymbolRules& rules_snapshot,
                                                      void* user_data) noexcept {
    if (!user_data || !client_order_id) return {SubmitOutcome::NetworkError, 0, -1};
    auto* ctx = static_cast<SubmitAdapterContext*>(user_data);
    if (!ctx->client) return {SubmitOutcome::NetworkError, 0, -1};
    return ctx->client->submit_order(client_order_id, symbol_id, side, type, price_ticks,
                                      qty_ticks, rules_snapshot);
}

// Bridges SubmitAdapterContext to SubmitPort::CurrentRulesVersionFn. A null registry
// returns 0 -- live_submit_orchestrator.hpp's SubmitPort::current_rules_version() comment
// documents 0 as "unknown/unavailable", so Gate 1 fails closed against it exactly like a
// missing current_rules_version_fn does; this is not a new error path, just reuse of the
// existing fail-closed contract.
inline std::uint32_t composite_current_rules_version_adapter(void* user_data) noexcept {
    if (!user_data) return 0;
    auto* ctx = static_cast<SubmitAdapterContext*>(user_data);
    if (!ctx->registry) return 0;
    return ctx->registry->current_rules_version();
}

// Production wiring: `SubmitPort port = make_binance_submit_port(submit_ctx);` where
// `submit_ctx` is a named lvalue that outlives `port`. Lvalue-only on purpose -- see the
// deleted rvalue overload immediately below.
inline SubmitPort make_binance_submit_port(SubmitAdapterContext& ctx) noexcept {
    return SubmitPort{&composite_submit_order_adapter, static_cast<void*>(&ctx),
                       &composite_current_rules_version_adapter};
}

// Deleted to prevent binding a temporary SubmitAdapterContext: a temporary constructed
// in the same expression (e.g. `make_binance_submit_port(SubmitAdapterContext{...})`)
// would be destroyed at the end of that full expression, leaving SubmitPort::user_data
// dangling for every subsequent call. This turns that mistake into a compile error
// instead of a runtime use-after-free.
inline SubmitPort make_binance_submit_port(SubmitAdapterContext&&) = delete;

}  // namespace hy
