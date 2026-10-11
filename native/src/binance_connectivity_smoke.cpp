// SPDX-License-Identifier: proprietary
// binance_connectivity_smoke.cpp — Public-endpoint connectivity smoke test (P2-MD-02 / Track C).
//
// Single-purpose, HY_BUILD_DEMO-gated binary: runs BinanceWsSession against the real public
// trade stream for a fixed duration and calls fetch_depth_snapshot() once against the real
// REST endpoint, then reports a machine-parseable one-line summary and a bitmask exit code.
// This is the concrete "VPS manual verification = L4" procedure referenced in
// binance_ws_session.hpp's and binance_rest_snapshot.hpp's own header comments -- manual, run
// by a human on demand, not part of CI (a live-network smoke test on every PR/push would be
// flaky and rate-limit-prone by nature; the deterministic connectivity/timeout/TLS behavior
// this binary depends on is already covered offline by test_binance_rest_snapshot.cpp and
// test_binance_ws_session.cpp).
//
// Research/backtesting connectivity check only -- NOT trading authorization, NOT a live-feed
// guarantee, NOT a statement about production readiness of anything beyond "can this process
// reach Binance's public endpoints."
//
// Usage: ./binance_connectivity_smoke [--symbol SYM] [--duration N]
//   --symbol SYM     Binance spot symbol, uppercase (default BTCUSDT)
//   --duration N     seconds to run the WS leg (default 20)
//
// Exit code is a bitmask (0 = both legs OK):
//   bit 0 (1) — WS leg failed (zero messages received in the given duration)
//   bit 1 (2) — REST leg failed (fetch_depth_snapshot returned nullopt)

#include <hengyuan/binance_json_parser.hpp>
#include <hengyuan/binance_rest_snapshot.hpp>
#include <hengyuan/binance_tls.hpp>
#include <hengyuan/binance_ws_session.hpp>
#include <hengyuan/spsc_ring.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string symbol = "BTCUSDT";
    int duration_s = 20;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--symbol" && i + 1 < argc) {
            symbol = argv[++i];
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_s = std::atoi(argv[++i]);
        }
    }
    if (duration_s <= 0) duration_s = 20;

    std::printf(
        "[NOTICE] Research/backtesting connectivity check only. NOT trading authorization.\n");
    std::printf("Connecting to Binance public WS + REST for symbol=%s, duration=%ds...\n\n",
                symbol.c_str(), duration_s);

    constexpr std::size_t kRingSize = 4096;

    hy::BinanceJsonParser parser;
    if (!parser.register_symbol(symbol, 0)) {
        std::fprintf(stderr, "FATAL: could not register symbol %s\n", symbol.c_str());
        return 1;
    }

    auto ring = std::make_unique<hy::SpscRing<hy::BinanceMarketEvent, kRingSize>>();

    hy::WsSessionConfig ws_cfg;
    ws_cfg.subscribe_streams = {to_lower(symbol) + "@trade"};

    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);
    hy::configure_binance_ssl_context(ssl_ctx);

    auto session = std::make_shared<hy::BinanceWsSession<kRingSize>>(
        ioc, ssl_ctx, *ring, parser, ws_cfg);
    session->start();

    std::thread io_thread([&ioc] { ioc.run(); });
    std::this_thread::sleep_for(std::chrono::seconds(duration_s));

    // Shutdown -- deliberately NOT calling ioc.stop(); see binance_ws_session.hpp's
    // shutdown-contract header comment (stop() + join(), never ioc.stop()).
    session->stop();
    if (io_thread.joinable()) io_thread.join();
    auto ws_stats = session->stats_snapshot();

    std::printf("Fetching REST depth snapshot for %s...\n", symbol.c_str());
    hy::FetchError rest_error{};
    // Production market data, named explicitly (there is no default environment any more).
    auto snapshot = hy::fetch_depth_snapshot(hy::EnvironmentBinding::production(), symbol, 100'000'000,
                                             100'000'000, {}, rest_error);

    bool ws_ok = ws_stats.messages_received > 0;
    bool rest_ok = snapshot.has_value();
    bool ws_tls_error = ws_stats.last_error_stage == "ssl_sni" ||
                         ws_stats.last_error_stage == "ssl_handshake";
    bool rest_tls_error = rest_error == hy::FetchError::TlsHandshake;

    int exit_code = (ws_ok ? 0 : 1) | (rest_ok ? 0 : 2);

    std::printf("\n");
    std::printf(
        "SMOKE_RESULT symbol=%s ws_messages=%llu ws_errors=%llu ws_tls_error=%d rest_ok=%d "
        "rest_bids=%zu rest_asks=%zu rest_tls_error=%d exit_code=%d\n",
        symbol.c_str(), static_cast<unsigned long long>(ws_stats.messages_received),
        static_cast<unsigned long long>(ws_stats.errors), ws_tls_error ? 1 : 0, rest_ok ? 1 : 0,
        rest_ok ? snapshot->bid_count : std::size_t{0},
        rest_ok ? snapshot->ask_count : std::size_t{0}, rest_tls_error ? 1 : 0, exit_code);

    return exit_code;
}
