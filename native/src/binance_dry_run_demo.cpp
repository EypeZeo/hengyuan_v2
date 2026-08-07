// SPDX-License-Identifier: proprietary
// binance_dry_run_demo.cpp — Dry-run simulation against real Binance public WS feed.
//
// Real market data → simdjson → SPSC → HotThread → OrderBook (real TOB).
// SimExecutor receives periodic test intents, simulates fills against real book.
// Tracks paper position, PnL, fees, kill switch state.
//
// SIMULATION ONLY. No Binance Private API, no HMAC, no real order, no money.
// Governance: L4 (real public WS). SimExecutor component is L2.
//
// Usage: ./binance_dry_run_demo [duration_s] [options]
//   --interval N    seconds between simulated trades (default: 10)
//   --qty Q         quantity in lots per trade (default: 100000 = 0.001 BTC)
//   --max-pos P     max position lots (default: 1000000 = 0.01 BTC)
//   --drawdown D    max drawdown ticks*lots to arm kill switch (default: 0=off)
//   --record FILE   write all events to .hyf binary file

#include <hengyuan/binance_json_parser.hpp>
#include <hengyuan/binance_rest_snapshot.hpp>
#include <hengyuan/binance_tls.hpp>
#include <hengyuan/binance_ws_session.hpp>
#include <hengyuan/depth_manager.hpp>
#include <hengyuan/event_recorder.hpp>
#include <hengyuan/hot_thread.hpp>
#include <hengyuan/preflight_gate.hpp>
#include <hengyuan/sim_executor.hpp>
#include <hengyuan/snapshot_refresh_gate.hpp>
#include <hengyuan/trade_logger.hpp>

#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#ifdef __linux__
#include <unistd.h>
#endif

static std::atomic<bool> g_stop{false};
static void signal_handler(int) { g_stop.store(true); }

// Fixed-point conversion: multiplier = 1e8 for both price and qty.
static constexpr double kPriceMult = 1e8;
static constexpr double kQtyMult = 1e8;
static constexpr double kPnlMult = 1e16;  // price_mult * qty_mult

static double ticks_to_usd(std::int64_t ticks) { return static_cast<double>(ticks) / kPriceMult; }
static double lots_to_qty(std::int64_t lots) { return static_cast<double>(lots) / kQtyMult; }
static double pnl_to_usd(std::int64_t pnl) { return static_cast<double>(pnl) / kPnlMult; }

static const char* side_str(hy::OrderSide s) {
    return s == hy::OrderSide::Buy ? "BUY" : "SELL";
}

static const char* fill_str(hy::FillStatus s) {
    switch (s) {
        case hy::FillStatus::Filled:     return "FILLED";
        case hy::FillStatus::Rejected:   return "REJECTED";
        case hy::FillStatus::NoLiquidity: return "NO_LIQ";
        case hy::FillStatus::NoFill:     return "NO_FILL";
    }
    return "?";
}

static const char* kill_str(hy::KillState s) {
    switch (s) {
        case hy::KillState::Normal:    return "NORMAL";
        case hy::KillState::Armed:     return "ARMED";
        case hy::KillState::Triggered: return "TRIGGERED";
        case hy::KillState::Latched:   return "LATCHED";
    }
    return "?";
}

int main(int argc, char* argv[]) {
    int duration_s = 60;
    int interval_s = 10;
    std::int64_t qty_lots = 100'000;   // 0.001 BTC at 1e8 multiplier
    std::int64_t max_pos = 1'000'000;  // 0.01 BTC
    std::int64_t max_drawdown = 0;
    std::string record_path;
    std::string trade_log_path;
    std::string heartbeat_shm;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--interval" && i + 1 < argc) {
            interval_s = std::atoi(argv[++i]);
        } else if (arg == "--qty" && i + 1 < argc) {
            qty_lots = std::atoll(argv[++i]);
        } else if (arg == "--max-pos" && i + 1 < argc) {
            max_pos = std::atoll(argv[++i]);
        } else if (arg == "--drawdown" && i + 1 < argc) {
            max_drawdown = std::atoll(argv[++i]);
        } else if (arg == "--record" && i + 1 < argc) {
            record_path = argv[++i];
        } else if (arg == "--trade-log" && i + 1 < argc) {
            trade_log_path = argv[++i];
        } else if (arg == "--heartbeat" && i + 1 < argc) {
            heartbeat_shm = argv[++i];
        } else if (arg.size() > 0 && arg[0] != '-') {
            duration_s = std::atoi(argv[i]);
        }
    }
    if (duration_s <= 0) duration_s = 60;
    if (interval_s <= 0) interval_s = 10;
    if (qty_lots <= 0) qty_lots = 100'000;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    constexpr std::size_t kRingSize = 65536;

    // Parser with symbols
    hy::BinanceJsonParser parser;
    // register_symbol() is [[nodiscard]] (audit API-SYM-020): a rejected registration
    // means every event for that symbol would be silently dropped as UnknownSymbol,
    // which is exactly the kind of quiet degradation worth a hard stop at startup.
    {
        const struct { const char* name; std::uint32_t id; } kSymbols[] = {
            {"BTCUSDT", 0}, {"ETHUSDT", 1}, {"SOLUSDT", 2}, {"DOGEUSDT", 3}, {"ADAUSDT", 4},
        };
        for (const auto& s : kSymbols) {
            if (!parser.register_symbol(s.name, s.id)) {
                std::fprintf(stderr, "FATAL: could not register symbol %s (id %u)\n", s.name,
                             s.id);
                return 1;
            }
        }
    }

    // SPSC ring
    auto ring = std::make_unique<hy::SpscRing<hy::BinanceMarketEvent, kRingSize>>();

    // Hot thread (OrderBook consumer)
    hy::HotThread<kRingSize> hot(*ring);

    // DepthManager for snapshot-based sync
    hy::DepthManager depth_mgr;
    hot.set_depth_manager(&depth_mgr);

    // SHM heartbeat (Linux only — watchdog_daemon reads this)
#ifdef __linux__
    hy::ShmSegment shm_seg;
    hy::ShmHeartbeatWriter hb_writer(nullptr);
    if (!heartbeat_shm.empty()) {
        auto* blk = shm_seg.open(heartbeat_shm.c_str(), true);
        if (blk) {
            hb_writer = hy::ShmHeartbeatWriter(blk);
            hb_writer.init(static_cast<std::uint64_t>(getpid()));
            hot.set_heartbeat(&hb_writer);
            std::printf("Heartbeat SHM: %s (PID=%d)\n", heartbeat_shm.c_str(), getpid());
        } else {
            std::printf("WARNING: failed to create SHM %s\n", heartbeat_shm.c_str());
        }
    }
#endif

    // SimExecutor
    hy::SimConfig sim_cfg{};
    sim_cfg.limits.max_position_lots = max_pos;
    sim_cfg.max_drawdown = max_drawdown;
    hy::SimExecutor<> sim(sim_cfg);

    // Trade logger
    hy::TradeLogger trade_log;
    if (!trade_log_path.empty()) {
        if (trade_log.open(trade_log_path)) {
            std::printf("Trade log: %s\n", trade_log_path.c_str());
        } else {
            std::printf("WARNING: failed to open trade log: %s\n", trade_log_path.c_str());
        }
    }

    static const char* sym_names[] = {"BTC", "ETH", "SOL", "DOGE", "ADA"};
    // TOB callback — only print when price actually changes (dedup)
    static std::int64_t last_bid[5]{}, last_ask[5]{};
    hot.set_on_top_of_book([](std::uint32_t sym, std::int64_t bid, std::int64_t ask) {
        if (sym < 5 && bid == last_bid[sym] && ask == last_ask[sym]) return;
        if (sym < 5) { last_bid[sym] = bid; last_ask[sym] = ask; }
        const char* name = sym < 5 ? sym_names[sym] : "???";
        std::printf("  TOB[%s]: bid=$%.2f  ask=$%.2f  spread=$%.4f\n",
                    name, ticks_to_usd(bid), ticks_to_usd(ask),
                    ticks_to_usd(ask - bid));
    });

    // Optional recorder
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

    // WS session
    hy::WsSessionConfig ws_cfg;
    ws_cfg.subscribe_streams = {
        "btcusdt@trade", "ethusdt@trade", "solusdt@trade",
        "dogeusdt@trade", "adausdt@trade",
        "btcusdt@depth@100ms",
    };

    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);
    hy::configure_binance_ssl_context(ssl_ctx);

    auto session = std::make_shared<hy::BinanceWsSession<kRingSize>>(
        ioc, ssl_ctx, *ring, parser, ws_cfg);
    session->start();

    std::thread io_thread([&ioc]() { ioc.run(); });

    std::printf("=== Binance Dry-Run Demo ===\n");
    std::printf("Duration: %ds  Interval: %ds  Qty: %.6f BTC  MaxPos: %.4f BTC  Fee: 750ppm (0.075%%)\n",
                duration_s, interval_s, lots_to_qty(qty_lots), lots_to_qty(max_pos));
    std::printf("SIMULATION ONLY — no real orders, no real money.\n\n");

    // Depth snapshot acquisition -- both the initial snapshot and every later resync go
    // through SnapshotRefreshGate, run on a background worker thread. There is no special-cased
    // synchronous bootstrap path: WS session I/O has already started (session->start() above)
    // and is producing into the SPSC ring, so a synchronous fetch here would block hot.run_once()
    // from draining it for the fetch's full timeout budget -- exactly the same problem a
    // synchronous fetch mid-loop would cause during a resync. depth_mgr starts in Buffering
    // state, so the first loop iteration's needs_snapshot() is naturally true and drives the
    // gate to fetch the initial snapshot the same way a later resync does.
    hy::SnapshotRefreshGate depth_gate;

    // D3-LIVE preflight check (informational — this is dry-run, not live). Runs before the book
    // is synced (depth_mgr starts in Buffering), which honestly reflects "not ready yet at
    // startup" rather than the old fixed 2s sleep's unguaranteed hope that sync had finished.
    {
#ifdef __linux__
        auto pf = hy::check_preflight(sim.kill_switch(), max_pos > 0,
                                       depth_mgr, shm_seg.is_open() ? &hb_writer : nullptr, false);
#else
        auto pf = hy::check_preflight(sim.kill_switch(), max_pos > 0,
                                       depth_mgr, nullptr, false);
#endif
        hy::print_preflight(pf);
        std::printf("\n");
    }

    auto start = std::chrono::steady_clock::now();
    auto last_trade = start;
    bool next_is_buy = true;
    int trade_num = 0;

    while (!g_stop.load()) {
        hot.run_once();

        // Non-blocking: depth_gate.poll() never blocks the hot loop, whether this is the
        // initial snapshot (depth_mgr starts in Buffering) or a later resync (gap detected or
        // an overflowed buffer forced a fresh Buffering episode, see depth_manager.hpp).
        if (auto snap = depth_gate.poll(depth_mgr.needs_snapshot(),
                                         {"BTCUSDT", 100'000'000, 100'000'000})) {
            bool ok = depth_mgr.apply_snapshot(*snap);
            std::printf("Snapshot: lastUpdateId=%" PRIu64 "  bids=%zu  asks=%zu  sync=%s\n",
                        snap->last_update_id, snap->bid_count, snap->ask_count,
                        ok ? "OK" : "RESYNC_NEEDED");
            if (!ok) {
                std::printf("  Gap or buffer overflow detected; will re-snapshot after cooldown.\n");
            }
            depth_gate.notify_apply_result(ok);
        }

        auto now = std::chrono::steady_clock::now();
        auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        if (total_elapsed >= duration_s) break;

        auto since_trade = std::chrono::duration_cast<std::chrono::seconds>(now - last_trade).count();
        if (since_trade >= interval_s) {
            auto tob = hot.book().top_of_book();
            if (tob) {
                ++trade_num;
                hy::ExecutionIntent intent{};
                intent.symbol_id = 0;
                intent.side = next_is_buy ? hy::OrderSide::Buy : hy::OrderSide::Sell;
                intent.type = hy::OrderType::Market;
                intent.qty_lots = qty_lots;

                auto fill = sim.execute(intent, tob->first, tob->second);

                std::printf("\n[#%d t=%ds] %s %.6f BTC @ $%.2f\n",
                            trade_num, static_cast<int>(total_elapsed),
                            side_str(intent.side), lots_to_qty(qty_lots),
                            ticks_to_usd(fill.fill_price_ticks));
                std::printf("  %s  fee=$%.6f  book: bid=$%.2f ask=$%.2f\n",
                            fill_str(fill.status),
                            pnl_to_usd(fill.fee_ticks),
                            ticks_to_usd(tob->first), ticks_to_usd(tob->second));

                const auto& pos = sim.position(0);
                std::printf("  Position: %.6f BTC  avg=$%.2f  realized=$%.6f\n",
                            lots_to_qty(pos.net_qty_lots),
                            ticks_to_usd(pos.avg_entry_price_ticks),
                            pnl_to_usd(pos.realized_pnl));
                std::printf("  KillSwitch: %s  overflow_guard: %" PRIu64 "\n",
                            kill_str(sim.kill_switch().state()),
                            sim.stats().overflow_guard);

                if (trade_log.is_open() && fill.status == hy::FillStatus::Filled) {
                    std::int64_t mark = (tob->first + tob->second) / 2;
                    std::int64_t unreal = 0;
                    if (pos.net_qty_lots != 0 && mark > 0) {
                        unreal = (mark - pos.avg_entry_price_ticks) * pos.net_qty_lots;
                    }
                    auto now_ms = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()).count());
                    trade_log.log(trade_num, now_ms, 0, intent.side,
                                  fill, pos, unreal, sim.kill_switch().state());
                }

                next_is_buy = !next_is_buy;
            } else {
                std::printf("[t=%ds] Waiting for book...\n", static_cast<int>(total_elapsed));
            }
            last_trade = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Shutdown -- deliberately NOT calling ioc.stop() here; see binance_feed_demo.cpp's
    // equivalent comment / binance_ws_session.hpp's shutdown-contract header comment.
    session->stop();
    if (io_thread.joinable()) io_thread.join();
    hot.run_once();

    // Final summary -- safe to call now that io_thread has been joined.
    auto ws = session->stats_snapshot();
    const auto& hs = hot.stats();
    const auto& pc = parser.counters();
    const auto& ss = sim.stats();
    const auto& ds = depth_mgr.stats();

    std::printf("\n=== Session Stats ===\n");
    std::printf("Depth sync: snapshots=%" PRIu64 "  applied=%" PRIu64
                "  buffered=%" PRIu64 "  dropped=%" PRIu64
                "  resyncs=%" PRIu64 "  gaps=%" PRIu64 "\n",
                static_cast<std::uint64_t>(ds.snapshots),
                static_cast<std::uint64_t>(ds.events_applied),
                static_cast<std::uint64_t>(ds.events_buffered),
                static_cast<std::uint64_t>(ds.events_dropped),
                static_cast<std::uint64_t>(ds.resyncs),
                static_cast<std::uint64_t>(ds.gap_events));
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
                    ticks_to_usd(tob->first), ticks_to_usd(tob->second));
    }

    std::printf("\n=== Dry-Run Summary ===\n");
    std::printf("Intents: %" PRIu64 "  Filled: %" PRIu64 "  Rejected: %" PRIu64
                "  NoLiquidity: %" PRIu64 "  OverflowGuard: %" PRIu64 "\n",
                static_cast<std::uint64_t>(ss.intents),
                static_cast<std::uint64_t>(ss.filled),
                static_cast<std::uint64_t>(ss.rejected),
                static_cast<std::uint64_t>(ss.no_liquidity),
                static_cast<std::uint64_t>(ss.overflow_guard));

    const auto& final_pos = sim.position(0);
    std::printf("BTCUSDT: %.6f BTC  avg=$%.2f  realized=$%.6f\n",
                lots_to_qty(final_pos.net_qty_lots),
                ticks_to_usd(final_pos.avg_entry_price_ticks),
                pnl_to_usd(final_pos.realized_pnl));
    std::printf("Total PnL: $%.6f  KillSwitch: %s\n",
                pnl_to_usd(sim.total_realized_pnl()),
                kill_str(sim.kill_switch().state()));

    if (recorder->is_open()) {
        std::printf("Recorded: %" PRIu64 " events to %s\n",
                    static_cast<std::uint64_t>(recorder->records_written()),
                    record_path.c_str());
        recorder->close();
    }

    if (trade_log.is_open()) {
        std::printf("Trade log: %" PRIu64 " rows to %s\n",
                    trade_log.rows_written(), trade_log_path.c_str());
        trade_log.close();
    }

    return 0;
}
