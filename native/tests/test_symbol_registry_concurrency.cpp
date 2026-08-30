// L4 §5.3/§5.3.1 (PR 5b): SymbolRegistry multi-reader/single-writer concurrency test.
// std::shared_mutex is the first shared-state concurrency primitive this L4 branch has
// introduced -- must be run under WSL2 TSan (HY_SANITIZER=thread), same discipline
// test_reconcile_concurrency.cpp's own header documents: "ThreadSanitizer ... is what actually
// proves no missing acquire/release edge, and is required, not optional, before trusting this
// file's design." A plain (non-TSan) run only proves the LOGICAL contract across threads.
//
// GTEST + THREADS DISCIPLINE: never ASSERT_*/EXPECT_* on a worker thread (same reason
// test_reconcile_concurrency.cpp's header gives) -- each thread records into a shared atomic;
// all assertions run after every thread has joined.
#include <gtest/gtest.h>
#include "test_symbol_registry_fakes.hpp"

#include <atomic>
#include <thread>
#include <vector>

using hy::FakeSymbolRegistrySink;
using hy::SymbolRegistry;
using hy::SymbolRules;
using hy::make_single_symbol_parsed;

TEST(SymbolRegistryConcurrency, ReadersNeverObserveTornStateAcrossConcurrentRefreshes) {
    SymbolRegistry registry;
    FakeSymbolRegistrySink sink;

    // v1 is published before the race starts so every reader always has at least one
    // known-good, fully-published snapshot to observe from its very first read.
    ASSERT_TRUE(registry.refresh_from_exchange_info(sink,
                                                      make_single_symbol_parsed(1000, 1, 1, 100)));

    constexpr int kNumReaders = 4;
    std::atomic<bool> stop{false};
    std::atomic<int> torn_rules_count{0};
    std::atomic<int> bad_version_count{0};

    std::vector<std::thread> readers;
    readers.reserve(kNumReaders);
    for (int i = 0; i < kNumReaders; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                // current_rules() must return one atomic, complete snapshot -- never a struct
                // with fields mixed across generations (v1/v2/v3 each use a distinct,
                // internally-consistent combination of price_scale/qty_scale/
                // min_notional_ticks/rules_version, chosen specifically so any cross-generation
                // mix fails every one of these three checks).
                const SymbolRules r = registry.current_rules(0);
                const bool is_v1 = (r.price_scale == 1 && r.qty_scale == 1 &&
                                     r.min_notional_ticks == 100 && r.rules_version == 1);
                const bool is_v2 = (r.price_scale == 2 && r.qty_scale == 2 &&
                                     r.min_notional_ticks == 200 && r.rules_version == 2);
                const bool is_v3 = (r.price_scale == 3 && r.qty_scale == 3 &&
                                     r.min_notional_ticks == 300 && r.rules_version == 3);
                if (!is_v1 && !is_v2 && !is_v3) {
                    torn_rules_count.fetch_add(1, std::memory_order_relaxed);
                }

                // current_rules_version() covered too, not just current_rules(): must always be
                // one of the versions that has ever actually been published. Note this is NOT
                // asserted against the rules-snapshot read above for cross-call equality --
                // current_rules() and current_rules_version() are two independently-locked
                // accessors, so a real concurrent refresh landing between the two calls can
                // legitimately return different (but each individually valid) versions. That is
                // expected eventual-consistency behavior, not a bug.
                const std::uint32_t v = registry.current_rules_version();
                if (v != 1 && v != 2 && v != 3) {
                    bad_version_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::atomic<bool> refresh1_ok{false};
    std::atomic<bool> refresh2_ok{false};
    std::thread writer([&] {
        refresh1_ok.store(
            registry.refresh_from_exchange_info(sink, make_single_symbol_parsed(2000, 2, 2, 200)),
            std::memory_order_relaxed);
        refresh2_ok.store(
            registry.refresh_from_exchange_info(sink, make_single_symbol_parsed(3000, 3, 3, 300)),
            std::memory_order_relaxed);
    });

    writer.join();
    stop.store(true, std::memory_order_release);
    for (auto& t : readers) t.join();

    EXPECT_TRUE(refresh1_ok.load());
    EXPECT_TRUE(refresh2_ok.load());
    EXPECT_EQ(torn_rules_count.load(), 0);
    EXPECT_EQ(bad_version_count.load(), 0);
    EXPECT_EQ(registry.current_rules_version(), 3u);  // final state is the last published version
}
