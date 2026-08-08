// SPDX-License-Identifier: proprietary
// hot_thread.hpp — Data Plane hot consumer loop.
// Pops BinanceMarketEvent from SpscRing, validates, updates per-symbol OrderBooks.
// Optionally routes depth events through DepthManager for snapshot sync.
// Governance: L1/L2, no network/token/order.

#pragma once

#include <hengyuan/binance_json_parser.hpp>  // BinanceJsonParser::kMaxSymbolId (API-SYM-020 bound check)
#include <hengyuan/binance_market_event.hpp>
#include <hengyuan/depth_manager.hpp>
#include <hengyuan/input_validator.hpp>
#include <hengyuan/intent_channel.hpp>
#include <hengyuan/orderbook.hpp>
#include <hengyuan/shm_heartbeat.hpp>
#include <hengyuan/sim_executor.hpp>
#include <hengyuan/spsc_ring.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace hy {

struct HotThreadStats {
    std::uint64_t events_processed{0};
    std::uint64_t depth_updates{0};
    std::uint64_t trades{0};
    std::uint64_t rejected{0};
    std::uint64_t resync_requests{0};
    // AUDIT MD-BOOK-002: OrderBook::apply_delta()'s return value was discarded here,
    // so a delta that never reached the book looked exactly like one that did.
    // outside_window is expected and benign (deeper than the maintained top-N view);
    // invalid_deltas is not.
    std::uint64_t deltas_outside_window{0};
    std::uint64_t deltas_invalid{0};
    // Times run_once() hit kMaxEventsPerRun with the ring still non-empty (audit
    // HOT-DRAIN-027). Non-zero means the consumer is behind; sustained growth is the
    // signal to look at, not a single blip after a burst.
    std::uint64_t drain_limit_hits{0};
};

static constexpr std::size_t kMaxBookSymbols = 64;

// Audit API-SYM-020: book() clamps an out-of-range symbol_id to books_[0], which
// would report symbol 0's prices under someone else's id. BinanceJsonParser refuses
// to register such an id in the first place; this keeps the two bounds from drifting.
static_assert(BinanceJsonParser::kMaxSymbolId + 1 <= kMaxBookSymbols,
              "a registrable symbol_id must always have its own OrderBook slot");

using OnTopOfBookCallback = std::function<void(std::uint32_t symbol_id,
                                                std::int64_t best_bid,
                                                std::int64_t best_ask)>;
using OnEventCallback = std::function<void(const BinanceMarketEvent&)>;
using OnFillCallback = std::function<void(const TimestampedIntent&, const SimFill&)>;

template <std::size_t RingSize = 65536>
class HotThread {
public:
    explicit HotThread(SpscRing<BinanceMarketEvent, RingSize>& ring)
        : ring_(ring) {}

    void set_on_top_of_book(OnTopOfBookCallback cb) { on_tob_ = std::move(cb); }
    void set_on_event(OnEventCallback cb) { on_event_ = std::move(cb); }

    // When set, depth events for the given symbol are routed through the
    // DepthManager instead of being applied directly. The DepthManager
    // must outlive the HotThread.
    void set_heartbeat(ShmHeartbeatWriter* hb) noexcept { heartbeat_ = hb; }

    void set_depth_manager(DepthManager* dm, std::uint32_t symbol_id = 0) noexcept {
        depth_mgr_ = dm;
        depth_mgr_sym_ = symbol_id;
    }

    // Wire an intent channel + executor. Strategies push TimestampedIntents
    // into the channel; run_once() drains and executes them against the
    // current book. on_fill fires for each result (filled or rejected).
    void set_intent_executor(IntentChannel<>* ch, SimExecutor<>* ex,
                             OnFillCallback on_fill = {}) noexcept {
        intent_ch_ = ch;
        sim_exec_ = ex;
        on_fill_ = std::move(on_fill);
    }

    // Maximum market-data events consumed by one run_once() call.
    //
    // AUDIT HOT-DRAIN-027: this loop used to drain the ring to empty with no bound.
    // The ring holds RingSize (65536 by default) events, so a burst -- or simply a
    // slow consumer catching up -- could keep one call running for the whole backlog,
    // during which the snapshot gate, the intent channel and the watchdog kill check
    // (all after the loop) do not run at all. That is a tail-latency and
    // fail-closed-responsiveness problem, not a throughput one: the work still gets
    // done, just in one unbounded chunk instead of bounded slices.
    //
    // 4096 keeps the per-call cost bounded while staying far above any realistic
    // per-iteration arrival count, so the steady state still drains fully in one pass
    // and pays nothing for the bound.
    static constexpr std::size_t kMaxEventsPerRun = 4096;

    void run_once() noexcept {
        BinanceMarketEvent ev{};
        std::size_t drained = 0;
        while (drained < kMaxEventsPerRun && ring_.try_pop(ev)) {
            ++drained;
            auto vr = validator_.validate(ev);

            if (vr == ValidationResult::ResyncRequired) {
                if (depth_mgr_ && ev.symbol_id == depth_mgr_sym_) {
                    depth_mgr_->start_buffering();
                } else if (ev.symbol_id < kMaxBookSymbols) {
                    books_[ev.symbol_id].clear();
                }
                ++stats_.resync_requests;
                continue;
            }
            if (vr == ValidationResult::RejectNegativePrice ||
                vr == ValidationResult::RejectZeroPrice ||
                vr == ValidationResult::RejectNegativeQty ||
                vr == ValidationResult::RejectSeqRollback ||
                vr == ValidationResult::DropDuplicate) {
                ++stats_.rejected;
                continue;
            }

            ++stats_.events_processed;

            if (heartbeat_) {
                heartbeat_->tick();
                heartbeat_->set_incoming(ev.ts_recv_ns);
            }

            if (on_event_) {
                on_event_(ev);
            }

            if (ev.type == EventType::DepthDelta) {
                if (depth_mgr_ && ev.symbol_id == depth_mgr_sym_) {
                    depth_mgr_->on_depth_event(ev, ev.aux_id, ev.event_id);
                } else if (ev.symbol_id < kMaxBookSymbols) {
                    const auto ar =
                        books_[ev.symbol_id].apply_delta(ev.price_ticks, ev.qty_lots, ev.side);
                    if (ar == OrderBook::ApplyResult::OutsideWindow) {
                        ++stats_.deltas_outside_window;
                    } else if (ar == OrderBook::ApplyResult::InvalidInput) {
                        ++stats_.deltas_invalid;
                    }
                }
                ++stats_.depth_updates;

                if (on_tob_) {
                    auto tob = book(ev.symbol_id).top_of_book();
                    if (tob) {
                        on_tob_(ev.symbol_id, tob->first, tob->second);
                    }
                }
            } else if (ev.type == EventType::Trade || ev.type == EventType::AggTrade) {
                ++stats_.trades;
            }
        }

        if (drained == kMaxEventsPerRun && !ring_.empty_approx()) {
            ++stats_.drain_limit_hits;
        }

        // Watchdog kill check: if watchdog armed the kill flag, arm our
        // SimExecutor's kill switch (fail-closed, never ignore).
        if (heartbeat_ && heartbeat_->kill_requested() && sim_exec_) {
            sim_exec_->kill_switch().arm();
        }

        // Drain intent channel: execute pending strategy intents against book.
        if (intent_ch_ && sim_exec_) {
            TimestampedIntent ti{};
            while (intent_ch_->try_pop(ti)) {
                const auto sym = ti.intent.symbol_id;
                auto tob = book(sym).top_of_book();
                std::int64_t bid = tob ? tob->first : 0;
                std::int64_t ask = tob ? tob->second : 0;
                auto fill = sim_exec_->execute(ti.intent, bid, ask);
                if (heartbeat_ && fill.status == FillStatus::Filled) {
                    heartbeat_->set_outgoing(static_cast<std::uint64_t>(
                        std::chrono::steady_clock::now().time_since_epoch().count()));
                }
                if (on_fill_) {
                    on_fill_(ti, fill);
                }
            }
        }
    }

    void request_stop() noexcept { stop_.store(true, std::memory_order_relaxed); }
    bool should_stop() const noexcept { return stop_.load(std::memory_order_relaxed); }

    const HotThreadStats& stats() const noexcept { return stats_; }
    const InputValidator& validator() const noexcept { return validator_; }

    // Per-symbol book accessor. Returns DepthManager's book if that symbol
    // has a DepthManager, else the direct OrderBook.
    const OrderBook& book(std::uint32_t symbol_id = 0) const noexcept {
        if (depth_mgr_ && symbol_id == depth_mgr_sym_) {
            return depth_mgr_->book();
        }
        return books_[symbol_id < kMaxBookSymbols ? symbol_id : 0];
    }

private:
    SpscRing<BinanceMarketEvent, RingSize>& ring_;
    InputValidator validator_{};
    std::array<OrderBook, kMaxBookSymbols> books_{};
    HotThreadStats stats_{};
    std::atomic<bool> stop_{false};
    OnTopOfBookCallback on_tob_{};
    OnEventCallback on_event_{};
    DepthManager* depth_mgr_{nullptr};
    std::uint32_t depth_mgr_sym_{0};
    IntentChannel<>* intent_ch_{nullptr};
    SimExecutor<>* sim_exec_{nullptr};
    OnFillCallback on_fill_{};
    ShmHeartbeatWriter* heartbeat_{nullptr};
};

}  // namespace hy
