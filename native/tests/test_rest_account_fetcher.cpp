// Private-stream migration M5a-2a: the real account port (rest_account_fetcher.hpp) and the startup read of the account's uid.
//
// What is pinned here:
//   * account_fetch_outcome_of(): every PrivateRestError has its answer -- the ones that mean "nothing left the process" are NotSent, the ones that mean "the
//     network was tried or the answer is unusable" are Failed, an out-of-range value is Failed (never a success);
//   * the adapter hands the snapshot over only on a total success, whatever a failing client does with the buffer it was given, and never lets an exception out
//     of the noexcept port; its counters say which of the three things happened;
//   * it is the refresher's port in the way the refresher needs (a refusal and a failure both retry at the retry interval, a success is delivered);
//   * fetch_exchange_account_uid() assigns the uid only on a good answer and never throws;
//   * against the REAL client over loopback TLS: a good answer arrives, an HTTP error is Failed, no clock is NotSent, a missing credential is NotSent, and
//     credentials bound on ANOTHER thread are NotSent too (F11: the signing refuses) -- the adapter is only usable on the thread that owns the client.
#include <gtest/gtest.h>

#include <hengyuan/rest_account_fetcher.hpp>

#include "binance_private_rest_test_hooks.hpp"
#include "test_helpers/bound_test_credentials.hpp"
#include "test_helpers/tls_response_acceptor.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace hy;

namespace {

AccountSnapshot make_snapshot(std::int64_t btc_free, std::int64_t stamp = 1'700'000'000'000) {
    AccountSnapshotBuilder b;
    b.set_can_trade(true);
    EXPECT_EQ(b.add_asset("BTC", btc_free, 0), AccountSnapshotError::None);
    AccountSnapshot out{};
    EXPECT_EQ(b.build(stamp, out), AccountSnapshotError::None);
    return out;
}

// A client the test scripts: what fetch_account() answers, whether it throws, and what it does to the buffer it was given when it fails.
struct FakeClient {
    PrivateRestError answer{PrivateRestError::None};
    bool throws{false};
    bool scribble_on_failure{true};  // a hostile client: write garbage into `out` and still report an error
    std::uint64_t uid{354'937'868};
    int calls{0};
    int uid_calls{0};

    PrivateRestError fetch_account(AccountSnapshot& out) {
        ++calls;
        if (throws) throw std::runtime_error("boom");
        if (answer == PrivateRestError::None) {
            out = make_snapshot(5);
        } else if (scribble_on_failure) {
            out = make_snapshot(999);
        }
        return answer;
    }
    PrivateRestError fetch_account(AccountSnapshot& out, std::uint64_t& uid_out) {
        ++uid_calls;
        if (throws) throw std::runtime_error("boom");
        if (answer == PrivateRestError::None) {
            out = make_snapshot(5);
            uid_out = uid;
        } else if (scribble_on_failure) {
            out = make_snapshot(999);
            uid_out = 999;
        }
        return answer;
    }
};

constexpr PrivateRestError kAllErrors[] = {
    PrivateRestError::None,          PrivateRestError::InvalidConfig,     PrivateRestError::Resolve,           PrivateRestError::Connect,
    PrivateRestError::TlsHandshake,  PrivateRestError::Write,             PrivateRestError::Read,              PrivateRestError::HttpStatus,
    PrivateRestError::JsonParse,     PrivateRestError::MalformedResponse, PrivateRestError::CapacityExceeded,  PrivateRestError::ClockNotFresh,
    PrivateRestError::SigningFailed, PrivateRestError::ClockSkewTooLarge, PrivateRestError::RateLimitExhausted,
};

}  // namespace

// --- the mapping ------------------------------------------------------------------------------------------------------------------

TEST(AccountFetchOutcomeOf, EveryErrorHasItsAnswer) {
    for (const PrivateRestError e : kAllErrors) {
        AccountFetchOutcome expected = AccountFetchOutcome::Failed;
        switch (e) {
            case PrivateRestError::None: expected = AccountFetchOutcome::Fetched; break;
            case PrivateRestError::InvalidConfig:
            case PrivateRestError::ClockNotFresh:
            case PrivateRestError::SigningFailed:
            case PrivateRestError::ClockSkewTooLarge:
            case PrivateRestError::RateLimitExhausted: expected = AccountFetchOutcome::NotSent; break;
            default: expected = AccountFetchOutcome::Failed; break;
        }
        EXPECT_EQ(account_fetch_outcome_of(e), expected) << static_cast<int>(e);
    }
    static_assert(account_fetch_outcome_of(PrivateRestError::None) == AccountFetchOutcome::Fetched);
    static_assert(account_fetch_outcome_of(PrivateRestError::RateLimitExhausted) == AccountFetchOutcome::NotSent);
    static_assert(account_fetch_outcome_of(PrivateRestError::HttpStatus) == AccountFetchOutcome::Failed);
}

TEST(AccountFetchOutcomeOf, ASuccessIsNeverInventedFromAValueNoOneDefined) {
    EXPECT_EQ(account_fetch_outcome_of(static_cast<PrivateRestError>(200)), AccountFetchOutcome::Failed);
    EXPECT_EQ(account_fetch_outcome_of(static_cast<PrivateRestError>(15)), AccountFetchOutcome::Failed) << "the first value past the last enumerator";
}

// --- the adapter, against a scripted client -----------------------------------------------------------------------------------------

TEST(RestAccountFetcherFake, ASuccessHandsOverTheSnapshotAndIsCounted) {
    FakeClient client;
    BasicRestAccountFetcher<FakeClient> fetcher{client};
    AccountSnapshot out{};
    EXPECT_EQ(fetcher.port().call(out), AccountFetchOutcome::Fetched);
    ASSERT_NE(out.find("BTC"), nullptr);
    EXPECT_EQ(out.find("BTC")->free_ticks(), 5);
    EXPECT_EQ(fetcher.stats().calls, 1U);
    EXPECT_EQ(fetcher.stats().fetched, 1U);
    EXPECT_EQ(fetcher.stats().not_sent + fetcher.stats().failed, 0U);
    EXPECT_EQ(fetcher.stats().last_error, PrivateRestError::None);
    EXPECT_EQ(client.calls, 1);
    EXPECT_EQ(client.uid_calls, 0) << "the periodic and per-sweep fetches do not ask for the uid";
}

TEST(RestAccountFetcherFake, EveryErrorIsCountedForWhatItIsAndNeverTouchesOut) {
    for (const PrivateRestError e : kAllErrors) {
        if (e == PrivateRestError::None) continue;
        SCOPED_TRACE(static_cast<int>(e));
        FakeClient client;
        client.answer = e;  // the hostile client also scribbles over the buffer it was given
        BasicRestAccountFetcher<FakeClient> fetcher{client};
        AccountSnapshot out = make_snapshot(7);
        const AccountFetchOutcome outcome = fetcher.port().call(out);
        EXPECT_EQ(outcome, account_fetch_outcome_of(e));
        ASSERT_NE(out.find("BTC"), nullptr);
        EXPECT_EQ(out.find("BTC")->free_ticks(), 7) << "the caller's snapshot moves on a total success only";
        const RestAccountFetcherStats& s = fetcher.stats();
        EXPECT_EQ(s.calls, 1U);
        EXPECT_EQ(s.fetched, 0U);
        EXPECT_EQ(s.not_sent, outcome == AccountFetchOutcome::NotSent ? 1U : 0U);
        EXPECT_EQ(s.failed, outcome == AccountFetchOutcome::Failed ? 1U : 0U);
        EXPECT_EQ(s.last_error, e);
        EXPECT_EQ(s.threw, 0U);
    }
}

TEST(RestAccountFetcherFake, AnExceptionIsAFailureAndNeverEscapesThePort) {
    FakeClient client;
    client.throws = true;
    BasicRestAccountFetcher<FakeClient> fetcher{client};
    const AccountFetchPort port = fetcher.port();
    AccountSnapshot out = make_snapshot(7);
    static_assert(noexcept(port.call(out)), "the refresher calls the port from a noexcept context");
    EXPECT_EQ(port.call(out), AccountFetchOutcome::Failed);
    EXPECT_EQ(out.find("BTC")->free_ticks(), 7);
    EXPECT_EQ(fetcher.stats().threw, 1U);
    EXPECT_EQ(fetcher.stats().failed, 1U);
    EXPECT_EQ(fetcher.stats().last_error, PrivateRestError::Read);
}

TEST(RestAccountFetcherFake, TheRefresherTreatsARefusalAndAFailureAlikeAndDeliversASuccess) {
    FakeClient client;
    BasicRestAccountFetcher<FakeClient> fetcher{client};
    AccountUpdateRing ring;
    AccountRefresher refresher{ring, fetcher.port(), AccountRefreshPolicy{10'000, 2'000}};

    client.answer = PrivateRestError::RateLimitExhausted;
    refresher.tick(1'000);
    EXPECT_EQ(refresher.stats().not_sent, 1U);
    client.answer = PrivateRestError::HttpStatus;
    refresher.tick(1'000 + 2'000);
    EXPECT_EQ(refresher.stats().failed, 1U);
    EXPECT_EQ(client.calls, 2) << "both were retried at the retry interval, not after a period";
    client.answer = PrivateRestError::None;
    refresher.tick(1'000 + 2 * 2'000);
    EXPECT_EQ(refresher.stats().delivered, 1U);
    AccountUpdate update{};
    ASSERT_TRUE(ring.try_pop(update));
    EXPECT_EQ(update.sweep_epoch, 0U);
    EXPECT_EQ(update.snapshot.find("BTC")->free_ticks(), 5);
}

// --- the startup read of the uid ---------------------------------------------------------------------------------------------------

TEST(FetchExchangeAccountUid, TheUidTheExchangeReportedIsHandedOver) {
    FakeClient client;
    std::uint64_t uid = 0;
    EXPECT_EQ(fetch_exchange_account_uid(client, uid), PrivateRestError::None);
    EXPECT_EQ(uid, 354'937'868U);
    EXPECT_EQ(client.uid_calls, 1);
    EXPECT_EQ(client.calls, 0);
}

TEST(FetchExchangeAccountUid, AnErrorLeavesTheUidAlone) {
    for (const PrivateRestError e : kAllErrors) {
        if (e == PrivateRestError::None) continue;
        SCOPED_TRACE(static_cast<int>(e));
        FakeClient client;
        client.answer = e;
        std::uint64_t uid = 4242;
        EXPECT_EQ(fetch_exchange_account_uid(client, uid), e);
        EXPECT_EQ(uid, 4242U) << "even a client that scribbles into its own output does not move the caller's uid";
    }
}

TEST(FetchExchangeAccountUid, AnExceptionIsAReadFailureAndNeverEscapes) {
    FakeClient client;
    client.throws = true;
    std::uint64_t uid = 4242;
    static_assert(noexcept(fetch_exchange_account_uid(client, uid)));
    EXPECT_EQ(fetch_exchange_account_uid(client, uid), PrivateRestError::Read);
    EXPECT_EQ(uid, 4242U);
}

// --- the real client over loopback TLS ------------------------------------------------------------------------------------------------

namespace {

std::string fixture_path(const char* filename) { return std::string(HY_TEST_FIXTURE_DIR) + "/" + filename; }

constexpr const char* kAccountBody = R"({"uid":354937868,"canTrade":true,"balances":[{"asset":"BTC","free":"1.5","locked":"0.5"}]})";

class RealClientTest : public ::testing::Test {
protected:
    hy::test_helpers::BoundTestCredentials creds_{"restfetch"};

    std::unique_ptr<BinancePrivateRestClient> make_client(const PrivateRestConfig& cfg, bool fresh_clock = true) {
        EXPECT_TRUE(creds_.ok());
        auto client = std::make_unique<BinancePrivateRestClient>(EnvironmentBinding::testnet(), creds_.release(), nullptr, EndpointWeightTable{}, cfg);
        BinancePrivateRestClientTestHooks::use_loopback(*client, fixture_path("test_leaf_cert_testnet_host.pem"));
        if (fresh_clock) BinancePrivateRestClientTestHooks::publish_fresh_clock(*client);
        return client;
    }
    static std::unique_ptr<hy::test_helpers::TlsResponseAcceptor> server(int status, const std::string& body) {
        return std::make_unique<hy::test_helpers::TlsResponseAcceptor>(fixture_path("test_leaf_cert_testnet_host.pem"),
                                                                       fixture_path("test_leaf_key_testnet_host.pem"), status, body);
    }
    static PrivateRestConfig config_for(const hy::test_helpers::TlsResponseAcceptor& s) {
        PrivateRestConfig cfg;
        cfg.port = std::to_string(s.port());
        return cfg;
    }
};

}  // namespace

TEST_F(RealClientTest, ARealAnswerComesThroughThePort) {
    auto srv = server(200, kAccountBody);
    auto client = make_client(config_for(*srv));
    RestAccountFetcher fetcher{*client};
    AccountSnapshot out{};
    EXPECT_EQ(fetcher.port().call(out), AccountFetchOutcome::Fetched);
    ASSERT_NE(out.find("BTC"), nullptr);
    EXPECT_EQ(out.find("BTC")->free_ticks(), 150'000'000);
    EXPECT_EQ(srv->requests().size(), 1U);
}

TEST_F(RealClientTest, TheStartupReadGivesTheUidTheServerReported) {
    auto srv = server(200, kAccountBody);
    auto client = make_client(config_for(*srv));
    std::uint64_t uid = 0;
    EXPECT_EQ(fetch_exchange_account_uid(*client, uid), PrivateRestError::None);
    EXPECT_EQ(uid, 354'937'868U);
}

TEST_F(RealClientTest, AnHttpErrorIsFailed) {
    auto srv = server(401, R"({"code":-2015,"msg":"denied"})");
    auto client = make_client(config_for(*srv));
    RestAccountFetcher fetcher{*client};
    AccountSnapshot out = make_snapshot(7);
    EXPECT_EQ(fetcher.port().call(out), AccountFetchOutcome::Failed);
    EXPECT_EQ(fetcher.stats().last_error, PrivateRestError::HttpStatus);
    EXPECT_EQ(out.find("BTC")->free_ticks(), 7);
}

TEST_F(RealClientTest, WithoutAFreshClockNothingIsSent) {
    auto srv = server(200, kAccountBody);
    auto client = make_client(config_for(*srv), /*fresh_clock=*/false);
    RestAccountFetcher fetcher{*client};
    AccountSnapshot out{};
    EXPECT_EQ(fetcher.port().call(out), AccountFetchOutcome::NotSent);
    EXPECT_EQ(fetcher.stats().last_error, PrivateRestError::ClockNotFresh);
    EXPECT_TRUE(srv->requests().empty()) << "a refused request never reaches the server";
}

TEST_F(RealClientTest, WithoutCredentialsNothingIsSent) {
    auto srv = server(200, kAccountBody);
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), nullptr, nullptr, EndpointWeightTable{}, config_for(*srv));
    BinancePrivateRestClientTestHooks::use_loopback(client, fixture_path("test_leaf_cert_testnet_host.pem"));
    BinancePrivateRestClientTestHooks::publish_fresh_clock(client);
    RestAccountFetcher fetcher{client};
    AccountSnapshot out{};
    EXPECT_EQ(fetcher.port().call(out), AccountFetchOutcome::NotSent);
    EXPECT_EQ(fetcher.stats().last_error, PrivateRestError::SigningFailed);
    EXPECT_TRUE(srv->requests().empty());
}

TEST(RealClientThread, CredentialsBoundOnAnotherThreadCannotSignSoNothingIsSent) {
    // F11: BoundHmacCredentials answers only on the thread that bound it. The adapter must therefore be used on that thread; used anywhere else it is
    // refused at the signing step, which the port reports as NotSent -- the sweep's barrier stays Sweeping, it never trades on a guess.
    std::unique_ptr<BoundHmacCredentials> creds;
    std::thread binder([&creds] {
        hy::test_helpers::BoundTestCredentials bound{"restfetch-other"};
        if (bound.ok()) creds = bound.release();
    });
    binder.join();
    ASSERT_NE(creds, nullptr);

    hy::test_helpers::TlsResponseAcceptor srv(fixture_path("test_leaf_cert_testnet_host.pem"), fixture_path("test_leaf_key_testnet_host.pem"), 200,
                                              kAccountBody);
    PrivateRestConfig cfg;
    cfg.port = std::to_string(srv.port());
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), std::move(creds), nullptr, EndpointWeightTable{}, cfg);
    BinancePrivateRestClientTestHooks::use_loopback(client, fixture_path("test_leaf_cert_testnet_host.pem"));
    BinancePrivateRestClientTestHooks::publish_fresh_clock(client);

    RestAccountFetcher fetcher{client};
    AccountSnapshot out{};
    EXPECT_EQ(fetcher.port().call(out), AccountFetchOutcome::NotSent);
    EXPECT_EQ(fetcher.stats().last_error, PrivateRestError::SigningFailed);
    EXPECT_TRUE(srv.requests().empty());
}
