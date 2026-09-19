// SPDX-License-Identifier: proprietary
// live_bar_feed_signal_demo.cpp — 批次 6 6a-2: Binance public kline stream -> StreamingEvaluator
// -> HoldingStateTracker -> log only.
//
// Governance: L2/L4, same as binance_feed_demo.cpp -- public WS only, no credentials, no
// SymbolRegistry. This process deliberately never constructs an OrchestratorContext,
// OrderConfirmation, or any submit port -- there is NO order-related code anywhere in this file.
// That is this milestone's entire safety property (批次 6 计划's own words): prove the
// K-line-to-evaluator-to-suggested-action chain against real (non-synthetic) Binance market data
// with zero possible network write side effect, before 6b-1/6b-2 ever touch a real gate chain.
//
// Fail-closed market-binding check (external-review-verified gap, adopted into this batch's
// plan): a loaded spec's [market] section is checked against the ACTUAL symbol/timeframe this
// process subscribes to via market_binding_matches() (strategy_spec_toml_parser.hpp, Batch 5)
// before the WS session is ever started -- feeding a 1m real-time stream to a spec written for
// 1h bars (or vice versa) would silently corrupt every windowed indicator's warm-up assumption,
// so a mismatch here is fatal at startup, not a warning.
//
// Usage: ./live_bar_feed_signal_demo <spec_toml_path> [symbol=btcusdt] [interval=1h]
//   Runs until Ctrl+C. symbol must be lowercase (Binance stream-name convention); interval uses
//   Binance's own kline interval strings ("1m","1h","1d",...) -- see
//   binance_kline_ws_session.hpp's detail::is_valid_kline_interval() for the full set.

#include <hengyuan/binance_kline_ws_session.hpp>
#include <hengyuan/binance_tls.hpp>
#include <hengyuan/feed_validity_gate.hpp>
#include <hengyuan/holding_state_tracker.hpp>
#include <hengyuan/strategy_spec_evaluator.hpp>
#include <hengyuan/strategy_spec_toml_parser.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

static std::atomic<bool> g_stop{false};
static void signal_handler(int) { g_stop.store(true); }

namespace {

std::string to_upper_copy(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

const char* action_name(hy::SuggestedAction a) {
    switch (a) {
        case hy::SuggestedAction::Open: return "OPEN(BUY)";
        case hy::SuggestedAction::Close: return "CLOSE(SELL)";
        case hy::SuggestedAction::None: return "-";
    }
    return "-";
}

const char* spec_status_name(hy::SpecLoadStatus s) {
    switch (s) {
        case hy::SpecLoadStatus::Ok: return "Ok";
        case hy::SpecLoadStatus::TomlParseError: return "TomlParseError";
        case hy::SpecLoadStatus::UnknownOperator: return "UnknownOperator";
        case hy::SpecLoadStatus::ForwardReferenceViolation: return "ForwardReferenceViolation";
        case hy::SpecLoadStatus::DuplicateNodeId: return "DuplicateNodeId";
        case hy::SpecLoadStatus::MissingRequiredField: return "MissingRequiredField";
        case hy::SpecLoadStatus::InvalidOperatorParameter: return "InvalidOperatorParameter";
        case hy::SpecLoadStatus::TooManyIndicatorNodes: return "TooManyIndicatorNodes";
        case hy::SpecLoadStatus::UnsupportedSpecVersion: return "UnsupportedSpecVersion";
        case hy::SpecLoadStatus::MissingMarketBinding: return "MissingMarketBinding";
        case hy::SpecLoadStatus::MissingValidationSection: return "MissingValidationSection";
        case hy::SpecLoadStatus::IncompleteValidationSection: return "IncompleteValidationSection";
    }
    return "Unknown";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr,
                      "usage: %s <spec_toml_path> [symbol=btcusdt] [interval=1h]\n", argv[0]);
        return 1;
    }
    const std::string spec_path = argv[1];
    const std::string symbol_lower = argc >= 3 ? argv[2] : "btcusdt";
    const std::string interval = argc >= 4 ? argv[3] : "1h";

    // --- Load + validate the spec (cold path, once) ---
    std::string toml_text;
    {
        std::ifstream f(spec_path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "FATAL: could not open spec file: %s\n", spec_path.c_str());
            return 1;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        toml_text = ss.str();
    }

    const hy::SpecLoadResult load_result = hy::load_strategy_spec(toml_text);
    if (!load_result.ok()) {
        std::fprintf(stderr, "FATAL: spec load failed: %s (%s)\n",
                     spec_status_name(load_result.status), spec_path.c_str());
        return 1;
    }

    // Fail-closed market-binding check -- see this file's own header comment for why this is
    // fatal, not a warning.
    const std::string symbol_upper = to_upper_copy(symbol_lower);
    if (!hy::market_binding_matches(load_result.market, hy::ManualMarket::CryptoSpot,
                                     symbol_upper, interval)) {
        std::fprintf(stderr,
                      "FATAL: spec [market] binding (symbol=%s, timeframe=%s) does not match "
                      "the live feed being attached (symbol=%s, interval=%s) -- refusing to "
                      "start rather than silently feed a mismatched bar cadence into the "
                      "evaluator's windowed indicators.\n",
                      load_result.market.symbol, load_result.market.timeframe,
                      symbol_upper.c_str(), interval.c_str());
        return 1;
    }

    hy::StreamingEvaluator evaluator;
    if (!evaluator.init(load_result.dag)) {
        std::fprintf(stderr,
                      "FATAL: StreamingEvaluator::init() failed -- spec's aggregate "
                      "window/lag capacity exceeds kHistoryPoolCapacity.\n");
        return 1;
    }

    hy::HoldingStateTracker tracker;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // --- WS session (6a-1) ---
    hy::KlineWsSessionConfig ws_config;
    ws_config.symbol = symbol_lower;
    ws_config.interval = interval;
    ws_config.symbol_id = 0;  // single-symbol demo -- no SymbolRegistry needed

    hy::KlineWsEventRing ring;
    hy::KlineJsonParser parser;

    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);
    hy::configure_binance_ssl_context(ssl_ctx);

    auto session =
        std::make_shared<hy::BinanceKlineWsSession>(ioc, ssl_ctx, ring, parser, ws_config);
    session->start();

    std::thread io_thread([&ioc] { ioc.run(); });

    std::printf("Live Bar Feed + Signal Demo -- %s@kline_%s (Ctrl+C to stop)\n",
                symbol_lower.c_str(), interval.c_str());
    std::printf("Spec: %s (effective_warmup=%" PRIu32 " bars)\n", spec_path.c_str(),
                evaluator.effective_warmup());
    std::printf("This process NEVER constructs an order or submit port -- log-only milestone.\n\n");

    std::uint64_t bars_seen = 0;
    int exit_code = 0;
    // Consumer-side continuity check (外部复核 P0-04): independent of the session's own guard,
    // validated against the timestamps of bars that ACTUALLY reached this consumer.
    hy::KlineBarGapGuard consumer_guard;
    const auto run_start = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        hy::KlineWsEvent ev{};
        while (ring.try_pop(ev)) {
            if (!consumer_guard.accept(ev)) {
                // A bar is missing between what the session delivered and what came before it.
                // Never feed a gapped sequence into the windowed indicators; drop evaluator
                // state so nothing stale can be read afterwards. Recovery (REST backfill) is
                // 6b-0f -- until then this demo stops and must be restarted.
                evaluator.reset();
                break;
            }
            const hy::Bar bar{ev.open, ev.high, ev.low, ev.close, ev.volume};
            const double target_position = evaluator.step(bar);
            const auto action = tracker.on_target_position(target_position);
            ++bars_seen;

            std::printf(
                "[%" PRId64 "] O=%.8f H=%.8f L=%.8f C=%.8f V=%.8f  signal=%.4f  state=%s",
                ev.close_time_ms, bar.open, bar.high, bar.low, bar.close, bar.volume,
                target_position, tracker.state() == hy::HoldingState::Long ? "LONG" : "FLAT");
            if (action != hy::SuggestedAction::None) {
                std::printf("  SUGGESTED_ACTION=%s", action_name(action));
            }
            std::printf("\n");
        }

        // Feed validity is evaluated AFTER draining what the ring already holds (those bars are
        // contiguous and still valid). This demo has no depth stream, so depth_tracking is passed
        // as true and depth_session_stopped as false -- "not applicable", not a claim about depth.
        hy::FeedHealthInputs health;
        health.kline_session_stopped = session->stopped();
        health.kline_session_suspended = session->is_suspended();
        health.consumer_continuity_broken = consumer_guard.is_suspended();
        health.depth_tracking = true;
        const hy::FeedInvalidReason invalid = hy::evaluate_feed_validity(health);
        if (hy::feed_invalid_reason_is_terminal(invalid)) {
            std::fprintf(stderr,
                          "ALERT: feed invalid (%s) -- no further bars will be trusted. This "
                          "process stops here; restart to recover (in-process backfill is 6b-0f).\n",
                          hy::feed_invalid_reason_name(invalid));
            exit_code = 2;
            break;
        }

        const auto elapsed = std::chrono::steady_clock::now() - run_start;
        if (elapsed >= std::chrono::seconds(hy::kDefaultMaxRunSeconds)) {
            std::printf("Reached the default max run time (%d h) -- stopping cleanly before "
                        "Binance's 24h connection limit.\n",
                        hy::kDefaultMaxRunSeconds / 3600);
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Shutdown -- same "do not call ioc.stop() directly" discipline as binance_feed_demo.cpp's
    // own comment: session->stop() posts cancellation onto its own strand, and ioc.stop() could
    // make ioc.run() return before that posted work executes.
    session->stop();
    if (io_thread.joinable()) io_thread.join();

    const auto stats = session->stats_snapshot();
    std::printf("\n=== Session Stats ===\n");
    std::printf("WS messages: %" PRIu64 "  bytes: %" PRIu64 "  errors: %" PRIu64 "\n",
                stats.messages_received, stats.bytes_received, stats.errors);
    std::printf("Parse OK (closed bars): %" PRIu64 "  unclosed skipped: %" PRIu64
                "  parse failed: %" PRIu64 "\n",
                stats.parse_ok, stats.unclosed_skipped, stats.parse_failed);
    std::printf("Ring push OK: %" PRIu64 "  dropped: %" PRIu64 "  gap-detected: %" PRIu64
                "  ring-overflow-suspended: %" PRIu64 "\n",
                stats.push_ok, stats.push_dropped, stats.gap_detected_count,
                stats.ring_overflow_suspended);
    std::printf("Bars evaluated: %" PRIu64 "  final holding state: %s\n", bars_seen,
                tracker.state() == hy::HoldingState::Long ? "LONG" : "FLAT");

    return exit_code;
}
