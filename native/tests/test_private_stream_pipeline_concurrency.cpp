// Private-stream migration M5a-2b: the pipeline (private_stream_pipeline.hpp) with its two halves on TWO REAL THREADS -- the arrangement the header supports but today's single-loop
// harnesses do not use. The test thread is the hot/submit thread (supervisor, holder, recovery, readiness, the durable publishers and the real durable store); a second thread
// runs reconcile_tick() (account refresher, order tracker, sweep job, the scripted exchange and account behind the ports). The only things the halves share are the four rings,
// so a data race anywhere else -- a counter, a member read by the wrong half, a ring used by two producers -- is a TSan report; this binary is therefore a `concurrency` test and sits
// in the TSan and ARM64 target lists (the ring protocol itself has its own negative controls).
//
// Two scenarios, both over a REAL durable store on a temp file:
//   * nothing to recover: the barrier asks for a sweep on the hot thread, the reconcile thread answers it (the account step runs after the last answer and before the marker), the
//     hot thread takes the marker and the account update behind it, and the process becomes ready;
//   * an order left unanswered by a crash: the hot thread recovers it and queues it, the reconcile thread asks the exchange about it until the exchange answers, the hot thread makes
//     the resolution durable, applies the fill and releases the slot -- and only then is the process ready.
// Time is the real steady clock, as in a process. A scenario that does not finish in 20 s fails; on a healthy machine each takes well under a second (a few seconds under TSan).
#include <gtest/gtest.h>

#include <hengyuan/private_stream_pipeline.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __linux__
#include <unistd.h>
#endif

using namespace hy;

namespace {

std::int64_t real_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct FakeSession {
    static std::int64_t steady_now_ms() { return real_now_ms(); }
    void start() { started = true; }
    void stop() { stop_flag = true; }
    [[nodiscard]] bool stopped() const { return stop_flag; }
    [[nodiscard]] bool is_connected() const { return connected; }
    [[nodiscard]] std::int64_t last_activity_steady_ms() const { return real_now_ms(); }
    [[nodiscard]] std::uint64_t ring_drops() const { return 0; }
    [[nodiscard]] std::uint64_t lost_events() const { return 0; }

    bool started{false};
    bool stop_flag{false};
    bool connected{false};  // written and read by the hot thread only (the supervisor polls there)
};
static_assert(PipelineSession<FakeSession>);

// What the reconcile thread's ports touch: plain counters, read by the test only after that thread was joined.
struct Ports {
    std::size_t account_calls{0};
    std::size_t queries{0};
    std::size_t answer_from_query{0};  // the exchange answers "filled 10" from this query on; 0 = it is never asked in the scenario
};

AccountFetchOutcome fetch_ok(void* p, AccountSnapshot& out) noexcept {
    ++static_cast<Ports*>(p)->account_calls;
    AccountSnapshotBuilder b;
    b.set_can_trade(true);
    (void)b.add_asset("USDT", 1'000, 0);
    // The snapshot's own timestamp is replaced by the holder (the fetch start); it only has to be a positive number.
    (void)b.build(1'700'000'000'000, out);
    return AccountFetchOutcome::Fetched;
}

QueryResult query_exchange(const OrderExpectation&, void* p) {
    auto* ports = static_cast<Ports*>(p);
    ++ports->queries;
    QueryResult r{};
    if (ports->answer_from_query != 0 && ports->queries >= ports->answer_from_query) {
        r.outcome = QueryOutcome::Found;
        r.confirmed_state = OrderState::Filled;
        r.exchange_order_id = 555;
        r.filled_qty_ticks = 10;
        r.avg_fill_price_ticks = 100;
    } else {
        r.outcome = QueryOutcome::Inconclusive;
    }
    return r;
}

SymbolRules rules_for_test(void*, std::uint32_t symbol_id) noexcept {
    SymbolRules r{};
    if (symbol_id == 1) {
        r.rules_version = 1;
        r.price_scale = 2;
        r.qty_scale = 2;
        r.quote_scale = 4;
    }
    return r;
}

AuditRecord make_intent(const char* coid) {
    AuditRecord rec{};
    rec.timestamp_ms = 1000;
    rec.event_type = AuditEventType::OrderIntentCreated;
    rec.mode = ExecutionMode::Live;
    rec.symbol_id = 1;
    rec.set_client_order_id(coid);
    rec.price_ticks = 100;
    rec.qty_ticks = 10;
    rec.resulting_state = OrderState::Intent;
    rec.side = OrderSide::Buy;
    return rec;
}

AuditRecord make_submitted(const char* coid) {
    AuditRecord rec = make_intent(coid);
    rec.timestamp_ms = 1001;
    rec.event_type = AuditEventType::OrderSubmitted;
    rec.resulting_state = OrderState::Submitting;
    return rec;
}

std::vector<std::byte> bytes_of(const char* text) {
    std::vector<std::byte> k;
    for (const char* p = text; *p != '\0'; ++p) k.push_back(static_cast<std::byte>(*p));
    return k;
}

constexpr std::uint32_t kKeyId = 1;
constexpr std::uint64_t kUid = 123'456'789;

}  // namespace

class PipelineConcurrencyTest : public ::testing::Test {
protected:
    using Pipeline = PrivateStreamPipeline<FakeSession>;

    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        path_ = std::string(tmp) + "hy_pipeline_cc_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter) + ".audit.log";
#else
        path_ = "/tmp/hy_pipeline_cc_" + std::to_string(getpid()) + "_" + std::to_string(counter) + ".audit.log";
#endif
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x70 + i);
        ring_ = std::make_unique<KeyRing>(kek);
        WrappedKeyRecord rec{};
        ASSERT_EQ(ring_->add_key(kKeyId, bytes_of("pipeline-concurrency-key"), rec), KeyRingAddStatus::Ok);
        const ClockPairSample now = fetch_clock_pair();
        ClockOffsetSnapshot s{};
        s.offset_ms = 0;
        s.error_bound_ms = 100;
        s.system_at_fetch_ms = now.system_ms();
        s.steady_at_fetch_ms = now.steady_ms();
        ASSERT_TRUE(clock_.publish(s));
    }

    void TearDown() override {
        pipe_.reset();
        sink_.reset();
        std::error_code ec;
        for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".keyrotations", ".keyrotations.lock", ".keyrotations.tip", ".keyrotations.tip.tmp", ".storeid",
                                   ".storeid.lock", ".storeid.tip", ".storeid.tip.tmp", ".binding", ".binding.lock", ".binding.tip", ".binding.tip.tmp"}) {
            std::filesystem::remove(path_ + suffix, ec);
        }
    }

    void build() {
        sink_ = std::make_unique<DurableAuditSink>(path_, *ring_, kKeyId, kMaxIntentOrphans, AuditSinkOpenMode::Trading);
        ASSERT_TRUE(sink_->is_open());
        PipelineParts parts{*sink_, path_, *ring_, make_durable_order_audit_port(*sink_), audit_, kill_, truth_, in_flight_, fill_, user_events_, clock_};
        parts.query_port = QueryPort{&query_exchange, &ports_};
        parts.account_port = AccountFetchPort{&fetch_ok, &ports_};
        parts.rules = PipelineRulesLookup{&rules_for_test, nullptr};
        pipe_ = std::make_unique<Pipeline>(parts, PipelineConfig{}, [this](std::uint64_t) {
            auto s = std::make_shared<FakeSession>();
            sessions_.push_back(s);
            return s;
        });
        pipe_->set_exchange_info_loaded(true);
    }

    // The test thread is the hot thread; a second thread runs the reconcile half. Returns whether the process became ready in time.
    bool drive(int timeout_ms = 20'000) {
        std::atomic<bool> stop{false};
        std::thread reconcile([&] {
            while (!stop.load(std::memory_order_acquire)) {
                pipe_->reconcile_tick(real_now_ms());
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        });
        bool ok = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            const std::int64_t now = real_now_ms();
            pipe_->hot_tick(now);
            if (!sessions_.empty() && !sessions_.back()->connected) sessions_.back()->connected = true;
            if (pipe_->readiness().authorize(now).ready()) {
                ok = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
        stop.store(true, std::memory_order_release);
        reconcile.join();
        return ok;
    }

    std::string path_;
    std::unique_ptr<KeyRing> ring_;
    std::unique_ptr<DurableAuditSink> sink_;
    AuditRingSink audit_;
    KillSwitch kill_;
    PositionTruth truth_;
    InFlightRegistry in_flight_;
    OrderFillContext fill_;
    UserDataWsEventRing user_events_;
    ClockOffsetPublisher clock_;
    Ports ports_;
    std::vector<std::shared_ptr<FakeSession>> sessions_;
    std::unique_ptr<Pipeline> pipe_;
};

TEST_F(PipelineConcurrencyTest, TheTwoHalvesOnTwoThreadsReachAToken) {
    build();
    ASSERT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, 1'790'000'000'000), EnsureBindingStatus::BoundNew);
    ASSERT_TRUE(drive()) << "not ready in 20 s";
    // The reconcile thread is joined: both halves may be read now.
    EXPECT_EQ(pipe_->readiness().barrier().state(), BarrierState::Reconciled);
    EXPECT_EQ(pipe_->recovery().state(), RunState::Ready);
    EXPECT_GE(pipe_->sweep_job().stats().account_steps, 1U);
    EXPECT_GE(pipe_->holder().refreshed_epoch(), 1U) << "the account the sweep itself fetched crossed the ring behind its marker";
    EXPECT_GE(ports_.account_calls, 2U);
    EXPECT_GE(pipe_->reconcile_ticks(), 1U);
    EXPECT_GE(pipe_->stats().hot_ticks, 1U);
}

TEST_F(PipelineConcurrencyTest, AnUnfinishedOrderIsResolvedByTheReconcileThreadAndMadeDurableByTheHotThread) {
    {
        DurableAuditSink first(path_, *ring_, kKeyId);
        ASSERT_TRUE(first.append_durable(make_intent("HY-A"), 1000).acked());
        ASSERT_TRUE(first.append_durable(make_submitted("HY-A"), 1001).acked());
    }  // "crash"
    ports_.answer_from_query = 3;
    build();
    ASSERT_EQ(sink_->recovered_checkpoints().size(), 1U);
    ASSERT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, 1'790'000'000'000), EnsureBindingStatus::BoundLegacyTestnet);
    ASSERT_TRUE(drive()) << "not ready in 20 s";
    EXPECT_GE(ports_.queries, 3U) << "the reconcile thread asked until the exchange answered";
    EXPECT_FALSE(in_flight_.is_in_flight("HY-A")) << "released by the hot thread once the resolution was durable";
    EXPECT_EQ(truth_.net_qty_ticks(1), 10);
    EXPECT_EQ(pipe_->recovery().state(), RunState::Ready);
    // What the hot thread made durable replays: a restart over the same log finds nothing left to recover.
    pipe_.reset();
    sink_.reset();
    DurableAuditSink restarted(path_, *ring_, kKeyId);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_FALSE(restarted.fenced());
    EXPECT_EQ(restarted.recovered_checkpoints().size(), 0U);
}
