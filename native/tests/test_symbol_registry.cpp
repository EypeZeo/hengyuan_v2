// L4 §5.3/§5.3.1 (PR 5b): SymbolRegistry unit tests. Plain, single-threaded -- the
// multi-reader/single-writer concurrency test lives separately in
// test_symbol_registry_concurrency.cpp (same split-by-concurrency-label pattern as
// test_spsc_ring.cpp/test_spsc_concurrency.cpp), sharing this file's fixtures via
// test_symbol_registry_fakes.hpp.
#include <gtest/gtest.h>
#include "test_symbol_registry_fakes.hpp"

using hy::FakeSymbolRegistrySink;
using hy::ParsedExchangeInfo;
using hy::SymbolRegistry;
using hy::SymbolRules;
using hy::kMaxSymbols;
using hy::make_single_symbol_parsed;

// --- current_rules() / current_rules_version() on an empty registry ---

TEST(SymbolRegistry, UnregisteredSymbolIdReturnsDefaultConstructedRules) {
    SymbolRegistry registry;
    const SymbolRules r = registry.current_rules(0);
    EXPECT_EQ(r.rules_version, 0u);
    EXPECT_FALSE(r.is_trading);
}

TEST(SymbolRegistry, OutOfBoundsSymbolIdReturnsDefaultConstructedRules) {
    SymbolRegistry registry;
    const SymbolRules r = registry.current_rules(static_cast<std::uint32_t>(kMaxSymbols) + 100);
    EXPECT_EQ(r.rules_version, 0u);
}

TEST(SymbolRegistry, EmptyRegistryVersionIsZero) {
    SymbolRegistry registry;
    EXPECT_EQ(registry.current_rules_version(), 0u);
}

// --- refresh_from_exchange_info(): success path ---

TEST(SymbolRegistry, SuccessfulRefreshPublishesVersionOneAndStampsEveryEntry) {
    SymbolRegistry registry;
    FakeSymbolRegistrySink sink;

    const auto parsed = make_single_symbol_parsed(/*server_time_ms=*/123456, 4, 8, 1000);
    ASSERT_TRUE(registry.refresh_from_exchange_info(sink, parsed));

    EXPECT_EQ(registry.current_rules_version(), 1u);
    const SymbolRules r = registry.current_rules(0);
    EXPECT_EQ(r.rules_version, 1u);
    EXPECT_TRUE(r.is_trading);
    EXPECT_EQ(r.price_scale, 4);
    EXPECT_EQ(r.qty_scale, 8);
    EXPECT_EQ(r.min_notional_ticks, 1000);
}

TEST(SymbolRegistry, RefreshUsesParsedServerTimeAsSnapshotTimestamp) {
    SymbolRegistry registry;
    FakeSymbolRegistrySink sink;

    const auto parsed = make_single_symbol_parsed(/*server_time_ms=*/987654321, 0, 0, 0);
    ASSERT_TRUE(registry.refresh_from_exchange_info(sink, parsed));

    EXPECT_EQ(sink.last_payload().timestamp_ms, 987654321);
}

TEST(SymbolRegistry, SecondSuccessfulRefreshIncrementsVersionAndReplacesTable) {
    SymbolRegistry registry;
    FakeSymbolRegistrySink sink;

    ASSERT_TRUE(registry.refresh_from_exchange_info(
        sink, make_single_symbol_parsed(1000, 1, 1, 100)));
    ASSERT_TRUE(registry.refresh_from_exchange_info(
        sink, make_single_symbol_parsed(2000, 2, 2, 200)));

    EXPECT_EQ(registry.current_rules_version(), 2u);
    const SymbolRules r = registry.current_rules(0);
    EXPECT_EQ(r.rules_version, 2u);
    EXPECT_EQ(r.price_scale, 2);
    EXPECT_EQ(r.qty_scale, 2);
    EXPECT_EQ(r.min_notional_ticks, 200);
}

// --- refresh_from_exchange_info(): Ack failure (§5.3.1's core invariant) ---

TEST(SymbolRegistry, AckFailureLeavesRegistryCompletelyUnchanged) {
    SymbolRegistry registry;
    FakeSymbolRegistrySink sink;

    ASSERT_TRUE(registry.refresh_from_exchange_info(
        sink, make_single_symbol_parsed(1000, 1, 1, 100)));
    const SymbolRules before = registry.current_rules(0);
    const std::uint32_t version_before = registry.current_rules_version();

    sink.set_next_append_acked(false);
    EXPECT_FALSE(registry.refresh_from_exchange_info(
        sink, make_single_symbol_parsed(2000, 9, 9, 900)));

    const SymbolRules after = registry.current_rules(0);
    EXPECT_EQ(registry.current_rules_version(), version_before);
    EXPECT_EQ(after.rules_version, before.rules_version);
    EXPECT_EQ(after.price_scale, before.price_scale);
    EXPECT_EQ(after.qty_scale, before.qty_scale);
    EXPECT_EQ(after.min_notional_ticks, before.min_notional_ticks);
}

TEST(SymbolRegistry, AckFailureOnFirstEverRefreshLeavesRegistryEmpty) {
    SymbolRegistry registry;
    FakeSymbolRegistrySink sink;
    sink.set_next_append_acked(false);

    EXPECT_FALSE(registry.refresh_from_exchange_info(
        sink, make_single_symbol_parsed(1000, 1, 1, 100)));
    EXPECT_EQ(registry.current_rules_version(), 0u);
    EXPECT_EQ(registry.current_rules(0).rules_version, 0u);
}

// --- Capacity defense-in-depth (fetch_exchange_info() should already have rejected this) ---

TEST(SymbolRegistry, SymbolCountAboveCapacityIsRejectedWithoutTouchingSink) {
    SymbolRegistry registry;
    FakeSymbolRegistrySink sink;

    ParsedExchangeInfo parsed{};
    parsed.symbol_count = kMaxSymbols + 1;  // ParsedExchangeInfo::symbols itself can't hold this
                                             // many real entries, but symbol_count is an
                                             // independently-set field -- the defensive check
                                             // must catch a caller passing a bad count directly.

    EXPECT_FALSE(registry.refresh_from_exchange_info(sink, parsed));
    EXPECT_EQ(sink.append_snapshot_call_count(), 0u);
}
