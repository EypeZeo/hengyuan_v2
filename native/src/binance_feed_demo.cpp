// SPDX-License-Identifier: proprietary
// binance_feed_demo.cpp — Live demo: Binance public WS → simdjson → SPSC → OrderBook.
// Governance: L4 (real Binance public WS, no token/HMAC/Private API).
// Usage: ./binance_feed_demo [duration_seconds] [--record <path.hyf>]
//   Default: 30 seconds. Ctrl+C to stop early.
//   --record writes every accepted event to a .hyf binary file for later
//   conversion to Parquet/DuckDB via tools/perf/hyf_to_parquet.py.

#include <hengyuan/binance_json_parser.hpp>
#include <hengyuan/binance_ws_session.hpp>
#include <hengyuan/event_recorder.hpp>
#include <hengyuan/hot_thread.hpp>

#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

static std::atomic<bool> g_stop{false};
static void signal_handler(int) { g_stop.store(true); }

int main(int argc, char* argv[]) {
    int duration_s = 30;
    std::string record_path;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--record" && i + 1 < argc) {
            record_path = argv[++i];
        } else if (arg.size() > 0 && arg[0] != '-') {
            duration_s = std::atoi(argv[i]);
        }
    }
    if (duration_s <= 0) duration_s = 30;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    constexpr std::size_t kRingSize = 65536;

    // Set up parser with symbols
    hy::BinanceJsonParser parser;
    parser.register_symbol("BTCUSDT", 0);
    parser.register_symbol("ETHUSDT", 1);
    parser.register_symbol("SOLUSDT", 2);
    parser.register_symbol("DOGEUSDT", 3);
    parser.register_symbol("ADAUSDT", 4);

    // SPSC ring
    auto ring = std::make_unique<hy::SpscRing<hy::BinanceMarketEvent, kRingSize>>();

    // Hot thread consumer
    hy::HotThread<kRingSize> hot(*ring);
    static std::int64_t prev_bid = 0, prev_ask = 0;
    hot.set_on_top_of_book([](std::uint32_t /*sym*/, std::int64_t bid, std::int64_t ask) {
        if (bid == prev_bid && ask == prev_ask) return;
        prev_bid = bid; prev_ask = ask;
        std::printf("  TOB: bid=$%.2f  ask=$%.2f  spread=$%.4f\n",
                    static_cast<double>(bid) / 1e8,
                    static_cast<double>(ask) / 1e8,
                    static_cast<double>(ask - bid) / 1e8);
    });

    // Optional recorder: append every accepted event to a .hyf binary file.
    auto recorder = std::make_unique<hy::EventRecorder>();
    if (!record_path.empty()) {
        if (recorder->open(record_path)) {
            std::printf("Recording to: %s\n", record_path.c_str());
            hot.set_on_event([&recorder](const hy::BinanceMarketEvent& ev) {
                recorder->record(ev);
            });
        } else {
            std::printf("WARNING: failed to open record file: %s\n", record_path.c_str());
        }
    }

    // WS session config
    hy::WsSessionConfig config;
    config.subscribe_streams = {
        "btcusdt@trade", "ethusdt@trade", "solusdt@trade",
        "dogeusdt@trade", "adausdt@trade",
        "btcusdt@depth@100ms",
    };

    // Boost.Asio + SSL
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);

    auto session = std::make_shared<hy::BinanceWsSession<kRingSize>>(
        ioc, ssl_ctx, *ring, parser, config);
    session->start();

    // I/O thread (Boost.Asio event loop)
    std::thread io_thread([&ioc]() { ioc.run(); });

    std::printf("Binance Feed Demo — %d seconds (Ctrl+C to stop)\n", duration_s);
    std::printf("Subscribing to: BTC ETH SOL DOGE ADA (trade + BTC depth)\n\n");

    auto start = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        hot.run_once();

        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= duration_s) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Shutdown
    session->stop();
    ioc.stop();
    if (io_thread.joinable()) io_thread.join();

    // Final drain
    hot.run_once();

    // Print stats
    const auto& ws = session->stats();
    const auto& hs = hot.stats();
    const auto& pc = parser.counters();

    std::printf("\n=== Session Stats ===\n");
    std::printf("WS messages: %" PRIu64 "  bytes: %" PRIu64 "  errors: %" PRIu64 "\n",
                static_cast<std::uint64_t>(ws.messages_received),
                static_cast<std::uint64_t>(ws.bytes_received),
                static_cast<std::uint64_t>(ws.errors));
    std::printf("Parse OK: %" PRIu64 "  ignored: %" PRIu64 "  malformed: %" PRIu64 "\n",
                static_cast<std::uint64_t>(pc.parsed_ok),
                static_cast<std::uint64_t>(pc.ignored),
                static_cast<std::uint64_t>(pc.malformed));
    std::printf("Ring push OK: %" PRIu64 "  dropped: %" PRIu64 "\n",
                static_cast<std::uint64_t>(ws.push_ok),
                static_cast<std::uint64_t>(ws.push_dropped));
    std::printf("Hot events: %" PRIu64 "  trades: %" PRIu64 "  depth: %" PRIu64 "  rejected: %" PRIu64 "\n",
                static_cast<std::uint64_t>(hs.events_processed),
                static_cast<std::uint64_t>(hs.trades),
                static_cast<std::uint64_t>(hs.depth_updates),
                static_cast<std::uint64_t>(hs.rejected));

    auto tob = hot.book().top_of_book();
    if (tob) {
        std::printf("Final TOB: bid=$%.2f  ask=$%.2f\n",
                    static_cast<double>(tob->first) / 1e8,
                    static_cast<double>(tob->second) / 1e8);
    }

    if (recorder->is_open()) {
        std::printf("Recorded: %" PRIu64 " events to %s\n",
                    static_cast<std::uint64_t>(recorder->records_written()),
                    record_path.c_str());
        recorder->close();
    }

    return 0;
}
