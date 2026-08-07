// SPDX-License-Identifier: proprietary
// input_validator.hpp — P2-CORE-OB-01 / P2-CORE-03: fail-closed malformed input gate.
// Governance: L1, no network/token/order.

#pragma once

#include <hengyuan/binance_market_event.hpp>
#include <cstdint>

namespace hy {

enum class ValidationResult : std::uint8_t {
    Accept = 0,
    RejectNegativePrice = 1,
    RejectZeroPrice = 2,
    RejectNegativeQty = 3,
    RejectSeqRollback = 4,
    DropDuplicate = 5,
    AcceptClockAnomaly = 6,
    ResyncRequired = 7,
};

struct ValidationCounters {
    std::uint64_t accepted{0};
    std::uint64_t rejected_negative_price{0};
    std::uint64_t rejected_zero_price{0};
    std::uint64_t rejected_negative_qty{0};
    std::uint64_t rejected_seq_rollback{0};
    std::uint64_t dropped_duplicate{0};
    std::uint64_t clock_anomalies{0};
    std::uint64_t resync_requests{0};
};

class InputValidator {
public:
    ValidationResult validate(const BinanceMarketEvent& ev) noexcept;

    const ValidationCounters& counters() const noexcept { return counters_; }
    void reset_counters() noexcept { counters_ = {}; }

private:
    // Sequence/duplicate state is keyed by (symbol_id, event_type): Binance
    // trade ids, aggTrade ids and depth update ids are independent sequences
    // PER SYMBOL. Sharing one counter across symbols falsely rejects the
    // lower-id symbols as rollbacks.
    static constexpr std::size_t kEventTypes = 4;
    static constexpr std::size_t kMaxSymbols = 64;
    struct SeqState {
        std::uint64_t last_event_id{0};
        bool seen{false};
    };

    ValidationCounters counters_{};
    SeqState seq_state_[kMaxSymbols][kEventTypes]{};

    // AUDIT VAL-TS-028: this used to be a single cross-symbol counter, for exactly
    // the reason seq_state_ above is per-(symbol, type): different symbols carry
    // independent event-time streams. On a multi-symbol feed a plain interleave
    // (BTC at t, ETH at t-2000, BTC at t+1, ...) made every other event look like a
    // >1s backwards clock jump, so counters_.clock_anomalies was pure noise --
    // useless as a signal precisely when a real clock anomaly would matter most.
    // Per-symbol, it compares like with like.
    struct ClockState {
        std::uint64_t last_ts_event_ms{0};
        bool seen{false};
    };
    ClockState clock_state_[kMaxSymbols]{};
};

}  // namespace hy
