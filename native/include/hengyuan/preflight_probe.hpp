// SPDX-License-Identifier: proprietary
// preflight_probe.hpp — private-stream migration M5b-1: the order the preflight harness asks orchestrate_submit() about, and the one place its fields are put
// into the context.
//
// WHAT IT IS. live_submit_preflight_harness.cpp probes the order path whenever the strategy's holding state flips: it builds the order a real run would send
// (a Limit at the price that crosses the book, the smallest quantity the symbol's LOT_SIZE and MIN_NOTIONAL filters accept at that price), runs it through
// orchestrate_submit() and expects the chain to stop at OperatorNotConfirmed -- every gate before the operator's confirmation passed, and nothing was sent.
// Until M5b-1 that recipe lived inline in main(), which no test can reach, next to a rounding that was not overflow-checked. It is a few small functions
// here, called by the harness and by the tests alike:
//   preflight_quantity_ticks()   the smallest quantity the symbol accepts: the lot (min_qty_ticks rounded UP to the step grid), and -- given the price -- the
//                                first point of that grid whose notional also reaches min_notional_ticks; checked;
//   plan_preflight_probe()       suggested action + top of book + rules -> side, price, quantity (or nothing to probe);
//   fill_preflight_order()       puts the order and the facts only the harness knows into an OrchestratorContext that already carries the private stream's
//                                wiring (PrivateStreamPipeline::make_context()), and leaves the confirmation UNBOUND;
//   pre_trade_check_name()       the name of a pre-trade verdict, so the log says WHY the chain stopped at PreTradeFailed.
//
// WHY THE PRICE MATTERS (found by the first joint run on the Testnet, 2026-11-07). The probe used to carry the bare minimum lot. BTCUSDT's is 0.00001 BTC: at
// 75 920.32 that is 0.76 USDT, under the 5 USDT NOTIONAL.minNotional, so every probe stopped at PreTradeFailed instead of OperatorNotConfirmed. The test
// fixtures could not show it: their rules carried min_notional_ticks = 1000 (0.00001 USDT at kBalanceScale), nothing like a real symbol's.
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
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace hy {

// The smallest LOT_SIZE quantity that sits on the symbol's step grid: min_qty_ticks rounded UP to a multiple of step_size_ticks (a step of 0 means no grid: the
// minimum itself). Nothing when the rules cannot produce one -- a minimum that is not positive, a negative step, or a rounding that does not fit in 64 bits --
// so that the harness probes nothing for such a symbol rather than an order with a wrapped quantity.
//
// This is only the lot half of "a quantity the symbol accepts": it knows no price, so it can fall under MIN_NOTIONAL (see the header comment). The two-argument
// overload below adds that half; the harness sizes its probe with that one.
//
// Like the grid itself, this assumes min_qty_ticks is a multiple of step_size_ticks, as it is for every symbol the harness trades. validate_pre_trade() anchors
// its step check at the minimum ((qty - min_qty) % step), the exchange's own formula, and the two agree exactly then.
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

// The smallest quantity the symbol accepts for a Limit at `price_ticks`: the smallest point of the lot grid above whose notional also reaches
// rules.min_notional_ticks. The notional is computed exactly as validate_pre_trade() computes it -- the overflow-checked product price * qty at the native scale
// (price_scale + qty_scale), rescaled UP to kBalanceScale -- so the two cannot disagree at the boundary: a notional equal to the minimum passes, one tick less
// does not. Without a minimum notional (min_notional_ticks <= 0) this is the lot.
//
// Nothing when the lot is nothing, when the price is not positive, when no representable quantity reaches the minimum (the product or the rescale would
// overflow first -- validate_pre_trade() answers NotionalOverflow there), or when the quantity that does is above max_qty_ticks (judged, like there, only for rules
// that carry a step). Never a wrapped value.
//
// How: the notional never falls as the quantity rises, so the first grid point that reaches the minimum is found by doubling the number of steps above the lot
// until it is reached (or nothing larger is representable) and then bisecting -- at most about 2 * 62 evaluations, however far away the answer is, and each one
// is the same checked arithmetic the gate runs. An answer more than 2^62 steps above the lot is given up on (none: no symbol trades such a quantity). Cold path (the
// harness asks at most a few times a run); no allocation.
[[nodiscard]] inline std::optional<std::int64_t> preflight_quantity_ticks(const SymbolRules& rules, std::int64_t price_ticks) noexcept {
    const std::optional<std::int64_t> lot = preflight_quantity_ticks(rules);
    if (!lot || price_ticks <= 0) return std::nullopt;
    const auto under_max = [&rules](std::int64_t qty) noexcept -> std::optional<std::int64_t> {
        if (rules.step_size_ticks > 0 && rules.max_qty_ticks > 0 && qty > rules.max_qty_ticks) return std::nullopt;
        return qty;
    };
    if (rules.min_notional_ticks <= 0) return under_max(*lot);

    const std::int64_t grid = rules.step_size_ticks > 0 ? rules.step_size_ticks : 1;  // no step: every tick is a quantity
    const int native_scale = static_cast<int>(rules.price_scale) + static_cast<int>(rules.qty_scale);
    enum class Reach { Short, Reaches, Unrepresentable };
    // The lot plus `steps` grid steps (into `qty`), and how its notional stands against the minimum. Short < Reaches < Unrepresentable as `steps` grows.
    const auto at_steps = [&](std::int64_t steps, std::int64_t& qty) noexcept -> Reach {
        std::int64_t offset = 0;
        if (!checked_notional(steps, grid, offset) || !checked_add_i64(*lot, offset, qty)) return Reach::Unrepresentable;
        std::int64_t native = 0;
        if (!checked_notional(price_ticks, qty, native)) return Reach::Unrepresentable;
        std::int64_t notional = 0;
        if (!rescale_notional_ceil(native, native_scale, kBalanceScale, notional)) return Reach::Unrepresentable;
        return notional >= rules.min_notional_ticks ? Reach::Reaches : Reach::Short;
    };

    std::int64_t qty = 0;
    const Reach at_lot = at_steps(0, qty);
    if (at_lot != Reach::Short) return at_lot == Reach::Reaches ? under_max(qty) : std::nullopt;
    // The lot is short. Double the step count until the minimum is reached or nothing larger is representable ...
    std::int64_t lo = 0;
    std::int64_t hi = 1;
    while (at_steps(hi, qty) == Reach::Short) {
        lo = hi;
        if (hi > std::numeric_limits<std::int64_t>::max() / 2) return std::nullopt;
        hi *= 2;
    }
    // ... then bisect: `lo` is short of the minimum, `hi` is not; the first point that is not short is the answer if it reaches the minimum rather than overflowing.
    while (hi - lo > 1) {
        const std::int64_t mid = lo + (hi - lo) / 2;
        if (at_steps(mid, qty) == Reach::Short) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    if (at_steps(hi, qty) != Reach::Reaches) return std::nullopt;  // `qty` holds the last point tried, not necessarily `hi`
    return under_max(qty);
}

struct PreflightProbe {
    OrderSide side{OrderSide::Buy};
    std::int64_t price_ticks{0};
    std::int64_t qty_ticks{0};
};

// The order a suggested action calls for, against the best prices of the book (`top_of_book` as OrderBook::top_of_book() reports them: first = best bid,
// second = best ask). An OPEN is a BUY at the best ask, a CLOSE a SELL at the best bid: the price that crosses. The quantity is sized at that very price
// (preflight_quantity_ticks(rules, price_ticks)): a SELL at the bid can need a larger quantity than a BUY at the ask to reach the same minimum notional.
// Nothing for "no action" (or a value that is none of the three), for a side of the book that is not a price (not positive), or for a symbol that yields no
// quantity at that price.
[[nodiscard]] inline std::optional<PreflightProbe> plan_preflight_probe(SuggestedAction action, std::pair<std::int64_t, std::int64_t> top_of_book,
                                                                       const SymbolRules& rules) noexcept {
    const auto probe = [&rules](OrderSide side, std::int64_t price_ticks) -> std::optional<PreflightProbe> {
        if (price_ticks <= 0) return std::nullopt;
        const std::optional<std::int64_t> qty_ticks = preflight_quantity_ticks(rules, price_ticks);
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

// The name of a pre-trade verdict, for the log: the gate's name (PreTradeFailed) does not say which of the verdicts stopped the chain, and the harness's own
// audit ring is gone when the process exits. A switch with no default on purpose: a verdict added to PreTradeCheck is a -Wswitch error here under GCC (CI builds
// with -Werror; MSVC's C4062 is off by default), not a silent "Unknown". Ok is what OrchestratorResult::pre_trade_detail holds when the chain stopped at PreTradeFailed before validating anything (no rules or no account
// wired).
[[nodiscard]] constexpr const char* pre_trade_check_name(PreTradeCheck check) noexcept {
    switch (check) {
        case PreTradeCheck::Ok: return "Ok";
        case PreTradeCheck::SymbolNotTrading: return "SymbolNotTrading";
        case PreTradeCheck::PriceBelowMin: return "PriceBelowMin";
        case PreTradeCheck::PriceAboveMax: return "PriceAboveMax";
        case PreTradeCheck::PriceStepViolation: return "PriceStepViolation";
        case PreTradeCheck::QtyBelowMin: return "QtyBelowMin";
        case PreTradeCheck::QtyAboveMax: return "QtyAboveMax";
        case PreTradeCheck::QtyStepViolation: return "QtyStepViolation";
        case PreTradeCheck::BelowMinNotional: return "BelowMinNotional";
        case PreTradeCheck::ExceedsSingleOrderCap: return "ExceedsSingleOrderCap";
        case PreTradeCheck::ExceedsTotalExposureCap: return "ExceedsTotalExposureCap";
        case PreTradeCheck::InsufficientBalance: return "InsufficientBalance";
        case PreTradeCheck::BalanceStale: return "BalanceStale";
        case PreTradeCheck::AccountCannotTrade: return "AccountCannotTrade";
        case PreTradeCheck::InvalidInput: return "InvalidInput";
        case PreTradeCheck::NotionalOverflow: return "NotionalOverflow";
        case PreTradeCheck::BalanceFromFuture: return "BalanceFromFuture";
    }
    return "Unknown";  // a value outside the enumeration
}

}  // namespace hy
