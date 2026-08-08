// P2-CORE-OB-01: InputValidator unit tests — every malformed type from P2-CORE-03.
#include <gtest/gtest.h>
#include <hengyuan/input_validator.hpp>

#include <cstdint>

using hy::BinanceMarketEvent;
using hy::EventType;
using hy::InputValidator;
using hy::Side;
using hy::ValidationResult;

static BinanceMarketEvent make_event(std::uint64_t id, std::int64_t price,
                                      std::int64_t qty, std::uint64_t ts_ms,
                                      EventType type = EventType::Trade,
                                      std::uint32_t symbol_id = 0) {
    BinanceMarketEvent ev{};
    ev.event_id = id;
    ev.price_ticks = price;
    ev.qty_lots = qty;
    ev.ts_event_ms = ts_ms;
    ev.type = type;
    ev.side = Side::Buy;
    ev.symbol_id = symbol_id;
    ev.flags = 0;
    return ev;
}

TEST(InputValidator, AcceptsValidEvent) {
    InputValidator v;
    auto ev = make_event(1, 50000'00000000, 1'00000000, 1700000000000);
    EXPECT_EQ(v.validate(ev), ValidationResult::Accept);
    EXPECT_EQ(v.counters().accepted, 1u);
}

TEST(InputValidator, RejectsNegativePrice) {
    InputValidator v;
    auto ev = make_event(1, -100, 100, 1700000000000);
    EXPECT_EQ(v.validate(ev), ValidationResult::RejectNegativePrice);
    EXPECT_EQ(v.counters().rejected_negative_price, 1u);
}

TEST(InputValidator, RejectsZeroPriceWithNonZeroQty) {
    InputValidator v;
    auto ev = make_event(1, 0, 100, 1700000000000);
    EXPECT_EQ(v.validate(ev), ValidationResult::RejectZeroPrice);
}

TEST(InputValidator, AcceptsZeroPriceWithZeroQty) {
    InputValidator v;
    auto ev = make_event(1, 0, 0, 1700000000000);
    EXPECT_EQ(v.validate(ev), ValidationResult::Accept);
}

TEST(InputValidator, RejectsNegativeQty) {
    InputValidator v;
    auto ev = make_event(1, 50000, -10, 1700000000000);
    EXPECT_EQ(v.validate(ev), ValidationResult::RejectNegativeQty);
}

TEST(InputValidator, DropsDuplicate) {
    InputValidator v;
    auto ev1 = make_event(42, 50000, 100, 1700000000000);
    auto ev2 = make_event(42, 50000, 200, 1700000001000);
    EXPECT_EQ(v.validate(ev1), ValidationResult::Accept);
    EXPECT_EQ(v.validate(ev2), ValidationResult::DropDuplicate);
    EXPECT_EQ(v.counters().dropped_duplicate, 1u);
}

TEST(InputValidator, RejectsSeqRollback) {
    InputValidator v;
    auto ev1 = make_event(100, 50000, 100, 1700000000000);
    auto ev2 = make_event(50, 50000, 100, 1700000001000);
    EXPECT_EQ(v.validate(ev1), ValidationResult::Accept);
    EXPECT_EQ(v.validate(ev2), ValidationResult::RejectSeqRollback);
}

TEST(InputValidator, DetectsClockAnomaly) {
    InputValidator v;
    auto ev1 = make_event(1, 50000, 100, 1700000005000);
    auto ev2 = make_event(2, 50000, 100, 1700000002000);  // 3s rollback > 1s threshold
    EXPECT_EQ(v.validate(ev1), ValidationResult::Accept);
    EXPECT_EQ(v.validate(ev2), ValidationResult::AcceptClockAnomaly);
    EXPECT_EQ(v.counters().clock_anomalies, 1u);
    EXPECT_EQ(v.counters().accepted, 2u);
}

TEST(InputValidator, IgnoresSmallClockJitter) {
    InputValidator v;
    auto ev1 = make_event(1, 50000, 100, 1700000001000);
    auto ev2 = make_event(2, 50000, 100, 1700000000500);  // 500ms < 1s threshold
    EXPECT_EQ(v.validate(ev1), ValidationResult::Accept);
    EXPECT_EQ(v.validate(ev2), ValidationResult::Accept);
    EXPECT_EQ(v.counters().clock_anomalies, 0u);
}

TEST(InputValidator, ResyncRequired) {
    InputValidator v;
    BinanceMarketEvent ev{};
    ev.event_id = 1;
    ev.price_ticks = 50000;
    ev.qty_lots = 100;
    ev.flags = hy::event_flag::kResyncRequired;
    EXPECT_EQ(v.validate(ev), ValidationResult::ResyncRequired);
    EXPECT_EQ(v.counters().resync_requests, 1u);
}

TEST(InputValidator, ResetCounters) {
    InputValidator v;
    auto ev = make_event(1, -100, 100, 1700000000000);
    v.validate(ev);
    EXPECT_EQ(v.counters().rejected_negative_price, 1u);
    v.reset_counters();
    EXPECT_EQ(v.counters().rejected_negative_price, 0u);
}

TEST(InputValidator, MultiSymbolTradesDontConflict) {
    InputValidator v;
    // BTC (symbol 0) has a huge trade id; ETH (symbol 1) a much smaller one.
    // With a shared counter ETH would be falsely rejected as rollback.
    auto btc = make_event(5'000'000'000ULL, 50000, 100, 1700000000000, EventType::Trade, 0);
    auto eth = make_event(800'000ULL, 3000, 50, 1700000000001, EventType::Trade, 1);
    auto sol = make_event(12'345ULL, 150, 10, 1700000000002, EventType::Trade, 2);
    EXPECT_EQ(v.validate(btc), ValidationResult::Accept);
    EXPECT_EQ(v.validate(eth), ValidationResult::Accept);
    EXPECT_EQ(v.validate(sol), ValidationResult::Accept);
    EXPECT_EQ(v.counters().rejected_seq_rollback, 0u);
    EXPECT_EQ(v.counters().accepted, 3u);
}

TEST(InputValidator, SameSymbolRollbackStillCaught) {
    InputValidator v;
    auto t1 = make_event(100, 50000, 100, 1700000000000, EventType::Trade, 1);
    auto t2 = make_event(50, 50000, 100, 1700000001000, EventType::Trade, 1);
    EXPECT_EQ(v.validate(t1), ValidationResult::Accept);
    EXPECT_EQ(v.validate(t2), ValidationResult::RejectSeqRollback);
}

TEST(InputValidator, MixedEventTypesDontConflict) {
    InputValidator v;
    auto trade = make_event(100000, 50000, 100, 1700000000000, EventType::Trade);
    auto depth = make_event(200, 50000, 10, 1700000000001);
    depth.type = hy::EventType::DepthDelta;
    auto agg = make_event(500, 50000, 50, 1700000000002);
    agg.type = hy::EventType::AggTrade;

    EXPECT_EQ(v.validate(trade), ValidationResult::Accept);
    EXPECT_EQ(v.validate(depth), ValidationResult::Accept);
    EXPECT_EQ(v.validate(agg), ValidationResult::Accept);
    EXPECT_EQ(v.counters().rejected_seq_rollback, 0u);
    EXPECT_EQ(v.counters().accepted, 3u);
}

TEST(InputValidator, SameTypeDuplicateStillCaught) {
    InputValidator v;
    auto t1 = make_event(100, 50000, 100, 1700000000000, EventType::Trade);
    auto t2 = make_event(100, 50000, 200, 1700000000001, EventType::Trade);
    EXPECT_EQ(v.validate(t1), ValidationResult::Accept);
    EXPECT_EQ(v.validate(t2), ValidationResult::DropDuplicate);
}

TEST(InputValidator, DepthDeltaSameEventIdAccepted) {
    InputValidator v;
    // Multiple depth levels from the same depthUpdate message share the same event_id.
    auto d1 = make_event(200, 67890, 100, 1700000000000, EventType::DepthDelta);
    auto d2 = make_event(200, 67891, 50, 1700000000000, EventType::DepthDelta);
    auto d3 = make_event(200, 67889, 200, 1700000000000, EventType::DepthDelta);
    EXPECT_EQ(v.validate(d1), ValidationResult::Accept);
    EXPECT_EQ(v.validate(d2), ValidationResult::Accept);
    EXPECT_EQ(v.validate(d3), ValidationResult::Accept);
    EXPECT_EQ(v.counters().dropped_duplicate, 0u);
    EXPECT_EQ(v.counters().accepted, 3u);
}

TEST(InputValidator, DepthDeltaRollbackStillRejected) {
    InputValidator v;
    auto d1 = make_event(200, 67890, 100, 1700000000000, EventType::DepthDelta);
    auto d2 = make_event(199, 67891, 50, 1700000000001, EventType::DepthDelta);
    EXPECT_EQ(v.validate(d1), ValidationResult::Accept);
    EXPECT_EQ(v.validate(d2), ValidationResult::RejectSeqRollback);
}

TEST(InputValidator, MonotonicSequence) {
    InputValidator v;
    for (std::uint64_t i = 1; i <= 100; ++i) {
        auto ev = make_event(i, 50000, 100, 1700000000000 + i * 100);
        EXPECT_EQ(v.validate(ev), ValidationResult::Accept);
    }
    EXPECT_EQ(v.counters().accepted, 100u);
}

// --- Clock-anomaly detection is per-symbol (audit VAL-TS-028) ---
//
// last_ts_event_ms_ used to be a single cross-symbol counter, for exactly the reason
// seq_state_ is already per-(symbol, type): different symbols carry independent
// event-time streams. On a multi-symbol feed a plain interleave made every other
// event look like a >1s backwards clock jump.

TEST(InputValidatorClock, InterleavedSymbolsDoNotFakeClockAnomalies) {
    hy::InputValidator v;
    // Two symbols whose event clocks are legitimately 5s apart -- perfectly normal
    // when two streams are multiplexed onto one connection.
    for (int i = 0; i < 50; ++i) {
        hy::BinanceMarketEvent a{};
        a.type = hy::EventType::Trade;
        a.symbol_id = 0;
        a.event_id = static_cast<std::uint64_t>(i) + 1;
        a.ts_event_ms = std::uint64_t{1'700'000'000'000} + static_cast<std::uint64_t>(i);
        a.price_ticks = 100;
        a.qty_lots = 1;
        EXPECT_EQ(v.validate(a), hy::ValidationResult::Accept);

        hy::BinanceMarketEvent b{};
        b.type = hy::EventType::Trade;
        b.symbol_id = 1;
        b.event_id = static_cast<std::uint64_t>(i) + 1;
        b.ts_event_ms = std::uint64_t{1'699'999'995'000} + static_cast<std::uint64_t>(i);
        b.price_ticks = 100;
        b.qty_lots = 1;
        EXPECT_EQ(v.validate(b), hy::ValidationResult::Accept);
    }
    EXPECT_EQ(v.counters().clock_anomalies, 0u)
        << "a second symbol running 5s behind is not a clock anomaly";
}

TEST(InputValidatorClock, RealBackwardsJumpOnOneSymbolIsStillDetected) {
    hy::InputValidator v;
    hy::BinanceMarketEvent e{};
    e.type = hy::EventType::Trade;
    e.symbol_id = 0;
    e.price_ticks = 100;
    e.qty_lots = 1;

    e.event_id = 1;
    e.ts_event_ms = std::uint64_t{1'700'000'000'000};
    EXPECT_EQ(v.validate(e), hy::ValidationResult::Accept);

    e.event_id = 2;
    e.ts_event_ms = std::uint64_t{1'700'000'000'000} - 5000;  // same symbol, 5s backwards
    EXPECT_EQ(v.validate(e), hy::ValidationResult::AcceptClockAnomaly);
    EXPECT_EQ(v.counters().clock_anomalies, 1u);
}
