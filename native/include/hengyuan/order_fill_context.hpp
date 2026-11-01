// SPDX-License-Identifier: proprietary
// order_fill_context.hpp — TODO 1A.4 batch 2: per-in-flight-order WS-fill-safe delta state,
// shared by all three fill-application paths (direct POST-accept, REST reconciliation, WS
// executionReport).
//
// WHY THIS EXISTS: orchestrate_submit() calls drain_reconcile_events() and
// drain_user_data_events() unconditionally at its own top, both on the same hot/submit thread
// that also runs the direct-POST-fill branch (live_submit_orchestrator.hpp's Accepted case) --
// so that ONE thread is the single serialization point for all three fill sources, and dedup
// across them needs no cross-thread primitive, just one thread-owned table. But each of the
// two REST-era paths already computed its own "delta" against an independent, mutually-unaware
// baseline: order_tracker.hpp's OrderTracker::Slot::record.filled_qty_ticks (private,
// reconcile-thread-owned) vs. the direct-fill branch's own implicit "baseline is always 0"
// assumption (only true when direct-fill was the sole fill source). Adding WS as a THIRD,
// independently-baselined source makes double-counting a real, constructible scenario, not a
// hypothetical -- see this file's own apply_fill()-adjacent callers' comments for the worked
// numeric example. This class is the fix: every fill-application call site routes through the
// same consume_delta(), making the hot thread the single arbiter of "how much of this order's
// cumulative fill has already been credited," regardless of which mechanism observed it.
//
// Deliberately a NEW, PARALLEL table, not an extension of InFlightRegistry::Slot
// (order_lifecycle.hpp) -- that class is this codebase's deliberately minimal foundational
// component ({active, generation, id}, nothing else), depended on by many call sites; growing
// it taxes every one of them for a concern only the fill-application paths need. This table
// costs existing InFlightRegistry consumers nothing and is symmetric-by-construction with its
// lifecycle when track()/remove() are called at the exact same call sites as
// register_submit_handle()/mark_resolved()/mark_resolved_handle() (see this batch's own plan
// for the exhaustive, directly-read-code-verified list of exactly 3 real call sites -- do not
// add a 4th without re-verifying against the real current call sites, not a remembered list).
//
// THREAD OWNERSHIP: no internal synchronization, exactly like InFlightRegistry/PositionTruth.
// Owned EXCLUSIVELY by the hot/submit thread.

#pragma once

#include <hengyuan/account_truth.hpp>  // OrderSide
#include <hengyuan/order_durable_path.hpp>  // DurableShadow, after_ack (private-stream migration M4)
#include <hengyuan/order_lifecycle.hpp>  // kClientOrderIdLen, kMaxInFlight

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace hy {

struct OrderFillContextEntry {
    std::uint32_t symbol_id{0};
    OrderSide side{OrderSide::Buy};
    std::uint8_t price_scale{0};
    std::uint8_t qty_scale{0};
    std::uint8_t quote_scale{0};  // for an avg-fill-price computation at parity with the REST
                                   // formula (checked_scaled_mul_div(), account_truth.hpp)
    // Private-stream migration M4 (order_durable_path.hpp). Appended last, like every additive field of a struct that crosses call sites.
    // `shadow` is what the durable log holds for this order: Intent from track() on (the Intent frame was acknowledged before the order was
    // tracked), advanced ONLY after a frame's durable ACK -- durable_acked() for every frame a writer makes durable, seed_shadow() for an order
    // recovery hands back. `intended_qty_ticks` is the quantity the order was sent with, the bound a planned fill is held against; 0 = unknown, and
    // plan_observation() then refuses every fill for the order (fail closed).
    DurableShadow shadow{};
    std::int64_t intended_qty_ticks{0};
};

// Fixed-capacity (kMaxInFlight, same as InFlightRegistry), zero-heap. Linear find/insert, same
// bounded-scan idiom used throughout this codebase (durable_audit_sink.hpp's
// observed_key_ids_/note_observed_key_id, position_truth.hpp's find_or_insert).
class OrderFillContext {
public:
    // Called at the exact same call site as InFlightRegistry::register_submit_handle()
    // succeeding. Baseline (last_applied_cumulative_qty_ticks) defaults to 0 -- the correct
    // starting point for a freshly-registered order that has filled nothing yet. Returns false
    // (no-op) if already tracked (defensive; should be unreachable given the paired lifecycle)
    // or at capacity.
    //
    // `intended_qty_ticks` (M4, trailing default 0 = unknown -- omitted, nothing changes for an existing caller) is the quantity the order was sent
    // with. The shadow starts at Intent: every caller tracks an order only after its Intent frame was acknowledged (and apply_recovery() overwrites
    // it with seed_shadow() before the order is used).
    bool track(std::string_view coid, std::uint32_t symbol_id, OrderSide side,
               std::uint8_t price_scale, std::uint8_t qty_scale, std::uint8_t quote_scale,
               std::int64_t intended_qty_ticks = 0) noexcept {
        if (coid.empty() || coid.size() > kClientOrderIdLen) return false;
        if (find_slot(coid)) return false;  // already tracked -- defensive, should be unreachable
        if (count_ >= slots_.size()) return false;

        Slot& s = slots_[count_];
        s.active = true;
        std::memcpy(s.coid, coid.data(), coid.size());
        s.coid[coid.size()] = '\0';
        s.ctx.symbol_id = symbol_id;
        s.ctx.side = side;
        s.ctx.price_scale = price_scale;
        s.ctx.qty_scale = qty_scale;
        s.ctx.quote_scale = quote_scale;
        s.ctx.shadow = DurableShadow{};
        s.ctx.intended_qty_ticks = intended_qty_ticks;
        s.last_applied_cumulative_qty_ticks = 0;
        ++count_;
        return true;
    }

    // M4: the shadow of an order recovery handed back (shadow_of_recovered()). Overwrites whatever the entry held. False (nothing changed) for an
    // untracked coid.
    bool seed_shadow(std::string_view coid, const DurableShadow& shadow) noexcept {
        Slot* s = find_slot(coid);
        if (!s) return false;
        s->ctx.shadow = shadow;
        return true;
    }

    // M4: a frame `(state, filled_qty_ticks)` of this order has been ACKNOWLEDGED by the durable log -- the one thing that may move the shadow. Called by
    // every writer of order frames right after the ACK and never before it (a shadow ahead of the log makes the next planned frame illegal). An untracked
    // coid is a normal no-op: the orchestrator's Intent frame is acknowledged before the order is tracked, and a late event can name an order that is gone.
    void durable_acked(std::string_view coid, OrderState state, std::int64_t filled_qty_ticks) noexcept {
        Slot* s = find_slot(coid);
        if (!s) return;
        s->ctx.shadow = after_ack(s->ctx.shadow, state, filled_qty_ticks);
    }

    // Called at the exact same call sites as InFlightRegistry::mark_resolved()/
    // mark_resolved_handle() -- see this file's own header comment for the verified-exhaustive
    // list of exactly 3 real call sites. Swap-with-last removal (order doesn't matter, this is
    // an unordered set keyed by coid).
    void remove(std::string_view coid) noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            if (slots_[i].active && coid_matches(slots_[i], coid)) {
                slots_[i] = slots_[count_ - 1];
                slots_[count_ - 1] = Slot{};
                --count_;
                return;
            }
        }
    }

    const OrderFillContextEntry* find(std::string_view coid) const noexcept {
        const Slot* s = find_slot(coid);
        return s ? &s->ctx : nullptr;
    }

    // The dedup-safe delta primitive. One call does read-baseline + compute + conditionally-
    // advance -- deliberately not split into separate read/write calls (this table is single-
    // hot-thread-owned so there is no cross-thread TOCTOU, but a split API would still be a
    // real sequencing-correctness hazard for a caller that reads, does other work, then writes
    // stale). Untracked coid (defensive -- see this batch's own plan for why this is a normal,
    // expected outcome for a late-arriving WS message about an already-resolved order, not an
    // error) returns 0, matching drain_user_data_events()'s established "miss = silently
    // ignored" philosophy.
    //
    // Monotonic by construction: the baseline only ever advances when the computed delta is
    // positive, so a stale/out-of-order observed_cumulative_qty_ticks (less than what's already
    // been applied) produces a negative delta (which apply_fill() itself already no-ops on) and
    // never regresses the baseline.
    std::int64_t consume_delta(std::string_view coid, std::int64_t observed_cumulative_qty_ticks) noexcept {
        Slot* s = find_slot(coid);
        if (!s) return 0;
        const std::int64_t delta = observed_cumulative_qty_ticks - s->last_applied_cumulative_qty_ticks;
        if (delta > 0) s->last_applied_cumulative_qty_ticks = observed_cumulative_qty_ticks;
        return delta;
    }

    std::size_t count() const noexcept { return count_; }

private:
    struct Slot {
        bool active{false};
        char coid[kClientOrderIdLen + 1]{};
        OrderFillContextEntry ctx{};
        std::int64_t last_applied_cumulative_qty_ticks{0};
    };

    static bool coid_matches(const Slot& s, std::string_view coid) noexcept {
        return std::string_view(s.coid) == coid;
    }

    Slot* find_slot(std::string_view coid) noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            if (slots_[i].active && coid_matches(slots_[i], coid)) return &slots_[i];
        }
        return nullptr;
    }
    const Slot* find_slot(std::string_view coid) const noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            if (slots_[i].active && coid_matches(slots_[i], coid)) return &slots_[i];
        }
        return nullptr;
    }

    std::array<Slot, kMaxInFlight> slots_{};
    std::size_t count_{0};
};

}  // namespace hy
