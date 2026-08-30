// SPDX-License-Identifier: proprietary
// symbol_registry.hpp — L4 §5.3/§5.3.1: the live, durably-backed symbol-rules registry.
//
// account_truth.hpp's own comment on SymbolRules::rules_version used to say "No SymbolRegistry
// class exists in this codebase yet" -- this file is that class.
//
// Governance: this is NOT account_truth.hpp's L1 ("pure logic, no network, no secret") tier --
// refresh_from_exchange_info() takes a DurableControlPlaneSink&, a persistence dependency that
// would break that file's boundary. It also has no network dependency of its own (that's
// BinancePrivateRestClient::fetch_exchange_info() in binance_private_rest.hpp) — this class only
// consumes an already-fetched-and-parsed ParsedExchangeInfo and durably publishes it.
//
// §5.3.1's core invariant, enforced by construction here, not by caller discipline: a symbol
// registry refresh is durable-write-then-swap, never swap-then-write. If the durable append is
// not Acked, current_rules()/current_rules_version() keep returning exactly what they returned
// before this call -- the old rules stay in effect, and they are still a fully-persisted state
// (not a half-applied one), because nothing about the in-memory table was ever touched before
// the Ack.

#pragma once

#include <hengyuan/account_truth.hpp>
#include <hengyuan/durable_control_plane.hpp>

#include <array>
#include <cstdint>
#include <shared_mutex>

namespace hy {

class SymbolRegistry {
public:
    // Returns the SymbolRules currently published for `symbol_id`, or a default-constructed
    // SymbolRules{} (rules_version == 0) if `symbol_id` is out of range or has never been
    // registered by a successful refresh_from_exchange_info() call. rules_version == 0 is
    // reserved for exactly this "unknown/unavailable" case (account_truth.hpp's own comment on
    // SymbolRules::rules_version) -- a caller reading an unregistered symbol_id therefore fails
    // closed at live_submit_orchestrator.hpp's Gate "1" stale-version check by construction,
    // with no separate null-check required.
    //
    // Returned BY VALUE, copied inside the shared lock: this system's own SubmitPort doc comment
    // (live_submit_orchestrator.hpp) already makes the point this class's design follows —  "a
    // bare atomic swap of a multi-field struct cannot honestly provide no-torn-read without a
    // lock or a real double-buffer scheme". A reader here always sees one complete, internally
    // consistent SymbolRules snapshot, never a partially-updated one.
    SymbolRules current_rules(std::uint32_t symbol_id) const noexcept {
        std::shared_lock lock(mu_);
        if (symbol_id >= registered_count_) return SymbolRules{};
        // registered_count_ is only ever assigned kMaxSymbols-bounded values (see
        // refresh_from_exchange_info()'s own capacity check), so this access is genuinely safe
        // -- but GCC 14 at -O3 emits a false-positive -Warray-bounds here once a caller passes a
        // compile-time-constant, well-out-of-range symbol_id (e.g. this class's own test suite
        // deliberately does, to exercise the bounds check above): it evaluates table_[symbol_id]
        // symbolically against std::array's static extent using that literal before fully
        // proving the guard above makes this line unreachable for that value. A well-documented
        // GCC diagnostic-quality limitation with std::array::operator[] behind a runtime bounds
        // check, not a real bug -- see e.g. GCC PR 105651/106433. MSVC and Clang do not raise
        // this; scoped narrowly to this one line rather than weakened repo-wide.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
        return table_[symbol_id];
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    }

    // A version-only, symbol-independent fast check -- the natural injection point for
    // SubmitPort::CurrentRulesVersionFn (live_submit_orchestrator.hpp's Gate "1" quick
    // pre-check, which compares a version number, not a specific symbol's rules). Wiring an
    // actual CurrentRulesVersionFn to a SymbolRegistry instance (e.g. a small free function or
    // lambda closing over `this`) is a follow-up integration task, not done here.
    std::uint32_t current_rules_version() const noexcept {
        std::shared_lock lock(mu_);
        return published_version_;
    }

    // §5.3.1's mandatory three-step sequence. `parsed` is already-fetched-and-parsed data
    // (BinancePrivateRestClient::fetch_exchange_info(), binance_private_rest.hpp); this function
    // performs no network I/O of its own.
    //   1. Build the new snapshot in a LOCAL array. table_ is not touched yet.
    //   2. sink.append_snapshot(...) — durably write and require Ack. Not Acked -> return false
    //      immediately; table_, registered_count_, and published_version_ are all left exactly
    //      as they were (the old, already-durable state remains in effect).
    //   3. Only once Acked: take the write lock, replace table_ wholesale, and stamp the new
    //      version into every replaced entry's own rules_version field -- Gate "1"
    //      (live_submit_orchestrator.hpp) reads pre_trade_rules_snapshot.rules_version, i.e. the
    //      version carried BY EACH RECORD, not a version read from a side channel.
    //
    // Caller contract (documented here, not runtime-enforced -- §9's full single-owner
    // actor/scheduler model is out of this slice's scope, same posture BoundHmacCredentials
    // already takes for its own single-thread contract): only the owner actor thread may call
    // this, and only after finishing a RefreshRegistry task.
    //
    // A second contract this function relies on but does not itself enforce: `parsed.symbols`
    // is treated as positionally keyed by symbol_id -- parsed.symbols[i] becomes the rules for
    // symbol_id == i. The caller is responsible for requesting exchangeInfo's `symbols` filter
    // in the same order on every refresh so a given symbol_id keeps naming the same symbol
    // across refreshes (the same "stable index space" convention this codebase already uses
    // elsewhere for symbol_id, e.g. hot_thread.hpp's books_[symbol_id]).
    bool refresh_from_exchange_info(DurableControlPlaneSink& sink,
                                     const ParsedExchangeInfo& parsed) noexcept {
        // Defensive re-check: fetch_exchange_info() is documented to already fail closed with
        // CapacityExceeded before ever writing past kMaxSymbols, but this is a public entry
        // point taking externally-sourced data -- verify the invariant here too rather than
        // trust the one producer never to violate it.
        if (parsed.symbol_count > kMaxSymbols) return false;

        // rules_version 0 is reserved for "unknown/unavailable" (account_truth.hpp); the first
        // successful refresh must therefore publish version 1, and every later refresh strictly
        // increments -- a rules_version can never be reused or go backwards.
        const std::uint32_t new_version = published_version_ + 1;

        std::array<SymbolRules, kMaxSymbols> new_table{};
        for (std::size_t i = 0; i < parsed.symbol_count; ++i) {
            new_table[i] = parsed.symbols[i];
            new_table[i].rules_version = new_version;
        }

        SymbolRegistrySnapshotPayload snap{};
        snap.timestamp_ms = parsed.server_time_ms;
        snap.rules_version = new_version;
        snap.symbol_count = static_cast<std::uint32_t>(parsed.symbol_count);

        const auto result = sink.append_snapshot(
            snap, std::span<const SymbolRules>(new_table.data(), parsed.symbol_count));
        if (!result.acked()) return false;

        {
            std::unique_lock lock(mu_);
            table_ = new_table;
            registered_count_ = parsed.symbol_count;
            published_version_ = new_version;
        }
        return true;
    }

private:
    mutable std::shared_mutex mu_;
    std::array<SymbolRules, kMaxSymbols> table_{};
    std::size_t registered_count_{0};
    std::uint32_t published_version_{0};
};

}  // namespace hy
