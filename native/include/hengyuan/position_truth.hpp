// SPDX-License-Identifier: proprietary
// position_truth.hpp — TODO 1A.3 follow-up: minimal net-quantity position
// aggregator ("Minimal PositionTruth" from TODOLIST's vertical-slice diagram,
// "Intent WAL -> POST -> UNKNOWN -> GET(Reconcile) -> Terminal WAL -> Minimal
// PositionTruth").
//
// Governance: L1 (pure computation, no I/O, no threading of its own).
//
// Scope, deliberately narrow (see the batch's own plan for full reasoning):
// net quantity only, per symbol -- no average entry price, no realized PnL,
// no VenueExecutionId-level dedup (not needed; see apply_fill()'s callers in
// live_submit_orchestrator.hpp/order_tracker.hpp, which guarantee each fill
// delta is applied exactly once by construction, not by an execution-id
// ledger). Deliberately NOT execution_types.hpp::Position: that struct is
// P2-EXEC-SIM-01 simulation-governance-tier (lots, not ticks; carries
// avg_entry_price_ticks/realized_pnl this batch doesn't need; and pulls in
// execution_types.hpp's own, separately-defined OrderSide -- a same-name,
// different-definition enum from the hy::OrderSide (account_truth.hpp) this
// file uses. Co-including both headers in one TU is a latent ODR landmine).
//
// THREAD OWNERSHIP (load-bearing, not a convention comment, mirrors
// order_tracker.hpp's and spot_rate_limit_budget.hpp's own wording): no
// internal synchronization. Owned EXCLUSIVELY by the hot/submit thread, via
// live_submit_orchestrator.hpp's OrchestratorContext::position_truth -- the
// same thread that already owns InFlightRegistry and calls
// orchestrate_submit()/drain_reconcile_events(). Nothing in this codebase
// reads PositionTruth from a second thread today (RiskGate::check() takes
// current_net as a caller-supplied parameter with zero call sites in
// live_submit_orchestrator.hpp; account_truth.hpp's
// ExposureLimits::current_exposure_notional is only ever set in test files).
// Do NOT add a ClockOffsetPublisher-style (binance_clock_sync.hpp)
// mutex+snapshot publisher speculatively -- add one only when a real
// second-thread reader shows up (e.g. a future RiskGate-integration batch).
//
// KNOWN LIMITATION (pre-existing, not introduced here): order_tracker.hpp's
// poll_once() "confirmed state == previous state" branch (a partial fill
// growing WITHOUT a state transition, e.g. PartialFill(30)->PartialFill(60))
// never pushes a ReconcileEvent and never writes an audit record -- that
// growth is invisible both to the existing audit trail and to a
// PositionTruth fed only via drain_reconcile_events(). Fixing it means
// changing poll_once()'s already-tested reconcile state machine, real
// regression risk beyond this file's scope. Documented here so this stays an
// honestly "minimal" PositionTruth rather than one that quietly implies more
// completeness than it has.

#pragma once

#include <hengyuan/account_truth.hpp>  // OrderSide, kMaxSymbols

#include <array>
#include <cstdint>
#include <type_traits>

namespace hy {

struct PositionEntry {
    std::uint32_t symbol_id{0};
    std::int64_t net_qty_ticks{0};  // signed: +buy accumulation, -sell accumulation
};
static_assert(std::is_trivially_copyable_v<PositionEntry>);
static_assert(std::is_standard_layout_v<PositionEntry>);

// Fixed-capacity (kMaxSymbols), zero-heap net-position tracker. Linear
// find-or-insert, the same bounded-scan idiom as durable_audit_sink.hpp's
// observed_key_ids_/note_observed_key_id -- kMaxSymbols(=64, account_truth.hpp's
// disambiguated definition, not the two unrelated same-named constants
// elsewhere in this codebase) is small enough that a linear scan beats a hash
// map's constant-factor overhead and keeps this trivially copyable.
class PositionTruth {
public:
    // qty_delta_ticks is the amount THIS call adds to the running total, not
    // a cumulative figure -- callers derive it as (new observed fill total -
    // previously applied fill total). Must be > 0; a call with a
    // non-positive delta is silently a no-op (defensive: every real caller
    // already guards this before calling, see live_submit_orchestrator.hpp's
    // direct-fill branch and order_tracker.hpp's drain_reconcile_events()).
    // `side` decides the sign applied to net_qty_ticks -- the caller passes
    // an unsigned fill quantity, never a pre-signed one.
    void apply_fill(std::uint32_t symbol_id, OrderSide side, std::int64_t qty_delta_ticks) noexcept {
        if (qty_delta_ticks <= 0) [[unlikely]] return;
        PositionEntry* e = find_or_insert(symbol_id);
        // At capacity (kMaxSymbols distinct symbols already tracked): refuse
        // silently rather than corrupt an unrelated symbol's slot. Unreachable
        // in practice -- kMaxSymbols already bounds SymbolRegistry itself, so
        // no caller can observe a fill for a symbol_id that wasn't already
        // within that same bound.
        if (!e) [[unlikely]] return;
        e->net_qty_ticks += (side == OrderSide::Buy) ? qty_delta_ticks : -qty_delta_ticks;
    }

    // Never-observed symbol reads as flat (0), not an error -- there is no
    // "unknown symbol" distinct from "no net position in this symbol yet".
    std::int64_t net_qty_ticks(std::uint32_t symbol_id) const noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            if (entries_[i].symbol_id == symbol_id) return entries_[i].net_qty_ticks;
        }
        return 0;
    }

    std::size_t tracked_symbol_count() const noexcept { return count_; }

private:
    PositionEntry* find_or_insert(std::uint32_t symbol_id) noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            if (entries_[i].symbol_id == symbol_id) return &entries_[i];
        }
        if (count_ < entries_.size()) {
            entries_[count_].symbol_id = symbol_id;
            entries_[count_].net_qty_ticks = 0;
            return &entries_[count_++];
        }
        return nullptr;
    }

    std::array<PositionEntry, kMaxSymbols> entries_{};
    std::size_t count_{0};
};

}  // namespace hy
