// SPDX-License-Identifier: proprietary
// preflight_probe.hpp — private-stream migration M5b-1: the order the preflight harness asks orchestrate_submit() about, and the one place its fields are put
// into the context.
//
// WHAT IT IS. live_submit_preflight_harness.cpp probes the order path whenever the strategy's holding state flips: it builds the order a real run would send
// (a Limit at the price that crosses the book, the smallest quantity the symbol accepts), runs it through orchestrate_submit() and expects the chain to stop
// at OperatorNotConfirmed -- every gate before the operator's confirmation passed, and nothing was sent. Until M5b-1 that recipe lived inline in main(),
// which no test can reach, next to a rounding that was not overflow-checked. It is three small functions here, called by the harness and by the tests alike:
//   preflight_quantity_ticks()   min_qty_ticks rounded UP to the symbol's step grid, checked;
//   plan_preflight_probe()       suggested action + top of book + rules -> side, price, quantity (or nothing to probe);
//   fill_preflight_order()       puts the order and the facts only the harness knows into an OrchestratorContext that already carries the private stream's
//                                wiring (PrivateStreamPipeline::make_context()), and leaves the confirmation UNBOUND.
//
// THE SAFETY PROPERTY lives in fill_preflight_order(): it resets ctx.confirmation to its default (not confirmed) and installs the SubmitPort it is handed,
// which the harness makes a mock that must never run (it prints FATAL-LOGIC and answers NetworkError). With the confirmation unbound orchestrate_submit()
// cannot get past the OperatorNotConfirmed gate, so nothing this process does can send an order -- whatever the readiness says, and however the rest of the
// context was filled. test_submit_readiness_guard.cpp pins that no other production file assigns ctx.confirmation or ctx.submit_port.
//
// Governance: L2 pure logic. No I/O, no state, no heap; it names no network port and holds no secret.

#pragma once

#include <hengyuan/binance_clock_sync.hpp>  // checked_add_i64
#include <hengyuan/holding_state_tracker.hpp>
#include <hengyuan/live_submit_orchestrator.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace hy {

// The smallest quantity the symbol accepts that sits on its step grid: min_qty_ticks rounded UP to a multiple of step_size_ticks (a step of 0 means no grid:
// the minimum itself). Nothing when the rules cannot produce one -- a minimum that is not positive, a negative step, or a rounding that does not fit in 64 bits
// -- so that the harness probes nothing for such a symbol rather than an order with a wrapped quantity.
[[nodiscard]] inline std::optional<std::int64_t> preflight_quantity_ticks(const SymbolRules& rules) noexcept {
    const std::int64_t minimum = rules.min_qty_ticks;
    const std::int64_t step = rules.step_size_ticks;
    if (minimum <= 0 || step < 0) return std::nullopt;
    if (step == 0) return minimum;
    const std::int64_t remainder = minimum % step;
    if (remainder == 0) return minimum;
    std::int64_t rounded = 0;
    if (!checked_add_i64(minimum, step - remainder, rounded)) return std::nullopt;  // step - remainder is in (0, step]: it cannot overflow itself
    return rounded;
}

struct PreflightProbe {
    OrderSide side{OrderSide::Buy};
    std::int64_t price_ticks{0};
    std::int64_t qty_ticks{0};
};

// The order a suggested action calls for, against the best prices of the book (`top_of_book` as OrderBook::top_of_book() reports them: first = best bid,
// second = best ask). An OPEN is a BUY at the best ask, a CLOSE a SELL at the best bid: the price that crosses. Nothing for "no action" (or a value that is
// none of the three), for a side of the book that is not a price (not positive), or for a symbol that yields no quantity.
[[nodiscard]] inline std::optional<PreflightProbe> plan_preflight_probe(SuggestedAction action, std::pair<std::int64_t, std::int64_t> top_of_book,
                                                                       const SymbolRules& rules) noexcept {
    const auto probe = [&rules](OrderSide side, std::int64_t price_ticks) -> std::optional<PreflightProbe> {
        if (price_ticks <= 0) return std::nullopt;
        const std::optional<std::int64_t> qty_ticks = preflight_quantity_ticks(rules);
        if (!qty_ticks) return std::nullopt;
        return PreflightProbe{side, price_ticks, *qty_ticks};
    };
    switch (action) {
        case SuggestedAction::Open: return probe(OrderSide::Buy, top_of_book.second);
        case SuggestedAction::Close: return probe(OrderSide::Sell, top_of_book.first);
        case SuggestedAction::None: return std::nullopt;
    }
    return std::nullopt;  // a value that is none of the three is no action
}

// What the harness adds to the pipeline's context besides the order. The pointers are the harness's and outlive the call.
struct PreflightEnvironment {
    const DryRunEvidenceChain* evidence{nullptr};
    const SymbolRules* rules{nullptr};
    std::string_view base_asset{};
    std::string_view quote_asset{};
    bool signer_ready{false};   // the credentials were bound (the harness has a signer or it would not have started)
    bool depth_synced{false};   // the depth book is tracking the exchange right now
    std::uint32_t symbol_id{0};
    std::uint32_t sequence{0};
    std::int64_t now_ms{0};     // in the clock the account's stamps are in: the private stream's steady clock
    SubmitPort submit_port{};   // a MOCK that must never run: the preflight never submits
};

// Puts the order and the harness-only facts into `ctx`. `ctx` is the pipeline's wiring (audit, kill switch, readiness, rate limiter, in-flight registry,
// durable ports, the account, the exposure limits): none of it is touched. The confirmation is reset to "not confirmed" and the submit port replaced by the
// one in `env`, whatever `ctx` carried in -- see the header comment.
inline void fill_preflight_order(OrchestratorContext& ctx, const PreflightProbe& probe, const PreflightEnvironment& env) noexcept {
    ctx.evidence = env.evidence;
    ctx.signer_ready = env.signer_ready;
    ctx.depth_synced = env.depth_synced;
    ctx.symbol_rules = env.rules;
    ctx.side = probe.side;
    ctx.order_type = OrderType::Limit;
    ctx.base_asset = env.base_asset;
    ctx.quote_asset = env.quote_asset;
    ctx.now_ms = env.now_ms;
    ctx.price_ticks = probe.price_ticks;
    ctx.qty_ticks = probe.qty_ticks;
    ctx.symbol_id = env.symbol_id;
    ctx.sequence = env.sequence;
    ctx.mode = ExecutionMode::Live;
    ctx.order_weight = 1;
    ctx.confirmation = OrderConfirmation{};
    ctx.submit_port = env.submit_port;
}

}  // namespace hy
