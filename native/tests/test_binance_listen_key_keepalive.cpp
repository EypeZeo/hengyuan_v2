// Batch H, H3: binance_listen_key_keepalive.hpp unit tests.
//
// Same no-real-network discipline as test_binance_private_rest.cpp: a local TlsResponseAcceptor
// (test_helpers/tls_response_acceptor.hpp) stands in for Binance's userDataStream endpoints.
// BinancePrivateRestClient's no-cfg keepalive_listen_key()/create_listen_key() overloads (Batch
// H, H2) are what the scheduler actually calls, so every client here is constructed with an
// explicit default_cfg pointed at the mock server (the port) and given a loopback RestTestSeam
// (use_fixture) -- there is no other way to route these calls away from the real Binance host in tests.
#include <gtest/gtest.h>
#include <hengyuan/binance_listen_key_keepalive.hpp>

#include "binance_private_rest_test_hooks.hpp"
#include "test_helpers/tls_response_acceptor.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using hy::BinancePrivateRestClient;
using hy::BoundHmacCredentials;
using hy::EnvAllowlist;
using hy::EnvironmentBinding;
using hy::ListenKeyKeepaliveScheduler;
using hy::ListenKeyKeepalivePolicy;
using hy::ListenKeyPublisher;
using hy::PrivateRestConfig;
using hy::PrivateRestError;
using hy::QuerySigningError;
using hy::SecureEnvLoader;

namespace {

std::string fixture_path(const char* filename) {
    return std::string(HY_TEST_FIXTURE_DIR) + "/" + filename;
}

constexpr std::string_view kSyntheticApiKey =
    "TESTKEYabcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234";
constexpr std::string_view kSyntheticSecret = "synthetic-test-secret-not-real";

constexpr std::string_view kTestnetAllowedKeys[] = {
    "HENGYUAN_BINANCE_TESTNET_API_KEY",
    "HENGYUAN_BINANCE_TESTNET_SECRET",
};
constexpr EnvAllowlist kTestnetAllowlist{kTestnetAllowedKeys, 2};

class BoundCredentialsFixture : public ::testing::Test {
protected:
    std::string tmp_path_;

    void SetUp() override {
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        tmp_path_ =
            std::string(tmp) + "hy_test_lkkeepalive_" + std::to_string(GetCurrentProcessId()) + ".env";
#else
        tmp_path_ = "/tmp/hy_test_lkkeepalive_" + std::to_string(getpid()) + ".env";
#endif
        std::ofstream f(tmp_path_, std::ios::binary);
        f << "HENGYUAN_BINANCE_TESTNET_API_KEY=" << kSyntheticApiKey << "\n";
        f << "HENGYUAN_BINANCE_TESTNET_SECRET=" << kSyntheticSecret << "\n";
        f.close();
#ifdef __linux__
        chmod(tmp_path_.c_str(), 0600);
#endif
    }

    void TearDown() override { std::remove(tmp_path_.c_str()); }

    std::unique_ptr<BoundHmacCredentials> make_creds() {
        SecureEnvLoader loader;
        EXPECT_EQ(loader.load(tmp_path_.c_str(), kTestnetAllowlist).status, hy::EnvLoadStatus::Ok);
        auto [err, creds] =
            BoundHmacCredentials::load_and_bind_credentials(EnvironmentBinding::testnet(), loader);
        EXPECT_EQ(err, QuerySigningError::Ok);
        return std::move(creds);
    }

    // default_cfg pointed at `server` -- see this file's own header comment on why every
    // client needs one (the scheduler only ever calls the no-cfg overloads).
    PrivateRestConfig cfg_for(const hy::test_helpers::TlsResponseAcceptor& server) {
        PrivateRestConfig cfg;
        cfg.port = std::to_string(server.port());
        return cfg;
    }

    // Routes the client's connections to the loopback fixture and trusts the fixture's certificate
    // (rest_test_seam.hpp); without it every call would go to the real testnet host.
    static void use_fixture(BinancePrivateRestClient& client) {
        hy::BinancePrivateRestClientTestHooks::use_loopback(client,
                                                            fixture_path("test_leaf_cert_testnet_host.pem"));
    }
};

}  // namespace

TEST_F(BoundCredentialsFixture, NeverPublishedKeyGuardIsSilentNoNetworkRequest) {
    hy::test_helpers::TlsResponseAcceptor server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200, R"({})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds(), nullptr, {},
                                     cfg_for(server));
    use_fixture(client);
    ListenKeyPublisher listen_key_pub;  // never published -- seq stays 0
    ListenKeyKeepaliveScheduler scheduler(client, listen_key_pub);

    scheduler.poll(0);

    EXPECT_EQ(server.requests().size(), 0u);
    EXPECT_EQ(scheduler.stats().keepalive_attempts, 0u);
    EXPECT_EQ(scheduler.stats().recreation_attempts, 0u);
}

TEST_F(BoundCredentialsFixture, SuccessfulKeepaliveExtendsExpiryAndSchedulesNextIntervalOut) {
    hy::test_helpers::TlsResponseAcceptor server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200, R"({})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds(), nullptr, {},
                                     cfg_for(server));
    use_fixture(client);
    ListenKeyPublisher listen_key_pub;
    ASSERT_TRUE(listen_key_pub.publish("initial-listen-key-0000000000000001", /*issued_at_ms=*/0,
                                        /*expires_at_ms=*/60 * 60 * 1000));

    ListenKeyKeepalivePolicy policy{};
    policy.keepalive_interval_ms = 50;
    policy.listen_key_ttl_ms = 60 * 60 * 1000;  // still "far from expiring" at t=50/100
    ListenKeyKeepaliveScheduler scheduler(client, listen_key_pub, policy);

    scheduler.poll(0);  // first tick: seq 1 != last_seen_seq_ 0 -> aligns, no network call yet
    EXPECT_EQ(server.requests().size(), 0u);

    scheduler.poll(50);  // now_ms reaches next_attempt_at_ms_ (issued_at_ms(0) + interval(50))
    EXPECT_EQ(server.requests().size(), 1u);
    EXPECT_NE(server.requests()[0].target.find("listenKey="), std::string::npos);
    EXPECT_EQ(scheduler.stats().keepalive_successes, 1u);
    EXPECT_EQ(scheduler.stats().keepalive_failures, 0u);
    EXPECT_EQ(scheduler.stats().consecutive_failures, 0u);

    const auto snap = listen_key_pub.load();
    EXPECT_EQ(snap.expires_at_ms, 50 + 60 * 60 * 1000);  // extended from now_ms(50), not stale
    EXPECT_EQ(snap.issued_at_ms, 0);  // keepalive never changes issued_at_ms

    // Regression guard for the self-triggered-storm bug (round 15): the scheduler's own
    // publish() bumped seq to 2; poll() must have immediately re-synced last_seen_seq_ so the
    // NEXT tick does not misclassify that as an external rotation and does not fire again
    // before the next real interval elapses.
    scheduler.poll(60);  // 10ms after the keepalive succeeded -- must NOT fire again
    EXPECT_EQ(server.requests().size(), 1u);
    scheduler.poll(99);  // still short of 50+50=100
    EXPECT_EQ(server.requests().size(), 1u);
    scheduler.poll(100);  // exactly one interval after the successful keepalive at t=50
    EXPECT_EQ(server.requests().size(), 2u);
    EXPECT_EQ(scheduler.stats().keepalive_successes, 2u);
}

TEST_F(BoundCredentialsFixture, FailedKeepaliveBacksOffByRetryIntervalNotFullInterval) {
    hy::test_helpers::TlsResponseAcceptor server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 500, R"({"code":-1000,"msg":"fail"})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds(), nullptr, {},
                                     cfg_for(server));
    use_fixture(client);
    ListenKeyPublisher listen_key_pub;
    ASSERT_TRUE(listen_key_pub.publish("initial-listen-key-0000000000000002", 0, 60 * 60 * 1000));

    ListenKeyKeepalivePolicy policy{};
    policy.keepalive_interval_ms = 1000;
    policy.retry_backoff_ms = 10;
    ListenKeyKeepaliveScheduler scheduler(client, listen_key_pub, policy);

    scheduler.poll(0);  // align tick
    scheduler.poll(1000);  // due -> fires, fails (HTTP 500)
    EXPECT_EQ(server.requests().size(), 1u);
    EXPECT_EQ(scheduler.stats().keepalive_failures, 1u);
    EXPECT_EQ(scheduler.stats().keepalive_successes, 0u);
    EXPECT_EQ(scheduler.stats().consecutive_failures, 1u);
    EXPECT_EQ(scheduler.stats().last_error, PrivateRestError::HttpStatus);

    scheduler.poll(1005);  // short of retry_backoff_ms(10)
    EXPECT_EQ(server.requests().size(), 1u);
    scheduler.poll(1010);  // exactly one backoff interval after the failed attempt
    EXPECT_EQ(server.requests().size(), 2u);
    EXPECT_EQ(scheduler.stats().keepalive_failures, 2u);
    EXPECT_EQ(scheduler.stats().consecutive_failures, 2u);
}

TEST_F(BoundCredentialsFixture, ExpiredKeyFallsBackToRecreationInsteadOfKeepalive) {
    hy::test_helpers::TlsResponseAcceptor server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"listenKey":"pqia91ma19a5s61cv6a81va65sdf19v8a65a1a5s61cv6a81va65sdf19v8a65a1"})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds(), nullptr, {},
                                     cfg_for(server));
    use_fixture(client);
    ListenKeyPublisher listen_key_pub;
    // expires_at_ms deliberately in the past relative to the poll() calls below.
    ASSERT_TRUE(listen_key_pub.publish("stale-listen-key-000000000000000001", 0, /*expires_at_ms=*/50));

    ListenKeyKeepalivePolicy policy{};
    policy.keepalive_interval_ms = 50;
    policy.listen_key_ttl_ms = 60 * 60 * 1000;
    ListenKeyKeepaliveScheduler scheduler(client, listen_key_pub, policy);

    scheduler.poll(0);  // align tick
    scheduler.poll(50);  // due, AND now_ms(50) >= expires_at_ms(50) -> recreation path, not PUT

    ASSERT_EQ(server.requests().size(), 1u);
    EXPECT_EQ(server.requests()[0].target, "/api/v3/userDataStream");  // POST create, not PUT
    EXPECT_EQ(scheduler.stats().recreation_attempts, 1u);
    EXPECT_EQ(scheduler.stats().recreation_successes, 1u);
    EXPECT_EQ(scheduler.stats().keepalive_attempts, 0u);

    const auto snap = listen_key_pub.load();
    EXPECT_EQ(snap.view(), "pqia91ma19a5s61cv6a81va65sdf19v8a65a1a5s61cv6a81va65sdf19v8a65a1");
    EXPECT_EQ(snap.issued_at_ms, 50);  // MUST be now_ms of recreation, not the stale old value
    EXPECT_EQ(snap.expires_at_ms, 50 + 60 * 60 * 1000);

    // Same self-triggered-storm regression guard as the keepalive-success test, for the
    // recreation path.
    scheduler.poll(60);
    EXPECT_EQ(server.requests().size(), 1u);
}

TEST_F(BoundCredentialsFixture, PollFromNonOwningThreadFoldsToFailureWithoutCrashing) {
    hy::test_helpers::TlsResponseAcceptor server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200, R"({})");
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds(), nullptr, {},
                                     cfg_for(server));
    use_fixture(client);
    ListenKeyPublisher listen_key_pub;
    ASSERT_TRUE(listen_key_pub.publish("initial-listen-key-0000000000000003", 0, 60 * 60 * 1000));

    ListenKeyKeepalivePolicy policy{};
    policy.keepalive_interval_ms = 0;  // due immediately on the aligned tick
    ListenKeyKeepaliveScheduler scheduler(client, listen_key_pub, policy);
    scheduler.poll(0);  // align tick on THIS (owning) thread

    // copy_api_key() fail-closes off the credential-owning thread (binance_environment.hpp) --
    // keepalive_listen_key() should return a non-None error, which poll() must fold into its
    // failure bookkeeping rather than let any exception/crash escape.
    std::thread t([&scheduler]() { scheduler.poll(0); });
    t.join();

    // No crash is the primary assertion (the test process is still alive to check this at
    // all); the call must also not have been silently treated as a success.
    EXPECT_EQ(scheduler.stats().keepalive_successes, 0u);
}
