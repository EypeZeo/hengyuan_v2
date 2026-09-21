// 批次 6 6b-0f-3c: kline_feed_driver.hpp tests -- Boost-free, no network.
//
// The driver is the orchestration that wires the supervisor, the fetch gate, the sync and the ring
// together, which is where recovery bugs would actually live (forgetting to resume the session,
// resuming every tick, draining the ring while a backfill is pending, hammering a source that keeps
// returning garbage). So everything except the session and the fetch is REAL here: a real
// SingleFlightFetchGate running a hand-released fetcher on a real worker thread, a real KlineFeedSync
// over a real StreamingEvaluator, a real KlineWsEventRing and a real PublicFeedSupervisor. The
// evaluator is checked against a reference fed the same contiguous bars. The binary carries the
// `concurrency` label because of the gate's worker thread.

#include <gtest/gtest.h>
#include <hengyuan/kline_feed_driver.hpp>
#include <hengyuan/strategy_spec_toml_parser.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using hy::BackfillApplyStatus;
using hy::KlineFeedDriverConfig;
using hy::KlineFeedTickReport;
using hy::KlinesBackfillRequest;
using hy::KlineWsEvent;
using hy::StreamingEvaluator;

namespace {

constexpr std::string_view kWorkedExample = R"(
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

// Loads, but its single 2000-bar window cannot fit the evaluator's history pool.
constexpr std::string_view kOversized = R"(
spec_version = 1
name = "oversized_window"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "huge"
op = "sma"
input = "close"
window = 2000

[signal]
node = "huge"
mode = "scaled"

[validation]
validated_at   = "2026-09-15T08:30:00Z"
validated_by   = "x"
data_start_utc = "2021-01-01T00:00:00Z"
data_end_utc   = "2026-06-30T00:00:00Z"
data_digest    = "sha256:0"
oos_sharpe     = 1.0
pbo            = 0.1
trials         = 1
)";

constexpr std::int64_t kHour = 3'600'000;

KlineWsEvent bar(std::size_t i) {
    KlineWsEvent e;
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

std::vector<KlineWsEvent> bars(std::size_t first, std::size_t count) {
    std::vector<KlineWsEvent> v;
    for (std::size_t i = 0; i < count; ++i) v.push_back(bar(first + i));
    return v;
}

bool same_double(double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

bool same_state(const StreamingEvaluator& a, const StreamingEvaluator& b, std::size_t node_count) {
    if (a.is_initialized() != b.is_initialized() || a.seen_bars() != b.seen_bars() ||
        a.warmup_complete() != b.warmup_complete()) {
        return false;
    }
    for (std::size_t i = 0; i < node_count; ++i) {
        if (!same_double(a.node_value(i), b.node_value(i))) return false;
    }
    return true;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// The session the supervisor manages AND the driver watches: the four calls the supervisor needs, plus
// is_suspended()/resume_after_gap() for the driver.
struct FakeSession {
    explicit FakeSession(std::uint64_t g) : generation(g) {}
    void start() {}
    void stop() { stopped_flag.store(true); }
    bool stopped() const { return stopped_flag.load(); }
    bool is_connected() const { return true; }
    bool is_suspended() const { return suspended.load(); }
    void resume_after_gap() {
        resume_calls.fetch_add(1);
        if (clear_on_resume) suspended.store(false);  // the real one clears asynchronously on its strand
    }

    const std::uint64_t generation;
    bool clear_on_resume{true};
    std::atomic<bool> stopped_flag{false};
    std::atomic<bool> suspended{false};
    std::atomic<int> resume_calls{0};
};
static_assert(hy::KlineFeedSession<FakeSession>);

// What the fetcher returns. Deliberately not the Boost-dependent KlinesBackfillOutcome: the driver
// only needs ok() and bars().
struct FakeOutcome {
    bool ok_flag{false};
    std::vector<KlineWsEvent> data;
    std::int64_t cooldown_ms{0};  // read back by the gate's CooldownFn in the Rig

    bool ok() const { return ok_flag; }
    std::span<const KlineWsEvent> bars() const { return {data.data(), data.size()}; }
};
static_assert(hy::KlinesBackfillOutcomeLike<FakeOutcome>);

FakeOutcome good(std::size_t first, std::size_t count) {
    FakeOutcome o;
    o.ok_flag = true;
    o.data = bars(first, count);
    return o;
}

FakeOutcome failed(std::int64_t cooldown_ms) {
    FakeOutcome o;
    o.ok_flag = false;
    o.cooldown_ms = cooldown_ms;
    return o;
}

// A fetcher the test releases by hand and scripts with planned outcomes.
class Controlled {
public:
    FakeOutcome operator()(const KlinesBackfillRequest& request) {
        // The request is recorded BEFORE `calls` is published: a test that waits for calls == n and then
        // reads seen() must be guaranteed to find the request (the other order raced -- caught by TSan's
        // slower timing, where seen() was still empty when calls already read 1).
        {
            std::lock_guard<std::mutex> lock(seen_mutex_);
            seen_.push_back(request);
        }
        const int n = ++calls;
        FakeOutcome planned;
        std::unique_lock<std::mutex> lock(m_);
        // Time-bounded so a failed ASSERT that abandons a test cannot hang the gate's destructor.
        cv_.wait_for(lock, std::chrono::seconds(10), [&] { return allowed_ >= n; });
        if (!plan_.empty()) {
            planned = std::move(plan_.front());
            plan_.pop_front();
        }
        return planned;
    }

    void plan(FakeOutcome o) {
        std::lock_guard<std::mutex> lock(m_);
        plan_.push_back(std::move(o));
    }
    void allow_next() {
        {
            std::lock_guard<std::mutex> lock(m_);
            ++allowed_;
        }
        cv_.notify_all();
    }
    // Lets every fetch, present and future, return at once (test teardown).
    void allow_all() {
        {
            std::lock_guard<std::mutex> lock(m_);
            allowed_ = std::numeric_limits<int>::max();
        }
        cv_.notify_all();
    }
    std::vector<KlinesBackfillRequest> seen() const {
        std::lock_guard<std::mutex> lock(seen_mutex_);
        return seen_;
    }

    std::atomic<int> calls{0};

private:
    std::mutex m_;
    std::condition_variable cv_;
    int allowed_{0};
    std::deque<FakeOutcome> plan_;
    mutable std::mutex seen_mutex_;
    std::vector<KlinesBackfillRequest> seen_;
};

hy::FeedSupervisorPolicy quiet_policy() {
    hy::FeedSupervisorPolicy p;
    p.jitter_percent = 0;
    return p;
}

struct Delivered {
    KlineWsEvent bar;
    double target;
};

struct Rig {
    using Driver = hy::KlineFeedDriver<FakeSession, FakeOutcome>;
    using Gate = Driver::Gate;

    explicit Rig(KlineFeedDriverConfig cfg = {}, std::string_view spec = kWorkedExample)
        : load(hy::load_strategy_spec(spec))
        , sync(evaluator, load.dag)
        , gate([this](const KlinesBackfillRequest& r) { return controlled(r); },
               [](const FakeOutcome& o) { return o.cooldown_ms; })
        , supervisor(
              [this](std::uint64_t generation) {
                  auto s = std::make_shared<FakeSession>(generation);
                  sessions.push_back(s);
                  return s;
              },
              quiet_policy())
        , driver(sync, gate, supervisor, ring, KlinesBackfillRequest{"BTCUSDT", "1h", 0, 0}, cfg) {
        supervisor.poll(0);  // starts generation 1: driver.tick() has a current() session to watch
    }

    // A fetch still blocked at the end of a test must be released before the gate's destructor joins it
    // (members are destroyed AFTER this body runs).
    ~Rig() { controlled.allow_all(); }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    FakeSession& session() { return *sessions.back(); }
    std::size_t nodes() const { return load.dag.node_count; }

    void push_bars(std::size_t first, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) ASSERT_TRUE(ring.try_push(bar(first + i)));
    }

    KlineFeedTickReport tick(std::int64_t now_ms) {
        return driver.tick(now_ms, [this](const KlineWsEvent& b, double target) { delivered.push_back({b, target}); });
    }

    // Ticks (at a frozen synthetic time) until the gate delivers an outcome; returns THAT tick's report.
    KlineFeedTickReport tick_until_collected(std::int64_t now_ms) {
        KlineFeedTickReport report;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            report = tick(now_ms);
            if (report.backfill_collected) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return report;
    }

    // Scripts the outcome of the fetch that is already out, releases it, and ticks until it is collected.
    KlineFeedTickReport complete_fetch(FakeOutcome outcome, std::int64_t now_ms) {
        controlled.plan(std::move(outcome));
        controlled.allow_next();
        return tick_until_collected(now_ms);
    }

    StreamingEvaluator reference(std::size_t first, std::size_t count) const {
        StreamingEvaluator ref;
        EXPECT_TRUE(ref.init(load.dag));
        for (std::size_t i = 0; i < count; ++i) {
            const KlineWsEvent b = bar(first + i);
            (void)ref.step(hy::Bar{b.open, b.high, b.low, b.close, b.volume});
        }
        return ref;
    }

    // Declaration order is construction order: the fetcher must outlive the gate (whose destructor
    // joins the worker that is calling it), and the session list the supervisor's factory appends to.
    Controlled controlled;
    hy::SpecLoadResult load;
    StreamingEvaluator evaluator;
    hy::KlineFeedSync sync;
    Gate gate;
    hy::KlineWsEventRing ring;
    std::vector<std::shared_ptr<FakeSession>> sessions;
    hy::PublicFeedSupervisor<FakeSession> supervisor;
    Driver driver;
    std::vector<Delivered> delivered;
};

// Gets a rig to the Live state at bar 59 (a full 60-bar history, warmed up).
void go_live(Rig& rig) {
    (void)rig.tick(0);  // starts the initial backfill
    const auto r = rig.complete_fetch(good(0, 60), 1000);
    ASSERT_TRUE(r.backfill_applied);
    ASSERT_TRUE(rig.sync.live());
}

}  // namespace

// --- startup & the join --------------------------------------------------------------------------------

TEST(KlineFeedDriver, StartupStartsOneBackfillForTheRightNumberOfBarsAndLeavesTheRingAlone) {
    Rig rig;
    rig.push_bars(0, 3);  // bars already buffered before any backfill

    const auto first = rig.tick(1000);
    EXPECT_TRUE(first.backfill_started);
    EXPECT_FALSE(rig.sync.live());
    EXPECT_EQ(first.bars_stepped, 0U);

    ASSERT_TRUE(wait_until([&] { return rig.controlled.calls.load() == 1; }));
    const auto seen = rig.controlled.seen();
    ASSERT_EQ(seen.size(), 1U);
    EXPECT_EQ(seen[0].symbol, "BTCUSDT");
    EXPECT_EQ(seen[0].interval, "1h");
    EXPECT_EQ(seen[0].limit, 53U);  // warm-up 50, +1 to complete it, +2 slack

    // More ticks while the fetch is out: no second fetch, and the ring is NOT drained.
    for (int i = 0; i < 50; ++i) EXPECT_FALSE(rig.tick(1001 + i).backfill_started);
    EXPECT_EQ(rig.gate.stats().fetches_started, 1U);
    EXPECT_EQ(rig.ring.size_approx(), 3U) << "the ring is the buffer: bars must wait for the backfill";
    EXPECT_TRUE(rig.delivered.empty());
}

TEST(KlineFeedDriver, ABackfillIsAppliedAndTheBufferedRingIsJoinedInTheSameTick) {
    Rig rig;
    rig.push_bars(58, 4);  // 58 and 59 overlap the backfill; 60 and 61 are new
    (void)rig.tick(1000);

    const auto r = rig.complete_fetch(good(0, 60), 1000);
    EXPECT_TRUE(r.backfill_collected);
    EXPECT_TRUE(r.backfill_fetch_ok);
    EXPECT_TRUE(r.backfill_applied);
    EXPECT_EQ(r.apply_status, BackfillApplyStatus::Applied);
    EXPECT_EQ(r.bars_applied, 60U);
    EXPECT_TRUE(r.warmup_complete_after_apply);
    EXPECT_EQ(r.duplicates_skipped, 2U);
    EXPECT_EQ(r.bars_stepped, 2U);
    EXPECT_TRUE(rig.sync.live());

    ASSERT_EQ(rig.delivered.size(), 2U);
    StreamingEvaluator ref = rig.reference(0, 60);
    for (std::size_t i = 0; i < 2; ++i) {
        const KlineWsEvent b = bar(60 + i);
        const double expected = ref.step(hy::Bar{b.open, b.high, b.low, b.close, b.volume});
        EXPECT_EQ(rig.delivered[i].bar.open_time_ms, b.open_time_ms);
        EXPECT_TRUE(same_double(rig.delivered[i].target, expected)) << "bar " << 60 + i;
    }
    EXPECT_TRUE(same_state(rig.evaluator, ref, rig.nodes()));
    EXPECT_EQ(rig.driver.stats().backfills_applied, 1U);
}

TEST(KlineFeedDriver, ABackfillThatOnlyPartlyWarmsTheEvaluatorIsReportedNotHidden) {
    Rig rig;
    (void)rig.tick(0);
    const auto r = rig.complete_fetch(good(0, 10), 1000);
    EXPECT_TRUE(r.backfill_applied);
    EXPECT_FALSE(r.warmup_complete_after_apply);
    EXPECT_FALSE(rig.evaluator.warmup_complete());
    EXPECT_TRUE(rig.sync.live()) << "live with warm-up incomplete is correct: the planner's gate blocks trading";
}

TEST(KlineFeedDriver, LiveBarsFlowInOrderAcrossManyTicksAndMatchTheReference) {
    Rig rig;
    go_live(rig);
    StreamingEvaluator ref = rig.reference(0, 60);

    for (std::size_t batch = 0; batch < 8; ++batch) {
        rig.push_bars(60 + batch * 5, 5);
        const auto r = rig.tick(2000 + static_cast<std::int64_t>(batch));
        EXPECT_EQ(r.bars_stepped, 5U);
        EXPECT_FALSE(r.backfill_started) << "a live, in-sync feed must not keep refetching history";
    }
    EXPECT_EQ(rig.gate.stats().fetches_started, 1U) << "only the initial backfill";
    ASSERT_EQ(rig.delivered.size(), 40U);
    for (std::size_t i = 0; i < 40; ++i) {
        const KlineWsEvent b = bar(60 + i);
        const double expected = ref.step(hy::Bar{b.open, b.high, b.low, b.close, b.volume});
        ASSERT_EQ(rig.delivered[i].bar.open_time_ms, b.open_time_ms) << "delivery " << i;
        ASSERT_TRUE(same_double(rig.delivered[i].target, expected)) << "delivery " << i;
    }
    EXPECT_TRUE(same_state(rig.evaluator, ref, rig.nodes()));
}

TEST(KlineFeedDriver, TheDrainIsBoundedPerTick) {
    KlineFeedDriverConfig cfg;
    cfg.max_bars_per_tick = 4;
    Rig rig(cfg);
    go_live(rig);

    rig.push_bars(60, 12);
    EXPECT_EQ(rig.tick(2000).bars_stepped, 4U);
    EXPECT_EQ(rig.ring.size_approx(), 8U);
    EXPECT_EQ(rig.tick(2001).bars_stepped, 4U);
    EXPECT_EQ(rig.tick(2002).bars_stepped, 4U);
    EXPECT_EQ(rig.ring.size_approx(), 0U);
    ASSERT_EQ(rig.delivered.size(), 12U);
    for (std::size_t i = 0; i < 12; ++i) EXPECT_EQ(rig.delivered[i].bar.open_time_ms, bar(60 + i).open_time_ms);
}

TEST(KlineFeedDriver, MalformedLiveBarsAreCountedNotDelivered) {
    Rig rig;
    go_live(rig);
    KlineWsEvent poisoned = bar(60);
    poisoned.close = std::numeric_limits<double>::quiet_NaN();
    ASSERT_TRUE(rig.ring.try_push(poisoned));
    rig.push_bars(60, 1);

    const auto r = rig.tick(2000);
    EXPECT_EQ(r.malformed_dropped, 1U);
    EXPECT_EQ(r.bars_stepped, 1U);
    ASSERT_EQ(rig.delivered.size(), 1U);
    EXPECT_EQ(rig.delivered[0].bar.open_time_ms, bar(60).open_time_ms);
}

// --- a hole in the live stream ------------------------------------------------------------------------------------

TEST(KlineFeedDriver, AGapEndsTheDrainKeepsWhatIsLeftInTheRingAndStartsAnotherRecovery) {
    Rig rig;
    go_live(rig);
    rig.push_bars(60, 1);  // fine
    rig.push_bars(62, 1);  // 61 is missing
    rig.push_bars(63, 1);  // must stay in the ring

    const auto r = rig.tick(2000);
    EXPECT_EQ(r.bars_stepped, 1U);
    EXPECT_TRUE(r.gap_detected);
    EXPECT_FALSE(rig.sync.live());
    EXPECT_EQ(rig.ring.size_approx(), 1U) << "the bar behind the gap must wait for the next backfill";

    EXPECT_TRUE(rig.tick(2001).backfill_started) << "no cooldown after a successful fetch: recovery starts at once";
    EXPECT_FALSE(rig.evaluator.warmup_complete());
}

// --- fetch failures & rejected backfills ----------------------------------------------------------------------------

TEST(KlineFeedDriver, AFailedFetchCoolsDownAsTheOutcomeSaysAndThenRetries) {
    Rig rig;
    (void)rig.tick(0);
    const auto r = rig.complete_fetch(failed(60'000), 1000);  // collected at t=1000: cooldown until 61000
    EXPECT_TRUE(r.backfill_collected);
    EXPECT_FALSE(r.backfill_fetch_ok);
    EXPECT_FALSE(r.backfill_applied);
    EXPECT_EQ(rig.driver.stats().backfill_fetch_failures, 1U);
    EXPECT_FALSE(rig.sync.live());

    for (std::int64_t t : {std::int64_t{1001}, std::int64_t{30'000}, std::int64_t{60'999}}) {
        EXPECT_FALSE(rig.tick(t).backfill_started) << "t=" << t;
    }
    EXPECT_EQ(rig.gate.stats().fetches_started, 1U);

    EXPECT_TRUE(rig.tick(61'000).backfill_started);  // the cooldown has elapsed
    const auto ok = rig.complete_fetch(good(0, 60), 61'000);
    EXPECT_TRUE(ok.backfill_applied);
    EXPECT_TRUE(rig.sync.live());
}

TEST(KlineFeedDriver, ABackfillTheSyncRefusesGetsTheDriversOwnCooldown) {
    struct Case {
        const char* what;
        FakeOutcome outcome;
        BackfillApplyStatus expected;
    };
    FakeOutcome holey = good(0, 60);
    holey.data.erase(holey.data.begin() + 30);
    FakeOutcome poisoned = good(0, 60);
    poisoned.data[10].close = std::numeric_limits<double>::quiet_NaN();
    FakeOutcome empty;
    empty.ok_flag = true;

    std::vector<Case> cases;
    cases.push_back({"a hole", holey, BackfillApplyStatus::NotContiguous});
    cases.push_back({"a poisoned bar", poisoned, BackfillApplyStatus::Malformed});
    cases.push_back({"no bars at all", empty, BackfillApplyStatus::Empty});

    for (const Case& c : cases) {
        Rig rig;
        (void)rig.tick(0);
        const auto r = rig.complete_fetch(c.outcome, 1000);
        EXPECT_TRUE(r.backfill_fetch_ok) << c.what;
        EXPECT_FALSE(r.backfill_applied) << c.what;
        EXPECT_EQ(r.apply_status, c.expected) << c.what;
        EXPECT_EQ(rig.driver.stats().backfills_rejected, 1U) << c.what;
        EXPECT_FALSE(rig.sync.live()) << c.what;

        // rejected_backfill_cooldown_ms (5000) from the collect at t=1000
        EXPECT_FALSE(rig.tick(1000 + 5000 - 1).backfill_started) << c.what;
        EXPECT_TRUE(rig.tick(1000 + 5000).backfill_started) << c.what;
    }
}

TEST(KlineFeedDriver, AnOutcomeThatArrivesAfterTheSyncIsAlreadyLiveIsIgnoredWithoutPunishment) {
    Rig rig;
    (void)rig.tick(0);  // a fetch is out
    ASSERT_EQ(rig.sync.apply_backfill(bars(0, 60)).status, BackfillApplyStatus::Applied);  // live by other means

    rig.controlled.plan(good(500, 60));  // would rewind the evaluator if it were applied
    rig.controlled.allow_next();
    const auto r = rig.tick_until_collected(1000);
    EXPECT_TRUE(r.backfill_collected);
    EXPECT_EQ(r.apply_status, BackfillApplyStatus::NotNeeded);
    EXPECT_FALSE(r.backfill_applied);
    EXPECT_EQ(rig.driver.stats().stale_outcomes_ignored, 1U);
    EXPECT_EQ(rig.driver.stats().backfills_rejected, 0U);
    EXPECT_EQ(rig.gate.state(), hy::FetchGateState::Idle) << "a stale outcome must not impose a cooldown";
    EXPECT_TRUE(same_state(rig.evaluator, rig.reference(0, 60), rig.nodes()));
}

TEST(KlineFeedDriver, TheRequestedLimitIsCappedAtBinancesMaximumOrTheConfiguredCap) {
    {
        Rig rig(KlineFeedDriverConfig{}, kOversized);  // warm-up 2000: wants 2003
        (void)rig.tick(0);
        ASSERT_TRUE(wait_until([&] { return rig.controlled.calls.load() == 1; }));
        EXPECT_EQ(rig.controlled.seen()[0].limit, 1000U);
        EXPECT_EQ(rig.controlled.seen()[0].limit, static_cast<std::uint32_t>(hy::kMaxBackfillBars));
    }
    {
        KlineFeedDriverConfig cfg;
        cfg.max_backfill_bars = 40;
        Rig rig(cfg);  // wants 53
        (void)rig.tick(0);
        ASSERT_TRUE(wait_until([&] { return rig.controlled.calls.load() == 1; }));
        EXPECT_EQ(rig.controlled.seen()[0].limit, 40U);
    }
}

// --- the session's own guard ---------------------------------------------------------------------------------------------

TEST(KlineFeedDriver, ASuspendedSessionInvalidatesALiveSyncAndIsResumedOncePerSuspension) {
    Rig rig;
    go_live(rig);
    rig.session().clear_on_resume = false;  // stays suspended, as it would until its strand runs
    rig.session().suspended.store(true);

    const auto first = rig.tick(5000);
    EXPECT_TRUE(first.session_suspended);
    EXPECT_TRUE(first.resume_posted);
    EXPECT_EQ(rig.session().resume_calls.load(), 1);
    EXPECT_TRUE(rig.sync.needs_backfill());
    EXPECT_EQ(rig.sync.stats().session_suspended_invalidations, 1U);
    EXPECT_FALSE(rig.evaluator.warmup_complete());

    // Still suspended: the resume is pending, not repeated every tick.
    for (std::int64_t t : {std::int64_t{5001}, std::int64_t{5500}, std::int64_t{6999}}) {
        EXPECT_FALSE(rig.tick(t).resume_posted) << "t=" << t;
    }
    EXPECT_EQ(rig.session().resume_calls.load(), 1);

    // Unanswered past resume_retry_ms (2000): repeated -- a session that flapped suspended again before
    // this thread ever saw it clear must not be left waiting on a "once" flag forever.
    EXPECT_TRUE(rig.tick(7000).resume_posted);
    EXPECT_EQ(rig.session().resume_calls.load(), 2);

    // It clears; the NEXT suspension gets a fresh resume immediately.
    rig.session().suspended.store(false);
    EXPECT_FALSE(rig.tick(7001).session_suspended);
    rig.session().suspended.store(true);
    EXPECT_TRUE(rig.tick(7002).resume_posted);
    EXPECT_EQ(rig.session().resume_calls.load(), 3);
}

TEST(KlineFeedDriver, ASessionThatClearsOnResumeIsResumedOnceAndTheFeedRecovers) {
    Rig rig;
    go_live(rig);
    rig.session().suspended.store(true);  // clear_on_resume defaults to true

    const auto first = rig.tick(5000);
    EXPECT_TRUE(first.resume_posted);
    EXPECT_TRUE(first.backfill_started) << "resuming early: the fetch starts in the same tick, not after it";

    const auto second = rig.tick(5001);
    EXPECT_FALSE(second.session_suspended);
    EXPECT_FALSE(second.resume_posted);
    EXPECT_EQ(rig.session().resume_calls.load(), 1);

    const auto r = rig.complete_fetch(good(0, 70), 5002);  // history through bar 69
    EXPECT_TRUE(r.backfill_applied);
    EXPECT_TRUE(same_state(rig.evaluator, rig.reference(0, 70), rig.nodes()));
}

TEST(KlineFeedDriver, ASessionSuspendedWhileAlreadyNeedingABackfillIsResumedButNothingIsInvalidated) {
    Rig rig;
    rig.session().suspended.store(true);  // suspended at startup, before the first backfill

    const auto r = rig.tick(0);
    EXPECT_TRUE(r.session_suspended);
    EXPECT_TRUE(r.resume_posted);
    EXPECT_TRUE(r.backfill_started);
    EXPECT_EQ(rig.sync.stats().invalidations, 0U) << "there was nothing live to invalidate";
    EXPECT_EQ(rig.session().resume_calls.load(), 1);
}

TEST(KlineFeedDriver, WithoutACurrentSessionThereIsNothingToResumeAndRecoveryStillRuns) {
    Rig rig;
    rig.supervisor.shutdown();  // current() is now null
    ASSERT_EQ(rig.supervisor.current(), nullptr);

    const auto r = rig.tick(0);
    EXPECT_FALSE(r.session_suspended);
    EXPECT_FALSE(r.resume_posted);
    EXPECT_TRUE(r.backfill_started);
}
