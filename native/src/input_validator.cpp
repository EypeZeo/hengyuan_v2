// SPDX-License-Identifier: proprietary
#include <hengyuan/input_validator.hpp>

namespace hy {

ValidationResult InputValidator::validate(const BinanceMarketEvent& ev) noexcept {
    if (ev.flags & event_flag::kResyncRequired) {
        ++counters_.resync_requests;
        return ValidationResult::ResyncRequired;
    }

    if (ev.price_ticks < 0) {
        ++counters_.rejected_negative_price;
        return ValidationResult::RejectNegativePrice;
    }

    if (ev.price_ticks == 0 && ev.qty_lots != 0) {
        ++counters_.rejected_zero_price;
        return ValidationResult::RejectZeroPrice;
    }

    if (ev.qty_lots < 0) {
        ++counters_.rejected_negative_qty;
        return ValidationResult::RejectNegativeQty;
    }

    auto type_idx = static_cast<std::size_t>(ev.type);
    if (type_idx < kEventTypes && ev.symbol_id < kMaxSymbols) {
        auto& state = seq_state_[ev.symbol_id][type_idx];
        if (state.seen) {
            if (ev.event_id < state.last_event_id) {
                ++counters_.rejected_seq_rollback;
                return ValidationResult::RejectSeqRollback;
            }
            // DepthDelta: multiple levels share the same event_id per message,
            // so duplicates are expected and valid. Only trades use strict dedup.
            if (ev.event_id == state.last_event_id &&
                ev.type != EventType::DepthDelta) {
                ++counters_.dropped_duplicate;
                return ValidationResult::DropDuplicate;
            }
        }
        state.last_event_id = ev.event_id;
        state.seen = true;
    }

    // Clock-anomaly detection is per-symbol (audit VAL-TS-028): a symbol's event
    // times are only monotonic against ITS OWN previous event, never against another
    // symbol's. An out-of-range symbol_id cannot reach here with a tracked clock,
    // and is simply not clock-checked rather than being folded into a shared
    // counter.
    auto result = ValidationResult::Accept;
    if (ev.symbol_id < kMaxSymbols) {
        auto& clock = clock_state_[ev.symbol_id];
        if (clock.seen && ev.ts_event_ms < clock.last_ts_event_ms &&
            (clock.last_ts_event_ms - ev.ts_event_ms) > 1000) {
            ++counters_.clock_anomalies;
            result = ValidationResult::AcceptClockAnomaly;
        }
        clock.last_ts_event_ms = ev.ts_event_ms;
        clock.seen = true;
    }

    ++counters_.accepted;

    return result;
}

void InputValidator::reset_sequences() noexcept {
    for (auto& per_symbol : seq_state_) {
        for (auto& state : per_symbol) state = SeqState{};
    }
    for (auto& clock : clock_state_) clock = ClockState{};
}

}  // namespace hy
