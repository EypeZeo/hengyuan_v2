// 批次 6 6b-0f-7: public_feed_pipeline.hpp against REAL sessions.
//
// The unit tests (test_kline_feed_driver, test_depth_feed_driver, ...) prove each piece against fake
// sessions. What they cannot prove is the WIRING -- which factory builds which session against which ring,
// what the kline supervisor asks before a planned rollover, that the validity inputs are read from the right
// objects -- nor how the real sessions behave under the drivers' protocols (a real gap suspends the real
// session; resume_after_gap() is a real strand post; a real drop is a real generation). So here both
// WebSocket feeds are real (BinanceKlineWsSession / BinanceWsSession over TLS to WsLoopbackServer) and only
// the two REST fetches are scripted: real-REST behaviour is already covered by test_binance_klines_rest.cpp.
//
// FakeExchange is the scripted side. It models "what the exchange has closed so far" (closed_bars) and
// answers a klines backfill with the most recent `limit` closed bars, exactly like the real endpoint -- so a
// backfill after a gap naturally contains the bar the live stream dropped. Either fetch can be HELD open by
// the test, which is what makes the intermediate validity states (KlineNotSynced, DepthNotTracking)
// observable rather than racy.
//
// The pipeline is a hot-thread object: the test thread IS the hot thread and drives tick() with a real
// steady clock (the supervisors' backoff and rollover ages are real time here).

#include <gtest/gtest.h>
#include <hengyuan/public_feed_pipeline.hpp>
#include <hengyuan/strategy_spec_toml_parser.hpp>

#include "test_helpers/ws_client_rig.hpp"
#include "test_helpers/ws_loopback_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using hy::test_helpers::fixture_path;
using hy::test_helpers::steady_ms;
using hy::test_helpers::wait_until;
using hy::test_helpers::WsClientRig;
using hy::test_helpers::WsLoopbackServer;

constexpr std::string_view kSpec = R"(
spec_version = 1
name = "sma_crossover_btc_1h"

[market]
market    = "crypto_spot"
symbol    = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "fast"
op = "sma"
input = "close"
window = 20

[[indicators]]
id = "slow"
op = "sma"
input = "close"
window = 50

[[indicators]]
id = "rsi14"
op = "rsi"
input = "close"
window = 14

[[indicators]]
id = "not_overbought"
op = "lt"
left = "rsi14"
right = 70.0

[[indicators]]
id = "trend_up"
op = "gt"
left = "fast"
right = "slow"

[[indicators]]
id = "entry"
op = "and"
left = "trend_up"
right = "not_overbought"

[signal]
node = "entry"
mode = "scaled"

[validation]
validated_at   = "2026-09-15T08:30:00Z"
validated_by   = "walk_forward+cpcv"
data_start_utc = "2021-01-01T00:00:00Z"
data_end_utc   = "2026-06-30T00:00:00Z"
data_digest    = "sha256:0000000000000000000000000000000000000000000000000000000000000000"
oos_sharpe     = 1.34
pbo            = 0.21
trials         = 480
)";

constexpr std::int64_t kHour = 3'600'000;
constexpr const char* kKlineTarget = "/ws/btcusdt@kline_1h";
constexpr const char* kDepthTarget = "/ws/btcusdt@depth@100ms";
constexpr std::size_t kDepthRing = 8192;  // heap-allocated inside the pipeline; DepthManager::kMaxBuffered is 4096

using Pipeline = hy::PublicFeedPipeline<kDepthRing>;
using Report = hy::PublicFeedTickReport;
using Invalid = hy::SupervisedFeedInvalidReason;
using hy::FeedState;

// Bar i of the synthetic history: contiguous hourly bars, sane candles, prices that move.
hy::KlineWsEvent bar(std::size_t i) {
    hy::KlineWsEvent e;
    const double x = static_cast<double>(i);
    e.open_time_ms = static_cast<std::int64_t>(i) * kHour;
    e.close_time_ms = e.open_time_ms + kHour - 1;
    e.close = 100.0 + 10.0 * std::sin(x * 0.3) + static_cast<double>(i % 5) * 0.1;
    e.open = 100.0 + 10.0 * std::sin((x - 1.0) * 0.3);
    e.high = (e.open > e.close ? e.open : e.close) + 0.5;
    e.low = (e.open < e.close ? e.open : e.close) - 0.5;
    e.volume = 10.0 + static_cast<double>(i % 3);
    e.symbol_id = 0;
    e.is_closed = true;
    return e;
}

// The same bar as Binance's kline stream would push it (closed: "x":true).
std::string kline_json(std::size_t i) {
    const hy::KlineWsEvent b = bar(i);
    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  R"({"e":"kline","E":1,"s":"BTCUSDT","k":{"t":%lld,"T":%lld,"s":"BTCUSDT","i":"1h","f":1,"L":2,)"
                  R"("o":"%.8f","c":"%.8f","h":"%.8f","l":"%.8f","v":"%.8f","n":1,"x":true,)"
                  R"("q":"1.0","V":"1.0","Q":"1.0","B":"0"}})",
                  static_cast<long long>(b.open_time_ms), static_cast<long long>(b.close_time_ms), b.open, b.close,
                  b.high, b.low, b.volume);
    return buf;
}

// One depthUpdate message covering update ids [first, last]: a bid level and an ask level.
std::string depth_json(std::uint64_t first, std::uint64_t last) {
    return R"({"e":"depthUpdate","E":1700000002000,"s":"BTCUSDT","U":)" + std::to_string(first) + R"(,"u":)" +
           std::to_string(last) + R"(,"b":[["67890.00000000","1.00000000"]],"a":[["67891.00000000","0.50000000"]]})";
}

// The scripted REST side.
struct FakeExchange {
    std::atomic<std::size_t> closed_bars{0};  // the exchange has closed bars 0 .. closed_bars-1
    std::atomic<std::uint64_t> depth_last_update_id{100};
    std::atomic<bool> hold_klines{false};
    std::atomic<bool> hold_snapshot{false};
    std::atomic<int> klines_fetches{0};
    std::atomic<int> snapshot_fetches{0};
    std::atomic<int> fail_klines{0};  // the next N backfills fail like a rate-limited endpoint (HTTP 429)

    hy::KlinesBackfillRequest last_request() const {
        std::lock_guard<std::mutex> lock(mu_);
        return last_request_;
    }
    hy::SnapshotRequest last_snapshot_request() const {
        std::lock_guard<std::mutex> lock(mu_);
        return last_snapshot_request_;
    }

    hy::KlinesBackfillOutcome klines(const hy::KlinesBackfillRequest& r) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            last_request_ = r;
        }
        ++klines_fetches;
        hold_while(hold_klines);
        hy::KlinesBackfillOutcome outcome;  // status defaults to a clean success
        if (fail_klines.load() > 0) {
            --fail_klines;
            outcome.status.transport = hy::PublicRestError::Read;
            outcome.status.http_status = 429;
            return outcome;
        }
        const std::size_t have = closed_bars.load();
        const std::size_t count = std::min<std::size_t>(r.limit, have);
        const std::size_t first = have - count;
        for (std::size_t i = 0; i < count; ++i) outcome.data.bars[i] = bar(first + i);
        outcome.data.count = count;
        return outcome;
    }

    std::optional<hy::DepthSnapshot> snapshot(const hy::SnapshotRequest& r) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            last_snapshot_request_ = r;
        }
        ++snapshot_fetches;
        hold_while(hold_snapshot);
        hy::DepthSnapshot s;
        s.last_update_id = depth_last_update_id.load();
        s.bids[0] = hy::PriceLevel{6789000000000LL, 100'000'000};
        s.bid_count = 1;
        s.asks[0] = hy::PriceLevel{6789100000000LL, 50'000'000};
        s.ask_count = 1;
        return s;
    }

private:
    // Bounded: a test that abandons a held fetch must not hang the gate's destructor.
    static void hold_while(const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (flag.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    mutable std::mutex mu_;
    hy::KlinesBackfillRequest last_request_;
    hy::SnapshotRequest last_snapshot_request_;
};

hy::FeedSupervisorPolicy fast_policy() {
    hy::FeedSupervisorPolicy p;  // never gives up, no planned rollover: tests opt in to those
    p.initial_backoff_ms = 1;
    p.max_backoff_ms = 5;
    p.jitter_percent = 0;
    p.connect_deadline_ms = 5000;
    p.drain_timeout_ms = 200;
    return p;
}

hy::PublicFeedPipelineConfig make_config(unsigned short port) {
    hy::PublicFeedPipelineConfig c;
    c.ws_host = "127.0.0.1";
    c.ws_port = std::to_string(port);
    c.symbol = "BTCUSDT";
    c.interval = "1h";
    c.symbol_id = 0;
    c.price_multiplier = 100'000'000;
    c.qty_multiplier = 100'000'000;
    c.kline_policy = fast_policy();
    c.depth_policy = fast_policy();
    // Overdue-bar detection is judged against the exchange clock, and these tests' bars are dated 1970 while
    // the default epoch clock below is the real one: off unless a test supplies a consistent clock and turns
    // it on (see the overdue test at the end).
    c.kline_driver.overdue_grace_ms = 0;
    return c;
}

struct Delivered {
    hy::KlineWsEvent bar;
    double target;
};

// What the ticks so far have shown, accumulated -- a transient state (a disconnect that lasts a few
// milliseconds) is asserted through this rather than by hoping a single tick lands inside it.
struct Seen {
    bool session_suspended{false};
    bool resume_posted{false};
    bool consumer_gap{false};
    int backfills_applied{0};
    bool kline_disconnected{false};
    bool depth_disconnected{false};
    bool kline_not_synced{false};
    bool depth_not_tracking{false};
    bool depth_stale{false};
    bool bar_overdue{false};
    bool overdue_restarted{false};
    bool ever_valid{false};
};

std::unique_ptr<WsLoopbackServer> make_server() {
    return std::make_unique<WsLoopbackServer>(fixture_path("test_leaf_cert_loopback.pem"),
                                              fixture_path("test_leaf_key_loopback.pem"));
}

// Declaration order is construction order and destruction runs the other way, except where the destructor
// below is explicit. Everything the pipeline refers to (exchange, spec, evaluator) precedes it; the I/O
// thread is stopped and joined BEFORE the pipeline is destroyed (public_feed_pipeline.hpp's lifetime
// contract), which the destructor body does by hand.
struct Rig {
    using Tweak = std::function<void(hy::PublicFeedPipelineConfig&)>;

    explicit Rig(const Tweak& tweak = {}, Pipeline::EpochClock epoch = {})
        : server(make_server())
        , load(hy::load_strategy_spec(kSpec))
        , client(std::make_unique<WsClientRig>()) {
        if (!load.ok()) {
            ADD_FAILURE() << "the worked-example spec failed to load";
            return;
        }
        hy::PublicFeedPipelineConfig cfg = make_config(server->port());
        if (tweak) tweak(cfg);
        if (!epoch) {
            epoch = [] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                    .count();
            };
        }
        hy::PipelineInitError error = hy::PipelineInitError::None;
        pipeline = Pipeline::create(
            client->ioc, client->ssl_ctx, evaluator, load.dag, std::move(cfg),
            [this](const hy::KlinesBackfillRequest& r) { return exchange.klines(r); },
            [this](const hy::SnapshotRequest& r) { return exchange.snapshot(r); }, std::move(epoch), error);
        if (!pipeline) ADD_FAILURE() << "create() failed: " << hy::pipeline_init_error_name(error);
    }

    // A fetch still held at the end of a test must be released before the gates' destructors join it.
    ~Rig() {
        exchange.hold_klines = false;
        exchange.hold_snapshot = false;
        if (pipeline) pipeline->shutdown();
        if (client) client->stop_and_join();
        pipeline.reset();
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Report tick() {
        Report r = pipeline->tick(steady_ms(), [this](const hy::KlineWsEvent& b, double target) {
            delivered.push_back({b, target});
        });
        note(r);
        last = r;
        return r;
    }

    // Ticks (1 ms apart) until `pred(report)` holds. False on timeout.
    bool tick_until(const std::function<bool(const Report&)>& pred, int timeout_ms = 8000) {
        return wait_until([&] { return pred(tick()); }, timeout_ms);
    }

    // Keeps ticking for a fixed real-time span (for asserting that something does NOT happen).
    void tick_for(int ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline) {
            (void)tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void note(const Report& r) {
        if (r.feed_valid()) seen.ever_valid = true;
        if (r.kline.session_suspended) seen.session_suspended = true;
        if (r.kline.resume_posted) seen.resume_posted = true;
        if (r.kline.gap_detected) seen.consumer_gap = true;
        if (r.kline.backfill_applied) ++seen.backfills_applied;
        if (r.kline.bar_overdue) seen.bar_overdue = true;
        if (r.kline.overdue_session_restarted) seen.overdue_restarted = true;
        switch (r.validity) {
            case Invalid::KlineDisconnected: seen.kline_disconnected = true; break;
            case Invalid::DepthDisconnected: seen.depth_disconnected = true; break;
            case Invalid::KlineNotSynced: seen.kline_not_synced = true; break;
            case Invalid::DepthNotTracking: seen.depth_not_tracking = true; break;
            case Invalid::DepthStale: seen.depth_stale = true; break;
            default: break;
        }
    }

    FakeExchange exchange;
    std::unique_ptr<WsLoopbackServer> server;
    hy::SpecLoadResult load;
    hy::StreamingEvaluator evaluator;
    std::unique_ptr<WsClientRig> client;
    std::unique_ptr<Pipeline> pipeline;
    std::vector<Delivered> delivered;
    Seen seen;
    Report last;
};

// Both feeds connected, the 60-bar history backfilled, the depth book snapshotted: a valid feed.
void bring_up(Rig& r, std::size_t bars = 60) {
    r.exchange.closed_bars = bars;
    ASSERT_TRUE(r.tick_until([](const Report& rep) { return rep.feed_valid(); })) << "the feed never became valid";
    ASSERT_EQ(r.pipeline->kline_sync().last_close_time_ms(), static_cast<std::int64_t>(bars) * kHour - 1);
    // What the bring-up itself passed through (KlineDisconnected until something connects, and so on) is
    // not what the scenarios below assert about: they look at what happens AFTER it.
    r.seen = Seen{};
}

std::size_t warmup_bars_wanted(Rig& r) { return r.pipeline->kline_sync().backfill_bars_wanted(); }

}  // namespace

// --- creation ------------------------------------------------------------------------------------------

TEST(PublicFeedPipelineCreate, EveryUnusableConfigIsRefusedBeforeAnythingStarts) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);
    hy::StreamingEvaluator evaluator;
    const hy::SpecLoadResult load = hy::load_strategy_spec(kSpec);
    ASSERT_TRUE(load.ok());
    FakeExchange exchange;

    struct Case {
        const char* what;
        std::function<void(hy::PublicFeedPipelineConfig&)> mutate;
        hy::PipelineInitError expected;
    };
    const std::vector<Case> cases = {
        {"empty host", [](auto& c) { c.ws_host.clear(); }, hy::PipelineInitError::EmptyHostOrPort},
        {"empty port", [](auto& c) { c.ws_port.clear(); }, hy::PipelineInitError::EmptyHostOrPort},
        {"empty symbol", [](auto& c) { c.symbol.clear(); }, hy::PipelineInitError::InvalidSymbol},
        {"lowercase symbol (REST wants upper)", [](auto& c) { c.symbol = "btcusdt"; }, hy::PipelineInitError::InvalidSymbol},
        {"symbol with a separator", [](auto& c) { c.symbol = "BTC-USDT"; }, hy::PipelineInitError::InvalidSymbol},
        {"unknown interval", [](auto& c) { c.interval = "7m"; }, hy::PipelineInitError::InvalidInterval},
        {"zero price multiplier", [](auto& c) { c.price_multiplier = 0; }, hy::PipelineInitError::InvalidMultiplier},
        {"negative qty multiplier", [](auto& c) { c.qty_multiplier = -1; }, hy::PipelineInitError::InvalidMultiplier},
        {"symbol id the depth parser cannot register", [](auto& c) { c.symbol_id = 64; },
         hy::PipelineInitError::SymbolRegistrationFailed},
    };
    for (const Case& c : cases) {
        hy::PublicFeedPipelineConfig cfg = make_config(1);
        c.mutate(cfg);
        hy::PipelineInitError error = hy::PipelineInitError::None;
        auto p = Pipeline::create(
            ioc, ssl_ctx, evaluator, load.dag, std::move(cfg),
            [&](const hy::KlinesBackfillRequest& r) { return exchange.klines(r); },
            [&](const hy::SnapshotRequest& r) { return exchange.snapshot(r); }, [] { return std::int64_t{1}; }, error);
        EXPECT_FALSE(p) << c.what;
        EXPECT_EQ(error, c.expected) << c.what << " -> " << hy::pipeline_init_error_name(error);
    }
    EXPECT_EQ(exchange.klines_fetches.load(), 0) << "nothing may fetch before the first tick";
}

TEST(PublicFeedPipelineCreate, AMissingFetcherOrClockIsRefused) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);
    hy::StreamingEvaluator evaluator;
    const hy::SpecLoadResult load = hy::load_strategy_spec(kSpec);
    ASSERT_TRUE(load.ok());
    FakeExchange exchange;
    auto klines = [&](const hy::KlinesBackfillRequest& r) { return exchange.klines(r); };
    auto snapshot = [&](const hy::SnapshotRequest& r) { return exchange.snapshot(r); };
    auto clock = [] { return std::int64_t{1}; };

    hy::PipelineInitError error = hy::PipelineInitError::None;
    EXPECT_FALSE(Pipeline::create(ioc, ssl_ctx, evaluator, load.dag, make_config(1), {}, snapshot, clock, error));
    EXPECT_EQ(error, hy::PipelineInitError::MissingFetcherOrClock);
    EXPECT_FALSE(Pipeline::create(ioc, ssl_ctx, evaluator, load.dag, make_config(1), klines, {}, clock, error));
    EXPECT_EQ(error, hy::PipelineInitError::MissingFetcherOrClock);
    EXPECT_FALSE(Pipeline::create(ioc, ssl_ctx, evaluator, load.dag, make_config(1), klines, snapshot, {}, error));
    EXPECT_EQ(error, hy::PipelineInitError::MissingFetcherOrClock);

    // And the same inputs, complete, are accepted.
    EXPECT_TRUE(Pipeline::create(ioc, ssl_ctx, evaluator, load.dag, make_config(1), klines, snapshot, clock, error) != nullptr);
    EXPECT_EQ(error, hy::PipelineInitError::None);
}

TEST(PublicFeedPipelineCreate, AReportThatWasNeverFilledInIsNotAValidFeed) {
    const Report never_filled;
    EXPECT_FALSE(never_filled.feed_valid());
    EXPECT_FALSE(never_filled.terminal()) << "and it is not a give-up either: nothing is known yet";
    EXPECT_EQ(never_filled.validity, Invalid::KlineDisconnected);
}

TEST(PublicFeedPipelineCreate, AFreshPipelineReportsAnInvalidFeedUntilBothFeedsAreUp) {
    // Never ticked: no supervisor has started, so every input is at its fail-closed value.
    Rig r;
    ASSERT_NE(r.pipeline, nullptr);
    const hy::SupervisedFeedInputs in = r.pipeline->supervised_inputs();
    EXPECT_EQ(in.kline_feed_state, FeedState::Idle);
    EXPECT_EQ(in.depth_feed_state, FeedState::Idle);
    EXPECT_FALSE(in.kline_sync_live);
    EXPECT_FALSE(in.depth_tracking);
    EXPECT_NE(hy::evaluate_supervised_feed_validity(in), Invalid::None);
    EXPECT_TRUE(r.server->targets().empty()) << "creating a pipeline must not open a connection";
}

// --- bring-up ------------------------------------------------------------------------------------------

TEST(PublicFeedPipeline, BringUpWalksTheValiditySequenceAndEndsValid) {
    Rig r;
    ASSERT_NE(r.pipeline, nullptr);
    r.exchange.closed_bars = 60;
    r.exchange.hold_klines = true;
    r.exchange.hold_snapshot = true;

    // Both feeds connect for real; the kline sync is still waiting for its (held) backfill.
    ASSERT_TRUE(r.tick_until([](const Report& rep) {
        return rep.inputs.kline_feed_state == FeedState::Connected && rep.inputs.depth_feed_state == FeedState::Connected;
    }));
    EXPECT_EQ(r.last.validity, Invalid::KlineNotSynced);
    EXPECT_FALSE(r.last.feed_valid());
    EXPECT_FALSE(r.last.terminal());

    // Backfill lands: the sync is live and the depth book is still waiting for its snapshot.
    r.exchange.hold_klines = false;
    ASSERT_TRUE(r.tick_until([](const Report& rep) { return rep.inputs.kline_sync_live; }));
    EXPECT_EQ(r.last.validity, Invalid::DepthNotTracking);

    // Snapshot lands: valid.
    r.exchange.hold_snapshot = false;
    ASSERT_TRUE(r.tick_until([](const Report& rep) { return rep.feed_valid(); }));
    EXPECT_TRUE(r.pipeline->supervised_inputs().depth_tracking);
    EXPECT_EQ(r.pipeline->kline_sync().last_close_time_ms(), 60 * kHour - 1);
    EXPECT_TRUE(r.seen.kline_not_synced);
    EXPECT_TRUE(r.seen.depth_not_tracking);

    // Backfill bars replay into the evaluator silently: they never reach on_bar.
    EXPECT_TRUE(r.delivered.empty());
}

TEST(PublicFeedPipeline, EachFeedConnectsToItsOwnStreamAndTheBackfillAsksForWhatTheConfigSays) {
    // Distinct, non-default values so a swapped or dropped field cannot pass by coincidence.
    Rig r([](hy::PublicFeedPipelineConfig& c) {
        c.symbol_id = 3;
        c.price_multiplier = 100'000'000;
        c.qty_multiplier = 1'000'000;
    });
    ASSERT_NE(r.pipeline, nullptr);
    ASSERT_NO_FATAL_FAILURE(bring_up(r));

    // The stream names are the lowercase symbol; the depth one is the 100ms diff stream.
    const auto targets = r.server->targets();
    EXPECT_EQ(std::set<std::string>(targets.begin(), targets.end()), (std::set<std::string>{kKlineTarget, kDepthTarget}));
    EXPECT_EQ(targets.size(), 2U) << "one connection per feed";

    // The REST request is the UPPERCASE symbol and the configured interval, sized for the spec's warm-up.
    const hy::KlinesBackfillRequest req = r.exchange.last_request();
    EXPECT_EQ(req.symbol, "BTCUSDT");
    EXPECT_EQ(req.interval, "1h");
    EXPECT_EQ(req.symbol_id, 3U);
    EXPECT_EQ(req.limit, warmup_bars_wanted(r));
    EXPECT_GT(req.limit, 50U) << "the spec's 50-bar SMA needs at least that much history";
    // The depth snapshot request carries the same symbol and the SAME scales the depth parser was
    // registered with (they must agree with what validate_pre_trade() checks against).
    const hy::SnapshotRequest snap = r.exchange.last_snapshot_request();
    EXPECT_EQ(snap.symbol, "BTCUSDT");
    EXPECT_EQ(snap.price_multiplier, 100'000'000);
    EXPECT_EQ(snap.qty_multiplier, 1'000'000);
    EXPECT_EQ(r.exchange.klines_fetches.load(), 1) << "exactly one backfill for a clean start";
    EXPECT_EQ(r.exchange.snapshot_fetches.load(), 1) << "exactly one snapshot for a clean start";
}

// --- live bars -----------------------------------------------------------------------------------------

TEST(PublicFeedPipeline, LiveBarsFlowToOnBarInOrder) {
    Rig r([](hy::PublicFeedPipelineConfig& c) { c.symbol_id = 3; });
    ASSERT_NE(r.pipeline, nullptr);
    ASSERT_NO_FATAL_FAILURE(bring_up(r));

    r.exchange.closed_bars = 62;
    r.server->send_text(kline_json(60), kKlineTarget);
    r.server->send_text(kline_json(61), kKlineTarget);
    ASSERT_TRUE(wait_until([&] {
        (void)r.tick();
        return r.delivered.size() >= 2;
    }));
    ASSERT_EQ(r.delivered.size(), 2U);
    EXPECT_EQ(r.delivered[0].bar.open_time_ms, 60 * kHour);
    EXPECT_EQ(r.delivered[1].bar.open_time_ms, 61 * kHour);
    EXPECT_TRUE(r.delivered[0].bar.is_closed);
    EXPECT_EQ(r.delivered[0].bar.symbol_id, 3U) << "the session stamps the configured symbol id";
    EXPECT_EQ(r.pipeline->kline_sync().last_close_time_ms(), 62 * kHour - 1);
    EXPECT_TRUE(r.last.feed_valid());
    EXPECT_EQ(r.pipeline->kline_driver_stats().backfills_applied, 1U) << "live bars must not trigger another backfill";
}

// --- the kline recovery protocol against the REAL session ---------------------------------------------------

// A hole between two closed bars on ONE connection: the real session's own guard suspends, the driver
// invalidates the sync, resumes the session (a real strand post), the backfill supplies the bar the stream
// dropped, and live bars flow again.
TEST(PublicFeedPipeline, ARealSessionGapIsSuspendedResumedAndBackfilled) {
    Rig r;
    ASSERT_NE(r.pipeline, nullptr);
    ASSERT_NO_FATAL_FAILURE(bring_up(r));
    ASSERT_EQ(r.exchange.klines_fetches.load(), 1);

    // Bar 60 gives the session its baseline; bar 62 then skips 61.
    r.exchange.closed_bars = 63;
    r.server->send_text(kline_json(60), kKlineTarget);
    ASSERT_TRUE(wait_until([&] {
        (void)r.tick();
        return r.delivered.size() >= 1;
    }));
    r.server->send_text(kline_json(62), kKlineTarget);

    ASSERT_TRUE(r.tick_until([&](const Report&) { return r.pipeline->kline_driver_stats().backfills_applied >= 2; }))
        << "no second backfill: suspended=" << r.seen.session_suspended << " resume=" << r.seen.resume_posted;
    EXPECT_TRUE(r.seen.session_suspended) << "the real session must have suspended itself on the hole";
    EXPECT_TRUE(r.seen.resume_posted) << "the driver must have resumed it";
    EXPECT_GE(r.pipeline->kline_driver_stats().resumes_posted, 1U);
    EXPECT_GE(r.exchange.klines_fetches.load(), 2);

    // The backfill contained bars up to 62, so the sync is at 62's close -- and 62 itself was never
    // delivered through on_bar (the session dropped it; the backfill replayed it silently).
    ASSERT_TRUE(r.tick_until([](const Report& rep) { return rep.feed_valid(); }));
    EXPECT_EQ(r.pipeline->kline_sync().last_close_time_ms(), 63 * kHour - 1);
    ASSERT_EQ(r.delivered.size(), 1U);
    EXPECT_EQ(r.delivered[0].bar.open_time_ms, 60 * kHour);

    // And the session accepts bars again after the resume.
    r.exchange.closed_bars = 64;
    r.server->send_text(kline_json(63), kKlineTarget);
    ASSERT_TRUE(wait_until([&] {
        (void)r.tick();
        return r.delivered.size() >= 2;
    }));
    EXPECT_EQ(r.delivered[1].bar.open_time_ms, 63 * kHour);
}

// A dropped connection is a new generation: validity goes to KlineDisconnected (transient) while the
// supervisor backs off and reconnects, then recovers by itself.
TEST(PublicFeedPipeline, ADroppedKlineConnectionReconnectsAndTheFeedRecovers) {
    Rig r([](hy::PublicFeedPipelineConfig& c) { c.kline_policy.initial_backoff_ms = 30; c.kline_policy.max_backoff_ms = 30; });
    ASSERT_NE(r.pipeline, nullptr);
    ASSERT_NO_FATAL_FAILURE(bring_up(r));
    ASSERT_EQ(r.pipeline->kline_supervisor_stats().generation, 1U);

    r.server->drop_all(kKlineTarget);
    // Until the second generation is up and the feed is valid again. The disconnect itself is transient
    // (the 30 ms backoff), so it is asserted through what the ticks SAW, not through one tick landing in it.
    ASSERT_TRUE(r.tick_until([&](const Report& rep) {
        return r.pipeline->kline_supervisor_stats().generation >= 2 && rep.feed_valid();
    }));
    EXPECT_TRUE(r.seen.kline_disconnected) << "a reconnecting feed must have reported KlineDisconnected";
    EXPECT_FALSE(r.seen.depth_disconnected) << "the depth connection was never touched";
    EXPECT_EQ(r.pipeline->kline_supervisor_stats().total_failures, 1U);
    EXPECT_EQ(r.pipeline->depth_supervisor_stats().generation, 1U);
    EXPECT_EQ(r.server->targets().size(), 3U) << "the reconnect is a genuinely new connection";
}

// A bar that closed while the feed was down never arrives; the first bar of the new connection is then not
// adjacent to what the evaluator has -- the CONSUMER-side gap path (the new session has no baseline of its
// own, so it cannot notice) -- and the same backfill recovery closes it.
TEST(PublicFeedPipeline, ABarMissedWhileDisconnectedIsRecoveredByTheConsumerSideGapPath) {
    Rig r([](hy::PublicFeedPipelineConfig& c) { c.kline_policy.initial_backoff_ms = 30; c.kline_policy.max_backoff_ms = 30; });
    ASSERT_NE(r.pipeline, nullptr);
    ASSERT_NO_FATAL_FAILURE(bring_up(r));

    r.server->drop_all(kKlineTarget);
    r.exchange.closed_bars = 62;  // bars 60 and 61 close during the outage
    ASSERT_TRUE(r.tick_until([&](const Report& rep) {
        return r.pipeline->kline_supervisor_stats().generation >= 2 && rep.inputs.kline_feed_state == FeedState::Connected;
    }));

    // The new connection's first bar is 62; the evaluator is at 59.
    r.exchange.closed_bars = 63;
    r.server->send_text(kline_json(62), kKlineTarget);
    ASSERT_TRUE(r.tick_until([&](const Report&) { return r.seen.consumer_gap; })) << "the consumer must notice the hole";
    ASSERT_TRUE(r.tick_until([](const Report& rep) { return rep.feed_valid(); }));
    EXPECT_EQ(r.pipeline->kline_sync().last_close_time_ms(), 63 * kHour - 1);
    EXPECT_GE(r.pipeline->kline_driver_stats().backfills_applied, 2U);
    EXPECT_TRUE(r.delivered.empty()) << "the gap bar arrives through the backfill, not through on_bar";
}

// The backfill gate is handed a cooldown function (klines_backfill_cooldown_ms): a rate-limited endpoint
// must be left alone, not asked again on the very next tick. Without that function the gate has no
// cooldown for a failure the fetcher RETURNED, and would retry immediately.
TEST(PublicFeedPipeline, ARateLimitedBackfillIsLeftAloneInsteadOfBeingRetriedAtOnce) {
    Rig r;
    ASSERT_NE(r.pipeline, nullptr);
    r.exchange.closed_bars = 60;
    r.exchange.fail_klines = 1;

    r.tick_for(500);  // hundreds of ticks
    EXPECT_EQ(r.exchange.klines_fetches.load(), 1) << "a 429 must impose a cooldown, not an immediate retry";
    EXPECT_EQ(r.pipeline->kline_driver_stats().backfill_fetch_failures, 1U);
    EXPECT_FALSE(r.pipeline->supervised_inputs().kline_sync_live);
    EXPECT_EQ(r.last.validity, Invalid::KlineNotSynced) << "the feed stays closed while the backfill is cooling down";
    EXPECT_FALSE(r.seen.ever_valid);
}

// --- the depth feed --------------------------------------------------------------------------------------

TEST(PublicFeedPipeline, ADroppedDepthConnectionRebuildsTheBookFromAFreshSnapshot) {
    Rig r([](hy::PublicFeedPipelineConfig& c) { c.depth_policy.initial_backoff_ms = 30; c.depth_policy.max_backoff_ms = 30; });
    ASSERT_NE(r.pipeline, nullptr);
    ASSERT_NO_FATAL_FAILURE(bring_up(r));

    // Generation 1: a live depth message is applied.
    r.server->send_text(depth_json(101, 101), kDepthTarget);
    ASSERT_TRUE(wait_until([&] {
        (void)r.tick();
        return r.pipeline->depth_driver_stats().events_applied >= 2;  // one bid level + one ask level
    }));
    const std::uint64_t applied_before = r.pipeline->depth_driver_stats().events_applied;
    ASSERT_EQ(r.pipeline->depth_driver_stats().generation_resets, 1U);

    // The connection drops. The new generation's update ids are unrelated to the old one's.
    r.exchange.depth_last_update_id = 500;
    r.server->drop_all(kDepthTarget);
    ASSERT_TRUE(r.tick_until([&](const Report&) { return r.pipeline->depth_driver_stats().generation_resets >= 2; }));
    ASSERT_TRUE(r.tick_until([](const Report& rep) { return rep.feed_valid(); }))
        << "the book must be rebuilt from a fresh snapshot";
    EXPECT_TRUE(r.seen.depth_disconnected || r.seen.depth_not_tracking);
    EXPECT_FALSE(r.seen.kline_disconnected) << "the kline connection was never touched";
    EXPECT_EQ(r.pipeline->kline_supervisor_stats().generation, 1U);
    EXPECT_EQ(r.exchange.snapshot_fetches.load(), 2);

    // Events continuing from the NEW snapshot are applied; the old sequence is irrelevant.
    r.server->send_text(depth_json(501, 501), kDepthTarget);
    ASSERT_TRUE(wait_until([&] {
        (void)r.tick();
        return r.pipeline->depth_driver_stats().events_applied >= applied_before + 2;
    }));
    EXPECT_TRUE(r.last.feed_valid());
    EXPECT_EQ(r.pipeline->depth_manager().stats().resyncs, 0U) << "a proper reset is not a gap";
}

// --- planned rollover: the kline boundary callback is wired to the sync and the epoch clock --------------------

TEST(PublicFeedPipeline, TheKlineRolloverWaitsForTheWindowJustAfterABarCloses) {
    std::atomic<std::int64_t> epoch{0};
    Rig r([](hy::PublicFeedPipelineConfig& c) { c.kline_policy.max_connection_age_ms = 300; },
          [&epoch] { return epoch.load(); });
    ASSERT_NE(r.pipeline, nullptr);
    ASSERT_NO_FATAL_FAILURE(bring_up(r));
    const std::int64_t last_close = r.pipeline->kline_sync().last_close_time_ms();

    // Half an hour after the last bar closed: for 1h bars the window is the first 60s, so it is shut. The
    // connection outlives its planned age without being replaced.
    epoch = last_close + 30 * 60 * 1000;
    r.tick_for(800);
    EXPECT_EQ(r.pipeline->kline_supervisor_stats().rollovers, 0U);
    EXPECT_EQ(r.pipeline->kline_supervisor_stats().generation, 1U);
    EXPECT_EQ(r.pipeline->supervised_inputs().kline_feed_state, FeedState::Connected);

    // Ten seconds after a close: open. The rollover goes ahead, and a planned one is not a failure.
    epoch = last_close + 10'000;
    ASSERT_TRUE(r.tick_until([&](const Report&) { return r.pipeline->kline_supervisor_stats().generation >= 2; }));
    // Shut the window again at once: the new generation is itself 300 ms old soon enough, and a second
    // rollover in the middle of the assertions below would be the test's own doing, not the pipeline's.
    epoch = last_close + 30 * 60 * 1000;
    ASSERT_TRUE(r.tick_until([](const Report& rep) { return rep.feed_valid(); }));
    EXPECT_EQ(r.pipeline->kline_supervisor_stats().rollovers, 1U);
    EXPECT_EQ(r.pipeline->kline_supervisor_stats().total_failures, 0U);
    EXPECT_EQ(r.pipeline->depth_supervisor_stats().generation, 1U) << "the depth feed rolls over on its own schedule";
}

// --- giving up -------------------------------------------------------------------------------------------------

TEST(PublicFeedPipeline, FeedsThatNeverStartGiveUpAndTheReportIsTerminal) {
    // A host both sessions reject at start(): every attempt is a failed attempt, at once and without a
    // network round trip (refusing a loopback connection takes ~2 s on Windows, which would make this test
    // slow and OS-dependent for no gain -- the supervisor only ever sees "the session stopped").
    Rig r([](hy::PublicFeedPipelineConfig& c) {
        c.ws_host = "not a host";
        for (hy::FeedSupervisorPolicy* p : {&c.kline_policy, &c.depth_policy}) {
            p->max_consecutive_failures = 2;
            p->initial_backoff_ms = 1;
            p->max_backoff_ms = 1;
        }
    });
    ASSERT_NE(r.pipeline, nullptr);
    r.exchange.closed_bars = 60;

    // Either feed may give up first; the priority order is only observable once BOTH have.
    ASSERT_TRUE(r.tick_until(
        [](const Report& rep) {
            return rep.inputs.kline_feed_state == FeedState::Terminal && rep.inputs.depth_feed_state == FeedState::Terminal;
        },
        15000));
    EXPECT_TRUE(r.last.terminal());
    EXPECT_EQ(r.last.validity, Invalid::KlineFeedGaveUp) << "kline outranks depth in the fixed priority order";
    EXPECT_EQ(r.pipeline->kline_supervisor_stats().terminal_reason, hy::FeedTerminalReason::TooManyFailures);
    EXPECT_EQ(r.pipeline->depth_supervisor_stats().terminal_reason, hy::FeedTerminalReason::TooManyFailures);
    EXPECT_EQ(r.pipeline->kline_supervisor_stats().total_failures, 2U);
    EXPECT_FALSE(r.seen.ever_valid) << "a feed that never connected must never have read as valid, at any tick";
    EXPECT_TRUE(r.server->targets().empty()) << "no session got as far as the network";
}

TEST(PublicFeedPipeline, ShutdownEndsBothFeedsAndIsIdempotent) {
    Rig r;
    ASSERT_NE(r.pipeline, nullptr);
    ASSERT_NO_FATAL_FAILURE(bring_up(r));
    const auto kline = r.pipeline->kline_session();
    const auto depth = r.pipeline->depth_session();
    ASSERT_NE(kline, nullptr);
    ASSERT_NE(depth, nullptr);

    r.pipeline->shutdown();
    r.pipeline->shutdown();  // idempotent
    EXPECT_TRUE(wait_until([&] { return kline->stopped() && depth->stopped(); })) << "both sessions must be told to stop";
    const Report rep = r.tick();
    EXPECT_TRUE(rep.terminal());
    EXPECT_EQ(r.pipeline->kline_supervisor_stats().terminal_reason, hy::FeedTerminalReason::Shutdown);
    EXPECT_EQ(r.pipeline->depth_supervisor_stats().terminal_reason, hy::FeedTerminalReason::Shutdown);
}

// --- data-level staleness (6b-0f-9) ------------------------------------------------------------------------
//
// The websocket idle timeout is satisfied by Binance's pings, so a feed whose DATA stopped looks connected.
// Both sessions here are real; the "silence" is the loopback server simply not sending.

TEST(PublicFeedPipeline, ADepthFeedThatStopsDeliveringIsRestartedAndTheBookRebuilt) {
    Rig r([](hy::PublicFeedPipelineConfig& c) { c.depth_driver.stale_after_ms = 1000; });
    ASSERT_NE(r.pipeline, nullptr);
    ASSERT_NO_FATAL_FAILURE(bring_up(r));

    // Generation 1 delivers once; then the connection stays up and nothing more arrives.
    r.server->send_text(depth_json(101, 101), kDepthTarget);
    ASSERT_TRUE(wait_until([&] {
        (void)r.tick();
        return r.pipeline->depth_driver_stats().events_applied >= 2;
    }));
    EXPECT_FALSE(r.seen.depth_stale);

    ASSERT_TRUE(r.tick_until([&](const Report&) { return r.seen.depth_stale; })) << "a frozen book must be reported";
    EXPECT_GE(r.pipeline->depth_driver_stats().stale_restarts, 1U);
    EXPECT_FALSE(r.seen.kline_disconnected) << "the kline connection was never touched";

    ASSERT_TRUE(r.tick_until([&](const Report& rep) { return rep.feed_valid() && r.exchange.snapshot_fetches.load() >= 2; }))
        << "restart + a fresh snapshot: the feed is valid again";
    EXPECT_GE(r.pipeline->depth_supervisor_stats().generation, 2U);
    EXPECT_GE(r.pipeline->depth_supervisor_stats().total_failures, 1U) << "a stale restart is a failure, not a rollover";
    EXPECT_EQ(r.pipeline->depth_supervisor_stats().rollovers, 0U);
    EXPECT_EQ(r.pipeline->kline_supervisor_stats().generation, 1U);

    // And the new connection really delivers.
    const std::uint64_t applied_before = r.pipeline->depth_driver_stats().events_applied;
    r.server->send_text(depth_json(101, 101), kDepthTarget);
    ASSERT_TRUE(wait_until([&] {
        (void)r.tick();
        return r.pipeline->depth_driver_stats().events_applied >= applied_before + 2;
    }));
}

// Binance testnet is reset from time to time and update ids then restart low. A new generation must forget
// the old sequence, or the validator rejects the whole new connection as a rollback and the book freezes.
TEST(PublicFeedPipeline, AnExchangeSideResetToLowUpdateIdsIsAcceptedAfterAReconnect) {
    Rig r([](hy::PublicFeedPipelineConfig& c) { c.depth_policy.initial_backoff_ms = 30; c.depth_policy.max_backoff_ms = 30; });
    ASSERT_NE(r.pipeline, nullptr);
    r.exchange.depth_last_update_id = 5000;
    ASSERT_NO_FATAL_FAILURE(bring_up(r));

    r.server->send_text(depth_json(5001, 5001), kDepthTarget);
    ASSERT_TRUE(wait_until([&] {
        (void)r.tick();
        return r.pipeline->depth_driver_stats().events_applied >= 2;
    }));
    const std::uint64_t applied_before = r.pipeline->depth_driver_stats().events_applied;

    r.exchange.depth_last_update_id = 40;  // the exchange came back reset
    r.server->drop_all(kDepthTarget);
    ASSERT_TRUE(r.tick_until([&](const Report&) { return r.pipeline->depth_driver_stats().generation_resets >= 2; }));
    ASSERT_TRUE(r.tick_until([](const Report& rep) { return rep.feed_valid(); }));

    r.server->send_text(depth_json(41, 41), kDepthTarget);
    ASSERT_TRUE(wait_until([&] {
        (void)r.tick();
        return r.pipeline->depth_driver_stats().events_applied >= applied_before + 2;
    })) << "the reset ids must not be rejected as a rollback";
    EXPECT_EQ(r.pipeline->depth_driver_stats().events_rejected, 0U);
}

// The kline stream goes silent while staying connected: bar 60 closes and never arrives. Overdue detection
// (judged on the exchange clock) repairs it from REST -- which needs nothing from the websocket -- and
// replaces the connection, because it was up when the bar was due.
TEST(PublicFeedPipeline, AKlineBarThatNeverArrivesIsRepairedByABackfillAndTheSilentConnectionIsReplaced) {
    std::atomic<std::int64_t> epoch{60 * kHour + 1'000};  // shortly after bar 59 closed: nothing is due yet
    Rig r([](hy::PublicFeedPipelineConfig& c) { c.kline_driver.overdue_grace_ms = 200; },
          [&epoch] { return epoch.load(); });
    ASSERT_NE(r.pipeline, nullptr);
    ASSERT_NO_FATAL_FAILURE(bring_up(r));  // valid, last close = 60 * kHour - 1
    r.tick_for(1200);  // the connection is now older than the lateness that comes next
    EXPECT_FALSE(r.seen.bar_overdue) << "bar 60 is not due yet";
    EXPECT_TRUE(r.last.feed_valid());

    // Bar 60 closes -- the stream stays silent. REST has it.
    r.exchange.closed_bars = 61;
    epoch = 61 * kHour + 1'000;  // 1 s after it closed: past the 200 ms grace
    ASSERT_TRUE(r.tick_until([&](const Report&) { return r.seen.bar_overdue; }));
    EXPECT_TRUE(r.seen.overdue_restarted) << "the connection was up when the bar was due: it is the suspect";

    ASSERT_TRUE(r.tick_until([](const Report& rep) { return rep.feed_valid(); }))
        << "backfill + a fresh connection: valid again";
    EXPECT_EQ(r.pipeline->kline_sync().last_close_time_ms(), 61 * kHour - 1)
        << "REST supplied the bar the stream never delivered";
    EXPECT_EQ(r.pipeline->kline_driver_stats().overdue_invalidations, 1U);
    EXPECT_EQ(r.pipeline->kline_driver_stats().overdue_session_restarts, 1U);
    EXPECT_EQ(r.pipeline->kline_supervisor_stats().generation, 2U);
    EXPECT_EQ(r.pipeline->kline_supervisor_stats().total_failures, 1U);
    EXPECT_EQ(r.pipeline->depth_supervisor_stats().generation, 1U) << "the depth feed was not touched";

    // The new connection continues the sequence.
    r.server->send_text(kline_json(61), kKlineTarget);
    ASSERT_TRUE(r.tick_until([&](const Report&) { return !r.delivered.empty(); }));
    EXPECT_EQ(r.delivered.back().bar.open_time_ms, 61 * kHour);
    EXPECT_TRUE(r.last.feed_valid());
}
