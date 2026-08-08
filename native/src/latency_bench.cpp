// SPDX-License-Identifier: proprietary
// latency_bench.cpp — Micro-benchmark for native core components.
//
// Measures per-operation latency (ns) for parser, SPSC, OrderBook, and
// full pipeline. Prints P50/P99/P999 percentiles.
//
// Governance: L1 (pure synthetic data, no network/token/order).
// Usage: ./latency_bench [iterations]   (default: 100000)

#include <hengyuan/binance_json_parser.hpp>
#include <hengyuan/hot_thread.hpp>
#include <hengyuan/orderbook.hpp>
#include <hengyuan/spsc_ring.hpp>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

struct BenchResult {
    const char* name;
    std::uint64_t p50;
    std::uint64_t p99;
    std::uint64_t p999;
    std::uint64_t min_ns;
    std::uint64_t max_ns;
    double mean;
};

static BenchResult compute_stats(const char* name, std::vector<std::uint64_t>& samples) {
    std::sort(samples.begin(), samples.end());
    std::size_t n = samples.size();
    double sum = 0;
    for (auto s : samples) sum += static_cast<double>(s);
    return {
        name,
        samples[n / 2],
        samples[static_cast<std::size_t>(static_cast<double>(n) * 0.99)],
        samples[static_cast<std::size_t>(static_cast<double>(n) * 0.999)],
        samples[0],
        samples[n - 1],
        sum / static_cast<double>(n),
    };
}

static void print_result(const BenchResult& r) {
    std::printf("  %-25s  P50=%5" PRIu64 "ns  P99=%5" PRIu64
                "ns  P999=%6" PRIu64 "ns  min=%5" PRIu64 "  max=%6" PRIu64
                "  mean=%.0f\n",
                r.name, r.p50, r.p99, r.p999, r.min_ns, r.max_ns, r.mean);
}

// ---- Benchmarks ----

static BenchResult bench_parser(int iters) {
    hy::BinanceJsonParser parser;
    (void)parser.register_symbol("BTCUSDT", 0);  // fixed literal, cannot fail

    const std::string json = R"({
        "e": "trade", "E": 1700000000000, "s": "BTCUSDT", "t": 123456,
        "p": "67891.23000000", "q": "0.01000000", "b": 88888, "a": 88889,
        "T": 1700000000000, "m": true, "M": true
    })";

    // Warmup
    for (int i = 0; i < 1000; ++i) {
        hy::BinanceMarketEvent evs[4];
        std::size_t count = 0;
        parser.parse(json, 0, evs, 4, count);
    }

    std::vector<std::uint64_t> samples(static_cast<std::size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        hy::BinanceMarketEvent evs[4];
        std::size_t count = 0;
        auto t0 = now_ns();
        parser.parse(json, 0, evs, 4, count);
        samples[static_cast<std::size_t>(i)] = now_ns() - t0;
    }
    return compute_stats("parser (trade)", samples);
}

static BenchResult bench_parser_depth(int iters) {
    hy::BinanceJsonParser parser;
    (void)parser.register_symbol("BTCUSDT", 0);  // fixed literal, cannot fail

    const std::string json = R"({
        "e": "depthUpdate", "E": 1700000002000, "s": "BTCUSDT",
        "U": 100, "u": 200,
        "b": [["67890.00000000","1.00000000"],["67889.00000000","0.50000000"],
              ["67888.00000000","0.30000000"],["67887.00000000","0.20000000"],
              ["67886.00000000","0.10000000"]],
        "a": [["67891.00000000","0.50000000"],["67892.00000000","0.30000000"],
              ["67893.00000000","0.20000000"],["67894.00000000","0.15000000"],
              ["67895.00000000","0.10000000"]]
    })";

    for (int i = 0; i < 1000; ++i) {
        hy::BinanceMarketEvent evs[64];
        std::size_t count = 0;
        parser.parse(json, 0, evs, 64, count);
    }

    std::vector<std::uint64_t> samples(static_cast<std::size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        hy::BinanceMarketEvent evs[64];
        std::size_t count = 0;
        auto t0 = now_ns();
        parser.parse(json, 0, evs, 64, count);
        samples[static_cast<std::size_t>(i)] = now_ns() - t0;
    }
    return compute_stats("parser (depth 10lvl)", samples);
}

static BenchResult bench_spsc_roundtrip(int iters) {
    hy::SpscRing<hy::BinanceMarketEvent, 65536> ring;
    hy::BinanceMarketEvent ev{};
    ev.price_ticks = 5990000000000LL;
    ev.qty_lots = 100000;
    ev.type = hy::EventType::Trade;

    for (int i = 0; i < 1000; ++i) {
        ring.try_push(ev);
        hy::BinanceMarketEvent out{};
        ring.try_pop(out);
    }

    std::vector<std::uint64_t> samples(static_cast<std::size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        auto t0 = now_ns();
        ring.try_push(ev);
        hy::BinanceMarketEvent out{};
        ring.try_pop(out);
        samples[static_cast<std::size_t>(i)] = now_ns() - t0;
    }
    return compute_stats("SPSC push+pop", samples);
}

static BenchResult bench_orderbook_delta(int iters) {
    hy::OrderBook ob;
    hy::PriceLevel asks[] = {{100, 10}, {101, 20}};
    hy::PriceLevel bids[] = {{99, 15}, {98, 25}};
    ob.apply_snapshot(asks, 2, bids, 2);

    for (int i = 0; i < 1000; ++i) {
        ob.apply_delta(100, static_cast<std::int64_t>(10 + (i % 50)), hy::Side::Sell);
    }

    std::vector<std::uint64_t> samples(static_cast<std::size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        auto t0 = now_ns();
        ob.apply_delta(100, static_cast<std::int64_t>(10 + (i % 50)), hy::Side::Sell);
        samples[static_cast<std::size_t>(i)] = now_ns() - t0;
    }
    return compute_stats("OrderBook apply_delta", samples);
}

static BenchResult bench_validator(int iters) {
    hy::InputValidator v;
    hy::BinanceMarketEvent ev{};
    ev.price_ticks = 5990000000000LL;
    ev.qty_lots = 100000;
    ev.type = hy::EventType::Trade;
    ev.side = hy::Side::Buy;

    std::vector<std::uint64_t> samples(static_cast<std::size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        ev.event_id = static_cast<std::uint64_t>(static_cast<unsigned>(i + 1));
        ev.ts_event_ms = 1700000000000ULL + static_cast<std::uint64_t>(static_cast<unsigned>(i));
        auto t0 = now_ns();
        v.validate(ev);
        samples[static_cast<std::size_t>(i)] = now_ns() - t0;
    }
    return compute_stats("InputValidator", samples);
}

int main(int argc, char* argv[]) {
    int iters = 100000;
    if (argc > 1) iters = std::atoi(argv[1]);
    if (iters < 1000) iters = 1000;

    std::printf("=== HengYuan Native Latency Benchmark ===\n");
    std::printf("Iterations: %d\n\n", iters);

    print_result(bench_parser(iters));
    print_result(bench_parser_depth(iters));
    print_result(bench_spsc_roundtrip(iters));
    print_result(bench_orderbook_delta(iters));
    print_result(bench_validator(iters));

    std::printf("\nAll times in nanoseconds. Measured with std::chrono::steady_clock.\n");
    return 0;
}
