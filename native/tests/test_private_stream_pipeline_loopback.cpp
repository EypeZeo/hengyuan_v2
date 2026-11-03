// Private-stream migration M5a-2b: the pipeline (private_stream_pipeline.hpp) over the REAL WebSocket API session, its REAL factory and the REAL supervisor, against a LOCAL TLS
// WebSocket server (tests/test_helpers/ws_loopback_server.hpp) -- the whole private stream minus the exchange. The durable store is real too (a temp file), and the
// barrier, recovery, sweep, account path and publishers are the production objects. What is scripted: the answers of the server (it is not Binance: what a subscribe
// answer looks like is this file's responder, built from the shapes the official docs print), the exchange behind the query port, and the account behind the account port
// (the REST client has its own loopback tests, test_rest_account_fetcher.cpp).
//
// What it shows that the scripted-session tests cannot:
//   * the factory's signed subscribe request really goes out over TLS and the two answers (subscribe, session.subscriptions) really make the session "connected" in the
//     sense the barrier needs, so a token is issued on a stream that exists;
//   * a connection the server drops takes the process out of readiness at once (the barrier is Down), and the supervisor's next generation -- signed again, a new
//     subscription -- is swept again and brings it back, on a new generation number;
//   * an execution report that arrives on the wire reaches the user-data ring and is taken by the pipeline's drain.
//
// The test thread plays the hot/submit thread AND (like today's single-loop harnesses) the reconcile thread: it binds the synthetic credentials, owns the rate-limit
// tracker, polls the supervisor and ticks both halves. The io_context thread is the rig's. Teardown order: everything a session points at is declared BEFORE the rig.
#include <gtest/gtest.h>

#include <hengyuan/binance_clock_sync.hpp>
#include <hengyuan/binance_ws_api_codec.hpp>
#include <hengyuan/binance_ws_api_session_factory.hpp>
#include <hengyuan/private_stream_pipeline.hpp>
#include <hengyuan/spot_rate_limit_budget.hpp>
#include <hengyuan/transport_policy.hpp>

#include "test_helpers/bound_test_credentials.hpp"
#include "test_helpers/ws_client_rig.hpp"
#include "test_helpers/ws_loopback_server.hpp"
#include "ws_api_official_samples.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
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

namespace {

namespace samples = hy::test_samples;
using hy::BinanceWsApiUserDataSession;
using hy::test_helpers::fixture_path;
using hy::test_helpers::wait_until;
using hy::test_helpers::WsLoopbackServer;
using Session = BinanceWsApiUserDataSession;
using Pipeline = hy::PrivateStreamPipeline<Session>;

static_assert(hy::PipelineSession<Session>, "the real session is what the pipeline is for");

constexpr const char* kTarget = "/ws-api/v3";
constexpr std::uint32_t kKeyId = 1;
constexpr std::uint64_t kUid = 354'937'868;
constexpr std::int64_t kBoundAt = 1'790'000'000'000;

// --- what the server says -----------------------------------------------------------------------------------------

std::uint64_t id_of(const std::string& frame) {
    const auto p = frame.find("\"id\":");
    return p == std::string::npos ? 0U : std::strtoull(frame.c_str() + p + 5, nullptr, 10);
}
bool is_subscribe(const std::string& frame) { return frame.find("userDataStream.subscribe.signature") != std::string::npos; }
bool is_verify(const std::string& frame) { return frame.find("session.subscriptions") != std::string::npos; }
std::string ok_answer(std::uint64_t id, int subscription_id) {
    return R"({"id":)" + std::to_string(id) + R"(,"status":200,"result":{"subscriptionId":)" + std::to_string(subscription_id) + "}}";
}
std::string list_answer(std::uint64_t id, int subscription_id) {
    return R"({"id":)" + std::to_string(id) + R"(,"status":200,"result":[{"subscriptionId":)" + std::to_string(subscription_id) + "}]}";
}

hy::FeedSupervisorPolicy fast_policy() {
    hy::FeedSupervisorPolicy p;
    p.initial_backoff_ms = 10;
    p.max_backoff_ms = 50;
    p.jitter_percent = 0;
    p.connect_deadline_ms = 5000;
    p.drain_timeout_ms = 100;
    p.stable_after_ms = 60'000;
    return p;
}

// --- the account and the exchange behind the ports --------------------------------------------------------------------------------

hy::AccountSnapshot make_snapshot() {
    hy::AccountSnapshotBuilder b;
    b.set_can_trade(true);
    EXPECT_EQ(b.add_asset("USDT", 1'000, 0), hy::AccountSnapshotError::None);
    hy::AccountSnapshot out{};
    EXPECT_EQ(b.build(1'700'000'000'000, out), hy::AccountSnapshotError::None);
    return out;
}

hy::AccountFetchOutcome fetch_ok(void* calls, hy::AccountSnapshot& out) noexcept {
    ++*static_cast<std::size_t*>(calls);
    out = make_snapshot();
    return hy::AccountFetchOutcome::Fetched;
}

hy::QueryResult query_nothing(const hy::OrderExpectation&, void*) { return hy::QueryResult{}; }

hy::SymbolRules rules_for_test(void*, std::uint32_t symbol_id) noexcept {
    hy::SymbolRules r{};
    if (symbol_id == 1) {
        r.rules_version = 1;
        r.price_scale = 2;
        r.qty_scale = 2;
        r.quote_scale = 4;
    }
    return r;
}

std::vector<std::byte> bytes_of(const char* text) {
    std::vector<std::byte> k;
    for (const char* p = text; *p != '\0'; ++p) k.push_back(static_cast<std::byte>(*p));
    return k;
}

}  // namespace

class PipelineLoopbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        path_ = std::string(tmp) + "hy_pipeline_lb_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter) + ".audit.log";
#else
        path_ = "/tmp/hy_pipeline_lb_" + std::to_string(getpid()) + "_" + std::to_string(counter) + ".audit.log";
#endif
        std::array<std::byte, hy::kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x60 + i);
        key_ring_ = std::make_unique<hy::KeyRing>(kek);
        hy::WrappedKeyRecord rec{};
        ASSERT_EQ(key_ring_->add_key(kKeyId, bytes_of("pipeline-loopback-key"), rec), hy::KeyRingAddStatus::Ok);
        sink_ = std::make_unique<hy::DurableAuditSink>(path_, *key_ring_, kKeyId, hy::kMaxIntentOrphans, hy::AuditSinkOpenMode::Trading);
        ASSERT_TRUE(sink_->is_open());

        creds_ = std::make_unique<hy::test_helpers::BoundTestCredentials>("pipelb");
        ASSERT_TRUE(creds_->ok());
        ASSERT_TRUE(limiter_.configure(6000, 500, 6000, 500, 50, 5));
        policy_.ws_api_allowlist = hy::EndpointAllowlist{};
        policy_.ws_api_allowlist.hosts[0] = "127.0.0.1";
        policy_.ws_api_allowlist.count = 1;
        server_ = std::make_unique<WsLoopbackServer>(fixture_path("test_leaf_cert_loopback.pem"), fixture_path("test_leaf_key_loopback.pem"),
                                                     WsLoopbackServer::UpgradeBehavior::Answer);
        server_->set_responder([](const std::string&, const std::string& frame) -> std::vector<std::string> {
            if (is_subscribe(frame)) return {ok_answer(id_of(frame), 0)};
            if (is_verify(frame)) return {list_answer(id_of(frame), 0)};
            return {};
        });
        const hy::ClockPairSample now = hy::fetch_clock_pair();
        hy::ClockOffsetSnapshot s{};
        s.offset_ms = 0;
        s.error_bound_ms = 5;
        s.system_at_fetch_ms = now.system_ms();
        s.steady_at_fetch_ms = now.steady_ms();
        ASSERT_TRUE(clock_.publish(s));

        factory_ = std::make_unique<hy::WsApiSessionFactory>(
            hy::WsApiSessionFactory::Deps{rig_.ioc, rig_.ssl_ctx, user_events_, codec_, creds_->get(), clock_, limiter_},
            hy::WsApiFeedEndpoint{policy_, "127.0.0.1", std::to_string(server_->port()), kTarget});

        hy::PipelineParts parts{*sink_, path_, *key_ring_, hy::make_durable_order_audit_port(*sink_), audit_, kill_, truth_, in_flight_, fill_, user_events_, clock_};
        parts.query_port = hy::QueryPort{&query_nothing, nullptr};
        parts.account_port = hy::AccountFetchPort{&fetch_ok, &account_calls_};
        parts.rules = hy::PipelineRulesLookup{&rules_for_test, nullptr};
        hy::PipelineConfig config;
        config.feed = fast_policy();
        pipe_ = std::make_unique<Pipeline>(parts, config, [this](std::uint64_t generation) { return (*factory_)(generation); });
        pipe_->set_exchange_info_loaded(true);
        ASSERT_EQ(pipe_->bind_store(hy::StoreEnvironment::Testnet, kUid, kBoundAt), hy::EnsureBindingStatus::BoundNew);
    }

    void TearDown() override {
        if (pipe_) pipe_->supervisor().shutdown();
        rig_.stop_and_join();  // a failed ASSERT may have left a session connected
        pipe_.reset();
        sink_.reset();
        std::error_code ec;
        for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".keyrotations", ".keyrotations.lock", ".keyrotations.tip", ".keyrotations.tip.tmp", ".storeid",
                                   ".storeid.lock", ".storeid.tip", ".storeid.tip.tmp", ".binding", ".binding.lock", ".binding.tip", ".binding.tip.tmp"}) {
            std::filesystem::remove(path_ + suffix, ec);
        }
    }

    // Ticks both halves on the test thread until `pred` holds.
    template <typename Pred>
    bool run_until(Pred pred, int timeout_ms = 8000) {
        return wait_until(
            [&] {
                const std::int64_t now = Session::steady_now_ms();
                pipe_->hot_tick(now);
                pipe_->reconcile_tick(now);
                return pred();
            },
            timeout_ms);
    }

    bool ready() { return pipe_->readiness().authorize(Session::steady_now_ms()).ready(); }

    std::string path_;
    std::unique_ptr<hy::KeyRing> key_ring_;
    std::unique_ptr<hy::DurableAuditSink> sink_;
    hy::AuditRingSink audit_;
    hy::KillSwitch kill_;
    hy::PositionTruth truth_;
    hy::InFlightRegistry in_flight_;
    hy::OrderFillContext fill_;
    std::unique_ptr<hy::test_helpers::BoundTestCredentials> creds_;  // bound to THIS thread: the hot thread
    hy::SpotRateLimitTracker limiter_;
    hy::ClockOffsetPublisher clock_;
    hy::TransportPolicy policy_;
    std::unique_ptr<WsLoopbackServer> server_;
    std::size_t account_calls_{0};
    hy::UserDataWsEventRing user_events_;  // before the rig: the rig, destroyed first, joins the I/O thread
    hy::WsApiCodec codec_;
    hy::test_helpers::WsClientRig rig_;
    std::unique_ptr<hy::WsApiSessionFactory> factory_;
    std::unique_ptr<Pipeline> pipe_;
};

TEST_F(PipelineLoopbackTest, ARealSignedSubscriptionOverTlsEndsInAToken) {
    ASSERT_TRUE(run_until([&] { return ready(); })) << "stage: barrier " << static_cast<int>(pipe_->readiness().barrier().state()) << ", recovery "
                                                    << hy::run_state_name(pipe_->recovery().state());
    EXPECT_EQ(pipe_->supervisor().generation(), 1U);
    EXPECT_EQ(factory_->stats().created, 1U);
    EXPECT_EQ(pipe_->readiness().barrier().state(), hy::BarrierState::Reconciled);
    EXPECT_EQ(pipe_->recovery().state(), hy::RunState::Ready);

    const std::vector<std::string> frames = server_->received_frames();
    ASSERT_EQ(frames.size(), 2U) << "the signed subscribe request and the session.subscriptions request";
    EXPECT_TRUE(is_subscribe(frames[0]));
    EXPECT_TRUE(is_verify(frames[1]));
    EXPECT_EQ(frames[0].find("signature") == std::string::npos, false) << "the subscribe request is signed";
    EXPECT_EQ(frames[1].find("signature"), std::string::npos) << "the listing request is not";

    const hy::ReadinessResult token = pipe_->readiness().authorize(Session::steady_now_ms());
    ASSERT_TRUE(token.ready());
    EXPECT_EQ(token.token->generation(), 1U);
    EXPECT_GE(account_calls_, 2U) << "the periodic fetch and the sweep's own";
}

TEST_F(PipelineLoopbackTest, ADroppedConnectionTakesTheProcessOutAndTheNextGenerationBringsItBack) {
    ASSERT_TRUE(run_until([&] { return ready(); }));
    ASSERT_EQ(pipe_->supervisor().generation(), 1U);
    server_->drop_all();
    ASSERT_TRUE(run_until([&] { return !ready(); })) << "a dropped connection is not a connected stream";
    EXPECT_EQ(pipe_->readiness().barrier().state(), hy::BarrierState::Down) << "the barrier saw the stream go";
    EXPECT_EQ(pipe_->readiness().authorize(Session::steady_now_ms()).reason, hy::NotReadyReason::StartupNotReady)
        << "StartupRecovery's live prerequisites are judged first: the stream is no longer connected (the barrier's Down is the second line)";

    ASSERT_TRUE(run_until([&] { return ready(); })) << "generation " << pipe_->supervisor().generation();
    const hy::ReadinessResult token = pipe_->readiness().authorize(Session::steady_now_ms());
    ASSERT_TRUE(token.ready());
    EXPECT_GE(token.token->generation(), 2U) << "a token rests on the generation that was swept, not on the one that died";
    EXPECT_EQ(factory_->stats().created, pipe_->supervisor().generation());
    EXPECT_GE(pipe_->sweep_job().stats().sweeps_completed, 2U) << "every generation is swept before it is trusted";
    const std::vector<std::string> subscribes = [&] {
        std::vector<std::string> v;
        for (const std::string& f : server_->received_frames()) {
            if (is_subscribe(f)) v.push_back(f);
        }
        return v;
    }();
    ASSERT_GE(subscribes.size(), 2U);
    EXPECT_NE(subscribes[0], subscribes[1]) << "the second generation signs again: a new timestamp, a new request id, never a replay";
}

TEST_F(PipelineLoopbackTest, AnExecutionReportOnTheWireIsTakenByThePipelinesDrain) {
    ASSERT_TRUE(run_until([&] { return ready(); }));
    const std::shared_ptr<Session> session = pipe_->supervisor().current();
    ASSERT_NE(session, nullptr);
    // Not ticking while the frame travels: the point is that the ring the session fills is the ring the pipeline drains.
    server_->send_text(std::string(samples::kEventExecutionReport));
    ASSERT_TRUE(wait_until([&] { return session->stats_snapshot().events_delivered >= 1U; }));
    pipe_->hot_tick(Session::steady_now_ms());
    hy::UserDataWsEvent leftover{};
    EXPECT_FALSE(user_events_.try_pop(leftover)) << "the hot tick drained what the session pushed";
    // The report is about an order this process does not hold: nothing to apply, nothing to alarm, and the stream stays trusted.
    EXPECT_TRUE(ready());
    EXPECT_EQ(kill_.state(), hy::KillState::Normal);
}
