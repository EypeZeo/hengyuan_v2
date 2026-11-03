// Private-stream migration M5a-2b: the composition root of the private stream (private_stream_pipeline.hpp), over a scripted session, a REAL durable store and the real
// barrier / recovery / sweep / publisher objects. Nothing between the store and the readiness decision is mocked except the three things that touch the world: the
// session (scripted connected / not), the exchange behind the query port, and the account behind the account port.
//
// What is pinned, in the order the tests take it:
//   * from nothing to a token: the supervisor starts a generation, the barrier asks for a sweep, the reconcile half answers it (account step included), the hot half
//     takes the marker and the account, StartupRecovery is Ready, and only then does authorize() issue a token -- and the orchestrator, handed the pipeline's context,
//     gets past its readiness gate;
//   * every condition of that is necessary: no store binding, no exchangeInfo, no stream, no account, a stale account, a refused sweep account each keep the answer "no";
//   * the store binding: bound on first use, a mismatch (another account) is a refusal that leaves the process unready, the verdict is re-judged on every call;
//   * recovery: an unfinished order in the log is registered in flight and queued, the pipeline asks the exchange about it, and readiness waits for the answer; an unwired
//     rules lookup fails closed (the state machine degrades);
//   * the wiring: the context the pipeline makes carries every field the private stream needs, and they are the objects the pipeline itself drains with.
#include <gtest/gtest.h>

#include <hengyuan/private_stream_pipeline.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
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

// --- the test's steady clock and a scripted session ---------------------------------------------------------------------------------------

std::int64_t g_now = 1'000'000;

struct FakeSession {
    static std::int64_t steady_now_ms() { return g_now; }
    void start() { started = true; }
    void stop() { stop_flag = true; }
    [[nodiscard]] bool stopped() const { return stop_flag; }
    [[nodiscard]] bool is_connected() const { return connected; }
    [[nodiscard]] std::int64_t last_activity_steady_ms() const { return g_now; }  // a session that is never silent
    [[nodiscard]] std::uint64_t ring_drops() const { return 0; }
    [[nodiscard]] std::uint64_t lost_events() const { return 0; }

    bool started{false};
    bool stop_flag{false};
    bool connected{false};
};
static_assert(PipelineSession<FakeSession>);

// --- the account behind the account port -------------------------------------------------------------------------------------------------------

AccountSnapshot make_snapshot(std::int64_t usdt) {
    AccountSnapshotBuilder b;
    b.set_can_trade(true);
    EXPECT_EQ(b.add_asset("USDT", usdt, 0), AccountSnapshotError::None);
    AccountSnapshot out{};
    EXPECT_EQ(b.build(1'700'000'000'000, out), AccountSnapshotError::None);
    return out;
}

struct FakeAccount {
    std::vector<AccountFetchOutcome> outcomes{AccountFetchOutcome::Fetched};  // the last one repeats
    std::size_t calls{0};

    static AccountFetchOutcome call(void* self, AccountSnapshot& out) noexcept {
        auto* a = static_cast<FakeAccount*>(self);
        const std::size_t i = a->calls < a->outcomes.size() ? a->calls : a->outcomes.size() - 1;
        ++a->calls;
        if (a->outcomes[i] == AccountFetchOutcome::Fetched) out = make_snapshot(1'000);
        return a->outcomes[i];
    }
    [[nodiscard]] AccountFetchPort port() { return AccountFetchPort{&FakeAccount::call, this}; }
};

// --- the exchange behind the query port --------------------------------------------------------------------------------------------------------

struct FakeExchange {
    QueryOutcome outcome{QueryOutcome::Inconclusive};
    OrderState state{OrderState::Filled};
    std::int64_t filled_qty{0};
    int calls{0};
};

QueryResult fake_query(const OrderExpectation& exp, void* user_data) {
    auto* fx = static_cast<FakeExchange*>(user_data);
    ++fx->calls;
    QueryResult r{};
    r.outcome = fx->outcome;
    r.confirmed_state = fx->state;
    r.exchange_order_id = 555;
    r.filled_qty_ticks = fx->filled_qty;
    r.avg_fill_price_ticks = fx->filled_qty > 0 ? 100 : 0;
    (void)exp;
    return r;
}

// Symbol 1 is known (scales 2/2/4); anything else is "unknown symbol" (rules_version == 0).
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

// --- durable-log record builders ----------------------------------------------------------------------------------------------------------------

AuditRecord make_intent(const char* coid, std::int64_t price, std::int64_t qty, std::uint32_t symbol_id) {
    AuditRecord rec{};
    rec.timestamp_ms = 1000;
    rec.event_type = AuditEventType::OrderIntentCreated;
    rec.mode = ExecutionMode::Live;
    rec.symbol_id = symbol_id;
    rec.set_client_order_id(coid);
    rec.price_ticks = price;
    rec.qty_ticks = qty;
    rec.resulting_state = OrderState::Intent;
    rec.side = OrderSide::Buy;
    return rec;
}

AuditRecord make_submitted(const char* coid, std::int64_t price, std::int64_t qty, std::uint32_t symbol_id) {
    AuditRecord rec{};
    rec.timestamp_ms = 1001;
    rec.event_type = AuditEventType::OrderSubmitted;
    rec.mode = ExecutionMode::Live;
    rec.symbol_id = symbol_id;
    rec.set_client_order_id(coid);
    rec.price_ticks = price;
    rec.qty_ticks = qty;
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
constexpr std::int64_t kBoundAt = 1'790'000'000'000;

void publish_fresh_clock(ClockOffsetPublisher& pub) {
    const ClockPairSample now = fetch_clock_pair();
    ClockOffsetSnapshot s{};
    s.offset_ms = 0;
    s.error_bound_ms = 100;
    s.system_at_fetch_ms = now.system_ms();
    s.steady_at_fetch_ms = now.steady_ms();
    ASSERT_TRUE(pub.publish(s));
}

}  // namespace

class PipelineTest : public ::testing::Test {
protected:
    using Pipeline = PrivateStreamPipeline<FakeSession>;

    std::string stem_;
    std::string path_;
    std::vector<std::string> used_paths_;
    std::unique_ptr<KeyRing> ring_;

    // The process-owned parts. The sink is declared before the pipeline so the pipeline is destroyed first.
    std::unique_ptr<DurableAuditSink> sink_;
    AuditRingSink audit_;
    KillSwitch kill_;
    PositionTruth truth_;
    InFlightRegistry in_flight_;
    OrderFillContext fill_;
    UserDataWsEventRing user_events_;
    ClockOffsetPublisher clock_;
    FakeAccount account_;
    FakeExchange exchange_;
    std::vector<std::shared_ptr<FakeSession>> sessions_;
    std::unique_ptr<Pipeline> pipe_;

    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        stem_ = std::string(tmp) + "hy_pipeline_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter);
#else
        stem_ = "/tmp/hy_pipeline_" + std::to_string(getpid()) + "_" + std::to_string(counter);
#endif
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x50 + i);
        ring_ = std::make_unique<KeyRing>(kek);
        WrappedKeyRecord rec{};
        ASSERT_EQ(ring_->add_key(kKeyId, bytes_of("pipeline-test-key-one"), rec), KeyRingAddStatus::Ok);
        fresh_path();
        g_now = 1'000'000;
        publish_fresh_clock(clock_);
    }

    void TearDown() override {
        pipe_.reset();
        sink_.reset();
        std::error_code ec;
        for (const std::string& base : used_paths_) {
            for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".keyrotations", ".keyrotations.lock", ".keyrotations.tip", ".keyrotations.tip.tmp",
                                       ".storeid", ".storeid.lock", ".storeid.tip", ".storeid.tip.tmp", ".binding", ".binding.lock", ".binding.tip",
                                       ".binding.tip.tmp"}) {
                std::filesystem::remove(base + suffix, ec);
            }
        }
    }

    const std::string& fresh_path() {
        path_ = stem_ + "_" + std::to_string(used_paths_.size() + 1) + ".audit.log";
        used_paths_.push_back(path_);
        return path_;
    }

    void open_sink() { sink_ = std::make_unique<DurableAuditSink>(path_, *ring_, kKeyId, kMaxIntentOrphans, AuditSinkOpenMode::Trading); }

    // An order that was submitted and never answered: the log's last frame is Submitting, which a restart recovers as Ambiguous.
    void leave_an_unanswered_order(const char* coid) {
        {
            DurableAuditSink first(path_, *ring_, kKeyId);
            ASSERT_TRUE(first.append_durable(make_intent(coid, 100, 10, 1), 1000).acked());
            ASSERT_TRUE(first.append_durable(make_submitted(coid, 100, 10, 1), 1001).acked());
        }  // "crash"
    }

    void build(PipelineRulesLookup rules = PipelineRulesLookup{&rules_for_test, nullptr}) {
        ASSERT_NE(sink_, nullptr);
        PipelineParts parts{*sink_, path_, *ring_, make_durable_order_audit_port(*sink_), audit_, kill_, truth_, in_flight_, fill_, user_events_, clock_};
        parts.query_port = QueryPort{&fake_query, &exchange_};
        parts.account_port = account_.port();
        parts.rules = rules;
        PipelineConfig config;
        config.exposure_limits.freshness_max_age_ms = 30'000;
        config.reconcile_deadline_ms = 60'000;
        pipe_ = std::make_unique<Pipeline>(parts, config, [this](std::uint64_t) {
            auto s = std::make_shared<FakeSession>();
            sessions_.push_back(s);
            return s;
        });
    }

    // One cycle of both halves at the test clock, hot first (the order of today's single-loop harnesses is: drain, ..., poll_once, drain, supervisor.poll).
    void cycle(std::int64_t advance_ms = 10) {
        g_now += advance_ms;
        pipe_->hot_tick(g_now);
        pipe_->reconcile_tick(g_now);
    }

    void connect() {
        ASSERT_FALSE(sessions_.empty());
        sessions_.back()->connected = true;
    }

    // Runs cycles until the readiness decision says yes, or gives up.
    bool run_until_ready(int max_cycles = 30) {
        for (int i = 0; i < max_cycles; ++i) {
            cycle();
            if (!sessions_.empty() && !sessions_.back()->connected) sessions_.back()->connected = true;
            if (pipe_->readiness().authorize(g_now).ready()) return true;
        }
        return false;
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// from nothing to a token
// ---------------------------------------------------------------------------------------------------------------------------------------------

TEST_F(PipelineTest, FromNothingToATokenStepByStep) {
    open_sink();
    build();
    pipe_->set_exchange_info_loaded(true);
    ASSERT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, kBoundAt), EnsureBindingStatus::BoundNew);

    pipe_->hot_tick(g_now += 10);  // the supervisor starts generation 1; recovery has nothing to recover
    ASSERT_EQ(sessions_.size(), 1U);
    EXPECT_TRUE(sessions_[0]->started);
    EXPECT_EQ(pipe_->readiness().authorize(g_now).reason, NotReadyReason::StartupNotReady) << "the stream is not up, nothing is recovered into Ready";
    EXPECT_EQ(pipe_->recovery().state(), RunState::Reconciling);
    EXPECT_TRUE(pipe_->recovery_applied());
    EXPECT_EQ(pipe_->recovery_result().recovered, 0U);

    sessions_[0]->connected = true;
    pipe_->hot_tick(g_now += 10);  // Connected: the barrier observes it and asks for a sweep
    EXPECT_EQ(pipe_->readiness().barrier().state(), BarrierState::Sweeping);
    EXPECT_EQ(pipe_->sweep_pump().stats().requests_sent, 1U);
    EXPECT_FALSE(pipe_->holder().has_snapshot());

    pipe_->reconcile_tick(g_now += 10);  // the periodic fetch, the sweep request, no orders to ask about: the account step, then the marker
    EXPECT_EQ(pipe_->sweep_job().stats().sweeps_started, 1U);
    EXPECT_EQ(pipe_->sweep_job().stats().account_steps, 1U);
    EXPECT_EQ(pipe_->sweep_job().stats().sweeps_completed, 1U);
    EXPECT_EQ(account_.calls, 2U) << "the periodic fetch and the sweep's own";

    pipe_->hot_tick(g_now += 10);  // the account update and the marker arrive
    EXPECT_TRUE(pipe_->holder().has_snapshot());
    EXPECT_GE(pipe_->holder().refreshed_epoch(), 1U) << "the account the sweep itself fetched";
    EXPECT_EQ(pipe_->readiness().barrier().state(), BarrierState::Reconciled);
    EXPECT_EQ(pipe_->recovery().state(), RunState::Ready);

    const ReadinessResult ready = pipe_->readiness().authorize(g_now);
    ASSERT_TRUE(ready.ready()) << not_ready_reason_name(ready.reason);
    EXPECT_EQ(ready.token->generation(), pipe_->supervisor().generation());
}

TEST_F(PipelineTest, TheOrchestratorGetsPastItsReadinessGateWithThePipelinesContext) {
    open_sink();
    build();
    pipe_->set_exchange_info_loaded(true);
    ASSERT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, kBoundAt), EnsureBindingStatus::BoundNew);

    OrchestratorContext before = pipe_->make_context();
    before.now_ms = g_now;
    const OrchestratorResult refused = orchestrate_submit(before);
    EXPECT_EQ(refused.gate, OrchestratorGate::NotReady) << "nothing is up yet";
    EXPECT_EQ(refused.ready_generation, 0U);

    ASSERT_TRUE(run_until_ready());
    OrchestratorContext ctx = pipe_->make_context();
    ctx.now_ms = g_now;
    const OrchestratorResult result = orchestrate_submit(ctx);
    EXPECT_NE(result.gate, OrchestratorGate::NotReady) << "the readiness gate passed; some later gate (this context carries no order) refuses";
    EXPECT_EQ(result.ready_generation, pipe_->supervisor().generation());
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// every condition is necessary
// ---------------------------------------------------------------------------------------------------------------------------------------------

TEST_F(PipelineTest, WithoutAStoreBindingTheProcessIsNeverReady) {
    open_sink();
    build();
    pipe_->set_exchange_info_loaded(true);  // ... but bind_store() is never called
    EXPECT_FALSE(run_until_ready(40));
    EXPECT_EQ(pipe_->readiness().barrier().state(), BarrierState::Reconciled) << "everything else is in order";
    EXPECT_NE(pipe_->recovery().state(), RunState::Ready);
    EXPECT_FALSE(pipe_->store_bound());
    EXPECT_EQ(pipe_->binding_status(), EnsureBindingStatus::NotRun);
}

TEST_F(PipelineTest, WithoutExchangeInfoNothingIsRecoveredAndNothingIsReady) {
    open_sink();
    build();
    ASSERT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, kBoundAt), EnsureBindingStatus::BoundNew);
    EXPECT_FALSE(run_until_ready(40));
    EXPECT_EQ(pipe_->recovery().state(), RunState::Bootstrapping);
    EXPECT_FALSE(pipe_->recovery_applied());
    pipe_->set_exchange_info_loaded(true);
    EXPECT_TRUE(run_until_ready());
}

TEST_F(PipelineTest, WithoutAStreamNothingIsReady) {
    open_sink();
    build();
    pipe_->set_exchange_info_loaded(true);
    ASSERT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, kBoundAt), EnsureBindingStatus::BoundNew);
    for (int i = 0; i < 20; ++i) cycle();  // the session is started and never connects
    ASSERT_EQ(sessions_.size(), 1U);
    EXPECT_EQ(pipe_->readiness().authorize(g_now).reason, NotReadyReason::StartupNotReady);
    EXPECT_NE(pipe_->recovery().state(), RunState::Ready);
}

TEST_F(PipelineTest, AFailedSweepAccountKeepsTheBarrierSweepingAndTheNextSweepRecovers) {
    open_sink();
    build();
    pipe_->set_exchange_info_loaded(true);
    ASSERT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, kBoundAt), EnsureBindingStatus::BoundNew);
    // The periodic fetch succeeds, the sweep's own is refused by the rate limiter, the next ones succeed.
    account_.outcomes = {AccountFetchOutcome::Fetched, AccountFetchOutcome::NotSent, AccountFetchOutcome::Fetched};
    cycle();
    connect();
    for (int i = 0; i < 6; ++i) cycle();
    EXPECT_EQ(pipe_->sweep_job().stats().account_steps, 1U);
    EXPECT_EQ(pipe_->refresher().stats().not_sent, 1U);
    EXPECT_TRUE(pipe_->holder().has_snapshot()) << "the periodic account is there";
    EXPECT_EQ(pipe_->readiness().barrier().state(), BarrierState::Sweeping) << "a periodic account never stands in for the sweep's";
    EXPECT_FALSE(pipe_->readiness().authorize(g_now).ready());

    ASSERT_TRUE(run_until_ready(400)) << "the barrier asks again under a newer epoch";
    EXPECT_GE(pipe_->sweep_job().stats().account_steps, 2U);
}

TEST_F(PipelineTest, AStaleAccountTakesTheProcessOutOfReadinessAgain) {
    open_sink();
    build();
    pipe_->set_exchange_info_loaded(true);
    ASSERT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, kBoundAt), EnsureBindingStatus::BoundNew);
    ASSERT_TRUE(run_until_ready());
    // No reconcile tick any more: the account is not refreshed. After the 30 s window validate_pre_trade() also applies, the process is not ready.
    for (int i = 0; i < 4; ++i) {
        g_now += 10'000;
        pipe_->hot_tick(g_now);
    }
    const ReadinessResult stale = pipe_->readiness().authorize(g_now);
    EXPECT_FALSE(stale.ready());
    EXPECT_EQ(stale.reason, NotReadyReason::StartupNotReady);
    // The reconcile half comes back: a periodic fetch and the account is fresh again.
    pipe_->reconcile_tick(g_now);
    pipe_->hot_tick(g_now);
    EXPECT_TRUE(pipe_->readiness().authorize(g_now).ready());
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// the store binding
// ---------------------------------------------------------------------------------------------------------------------------------------------

TEST_F(PipelineTest, TheBindingIsJudgedAgainstTheStoreOnEveryCall) {
    open_sink();
    build();
    EXPECT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, kBoundAt), EnsureBindingStatus::BoundNew);
    EXPECT_TRUE(pipe_->store_bound());
    EXPECT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, kBoundAt + 1), EnsureBindingStatus::Verified);
    EXPECT_TRUE(pipe_->store_bound());
    EXPECT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid + 1, kBoundAt + 2), EnsureBindingStatus::AccountMismatch) << "another account";
    EXPECT_FALSE(pipe_->store_bound());
    EXPECT_EQ(pipe_->bind_store(StoreEnvironment::Production, kUid, kBoundAt + 3), EnsureBindingStatus::EnvironmentMismatch);
    EXPECT_FALSE(pipe_->store_bound());
    EXPECT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, kBoundAt + 4), EnsureBindingStatus::Verified) << "the right values are the right values";
    EXPECT_EQ(pipe_->stats().binding_attempts, 5U);
}

TEST_F(PipelineTest, ARefusedBindingKeepsAnOtherwisePerfectProcessOut) {
    open_sink();
    build();
    pipe_->set_exchange_info_loaded(true);
    ASSERT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, kBoundAt), EnsureBindingStatus::BoundNew);
    ASSERT_TRUE(run_until_ready());
    ASSERT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid + 7, kBoundAt + 1), EnsureBindingStatus::AccountMismatch);
    cycle();
    EXPECT_FALSE(pipe_->readiness().authorize(g_now).ready()) << "the account no longer counts as fresh for this store";
}

TEST_F(PipelineTest, AUidIsNotBoundToAStoreThatCannotBeUsed) {
    open_sink();
    build();
    EXPECT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, 0, kBoundAt), EnsureBindingStatus::BadArguments);
    EXPECT_FALSE(pipe_->store_bound());
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// recovery
// ---------------------------------------------------------------------------------------------------------------------------------------------

TEST_F(PipelineTest, AnUnfinishedOrderInTheLogIsRecoveredQueuedAskedAboutAndReadinessWaitsForTheAnswer) {
    leave_an_unanswered_order("HY-A");
    open_sink();
    ASSERT_EQ(sink_->recovered_checkpoints().size(), 1U);
    ASSERT_EQ(sink_->recovered_checkpoints()[0].resulting_state, OrderState::Ambiguous);
    build();
    pipe_->set_exchange_info_loaded(true);
    ASSERT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, kBoundAt), EnsureBindingStatus::BoundLegacyTestnet)
        << "a Testnet store with history and no binding: bound, and it says so";

    exchange_.outcome = QueryOutcome::Inconclusive;  // the exchange does not answer yet
    for (int i = 0; i < 25; ++i) {
        cycle();
        if (!sessions_.empty()) sessions_.back()->connected = true;
    }
    EXPECT_TRUE(pipe_->recovery_applied());
    EXPECT_EQ(pipe_->recovery_result().recovered, 1U);
    EXPECT_TRUE(pipe_->recovery_result().ok());
    EXPECT_TRUE(in_flight_.is_in_flight("HY-A")) << "registered in flight by the recovery";
    EXPECT_GE(exchange_.calls, 1) << "the reconcile half asked about it by client order id";
    EXPECT_EQ(pipe_->recovery().state(), RunState::Reconciling) << "an order the process does not know about is not ready";
    EXPECT_FALSE(pipe_->readiness().authorize(g_now).ready());
    EXPECT_EQ(pipe_->readiness().barrier().state(), BarrierState::Sweeping) << "the sweep asks about every tracked order and has no answer";
    EXPECT_EQ(pipe_->sweep_job().stats().sweeps_completed, 0U);

    // The exchange answers: the order was filled while the process was down.
    exchange_.outcome = QueryOutcome::Found;
    exchange_.state = OrderState::Filled;
    exchange_.filled_qty = 10;
    ASSERT_TRUE(run_until_ready(600));
    EXPECT_FALSE(in_flight_.is_in_flight("HY-A")) << "released once final";
    EXPECT_EQ(truth_.net_qty_ticks(1), 10) << "and the fill is in PositionTruth";
    EXPECT_EQ(pipe_->recovery().state(), RunState::Ready);
}

TEST_F(PipelineTest, AnUnwiredRulesLookupFailsClosedAndTheStateMachineDegrades) {
    leave_an_unanswered_order("HY-A");
    open_sink();
    build(PipelineRulesLookup{});  // no rules: the recovered order cannot be scaled
    pipe_->set_exchange_info_loaded(true);
    ASSERT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, kBoundAt), EnsureBindingStatus::BoundLegacyTestnet);
    EXPECT_FALSE(run_until_ready(60));
    EXPECT_EQ(pipe_->recovery().state(), RunState::Degraded);
    EXPECT_EQ(pipe_->recovery().degraded_reason(), DegradedReason::RepopulateIncomplete);
    EXPECT_FALSE(in_flight_.is_in_flight("HY-A")) << "nothing was registered half-way";
}

TEST_F(PipelineTest, AFencedStoreDegradesTheStateMachineAtOnce) {
    {
        // More unfinished orders than the in-flight registry holds: the recovery scan fences the store at open.
        DurableAuditSink first(path_, *ring_, kKeyId);
        for (std::size_t i = 0; i < kMaxInFlight + 1; ++i) {
            const std::string coid = "HY-F" + std::to_string(i);
            const std::int64_t t = 1000 + 2 * static_cast<std::int64_t>(i);
            ASSERT_TRUE(first.append_durable(make_intent(coid.c_str(), 100, 10, 1), t).acked());
            ASSERT_TRUE(first.append_durable(make_submitted(coid.c_str(), 100, 10, 1), t + 1).acked());
        }
    }
    open_sink();
    ASSERT_TRUE(sink_->fenced());
    build();
    pipe_->set_exchange_info_loaded(true);
    EXPECT_EQ(pipe_->bind_store(StoreEnvironment::Testnet, kUid, kBoundAt), EnsureBindingStatus::StoreNotUsable) << "a fenced store is not bound";
    for (int i = 0; i < 5; ++i) cycle();
    EXPECT_EQ(pipe_->recovery().state(), RunState::Degraded);
    EXPECT_EQ(pipe_->recovery().degraded_reason(), DegradedReason::SinkFenced) << "the pipeline reads the sink's own verdict";
    EXPECT_FALSE(pipe_->readiness().authorize(g_now).ready());
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// the wiring
// ---------------------------------------------------------------------------------------------------------------------------------------------

TEST_F(PipelineTest, TheContextCarriesTheWholePrivateStreamWiring) {
    open_sink();
    build();
    const OrchestratorContext ctx = pipe_->make_context();
    EXPECT_EQ(ctx.audit, &audit_);
    EXPECT_EQ(ctx.kill_switch, &kill_) << "the ONE breaker";
    EXPECT_EQ(ctx.in_flight, &in_flight_);
    EXPECT_TRUE(ctx.durable_audit.is_valid());
    EXPECT_NE(ctx.to_reconcile, nullptr);
    EXPECT_NE(ctx.reconcile_events, nullptr);
    EXPECT_NE(ctx.reconcile_durable, nullptr) << "the ring is never drained memory-first";
    EXPECT_EQ(ctx.user_data_events, &user_events_);
    EXPECT_NE(ctx.user_data_durable, nullptr) << "a context with a user-data ring and a durable sink must carry the durable publisher";
    EXPECT_EQ(ctx.late_fill, &pipe_->late_fill());
    EXPECT_EQ(ctx.readiness, &pipe_->readiness());
    EXPECT_EQ(ctx.sweep, &pipe_->sweep_pump());
    EXPECT_EQ(ctx.position_truth, &truth_);
    EXPECT_EQ(ctx.fill_context, &fill_);
    EXPECT_EQ(ctx.account, &pipe_->holder().view()) << "wired once; the holder updates the snapshot behind it";
    EXPECT_EQ(ctx.exposure_limits.freshness_max_age_ms, 30'000);
    EXPECT_TRUE(pipe_->late_fill().complete());
    EXPECT_EQ(pipe_->late_fill().watched(), 0U) << "no operator has resolved an order in this store";
}

TEST_F(PipelineTest, TwoContextsAreTheSameWiring) {
    open_sink();
    build();
    const OrchestratorContext a = pipe_->make_context();
    const OrchestratorContext b = pipe_->make_context();
    EXPECT_EQ(a.readiness, b.readiness);
    EXPECT_EQ(a.sweep, b.sweep);
    EXPECT_EQ(a.reconcile_durable, b.reconcile_durable);
    EXPECT_EQ(a.user_data_durable, b.user_data_durable);
    EXPECT_EQ(a.to_reconcile, b.to_reconcile);
}

TEST_F(PipelineTest, TheTicksAreCountedAndTheHalvesAreIndependent) {
    open_sink();
    build();
    pipe_->hot_tick(g_now += 5);
    pipe_->hot_tick(g_now += 5);
    EXPECT_EQ(pipe_->stats().hot_ticks, 2U);
    EXPECT_EQ(pipe_->stats().reconcile_ticks, 0U);
    EXPECT_EQ(account_.calls, 0U) << "the hot half never touches the account port";
    pipe_->reconcile_tick(g_now += 5);
    EXPECT_EQ(pipe_->stats().reconcile_ticks, 1U);
    EXPECT_EQ(account_.calls, 1U) << "the first reconcile tick fetches at once";
    EXPECT_EQ(sessions_.size(), 1U) << "the reconcile half starts no session";
}
