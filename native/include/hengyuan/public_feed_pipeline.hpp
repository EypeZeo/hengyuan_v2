// SPDX-License-Identifier: proprietary
// public_feed_pipeline.hpp — 批次 6 6b-0f-7: BOTH public feeds (kline + depth), supervised, in one
// place. Every piece was built and tested in isolation (PublicFeedSupervisor 6b-0f-2, SingleFlightFetchGate
// 3a, KlineFeedSync 3b, KlineFeedDriver 3c, the real-session loopback tests 3d, FeedGenerationTracker 4,
// the supervised validity vocabulary 5, DepthFeedDriver 6). What nothing tested yet was the WIRING: which
// factory builds which session against which ring and parser, which callback the kline supervisor asks
// before a planned rollover, that the validity inputs are read from the right objects. That wiring used to
// live inline in a harness main(), which cannot be unit-tested -- this class is that wiring, so it can be
// exercised against real sessions (test_public_feed_pipeline.cpp) and the harness shrinks to a caller.
//
// WHAT IT DOES per tick(): polls the kline supervisor, ticks the kline driver (session watch -> backfill
// recovery -> drain live bars into the evaluator), ticks the depth driver (which polls its own supervisor:
// reset on a new generation -> validate + drain depth events -> snapshot), and reports the resulting
// SupervisedFeedInputs and its verdict (evaluate_supervised_feed_validity). "Feed valid" therefore means
// what feed_validity_gate.hpp says it means, evaluated over the real objects.
//
// WHAT IT DELIBERATELY DOES NOT OWN:
//   * the io_context, the TLS context and the thread that runs the io_context (the harness also runs the
//     private user-data feed on the same loop). The io_context MUST be kept alive by a work guard: the
//     sessions are created LATER, by the supervisors' poll(), and run() returns at once when nothing is
//     pending (ws_client_rig.hpp / WsLoopbackWorkGuard.* show the failure).
//   * the StreamingEvaluator and the loaded SpecDag (the caller loaded the spec and needs the evaluator).
//   * how a klines backfill or a depth snapshot is FETCHED. Both are injected: they run on the gates'
//     worker threads, so they must not throw and may read only thread-safe state (a ClockOffsetPublisher
//     snapshot, an immutable config copy). The production fetchers live with the harness; tests script them.
//   * the clock the kline rollover window is judged against (epoch ms, hot thread only).
//
// LIFETIME CONTRACT -- read before destroying one. The sessions hold references into this object (the
// rings and parsers) and outlive a supervisor's shutdown() (their own pending handlers keep them alive
// until cancelled). So the order is fixed: shutdown(); stop the io_context and join its thread; THEN destroy
// the pipeline. The destructor calls shutdown() too, but it cannot wait for an I/O thread it does not own.
//
// Big objects (the depth ring, DepthManager, both gates' mailboxes) are heap-allocated inside, so the
// object itself is small; it is still created through create() and never moved (the supervisors' factories
// capture `this`).

#pragma once

#include <hengyuan/binance_json_parser.hpp>
#include <hengyuan/binance_kline_ws_session.hpp>
#include <hengyuan/binance_klines_rest.hpp>
#include <hengyuan/binance_ws_session.hpp>
#include <hengyuan/depth_feed_driver.hpp>
#include <hengyuan/depth_manager.hpp>
#include <hengyuan/feed_validity_gate.hpp>
#include <hengyuan/input_validator.hpp>
#include <hengyuan/kline_bar.hpp>
#include <hengyuan/kline_feed_driver.hpp>
#include <hengyuan/kline_feed_sync.hpp>
#include <hengyuan/public_feed_policy.hpp>
#include <hengyuan/public_feed_supervisor.hpp>
#include <hengyuan/single_flight_fetch_gate.hpp>
#include <hengyuan/snapshot_refresh_gate.hpp>
#include <hengyuan/strategy_spec_evaluator.hpp>
#include <hengyuan/strategy_spec_types.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace hy {

// Distinct seeds so the two feeds do not walk the identical backoff sequence. Production should override
// both with real entropy (FeedSupervisorPolicy::jitter_seed's own comment).
inline constexpr std::uint64_t kKlineFeedJitterSeed = 0x4B4C494E45'FEED01ULL;
inline constexpr std::uint64_t kDepthFeedJitterSeed = 0x4445505448'FEED02ULL;

struct PublicFeedPipelineConfig {
    std::string ws_host;  // EnvironmentBinding::ws_host()
    std::string ws_port;  // EnvironmentBinding::ws_port()
    std::string symbol;   // UPPERCASE ("BTCUSDT"): the REST and DepthManager spelling. The WS stream names
                          // are derived from it, lowercased.
    std::string interval; // Binance's own spelling ("1h"): the kline stream and the REST backfill both use it
    std::uint32_t symbol_id{0};
    std::int64_t price_multiplier{0};  // 10^price_scale from the symbol's rules: BOTH the depth parser and the
    std::int64_t qty_multiplier{0};    // REST snapshot must agree with what validate_pre_trade() checks against
    FeedSupervisorPolicy kline_policy = make_public_feed_policy(kKlineFeedJitterSeed);
    FeedSupervisorPolicy depth_policy = make_public_feed_policy(kDepthFeedJitterSeed);
    KlineFeedDriverConfig kline_driver{};
    DepthFeedDriverConfig depth_driver{};
};

enum class PipelineInitError : std::uint8_t {
    None = 0,
    EmptyHostOrPort = 1,
    InvalidSymbol = 2,
    InvalidInterval = 3,
    InvalidMultiplier = 4,
    MissingFetcherOrClock = 5,
    SymbolRegistrationFailed = 6,
};

inline constexpr const char* pipeline_init_error_name(PipelineInitError e) noexcept {
    switch (e) {
        case PipelineInitError::None: return "None";
        case PipelineInitError::EmptyHostOrPort: return "EmptyHostOrPort";
        case PipelineInitError::InvalidSymbol: return "InvalidSymbol";
        case PipelineInitError::InvalidInterval: return "InvalidInterval";
        case PipelineInitError::InvalidMultiplier: return "InvalidMultiplier";
        case PipelineInitError::MissingFetcherOrClock: return "MissingFetcherOrClock";
        case PipelineInitError::SymbolRegistrationFailed: return "SymbolRegistrationFailed";
    }
    return "?";
}

// Pure: everything checkable without touching a socket. The sessions validate their own host/port when they
// start (a failure there is a failed attempt the supervisor backs off from, never a crash).
inline PipelineInitError validate_pipeline_config(const PublicFeedPipelineConfig& c) noexcept {
    if (c.ws_host.empty() || c.ws_port.empty()) return PipelineInitError::EmptyHostOrPort;
    if (c.symbol.empty() || c.symbol.size() > 20) return PipelineInitError::InvalidSymbol;
    for (const char ch : c.symbol) {
        const bool upper = ch >= 'A' && ch <= 'Z';
        const bool digit = ch >= '0' && ch <= '9';
        if (!upper && !digit) return PipelineInitError::InvalidSymbol;
    }
    if (!is_valid_kline_interval(c.interval)) return PipelineInitError::InvalidInterval;
    if (c.price_multiplier <= 0 || c.qty_multiplier <= 0) return PipelineInitError::InvalidMultiplier;
    return PipelineInitError::None;
}

struct PublicFeedTickReport {
    KlineFeedTickReport kline;
    DepthFeedTickReport depth;
    SupervisedFeedInputs inputs;
    // Fail-closed default: a report that was never filled in must not read as a valid feed.
    SupervisedFeedInvalidReason validity{SupervisedFeedInvalidReason::KlineDisconnected};

    bool feed_valid() const noexcept { return validity == SupervisedFeedInvalidReason::None; }
    // A supervisor has given up: no reconnect is coming, the caller should end the run.
    bool terminal() const noexcept { return supervised_feed_invalid_reason_is_terminal(validity); }
};

template <std::size_t DepthRingCapacity = 65536>
class PublicFeedPipeline {
public:
    using KlineSession = BinanceKlineWsSession;
    using DepthSession = BinanceWsSession<DepthRingCapacity>;
    using KlineGate = SingleFlightFetchGate<KlinesBackfillRequest, KlinesBackfillOutcome>;
    using KlinesFetcher = typename KlineGate::Fetcher;  // runs on the gate's worker thread; must not throw
    using EpochClock = std::function<std::int64_t()>;   // epoch ms; called on the hot thread only
    using DepthRing = SpscRing<BinanceMarketEvent, DepthRingCapacity>;

    // Null (and `error` set) if the config is unusable or a dependency is missing; nothing is started
    // either way -- the sessions only start on the first tick(), through their supervisors.
    static std::unique_ptr<PublicFeedPipeline> create(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl_ctx,
                                                      StreamingEvaluator& evaluator, const SpecDag& dag,
                                                      PublicFeedPipelineConfig config, KlinesFetcher klines_fetcher,
                                                      SnapshotFetcher snapshot_fetcher, EpochClock epoch_now_ms,
                                                      PipelineInitError& error) {
        error = validate_pipeline_config(config);
        if (error == PipelineInitError::None && (!klines_fetcher || !snapshot_fetcher || !epoch_now_ms)) {
            error = PipelineInitError::MissingFetcherOrClock;
        }
        if (error != PipelineInitError::None) return nullptr;

        std::unique_ptr<PublicFeedPipeline> pipeline(new PublicFeedPipeline(
            ioc, ssl_ctx, evaluator, dag, std::move(config), std::move(klines_fetcher), std::move(snapshot_fetcher),
            std::move(epoch_now_ms)));
        if (!pipeline->depth_parser_.register_symbol(pipeline->config_.symbol, pipeline->config_.symbol_id,
                                                     pipeline->config_.price_multiplier,
                                                     pipeline->config_.qty_multiplier)) {
            error = PipelineInitError::SymbolRegistrationFailed;
            return nullptr;
        }
        return pipeline;
    }

    // See the header comment's LIFETIME CONTRACT: shutdown(), stop + join the I/O thread, then destroy.
    ~PublicFeedPipeline() { shutdown(); }

    PublicFeedPipeline(const PublicFeedPipeline&) = delete;
    PublicFeedPipeline& operator=(const PublicFeedPipeline&) = delete;

    // Once per hot-loop iteration, with the caller's monotonic millisecond clock. `on_bar(const
    // KlineWsEvent&, double target_position)` is called for every LIVE closed bar that reached the evaluator,
    // in order (KlineFeedDriver::tick's contract; a backfill replay warms the evaluator without calling it).
    // It runs inside this call, so it may read the const accessors below (supervised_inputs() is the way to
    // ask whether the feed is trustworthy right now) but must not call tick() or shutdown().
    template <typename OnBar>
    PublicFeedTickReport tick(std::int64_t now_ms, OnBar&& on_bar) {
        PublicFeedTickReport report;
        // The kline driver watches the supervisor's current session but does not poll it (the caller
        // does); the depth driver polls its own supervisor as part of its tick.
        kline_sup_.poll(now_ms);
        report.kline = kline_driver_.tick(now_ms, on_bar);
        report.depth = depth_driver_.tick(now_ms);
        report.inputs = supervised_inputs();
        report.validity = evaluate_supervised_feed_validity(report.inputs);
        return report;
    }

    // What feed_validity_gate.hpp is asked, read straight off the real objects. Also what tick() reports.
    SupervisedFeedInputs supervised_inputs() const noexcept {
        SupervisedFeedInputs in;
        in.kline_feed_state = kline_sup_.state();
        in.depth_feed_state = depth_sup_.state();
        in.kline_sync_live = kline_sync_.live();
        in.depth_tracking = depth_mgr_->state() == DepthState::Tracking;
        return in;
    }

    // Stops both feeds' current sessions and makes both supervisors Terminal. Idempotent, does not block.
    void shutdown() noexcept {
        kline_sup_.shutdown();
        depth_sup_.shutdown();
    }

    // --- read-only views for the caller's logging and its order path ------------------------------------
    const KlineFeedSync& kline_sync() const noexcept { return kline_sync_; }
    // The book (top_of_book() for pricing) -- meaningful only while supervised_inputs().depth_tracking.
    const DepthManager& depth_manager() const noexcept { return *depth_mgr_; }
    FeedSupervisorStats kline_supervisor_stats() const noexcept { return kline_sup_.stats(); }
    FeedSupervisorStats depth_supervisor_stats() const noexcept { return depth_sup_.stats(); }
    const KlineFeedDriverStats& kline_driver_stats() const noexcept { return kline_driver_.stats(); }
    const DepthFeedDriverStats& depth_driver_stats() const noexcept { return depth_driver_.stats(); }
    // The live sessions, or null between generations (stats_snapshot() is the caller's, after the I/O
    // thread has been joined, exactly as for a bare session).
    std::shared_ptr<KlineSession> kline_session() const noexcept { return kline_sup_.current(); }
    std::shared_ptr<DepthSession> depth_session() const noexcept { return depth_sup_.current(); }
    const PublicFeedPipelineConfig& config() const noexcept { return config_; }

private:
    static std::string lowercase(std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    }

    static KlineWsSessionConfig make_kline_ws_config(const PublicFeedPipelineConfig& c) {
        KlineWsSessionConfig k;
        k.host = c.ws_host;
        k.port = c.ws_port;
        k.symbol = lowercase(c.symbol);
        k.interval = c.interval;
        k.symbol_id = c.symbol_id;
        return k;
    }

    static WsSessionConfig make_depth_ws_config(const PublicFeedPipelineConfig& c) {
        WsSessionConfig d;
        d.host = c.ws_host;
        d.port = c.ws_port;
        d.subscribe_streams = {lowercase(c.symbol) + "@depth@100ms"};
        return d;
    }

    // Private: create() validates first. Members are constructed in DECLARATION order, and each later one
    // may read the earlier ones (config_ above all).
    PublicFeedPipeline(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl_ctx, StreamingEvaluator& evaluator,
                       const SpecDag& dag, PublicFeedPipelineConfig config, KlinesFetcher klines_fetcher,
                       SnapshotFetcher snapshot_fetcher, EpochClock epoch_now_ms)
        : ioc_(ioc)
        , ssl_ctx_(ssl_ctx)
        , config_(std::move(config))
        , epoch_now_ms_(std::move(epoch_now_ms))
        , interval_span_ms_(kline_interval_span_ms(config_.interval))
        , kline_ws_cfg_(make_kline_ws_config(config_))
        , kline_sync_(evaluator, dag)
        , kline_gate_(std::make_unique<KlineGate>(
              std::move(klines_fetcher),
              [](const KlinesBackfillOutcome& outcome) { return klines_backfill_cooldown_ms(outcome); }))
        , kline_sup_(
              [this](std::uint64_t) -> std::shared_ptr<KlineSession> {
                  return std::make_shared<KlineSession>(ioc_, ssl_ctx_, kline_ring_, kline_parser_, kline_ws_cfg_);
              },
              config_.kline_policy, /*drain=*/{},
              // A planned rollover leaves the feed briefly without a session, so it waits for the moment
              // right after a bar has closed (public_feed_policy.hpp). The supervisor's own steady
              // timestamp is not what this asks about: it needs epoch time.
              [this](std::int64_t) {
                  return kline_rollover_window_open(kline_sync_.live(), kline_sync_.last_close_time_ms(),
                                                    epoch_now_ms_(), interval_span_ms_);
              })
        , kline_driver_(kline_sync_, *kline_gate_, kline_sup_, kline_ring_,
                        KlinesBackfillRequest{config_.symbol, config_.interval, 0, config_.symbol_id},
                        config_.kline_driver)
        , depth_ring_(std::make_unique<DepthRing>())
        , depth_mgr_(std::make_unique<DepthManager>())
        , snapshot_gate_(std::make_unique<SnapshotRefreshGate>(std::move(snapshot_fetcher)))
        , depth_ws_cfg_(make_depth_ws_config(config_))
        , depth_sup_(
              [this](std::uint64_t) -> std::shared_ptr<DepthSession> {
                  return std::make_shared<DepthSession>(ioc_, ssl_ctx_, *depth_ring_, depth_parser_, depth_ws_cfg_);
              },
              config_.depth_policy)
        , depth_driver_(depth_sup_, *depth_ring_, depth_validator_, *depth_mgr_, *snapshot_gate_,
                        SnapshotRequest{config_.symbol, config_.price_multiplier, config_.qty_multiplier},
                        config_.depth_driver) {}

    boost::asio::io_context& ioc_;
    boost::asio::ssl::context& ssl_ctx_;
    PublicFeedPipelineConfig config_;
    EpochClock epoch_now_ms_;
    std::int64_t interval_span_ms_;

    // --- kline feed. The ring and parser precede the sync/gate/supervisor/driver that refer to them.
    KlineWsEventRing kline_ring_;
    KlineJsonParser kline_parser_;
    KlineWsSessionConfig kline_ws_cfg_;
    KlineFeedSync kline_sync_;
    std::unique_ptr<KlineGate> kline_gate_;  // heap: its mailbox holds a ~64 KB KlineBackfill
    PublicFeedSupervisor<KlineSession> kline_sup_;
    KlineFeedDriver<KlineSession, KlinesBackfillOutcome> kline_driver_;

    // --- depth feed. depth_parser_ is registered in create(), before anything can start.
    BinanceJsonParser depth_parser_;
    std::unique_ptr<DepthRing> depth_ring_;  // heap: DepthRingCapacity events
    InputValidator depth_validator_;
    std::unique_ptr<DepthManager> depth_mgr_;  // heap: ~320 KB of buffered-event arrays
    std::unique_ptr<SnapshotRefreshGate> snapshot_gate_;  // heap: its mailbox holds a ~32 KB DepthSnapshot
    WsSessionConfig depth_ws_cfg_;
    PublicFeedSupervisor<DepthSession> depth_sup_;
    DepthFeedDriver<DepthSession, DepthRingCapacity> depth_driver_;
};

}  // namespace hy
