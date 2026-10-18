// Private-stream migration M2: the REAL WebSocket API user-data session (binance_ws_api_user_data_session.hpp), its hot-thread factory
// (binance_ws_api_session_factory.hpp) and PublicFeedSupervisor against a LOCAL TLS WebSocket server (tests/test_helpers/ws_loopback_server.hpp).
//
// Everything is local: the server binds 127.0.0.1:0, the client connects to host "127.0.0.1" and verifies its certificate
// (test_leaf_cert_loopback.pem, SAN IP:127.0.0.1) against the same file used as a trust anchor, so the REAL TLS + hostname-verification +
// WebSocket-upgrade code runs, minus the network. The server is not Binance: what an "answer to the subscribe request" looks like is
// entirely each test's responder, built from the shapes the official docs print (blueprint 5.7 P-25, P-28, P-29; the event fixtures are the
// docs' own examples, tests/ws_api_official_samples.hpp).
//
// The test thread plays the hot/submit thread: it binds the (synthetic) credentials, publishes the clock, owns the rate-limit tracker and
// polls the supervisor. The io_context thread is the rig's.
//
// Teardown order: everything a session points at (ring, codec) is declared BEFORE the rig, so the rig -- destroyed first -- has already joined
// the I/O thread when they go.

#include <gtest/gtest.h>
#include <hengyuan/binance_clock_sync.hpp>
#include <hengyuan/binance_ws_api_codec.hpp>
#include <hengyuan/binance_ws_api_session_factory.hpp>
#include <hengyuan/binance_ws_api_signing.hpp>
#include <hengyuan/binance_ws_api_user_data_session.hpp>
#include <hengyuan/public_feed_supervisor.hpp>
#include <hengyuan/spot_rate_limit_budget.hpp>
#include <hengyuan/transport_policy.hpp>

#include "binance_clock_sync_test_hooks.hpp"
#include "test_helpers/bound_test_credentials.hpp"
#include "test_helpers/ws_client_rig.hpp"
#include "test_helpers/ws_loopback_server.hpp"
#include "ws_api_official_samples.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace samples = hy::test_samples;
using hy::BinanceWsApiUserDataSession;
using hy::UserDataEventKind;
using hy::UserDataWsEvent;
using hy::WsApiSessionEnd;
using hy::test_helpers::fixture_path;
using hy::test_helpers::steady_ms;
using hy::test_helpers::wait_until;
using hy::test_helpers::WsLoopbackServer;
using Session = BinanceWsApiUserDataSession;
using Supervisor = hy::PublicFeedSupervisor<Session>;

constexpr const char* kTarget = "/ws-api/v3";

std::unique_ptr<WsLoopbackServer> make_server(WsLoopbackServer::UpgradeBehavior behavior = WsLoopbackServer::UpgradeBehavior::Answer) {
    return std::make_unique<WsLoopbackServer>(fixture_path("test_leaf_cert_loopback.pem"), fixture_path("test_leaf_key_loopback.pem"),
                                              behavior);
}

std::int64_t system_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

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

// The answer to session.subscriptions: a list with one entry per subscription the connection holds (the official example, P-25).
std::string list_answer(std::uint64_t id, const std::vector<int>& subscription_ids) {
    std::string s = R"({"id":)" + std::to_string(id) + R"(,"status":200,"result":[)";
    for (std::size_t i = 0; i < subscription_ids.size(); ++i) {
        if (i > 0) s += ',';
        s += R"({"subscriptionId":)" + std::to_string(subscription_ids[i]) + "}";
    }
    return s + "]}";
}

std::size_t count_frames(const std::vector<std::string>& frames, bool (*is_kind)(const std::string&)) {
    std::size_t n = 0;
    for (const std::string& f : frames) {
        if (is_kind(f)) ++n;
    }
    return n;
}

std::vector<std::string> only(const std::vector<std::string>& frames, bool (*is_kind)(const std::string&)) {
    std::vector<std::string> out;
    for (const std::string& f : frames) {
        if (is_kind(f)) out.push_back(f);
    }
    return out;
}

std::string error_answer(std::uint64_t id, int status, int code, std::int64_t retry_after_ms) {
    std::string s = R"({"id":)" + std::to_string(id) + R"(,"status":)" + std::to_string(status) + R"(,"error":{"code":)" + std::to_string(code) +
                    R"(,"msg":"refused by the test server")";
    if (retry_after_ms > 0) s += R"(,"data":{"serverTime":1,"retryAfter":)" + std::to_string(retry_after_ms) + "}";
    return s + "}}";
}

std::string with_subscription(std::string_view sample, int subscription_id) {
    std::string s(sample);
    const std::string from = R"("subscriptionId":0)";
    const auto p = s.find(from);
    EXPECT_NE(p, std::string::npos);
    if (p != std::string::npos) s.replace(p, from.size(), R"("subscriptionId":)" + std::to_string(subscription_id));
    return s;
}

constexpr const char* kServerShutdown = R"({"event":{"e":"serverShutdown","E":1728973001334}})";

// --- the fixture --------------------------------------------------------------------------------------------------

// What a test can vary about a session.
struct Opts {
    bool deposit{true};
    std::int64_t signed_age_ms{0};  // how long ago (steady clock) the frame was "signed"
    std::uint32_t connect_weight{2};
    std::uint32_t subscribe_weight{2};
    std::uint32_t verify_weight{2};
    std::string port;  // empty = the fixture's server
    bool empty_target{false};
    hy::WsApiUserDataSessionConfig cfg{};
};

class WsApiSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        creds_ = std::make_unique<hy::test_helpers::BoundTestCredentials>("wsapisess");
        ASSERT_TRUE(creds_->ok());
        ASSERT_TRUE(limiter_.configure(6000, 500, 6000, 500, 50, 5));
        policy_.ws_api_allowlist = hy::EndpointAllowlist{};
        policy_.ws_api_allowlist.hosts[0] = "127.0.0.1";
        policy_.ws_api_allowlist.count = 1;
        server_ = make_server();
        answer_subscribe_with_success();
        publish_fresh_clock();
    }

    void TearDown() override {
        rig_.stop_and_join();  // a failed ASSERT may have left a session connected
    }

    void publish_fresh_clock() {
        const hy::ClockPairSample now = hy::fetch_clock_pair();
        hy::ClockOffsetSnapshot s{};
        s.offset_ms = 0;
        s.error_bound_ms = 5;
        s.system_at_fetch_ms = now.system_ms();
        s.steady_at_fetch_ms = now.steady_ms();
        ASSERT_TRUE(clock_.publish(s));
    }

    // The server of a healthy account: it confirms the subscription and, asked, lists exactly that one.
    void answer_subscribe_with_success(int subscription_id = 0) {
        server_->set_responder([subscription_id](const std::string&, const std::string& frame) -> std::vector<std::string> {
            if (is_subscribe(frame)) return {ok_answer(id_of(frame), subscription_id)};
            if (is_verify(frame)) return {list_answer(id_of(frame), {subscription_id})};
            return {};
        });
    }

    // A server that confirms the subscription and then answers session.subscriptions with `verify_reply(request id)`.
    template <typename Reply>
    void answer_verify_with(Reply verify_reply) {
        server_->set_responder([verify_reply](const std::string&, const std::string& frame) -> std::vector<std::string> {
            if (is_subscribe(frame)) return {ok_answer(id_of(frame), 0)};
            if (is_verify(frame)) return verify_reply(id_of(frame));
            return {};
        });
    }

    std::shared_ptr<Session> make_session(std::uint64_t generation, Opts o = Opts{}) {
        const auto permit = hy::issue_ws_endpoint_permit(policy_, "127.0.0.1");
        if (!permit) {
            ADD_FAILURE() << "no endpoint permit for 127.0.0.1";
            return nullptr;
        }
        const auto connect = limiter_.reserve_weight_only_explicit(hy::RateLimitLane::Reconciliation, o.connect_weight);
        if (!connect) {
            ADD_FAILURE() << "the test budget refused the connect weight";
            return nullptr;
        }
        const auto verify = limiter_.reserve_weight_only_explicit(hy::RateLimitLane::Reconciliation, o.verify_weight);
        if (!verify) {
            ADD_FAILURE() << "the test budget refused the verify weight";
            return nullptr;
        }
        o.cfg.port = o.port.empty() ? std::to_string(server_->port()) : o.port;
        o.cfg.target = o.empty_target ? "" : kTarget;
        auto session = std::make_shared<Session>(rig_.ioc, rig_.ssl_ctx, ring_, codec_, *permit, *connect, *verify, generation, o.cfg);
        if (o.deposit) {
            const Deposit d = sign_and_deposit(*session, o.signed_age_ms, o.subscribe_weight);
            if (o.subscribe_weight >= 2) {
                EXPECT_TRUE(d.ok);
            }
        }
        return session;
    }

    struct Deposit {
        bool ok{false};
        std::string frame;
        std::uint64_t request_id{0};
    };

    Deposit sign_and_deposit(Session& session, std::int64_t signed_age_ms = 0, std::uint32_t subscribe_weight = 2) {
        Deposit d;
        const auto sub = limiter_.reserve_weight_only_explicit(hy::RateLimitLane::Reconciliation, subscribe_weight);
        if (!sub) {
            ADD_FAILURE() << "the test budget refused the subscribe weight";
            return d;
        }
        d.request_id = next_request_id_++;
        auto [err, req] = hy::build_subscribe_signature_request(creds_->get(), d.request_id,
                                                                 hy::ClockReadyTokenTestHooks::make(system_now_ms()), 0);
        if (err != hy::WsApiSigningError::Ok) {
            ADD_FAILURE() << "signing failed";
            return d;
        }
        d.frame = std::string(req.frame());
        last_frame_ = d.frame;
        d.ok = session.deposit_subscribe(req, *sub, Session::steady_now_ms() - signed_age_ms);
        return d;
    }

    std::vector<UserDataWsEvent> drain_ring() {
        std::vector<UserDataWsEvent> v;
        UserDataWsEvent ev{};
        while (ring_.try_pop(ev)) v.push_back(ev);
        return v;
    }

    bool wait_for_events(std::size_t n, std::vector<UserDataWsEvent>& out, int timeout_ms = 5000) {
        return wait_until(
            [&] {
                for (const UserDataWsEvent& e : drain_ring()) out.push_back(e);
                return out.size() >= n;
            },
            timeout_ms);
    }

    static bool wait_stopped(const Session& s, int timeout_ms = 5000) {
        return wait_until([&] { return s.stopped(); }, timeout_ms);
    }

    // Spins instead of sleeping: the point is to look at the very moment stopped() turns true.
    static bool spin_until_stopped(const Session& s, int timeout_ms = 5000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!s.stopped()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
        }
        return true;
    }

    static std::string diagnostics(const Session& s) {
        const auto st = s.stats_snapshot();
        return "stage='" + st.last_error_stage + "' ec=" + st.last_error_ec.message() + " errors=" + std::to_string(st.errors) +
               " handshake=" + std::to_string(s.handshake_done()) + " subscribed=" + std::to_string(s.is_connected()) +
               " stopped=" + std::to_string(s.stopped());
    }

    // The Reconciliation lane holds 10% of (6000 - 500) = 550 weight (SetUp's configure). A refused factory attempt must have left all of it:
    // reserving the whole lane afterwards is the black-box proof that nothing was spent. (Consumes it, so call it last.)
    bool reconciliation_lane_untouched() {
        return limiter_.reserve_weight_only_explicit(hy::RateLimitLane::Reconciliation, 550).has_value();
    }

    // Stop and wait out a session at the end of a test. A stopped session has closed its transport: the server sees the connection go.
    void finish(Session& s) {
        s.stop();
        EXPECT_TRUE(wait_stopped(s));
        EXPECT_TRUE(wait_until([&] { return server_->open_connections() == 0U; })) << "the session stopped but left its connection open";
    }

    std::unique_ptr<hy::test_helpers::BoundTestCredentials> creds_;  // bound to THIS thread: the hot thread
    hy::SpotRateLimitTracker limiter_;
    hy::ClockOffsetPublisher clock_;
    hy::TransportPolicy policy_;
    std::unique_ptr<WsLoopbackServer> server_;
    std::uint64_t next_request_id_{1};
    std::string last_frame_;
    hy::UserDataWsEventRing ring_;  // before the rig: the rig, destroyed first, joins the I/O thread
    hy::WsApiCodec codec_;
    hy::test_helpers::WsClientRig rig_;
};

}  // namespace

// --- the happy path -------------------------------------------------------------------------------------------------

TEST_F(WsApiSessionTest, SubscribesOnceAndDeliversEventsStampedWithTheGenerationAndTheSubscription) {
    auto session = make_session(7);
    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(session->is_connected());
    EXPECT_EQ(session->subscription_id(), -1);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); })) << diagnostics(*session);
    EXPECT_TRUE(session->handshake_done());
    EXPECT_EQ(session->subscription_id(), 0);
    EXPECT_EQ(session->generation(), 7U);
    ASSERT_EQ(server_->targets().size(), 1U);
    EXPECT_EQ(server_->targets()[0], kTarget);

    const std::vector<std::string> frames = server_->received_frames();
    ASSERT_EQ(frames.size(), 2U) << "the subscribe frame, then the session.subscriptions request";
    EXPECT_EQ(frames[0], last_frame_) << "the server must see exactly the bytes the hot thread signed";
    EXPECT_TRUE(is_subscribe(frames[0]));
    EXPECT_EQ(frames[1], R"({"id":)" + std::to_string(id_of(frames[0]) + 1) + R"(,"method":"session.subscriptions","params":{}})");

    server_->send_text(std::string(samples::kEventExecutionReport));
    std::vector<UserDataWsEvent> events;
    ASSERT_TRUE(wait_for_events(1, events)) << diagnostics(*session);
    EXPECT_EQ(events[0].kind, UserDataEventKind::ExecutionReport);
    EXPECT_EQ(events[0].generation, 7U);
    EXPECT_EQ(events[0].subscription_id, 0);
    EXPECT_EQ(events[0].coid.view(), "mUvoqJxFIILMdfAW5iGSOW");

    const auto st = session->stats_snapshot();
    EXPECT_EQ(st.events_delivered, 1U);
    EXPECT_EQ(st.subscribe_status, 200);
    EXPECT_EQ(st.verify_status, 200);
    EXPECT_EQ(st.subscriptions_listed, 1U);
    EXPECT_EQ(st.errors, 0U);

    finish(*session);
    EXPECT_EQ(session->end_cause(), WsApiSessionEnd::Requested);
    EXPECT_EQ(session->stats_snapshot().errors, 0U) << "a requested stop is not an error";
}

TEST_F(WsApiSessionTest, TheSubscribeFrameIsSentExactlyOncePerConnection) {
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); })) << diagnostics(*session);
    // more traffic in both directions must not make it write the frame again
    server_->send_text(std::string(samples::kEventBalanceUpdate));
    server_->ping_all();
    std::vector<UserDataWsEvent> events;
    ASSERT_TRUE(wait_for_events(1, events));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const std::vector<std::string> frames = server_->received_frames();
    EXPECT_EQ(count_frames(frames, &is_subscribe), 1U);
    EXPECT_EQ(count_frames(frames, &is_verify), 1U) << "and the listing is asked for once as well";
    EXPECT_EQ(frames.size(), 2U);
    finish(*session);
}

TEST_F(WsApiSessionTest, AFrameDepositedAfterStartIsStillSent) {
    // The mailbox is order independent: a producer that signs just in time (after the handshake) works with no change to the session.
    Opts o;
    o.deposit = false;
    auto session = make_session(1, o);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->handshake_done(); })) << diagnostics(*session);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(session->is_connected());
    EXPECT_TRUE(server_->received_frames().empty()) << "nothing may be sent before a frame was deposited";
    const Deposit d = sign_and_deposit(*session);
    ASSERT_TRUE(d.ok);
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); })) << diagnostics(*session);
    ASSERT_EQ(server_->received_frames().size(), 2U);
    EXPECT_EQ(server_->received_frames()[0], d.frame);
    finish(*session);
}

TEST_F(WsApiSessionTest, IsConnectedMeansTheSubscriptionIsConfirmedNotThatTheHandshakeIsDone) {
    server_->set_responder([](const std::string&, const std::string&) -> std::vector<std::string> { return {}; });  // never answers
    Opts o;
    o.cfg.response_timeout = std::chrono::milliseconds(400);
    auto session = make_session(1, o);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->handshake_done(); }));
    ASSERT_TRUE(wait_until([&] { return server_->received_frames().size() == 1U; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(session->handshake_done());
    EXPECT_FALSE(session->is_connected()) << "no answer yet: no events can flow, so the stream is not up";
    EXPECT_FALSE(session->stopped());
    // ... and with no answer the session gives up on its own
    ASSERT_TRUE(wait_stopped(*session));
    EXPECT_EQ(session->end_cause(), WsApiSessionEnd::Failed);
    EXPECT_EQ(session->stats_snapshot().last_error_stage, "subscribe_timeout");
}

// --- the subscribe answer -------------------------------------------------------------------------------------------

TEST_F(WsApiSessionTest, ARejectedSubscribeEndsTheSessionAndKeepsStatusCodeAndRetryAfter) {
    const std::int64_t retry_after = system_now_ms() + 120'000;
    server_->set_responder([retry_after](const std::string&, const std::string& frame) -> std::vector<std::string> {
        if (is_subscribe(frame)) return {error_answer(id_of(frame), 418, -1003, retry_after)};
        return {};
    });
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_stopped(*session)) << diagnostics(*session);
    EXPECT_FALSE(session->is_connected());
    EXPECT_EQ(session->end_cause(), WsApiSessionEnd::Failed);
    const auto st = session->stats_snapshot();
    EXPECT_EQ(st.last_error_stage, "subscribe_rejected");
    EXPECT_EQ(st.subscribe_status, 418);
    EXPECT_EQ(st.subscribe_error_code, -1003);
    EXPECT_EQ(st.retry_after_ms, retry_after);
    EXPECT_EQ(session->subscription_id(), -1);
    EXPECT_TRUE(drain_ring().empty());
    EXPECT_EQ(server_->received_frames().size(), 1U) << "a rejected subscription is not followed by the request that would verify it";
    EXPECT_TRUE(wait_until([&] { return server_->open_connections() == 0U; })) << "a failed session must close its connection";
    // a later stop() does not rewrite why it ended
    session->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(session->end_cause(), WsApiSessionEnd::Failed);
    EXPECT_EQ(session->stats_snapshot().errors, 1U);
}

TEST_F(WsApiSessionTest, AnErrorStatusIsNotASubscriptionEvenIfTheAnswerNamesOne) {
    server_->set_responder([](const std::string&, const std::string& frame) -> std::vector<std::string> {
        if (is_subscribe(frame)) return {R"({"id":)" + std::to_string(id_of(frame)) + R"(,"status":400,"result":{"subscriptionId":5}})"};
        return {};
    });
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_stopped(*session));
    EXPECT_FALSE(session->is_connected());
    EXPECT_EQ(session->subscription_id(), -1);
    EXPECT_EQ(session->stats_snapshot().last_error_stage, "subscribe_rejected");
    EXPECT_EQ(count_frames(server_->received_frames(), &is_verify), 0U);
}

TEST_F(WsApiSessionTest, AnAnswerWithoutASubscriptionIdIsNotASubscription) {
    server_->set_responder([](const std::string&, const std::string& frame) -> std::vector<std::string> {
        if (is_subscribe(frame)) return {R"({"id":)" + std::to_string(id_of(frame)) + R"(,"status":200,"result":{}})"};
        return {};
    });
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_stopped(*session));
    EXPECT_FALSE(session->is_connected());
    EXPECT_EQ(session->stats_snapshot().last_error_stage, "subscribe_rejected");
}

TEST_F(WsApiSessionTest, ASecondAnswerToTheSameRequestIsCountedAndIgnored) {
    server_->set_responder([](const std::string&, const std::string& frame) -> std::vector<std::string> {
        if (is_subscribe(frame)) return {ok_answer(id_of(frame), 0), ok_answer(id_of(frame), 7)};
        if (is_verify(frame)) return {list_answer(id_of(frame), {0})};
        return {};
    });
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));
    ASSERT_TRUE(wait_until([&] { return session->stats_snapshot().unexpected_responses == 1U; })) << diagnostics(*session);
    EXPECT_EQ(session->subscription_id(), 0) << "the first answer stands; a later one does not move the subscription";
    EXPECT_FALSE(session->stopped());
    finish(*session);
}

TEST_F(WsApiSessionTest, AnAnswerToAnotherRequestIdEndsTheSession) {
    server_->set_responder([](const std::string&, const std::string& frame) -> std::vector<std::string> {
        if (is_subscribe(frame)) return {ok_answer(id_of(frame) + 1000, 0)};
        return {};
    });
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_stopped(*session));
    EXPECT_FALSE(session->is_connected());
    EXPECT_EQ(session->stats_snapshot().last_error_stage, "response_id_mismatch");
}

TEST_F(WsApiSessionTest, AnEventBeforeTheAnswerEndsTheSessionAndIsNotDelivered) {
    server_->set_responder([](const std::string&, const std::string& frame) -> std::vector<std::string> {
        if (is_subscribe(frame)) return {std::string(samples::kEventExecutionReport), ok_answer(id_of(frame), 0)};
        return {};
    });
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_stopped(*session));
    EXPECT_FALSE(session->is_connected());
    EXPECT_EQ(session->stats_snapshot().last_error_stage, "event_before_subscribe_response");
    EXPECT_TRUE(drain_ring().empty()) << "no event is accepted before the subscription is confirmed";
}

TEST_F(WsApiSessionTest, AServerShutdownEvenBeforeTheAnswerIsDeliveredAndEndsTheGeneration) {
    // serverShutdown is a connection event: no subscriptionId, valid at any moment (P-29).
    server_->set_responder([](const std::string&, const std::string& frame) -> std::vector<std::string> {
        if (is_subscribe(frame)) return {kServerShutdown};
        return {};
    });
    auto session = make_session(3);
    session->start();
    ASSERT_TRUE(wait_stopped(*session)) << diagnostics(*session);
    EXPECT_EQ(session->end_cause(), WsApiSessionEnd::ServerShutdown);
    EXPECT_EQ(session->stats_snapshot().errors, 0U);
    const std::vector<UserDataWsEvent> events = drain_ring();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, UserDataEventKind::ServerShutdown);
    EXPECT_EQ(events[0].generation, 3U);
    EXPECT_EQ(events[0].subscription_id, -1);
}

// --- the verification: what does the exchange say this connection holds? ------------------------------------------------

TEST_F(WsApiSessionTest, TheStreamIsConnectedOnlyOnceTheListingNamesExactlyTheConfirmedSubscription) {
    answer_verify_with([](std::uint64_t) { return std::vector<std::string>{}; });  // the subscription is confirmed; the listing is not answered yet
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_until([&] { return server_->received_frames().size() == 2U; })) << diagnostics(*session);
    const std::vector<std::string> frames = server_->received_frames();
    ASSERT_TRUE(is_verify(frames[1]));
    const std::uint64_t verify_id = id_of(frames[1]);
    EXPECT_EQ(verify_id, id_of(frames[0]) + 1) << "the listing request takes the id after the subscribe request's";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(session->subscription_id(), 0) << "the subscribe answer is in";
    EXPECT_FALSE(session->is_connected()) << "but the exchange has not yet said what the connection holds";
    EXPECT_FALSE(session->stopped());

    server_->send_text(list_answer(verify_id, {0}));
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); })) << diagnostics(*session);
    EXPECT_EQ(session->stats_snapshot().errors, 0U);
    finish(*session);
}

TEST_F(WsApiSessionTest, EventsBetweenTheTwoAnswersAreDeliveredButDoNotMakeTheStreamConnected) {
    answer_verify_with([](std::uint64_t) { return std::vector<std::string>{}; });
    auto session = make_session(6);
    session->start();
    ASSERT_TRUE(wait_until([&] { return server_->received_frames().size() == 2U; })) << diagnostics(*session);
    server_->send_text(std::string(samples::kEventExecutionReport));
    std::vector<UserDataWsEvent> events;
    ASSERT_TRUE(wait_for_events(1, events)) << diagnostics(*session);
    EXPECT_EQ(events[0].kind, UserDataEventKind::ExecutionReport);
    EXPECT_EQ(events[0].generation, 6U) << "a fill that arrives before the listing must not be lost, and it is stamped like any other";
    EXPECT_EQ(events[0].subscription_id, 0);
    EXPECT_FALSE(session->is_connected()) << "an event is not the exchange's confirmation";

    server_->send_text(with_subscription(samples::kEventBalanceUpdate, 9));  // another subscription's event is dropped here too
    ASSERT_TRUE(wait_until([&] { return session->stats_snapshot().foreign_subscription_events == 1U; })) << diagnostics(*session);
    EXPECT_TRUE(drain_ring().empty());

    server_->send_text(list_answer(id_of(server_->received_frames()[1]), {0}));
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); })) << diagnostics(*session);
    finish(*session);
}

TEST_F(WsApiSessionTest, AListingThatIsNotExactlyTheConfirmedSubscriptionEndsTheSession) {
    struct Case {
        const char* name;
        std::string result;  // the "result" member of the answer
    };
    std::string twenty = "[";
    for (int i = 0; i < 20; ++i) {
        if (i > 0) twenty += ',';
        twenty += R"({"subscriptionId":)" + std::to_string(i) + "}";
    }
    twenty += "]";
    const std::vector<Case> cases = {
        {"none", "[]"},
        {"two, ours first", R"([{"subscriptionId":0},{"subscriptionId":1}])"},
        {"two, ours second", R"([{"subscriptionId":1},{"subscriptionId":0}])"},
        {"ours twice", R"([{"subscriptionId":0},{"subscriptionId":0}])"},
        {"another number", R"([{"subscriptionId":7}])"},
        {"ours and an entry that is not a subscription", R"([{"subscriptionId":0},5])"},
        {"only an entry that is not a subscription", R"(["x"])"},
        {"more than the codec keeps", twenty},
        {"not a list", R"({"subscriptionId":0})"},
        {"nothing", "null"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        const std::string result = c.result;
        answer_verify_with([result](std::uint64_t id) {
            return std::vector<std::string>{R"({"id":)" + std::to_string(id) + R"(,"status":200,"result":)" + result + "}"};
        });
        auto session = make_session(1);
        session->start();
        ASSERT_TRUE(wait_stopped(*session)) << diagnostics(*session);
        EXPECT_FALSE(session->is_connected());
        EXPECT_EQ(session->end_cause(), WsApiSessionEnd::Failed);
        const auto st = session->stats_snapshot();
        EXPECT_EQ(st.last_error_stage, "subscriptions_mismatch");
        EXPECT_EQ(st.verify_status, 200);
        EXPECT_EQ(st.errors, 1U);
        EXPECT_TRUE(wait_until([&] { return server_->open_connections() == 0U; })) << "a failed session must close its connection";
    }
}

TEST_F(WsApiSessionTest, TheListingNamesTheSubscriptionTheExchangeConfirmedNotJustNumberZero) {
    server_->set_responder([](const std::string&, const std::string& frame) -> std::vector<std::string> {
        if (is_subscribe(frame)) return {ok_answer(id_of(frame), 41)};
        if (is_verify(frame)) return {list_answer(id_of(frame), {41})};
        return {};
    });
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); })) << diagnostics(*session);
    EXPECT_EQ(session->subscription_id(), 41);
    EXPECT_EQ(session->stats_snapshot().errors, 0U);
    finish(*session);
}

TEST_F(WsApiSessionTest, ARefusedListingEndsTheSessionAndKeepsStatusCodeAndRetryAfter) {
    const std::int64_t retry_after = system_now_ms() + 90'000;
    answer_verify_with([retry_after](std::uint64_t id) { return std::vector<std::string>{error_answer(id, 429, -1003, retry_after)}; });
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_stopped(*session)) << diagnostics(*session);
    EXPECT_FALSE(session->is_connected());
    EXPECT_EQ(session->end_cause(), WsApiSessionEnd::Failed);
    const auto st = session->stats_snapshot();
    EXPECT_EQ(st.last_error_stage, "subscriptions_rejected");
    EXPECT_EQ(st.subscribe_status, 200);
    EXPECT_EQ(st.verify_status, 429);
    EXPECT_EQ(st.verify_error_code, -1003);
    EXPECT_EQ(st.retry_after_ms, retry_after) << "the factory reads this to honour the ban";
    EXPECT_EQ(session->subscription_id(), 0) << "the subscribe answer was accepted first; that alone does not make the stream connected";
}

TEST_F(WsApiSessionTest, AListingAnsweredForAnotherRequestIdEndsTheSession) {
    answer_verify_with([](std::uint64_t id) { return std::vector<std::string>{list_answer(id + 1000, {0})}; });
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_stopped(*session)) << diagnostics(*session);
    EXPECT_FALSE(session->is_connected());
    EXPECT_EQ(session->stats_snapshot().last_error_stage, "response_id_mismatch");
}

TEST_F(WsApiSessionTest, NoListingEndsTheSessionAfterTheResponseTimeout) {
    answer_verify_with([](std::uint64_t) { return std::vector<std::string>{}; });
    Opts o;
    o.cfg.response_timeout = std::chrono::milliseconds(400);
    auto session = make_session(1, o);
    session->start();
    ASSERT_TRUE(wait_until([&] { return server_->received_frames().size() == 2U; })) << diagnostics(*session);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(session->stopped()) << "the listing wait has its own full timeout";
    ASSERT_TRUE(wait_stopped(*session)) << diagnostics(*session);
    EXPECT_EQ(session->end_cause(), WsApiSessionEnd::Failed);
    EXPECT_EQ(session->stats_snapshot().last_error_stage, "subscriptions_timeout");
    EXPECT_FALSE(session->is_connected());
}

TEST_F(WsApiSessionTest, ASecondListingAnswerIsCountedAndIgnored) {
    answer_verify_with([](std::uint64_t id) { return std::vector<std::string>{list_answer(id, {0}), list_answer(id, {5})}; });
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); })) << diagnostics(*session);
    ASSERT_TRUE(wait_until([&] { return session->stats_snapshot().unexpected_responses == 1U; })) << diagnostics(*session);
    EXPECT_TRUE(session->is_connected()) << "a later, different listing does not take the confirmation back";
    EXPECT_FALSE(session->stopped());
    finish(*session);
}

TEST_F(WsApiSessionTest, AServerShutdownBetweenTheTwoAnswersIsDeliveredAndEndsTheGeneration) {
    answer_verify_with([](std::uint64_t) { return std::vector<std::string>{std::string(kServerShutdown)}; });
    auto session = make_session(8);
    session->start();
    ASSERT_TRUE(wait_stopped(*session)) << diagnostics(*session);
    EXPECT_EQ(session->end_cause(), WsApiSessionEnd::ServerShutdown);
    EXPECT_FALSE(session->is_connected());
    EXPECT_EQ(session->stats_snapshot().errors, 0U);
    const std::vector<UserDataWsEvent> events = drain_ring();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, UserDataEventKind::ServerShutdown);
    EXPECT_EQ(events[0].generation, 8U);
}

// --- the mailbox, the clock and the budget --------------------------------------------------------------------------

TEST_F(WsApiSessionTest, AFrameThatWaitedTooLongSinceItWasSignedIsNeverSent) {
    Opts o;
    o.signed_age_ms = 10'000;  // far beyond the 4 s the default recvWindow leaves
    auto session = make_session(1, o);
    session->start();
    ASSERT_TRUE(wait_stopped(*session));
    EXPECT_EQ(session->stats_snapshot().last_error_stage, "stale_subscribe_request");
    EXPECT_TRUE(server_->received_frames().empty()) << "a doomed request must not cost the exchange a request";
    EXPECT_FALSE(session->is_connected());
}

TEST_F(WsApiSessionTest, NoFrameInTheMailboxEndsTheSessionAfterTheWait) {
    Opts o;
    o.deposit = false;
    o.cfg.subscribe_wait = std::chrono::milliseconds(200);
    auto session = make_session(1, o);
    session->start();
    ASSERT_TRUE(wait_stopped(*session));
    EXPECT_EQ(session->stats_snapshot().last_error_stage, "no_subscribe_request");
    EXPECT_TRUE(session->handshake_done());
    EXPECT_TRUE(server_->received_frames().empty());
}

TEST_F(WsApiSessionTest, APermitReservedForACheaperRequestCoversNeitherTheConnectionNorTheSubscribeNorTheVerification) {
    {
        Opts o;
        o.connect_weight = 1;  // a real reservation, but of less than a connection costs (P-26: 2)
        auto session = make_session(1, o);
        ASSERT_NE(session, nullptr);
        session->start();
        EXPECT_TRUE(session->stopped()) << "refused at start(), before any I/O";
        EXPECT_EQ(session->stats_snapshot().last_error_stage, "connect_permit_too_small");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_EQ(server_->tls_handshakes(), 0U) << "no connection may be attempted";
    }
    {
        Opts o;
        o.verify_weight = 1;  // the session.subscriptions request is paid for in advance too, and a cheaper reservation is not enough
        auto session = make_session(3, o);
        ASSERT_NE(session, nullptr);
        session->start();
        EXPECT_TRUE(session->stopped()) << "refused at start(), before any I/O";
        EXPECT_EQ(session->stats_snapshot().last_error_stage, "verify_permit_too_small");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_EQ(server_->tls_handshakes(), 0U) << "no connection may be attempted";
    }
    {
        Opts o;
        o.deposit = false;
        auto session = make_session(2, o);
        const Deposit d = sign_and_deposit(*session, 0, /*subscribe_weight=*/1);
        EXPECT_FALSE(d.ok) << "a frame is accepted only together with a permit that covers its weight";
    }
}

// --- events -----------------------------------------------------------------------------------------------------------

TEST_F(WsApiSessionTest, EventsOfAnotherSubscriptionAreDroppedAndCounted) {
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));
    server_->send_text(with_subscription(samples::kEventBalanceUpdate, 9));
    server_->send_text(with_subscription(samples::kEventBalanceUpdate, 0));
    std::vector<UserDataWsEvent> events;
    ASSERT_TRUE(wait_for_events(1, events));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (const UserDataWsEvent& e : drain_ring()) events.push_back(e);
    ASSERT_EQ(events.size(), 1U) << "only the event of OUR subscription may reach the ring";
    EXPECT_EQ(events[0].subscription_id, 0);
    EXPECT_EQ(session->stats_snapshot().foreign_subscription_events, 1U);
    finish(*session);
}

TEST_F(WsApiSessionTest, AnExecutionReportTheCodecCannotReadWholeIsCountedAsLostNotDelivered) {
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));
    std::string no_order_id(samples::kEventExecutionReport);
    const std::string c = R"("c":"mUvoqJxFIILMdfAW5iGSOW",)";
    const auto p = no_order_id.find(c);
    ASSERT_NE(p, std::string::npos);
    no_order_id.erase(p, c.size());
    server_->send_text(no_order_id);
    ASSERT_TRUE(wait_until([&] { return session->lost_events() == 1U; })) << diagnostics(*session);
    EXPECT_TRUE(drain_ring().empty()) << "a fill we could not read is never delivered half-filled";
    EXPECT_EQ(session->stats_snapshot().lost_events, 1U);
    EXPECT_FALSE(session->stopped()) << "one unreadable report does not end the stream";
    // ... and the stream goes on
    server_->send_text(std::string(samples::kEventExecutionReport));
    std::vector<UserDataWsEvent> events;
    ASSERT_TRUE(wait_for_events(1, events));
    EXPECT_EQ(events[0].coid.view(), "mUvoqJxFIILMdfAW5iGSOW");
    finish(*session);
}

TEST_F(WsApiSessionTest, FramesThatAreNotFramesAreCountedAndTheSessionKeepsReading) {
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));
    server_->send_text("this is not json");
    server_->send_text(R"({"hello":1})");
    server_->send_text(R"({"subscriptionId":0,"event":{"x":1}})");  // an event without a type
    server_->send_text(std::string(samples::kEventBalanceUpdate));
    std::vector<UserDataWsEvent> events;
    ASSERT_TRUE(wait_for_events(1, events));
    EXPECT_EQ(events[0].kind, UserDataEventKind::BalanceUpdate);
    EXPECT_EQ(session->stats_snapshot().parse_failed, 3U);
    EXPECT_FALSE(session->stopped());
    finish(*session);
}

TEST_F(WsApiSessionTest, AnUnknownEventTypeIsCountedAndNotDelivered) {
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));
    server_->send_text(R"({"subscriptionId":0,"event":{"e":"someFutureEvent","E":1}})");
    server_->send_text(std::string(samples::kEventBalanceUpdate));
    std::vector<UserDataWsEvent> events;
    ASSERT_TRUE(wait_for_events(1, events));
    EXPECT_EQ(events[0].kind, UserDataEventKind::BalanceUpdate);
    EXPECT_EQ(session->stats_snapshot().unknown_events, 1U);
    finish(*session);
}

TEST_F(WsApiSessionTest, AnEventStreamTerminatedIsDeliveredAndEndsTheGeneration) {
    auto session = make_session(4);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));
    server_->send_text(std::string(samples::kEventEventStreamTerminated));
    ASSERT_TRUE(wait_stopped(*session));
    EXPECT_EQ(session->end_cause(), WsApiSessionEnd::StreamTerminated);
    EXPECT_EQ(session->stats_snapshot().errors, 0U);
    const std::vector<UserDataWsEvent> events = drain_ring();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, UserDataEventKind::EventStreamTerminated);
    EXPECT_EQ(events[0].generation, 4U);
}

TEST_F(WsApiSessionTest, AServerShutdownAfterTheSubscriptionIsDeliveredAndEndsTheGeneration) {
    auto session = make_session(5);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));
    server_->send_text(kServerShutdown);
    ASSERT_TRUE(wait_stopped(*session));
    EXPECT_EQ(session->end_cause(), WsApiSessionEnd::ServerShutdown);
    const std::vector<UserDataWsEvent> events = drain_ring();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, UserDataEventKind::ServerShutdown);
    EXPECT_EQ(events[0].generation, 5U);
    EXPECT_EQ(session->stats_snapshot().server_shutdowns, 1U);
    session->stop();  // the supervisor drops a stopped session with exactly this call, among others: the reason must not change
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(session->end_cause(), WsApiSessionEnd::ServerShutdown);
    EXPECT_TRUE(wait_until([&] { return server_->open_connections() == 0U; })) << "the generation is over: its connection is closed";
}

TEST_F(WsApiSessionTest, AFullRingDropsAndCountsInsteadOfBlocking) {
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));
    constexpr std::size_t kSent = 100;  // the ring holds 64 and nobody drains it
    for (std::size_t i = 0; i < kSent; ++i) server_->send_text(std::string(samples::kEventBalanceUpdate));
    ASSERT_TRUE(wait_until([&] {
        const auto st = session->stats_snapshot();
        return st.events_delivered + st.push_dropped == kSent;
    })) << diagnostics(*session);
    const auto st = session->stats_snapshot();
    EXPECT_GT(st.push_dropped, 0U);
    EXPECT_EQ(session->ring_drops(), st.push_dropped) << "the barrier reads this counter; it must agree with the stats";
    EXPECT_FALSE(session->stopped());
    finish(*session);
}

// --- liveness and failure ---------------------------------------------------------------------------------------------

TEST_F(WsApiSessionTest, PingFramesAreCountedAndKeepTheActivityClockMoving) {
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));
    const std::int64_t before = session->last_activity_steady_ms();
    EXPECT_GT(before, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    server_->ping_all();
    ASSERT_TRUE(wait_until([&] { return session->pings_received() >= 1U; })) << diagnostics(*session);
    EXPECT_GT(session->last_activity_steady_ms(), before) << "a control frame is a sign of life (P-30)";
    finish(*session);
}

TEST_F(WsApiSessionTest, AMessageAboveThePermitCapEndsTheSessionWhileItStreamsIn) {
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));
    server_->send_text(std::string(hy::kWsApiMaxMessageBytes + 1, 'x'));
    ASSERT_TRUE(wait_stopped(*session)) << diagnostics(*session);
    EXPECT_EQ(session->end_cause(), WsApiSessionEnd::Failed);
    EXPECT_EQ(session->stats_snapshot().last_error_stage, "read");
    EXPECT_TRUE(drain_ring().empty());
}

TEST_F(WsApiSessionTest, AServerSideDropEndsTheSession) {
    auto session = make_session(1);
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));
    server_->drop_all();
    ASSERT_TRUE(wait_stopped(*session));
    EXPECT_EQ(session->end_cause(), WsApiSessionEnd::Failed);
    EXPECT_EQ(session->stats_snapshot().errors, 1U) << "one failure, recorded once";
}

TEST_F(WsApiSessionTest, StopWhileTheUpgradeIsPendingCancelsCleanly) {
    auto stalled = make_server(WsLoopbackServer::UpgradeBehavior::Stall);
    Opts o;
    o.port = std::to_string(stalled->port());
    auto session = make_session(1, o);
    session->start();
    ASSERT_TRUE(wait_until([&] { return stalled->tls_handshakes() >= 1U; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(session->handshake_done()) << "TLS is up but the upgrade is never answered";
    EXPECT_FALSE(session->stopped());
    session->stop();
    ASSERT_TRUE(wait_stopped(*session));
    EXPECT_EQ(session->end_cause(), WsApiSessionEnd::Requested);
    EXPECT_EQ(session->stats_snapshot().errors, 0U);
}

TEST_F(WsApiSessionTest, AnEmptyTargetIsAnInvalidConfigAndNothingConnects) {
    Opts o;
    o.empty_target = true;
    auto session = make_session(1, o);
    session->start();
    EXPECT_TRUE(session->stopped());
    EXPECT_EQ(session->stats_snapshot().last_error_stage, "invalid_config");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(server_->tls_handshakes(), 0U);
}

// stopped() is what the supervisor and the reconnect barrier poll: the moment it is true, the reason must already be readable.
TEST_F(WsApiSessionTest, WhenStoppedTurnsTrueTheCauseAndTheRecordedFailureAreAlreadyThere) {
    server_->set_responder([](const std::string&, const std::string& frame) -> std::vector<std::string> {
        if (is_subscribe(frame)) return {error_answer(id_of(frame), 400, -1100, 0)};
        return {};
    });
    for (int round = 0; round < 12; ++round) {
        SCOPED_TRACE(round);
        auto session = make_session(1);
        session->start();
        ASSERT_TRUE(spin_until_stopped(*session)) << diagnostics(*session);
        // read at once, before anything else can run: neither may lag behind stopped()
        const WsApiSessionEnd cause = session->end_cause();
        const auto st = session->stats_snapshot();
        EXPECT_EQ(cause, WsApiSessionEnd::Failed);
        EXPECT_EQ(st.errors, 1U);
        EXPECT_EQ(st.last_error_stage, "subscribe_rejected");
    }
}

TEST_F(WsApiSessionTest, WhenAFailureRacesAStopTheCauseAndTheRecordedFailureAgree) {
    server_->set_responder([](const std::string&, const std::string& frame) -> std::vector<std::string> {
        if (is_subscribe(frame)) return {error_answer(id_of(frame), 400, -1100, 0)};
        return {};
    });
    for (int round = 0; round < 24; ++round) {
        SCOPED_TRACE(round);
        auto session = make_session(1);
        session->start();
        std::this_thread::sleep_for(std::chrono::microseconds(round * 1500));  // sweeps stop() across the moment the refusal arrives
        session->stop();
        ASSERT_TRUE(spin_until_stopped(*session)) << diagnostics(*session);
        const WsApiSessionEnd cause = session->end_cause();
        const auto st = session->stats_snapshot();
        EXPECT_NE(cause, WsApiSessionEnd::None);
        EXPECT_EQ(cause == WsApiSessionEnd::Failed, st.errors == 1U) << "a failure is recorded if and only if the cause says so";
        EXPECT_TRUE(cause == WsApiSessionEnd::Failed || cause == WsApiSessionEnd::Requested);
        EXPECT_TRUE(wait_until([&] { return server_->open_connections() == 0U; }));
    }
}

TEST_F(WsApiSessionTest, ASecondStartIsANoOp) {
    auto session = make_session(1);
    session->start();
    session->start();
    ASSERT_TRUE(wait_until([&] { return session->is_connected(); }));
    EXPECT_EQ(server_->received_frames().size(), 2U) << "one subscribe frame and one listing request, not two of each";
    ASSERT_TRUE(wait_until([&] { return server_->upgrades() >= 1U; }));
    EXPECT_EQ(server_->upgrades(), 1U);
    finish(*session);
}

// --- the factory and the supervisor ---------------------------------------------------------------------------------

namespace {

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

std::string between(const std::string& s, const std::string& from, const std::string& to) {
    const auto a = s.find(from);
    if (a == std::string::npos) return {};
    const auto b = s.find(to, a + from.size());
    return b == std::string::npos ? std::string{} : s.substr(a + from.size(), b - a - from.size());
}

}  // namespace

class WsApiFactoryTest : public WsApiSessionTest {
protected:
    hy::WsApiFeedEndpoint endpoint() const {
        return hy::WsApiFeedEndpoint{policy_, "127.0.0.1", std::to_string(server_->port()), kTarget};
    }
    hy::WsApiSessionFactory::Deps deps() {
        return hy::WsApiSessionFactory::Deps{rig_.ioc, rig_.ssl_ctx, ring_, codec_, creds_->get(), clock_, limiter_};
    }
    // Polls the supervisor on THIS thread (the hot thread) until `pred` holds.
    template <typename Pred>
    bool poll_until(Supervisor& sup, Pred pred, int timeout_ms = 5000) {
        return wait_until(
            [&] {
                sup.poll(steady_ms());
                return pred();
            },
            timeout_ms);
    }
};

TEST_F(WsApiFactoryTest, TheSupervisorRunsASignedGenerationAndStampsItsEvents) {
    hy::WsApiSessionFactory factory(deps(), endpoint());
    Supervisor sup([&factory](std::uint64_t g) { return factory(g); }, fast_policy());
    ASSERT_TRUE(poll_until(sup, [&] { return sup.healthy(); }));
    EXPECT_EQ(sup.generation(), 1U);
    EXPECT_EQ(factory.stats().created, 1U);
    const std::shared_ptr<Session> s = sup.current();
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->is_connected());

    const std::vector<std::string> frames = server_->received_frames();
    ASSERT_EQ(frames.size(), 2U) << "the subscribe request and the listing request";
    EXPECT_NE(frames[0].find(R"("method":"userDataStream.subscribe.signature")"), std::string::npos);
    EXPECT_NE(frames[0].find(std::string(R"("apiKey":")") + std::string(hy::test_helpers::BoundTestCredentials::kApiKey) + "\""),
              std::string::npos);
    const std::string signature = between(frames[0], R"("signature":")", "\"");
    EXPECT_EQ(signature.size(), 64U);
    EXPECT_NE(frames[1].find(R"("method":"session.subscriptions")"), std::string::npos);
    EXPECT_EQ(frames[1].find("signature"), std::string::npos) << "the listing request carries no signature";

    server_->send_text(std::string(samples::kEventExecutionReport));
    std::vector<UserDataWsEvent> events;
    ASSERT_TRUE(wait_for_events(1, events));
    EXPECT_EQ(events[0].generation, 1U);
    sup.shutdown();
    EXPECT_TRUE(wait_stopped(*s));

    // One generation cost three requests of weight 2 (P-26: the connection, the subscribe request and the listing request), all on the
    // Reconciliation lane (design D7): 550 - 6 left there, and nothing taken from the Strategy lane (4400) or the Emergency lane (550).
    EXPECT_FALSE(limiter_.reserve_weight_only_explicit(hy::RateLimitLane::Reconciliation, 545).has_value());
    EXPECT_TRUE(limiter_.reserve_weight_only_explicit(hy::RateLimitLane::Reconciliation, 544).has_value());
    EXPECT_TRUE(limiter_.reserve_weight_only_explicit(hy::RateLimitLane::Strategy, 4400).has_value());
    EXPECT_TRUE(limiter_.reserve_weight_only_explicit(hy::RateLimitLane::Emergency, 550).has_value());
}

TEST_F(WsApiFactoryTest, EveryGenerationSignsAgainAndNeverReplaysAnOldRequest) {
    hy::WsApiSessionFactory factory(deps(), endpoint());
    Supervisor sup([&factory](std::uint64_t g) { return factory(g); }, fast_policy());
    ASSERT_TRUE(poll_until(sup, [&] { return sup.healthy(); }));
    ASSERT_EQ(sup.generation(), 1U);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // so the second timestamp differs from the first
    server_->drop_all();
    ASSERT_TRUE(poll_until(sup, [&] { return sup.generation() == 2U && sup.healthy(); })) << "generation " << sup.generation();

    const std::vector<std::string> all = server_->received_frames();
    const std::vector<std::string> frames = only(all, &is_subscribe);
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_NE(frames[0], frames[1]) << "the second connection must carry a new request, not the old bytes again";
    EXPECT_NE(between(frames[0], R"("signature":")", "\""), between(frames[1], R"("signature":")", "\""));
    EXPECT_NE(between(frames[0], R"("timestamp":)", ","), between(frames[1], R"("timestamp":)", ","));
    // The ids advance by two: the session uses the next number for its own listing request, so no id repeats on any connection.
    EXPECT_EQ(id_of(frames[0]), 1U);
    EXPECT_EQ(id_of(frames[1]), 3U);
    const std::vector<std::string> listings = only(all, &is_verify);
    ASSERT_EQ(listings.size(), 2U);
    EXPECT_EQ(id_of(listings[0]), 2U);
    EXPECT_EQ(id_of(listings[1]), 4U);

    server_->send_text(std::string(samples::kEventExecutionReport));
    std::vector<UserDataWsEvent> events;
    ASSERT_TRUE(wait_for_events(1, events));
    EXPECT_EQ(events[0].generation, 2U) << "events are stamped with the generation of the connection that carried them";
    sup.shutdown();
}

TEST_F(WsApiFactoryTest, ABannedAddressIsNotConnectedToAgainUntilTheBanEnds) {
    const std::int64_t ban_until = system_now_ms() + 600'000;
    auto first = std::make_shared<std::atomic<bool>>(true);
    server_->set_responder([ban_until, first](const std::string&, const std::string& frame) -> std::vector<std::string> {
        if (is_verify(frame)) return {list_answer(id_of(frame), {0})};
        if (!is_subscribe(frame)) return {};
        if (first->exchange(false)) return {error_answer(id_of(frame), 418, -1003, ban_until)};
        return {ok_answer(id_of(frame), 0)};
    });
    hy::WsApiSessionFactory factory(deps(), endpoint());
    Supervisor sup([&factory](std::uint64_t g) { return factory(g); }, fast_policy());
    ASSERT_TRUE(poll_until(sup, [&] { return factory.stats().refused_ban >= 2U; })) << "refused_ban=" << factory.stats().refused_ban;
    EXPECT_EQ(factory.stats().created, 1U) << "after the 418 no new connection may be started";
    EXPECT_EQ(server_->upgrades(), 1U);
    EXPECT_EQ(factory.stats().ban_until_server_ms, ban_until);
    EXPECT_FALSE(sup.healthy());
    sup.shutdown();
}

TEST_F(WsApiFactoryTest, ABanAnnouncedByTheListingAnswerIsHonouredToo) {
    const std::int64_t ban_until = system_now_ms() + 600'000;
    answer_verify_with([ban_until](std::uint64_t id) { return std::vector<std::string>{error_answer(id, 429, -1003, ban_until)}; });
    hy::WsApiSessionFactory factory(deps(), endpoint());
    Supervisor sup([&factory](std::uint64_t g) { return factory(g); }, fast_policy());
    ASSERT_TRUE(poll_until(sup, [&] { return factory.stats().refused_ban >= 2U; })) << "refused_ban=" << factory.stats().refused_ban;
    EXPECT_EQ(factory.stats().created, 1U) << "the subscription was confirmed, the listing was refused for rate limiting: wait out the ban";
    EXPECT_EQ(server_->upgrades(), 1U);
    EXPECT_EQ(factory.stats().ban_until_server_ms, ban_until);
    sup.shutdown();
}

TEST_F(WsApiFactoryTest, AGenerationWhoseListingDoesNotMatchIsAFailureAndTheNextOneIsTried) {
    auto first = std::make_shared<std::atomic<bool>>(true);
    server_->set_responder([first](const std::string&, const std::string& frame) -> std::vector<std::string> {
        if (is_subscribe(frame)) return {ok_answer(id_of(frame), 0)};
        if (is_verify(frame)) {
            if (first->exchange(false)) return {list_answer(id_of(frame), {})};  // the first connection holds nothing
            return {list_answer(id_of(frame), {0})};
        }
        return {};
    });
    hy::WsApiSessionFactory factory(deps(), endpoint());
    Supervisor sup([&factory](std::uint64_t g) { return factory(g); }, fast_policy());
    ASSERT_TRUE(poll_until(sup, [&] { return sup.generation() == 2U && sup.healthy(); })) << "generation " << sup.generation();
    EXPECT_GE(sup.stats().total_failures, 1U);
    EXPECT_EQ(factory.stats().created, 2U);
    sup.shutdown();
}

TEST_F(WsApiFactoryTest, RefusesWithoutAFreshClockAndSpendsNothing) {
    hy::ClockOffsetPublisher never_published;
    hy::WsApiSessionFactory::Deps d = deps();
    hy::WsApiSessionFactory factory(hy::WsApiSessionFactory::Deps{d.ioc, d.ssl_ctx, d.ring, d.codec, d.credentials, never_published, d.limiter},
                                    endpoint());
    EXPECT_EQ(factory(1), nullptr);
    EXPECT_EQ(factory.stats().refused_clock, 1U);
    EXPECT_TRUE(reconciliation_lane_untouched()) << "a refused attempt costs no budget";
    EXPECT_EQ(server_->tls_handshakes(), 0U);
}

TEST_F(WsApiFactoryTest, RefusesAHostThatIsNotOnTheWebSocketApiAllowlist) {
    hy::WsApiFeedEndpoint e = endpoint();
    e.host = "stream.binance.com";  // a real Binance host, but not a WebSocket-API one
    hy::WsApiSessionFactory factory(deps(), e);
    EXPECT_EQ(factory(1), nullptr);
    EXPECT_EQ(factory.stats().refused_endpoint, 1U);
    EXPECT_TRUE(reconciliation_lane_untouched());
}

TEST_F(WsApiFactoryTest, RefusesWhenTheLaneCannotPayForTheConnectionAndBothRequests) {
    // Reconciliation lane = 10% of (limit - margin): with a limit of 30 that is 3 weight, enough for one request of weight 2 but not for two.
    hy::SpotRateLimitTracker tight;
    ASSERT_TRUE(tight.configure(30, 0, 30, 0, 10, 0));
    hy::WsApiSessionFactory::Deps d = deps();
    hy::WsApiSessionFactory factory(hy::WsApiSessionFactory::Deps{d.ioc, d.ssl_ctx, d.ring, d.codec, d.credentials, d.clock, tight}, endpoint());
    EXPECT_EQ(factory(1), nullptr);
    EXPECT_EQ(factory.stats().refused_budget, 1U);
    EXPECT_EQ(factory.stats().created, 0U);
    EXPECT_EQ(server_->tls_handshakes(), 0U);
}

TEST_F(WsApiFactoryTest, RefusesWhenTheLaneCanPayForTheConnectionAndTheSubscribeButNotForTheListing) {
    // 50 -> 5 weight on the Reconciliation lane: two requests of weight 2 fit, a third does not.
    hy::SpotRateLimitTracker tight;
    ASSERT_TRUE(tight.configure(50, 0, 30, 0, 10, 0));
    hy::WsApiSessionFactory::Deps d = deps();
    hy::WsApiSessionFactory factory(hy::WsApiSessionFactory::Deps{d.ioc, d.ssl_ctx, d.ring, d.codec, d.credentials, d.clock, tight}, endpoint());
    EXPECT_EQ(factory(1), nullptr) << "a generation that could not ask the exchange what it holds is not started";
    EXPECT_EQ(factory.stats().refused_budget, 1U);
    EXPECT_EQ(factory.stats().created, 0U);
    EXPECT_EQ(server_->tls_handshakes(), 0U);
}

TEST_F(WsApiFactoryTest, AGenerationCostsExactlySixWeightOnTheReconciliationLane) {
    // 60 -> 6 weight: the connection (2), the subscribe request (2) and the listing request (2), and not one unit more.
    hy::SpotRateLimitTracker exact;
    ASSERT_TRUE(exact.configure(60, 0, 30, 0, 10, 0));
    hy::WsApiSessionFactory::Deps d = deps();
    hy::WsApiSessionFactory factory(hy::WsApiSessionFactory::Deps{d.ioc, d.ssl_ctx, d.ring, d.codec, d.credentials, d.clock, exact}, endpoint());
    const std::shared_ptr<Session> session = factory(1);  // never started: nothing connects
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(factory.stats().created, 1U);
    EXPECT_FALSE(exact.reserve_weight_only_explicit(hy::RateLimitLane::Reconciliation, 1).has_value()) << "six paid for, nothing left";
    EXPECT_EQ(factory(2), nullptr);
    EXPECT_EQ(factory.stats().refused_budget, 1U);
}

TEST_F(WsApiFactoryTest, RefusesWhenTheCredentialsCannotSignOnTheCallingThread) {
    // BoundHmacCredentials answers only on the thread that bound it. A factory driven from any other thread must fail closed, not sign.
    hy::WsApiSessionFactory factory(deps(), endpoint());
    std::shared_ptr<Session> from_other_thread;
    std::thread t([&] { from_other_thread = factory(1); });
    t.join();
    EXPECT_EQ(from_other_thread, nullptr);
    EXPECT_EQ(factory.stats().sign_failed, 1U);
    EXPECT_EQ(factory.stats().created, 0U);
    EXPECT_TRUE(reconciliation_lane_untouched()) << "a signature that cannot be made must not have cost any budget";
    EXPECT_EQ(server_->tls_handshakes(), 0U);
}

TEST_F(WsApiFactoryTest, AServerShutdownLeadsToAFreshSignedGeneration) {
    hy::WsApiSessionFactory factory(deps(), endpoint());
    Supervisor sup([&factory](std::uint64_t g) { return factory(g); }, fast_policy());
    ASSERT_TRUE(poll_until(sup, [&] { return sup.healthy(); }));
    ASSERT_EQ(sup.generation(), 1U);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    server_->send_text(kServerShutdown);
    ASSERT_TRUE(poll_until(sup, [&] { return sup.generation() == 2U && sup.healthy(); })) << "generation " << sup.generation();
    std::vector<UserDataWsEvent> events;
    ASSERT_TRUE(wait_for_events(1, events));
    EXPECT_EQ(events[0].kind, UserDataEventKind::ServerShutdown) << "the old generation says why it ended before it goes";
    EXPECT_EQ(events[0].generation, 1U);
    const std::vector<std::string> frames = only(server_->received_frames(), &is_subscribe);
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_NE(frames[0], frames[1]);
    sup.shutdown();
}

TEST_F(WsApiFactoryTest, APlannedRolloverReplacesTheConnectionAndSignsAgain) {
    hy::FeedSupervisorPolicy p = fast_policy();
    p.max_connection_age_ms = 150;  // stands in for Binance's 24 hours
    hy::WsApiSessionFactory factory(deps(), endpoint());
    Supervisor sup([&factory](std::uint64_t g) { return factory(g); }, p);
    ASSERT_TRUE(poll_until(sup, [&] { return sup.healthy(); }));
    const std::shared_ptr<Session> first = sup.current();
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(poll_until(sup, [&] { return sup.generation() >= 2U && sup.healthy(); }));
    EXPECT_GE(sup.stats().rollovers, 1U);
    EXPECT_TRUE(first->stopped());
    EXPECT_EQ(first->end_cause(), WsApiSessionEnd::Requested) << "the supervisor asked it to stop: a rollover, not a failure";
    EXPECT_EQ(first->stats_snapshot().errors, 0U);
    const std::vector<std::string> frames = only(server_->received_frames(), &is_subscribe);
    ASSERT_GE(frames.size(), 2U);
    EXPECT_NE(frames[0], frames[1]);
    sup.shutdown();
}
