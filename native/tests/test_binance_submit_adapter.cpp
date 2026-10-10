// Batch H, H5: binance_submit_adapter.hpp unit + integration tests.
//
// Split into three tiers, matching this file's own design discussion in the batch plan:
//   1. Unit-level: composite_submit_order_adapter()/composite_current_rules_version_adapter()
//      called directly, no orchestrator involved -- null-guard behavior and the real
//      SymbolRegistry version passthrough (0 -> 1 -> 2), fully decoupled from gate ordering.
//   2. Integration-level Gate 1: orchestrate_submit() through a real SubmitPort built by
//      make_binance_submit_port(), reusing test_live_submit_orchestrator.cpp's LiveSubmitTest
//      fixture pattern verbatim (every earlier gate must be pre-satisfied, or a Gate 1 test
//      silently short-circuits on AuditUnavailable and never reaches the version check).
//   3. Real network dispatch: a TlsResponseAcceptor-backed BinancePrivateRestClient, driven
//      through SubmitPort::call() via the composite adapter -- same fixture/mock-server
//      pattern as test_binance_private_rest.cpp's SubmitOrderRealSuccessPathSignsAndParses.
//
// No real network, no real credentials -- synthetic test keys only, matching every other
// Boost-linked test file in this directory.

#include <gtest/gtest.h>
#include <hengyuan/binance_submit_adapter.hpp>

#include "binance_private_rest_test_hooks.hpp"
#include "test_helpers/tls_response_acceptor.hpp"
#include "test_symbol_registry_fakes.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace hy;

namespace {

// --- Shared helpers ---

SymbolRules make_btcusdt_rules(std::uint32_t rules_version) {
    SymbolRules rules{};
    std::strncpy(rules.symbol, "BTCUSDT", sizeof(rules.symbol) - 1);
    rules.is_trading = true;
    rules.price_scale = 2;
    rules.qty_scale = 6;
    rules.quote_scale = 8;
    rules.rules_version = rules_version;
    return rules;
}

// --- Tier 1: unit-level null-guard + version-passthrough tests, no orchestrator ---

TEST(BinanceSubmitAdapter, CompositeSubmitOrderAdapterNullUserDataReturnsNetworkError) {
    const auto resp = composite_submit_order_adapter("coid-1", 1, OrderSide::Buy, OrderType::Limit,
                                                       100, 10, make_btcusdt_rules(1), nullptr);
    EXPECT_EQ(resp.outcome, SubmitOutcome::NetworkError);
    EXPECT_EQ(resp.exchange_order_id, 0);
    EXPECT_EQ(resp.error_code, -1);
}

TEST(BinanceSubmitAdapter, CompositeSubmitOrderAdapterNullClientOrderIdReturnsNetworkError) {
    // A dummy client with no bound credentials is enough here -- the null client_order_id
    // guard must fire before anything touches the client at all.
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), nullptr);
    SymbolRegistry registry;
    SubmitAdapterContext ctx{&client, &registry};
    const auto resp = composite_submit_order_adapter(nullptr, 1, OrderSide::Buy, OrderType::Limit,
                                                       100, 10, make_btcusdt_rules(1), &ctx);
    EXPECT_EQ(resp.outcome, SubmitOutcome::NetworkError);
}

TEST(BinanceSubmitAdapter, CompositeSubmitOrderAdapterNullClientInContextReturnsNetworkError) {
    SymbolRegistry registry;
    SubmitAdapterContext ctx{nullptr, &registry};
    const auto resp = composite_submit_order_adapter("coid-2", 1, OrderSide::Buy, OrderType::Limit,
                                                       100, 10, make_btcusdt_rules(1), &ctx);
    EXPECT_EQ(resp.outcome, SubmitOutcome::NetworkError);
}

TEST(BinanceSubmitAdapter, CompositeCurrentRulesVersionAdapterNullUserDataReturnsZero) {
    EXPECT_EQ(composite_current_rules_version_adapter(nullptr), 0u);
}

TEST(BinanceSubmitAdapter, CompositeCurrentRulesVersionAdapterNullRegistryReturnsZero) {
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), nullptr);
    SubmitAdapterContext ctx{&client, nullptr};
    EXPECT_EQ(composite_current_rules_version_adapter(&ctx), 0u);
}

TEST(BinanceSubmitAdapter, CompositeCurrentRulesVersionAdapterPassthroughTracksRegistryRefreshes) {
    SymbolRegistry registry;
    SubmitAdapterContext ctx{nullptr, &registry};
    // Never refreshed -- rules_version 0 is "unknown/unavailable" by construction.
    EXPECT_EQ(composite_current_rules_version_adapter(&ctx), 0u);

    FakeSymbolRegistrySink sink;
    ASSERT_TRUE(registry.refresh_from_exchange_info(
        sink, make_single_symbol_parsed(1'700'000'000'000, 2, 6, 1000)));
    EXPECT_EQ(composite_current_rules_version_adapter(&ctx), 1u);

    ASSERT_TRUE(registry.refresh_from_exchange_info(
        sink, make_single_symbol_parsed(1'700'000'001'000, 2, 6, 1000)));
    EXPECT_EQ(composite_current_rules_version_adapter(&ctx), 2u);
}

TEST(BinanceSubmitAdapter, MakeBinanceSubmitPortWiresBothFunctionPointersToTheSameContext) {
    SymbolRegistry registry;
    FakeSymbolRegistrySink sink;
    ASSERT_TRUE(registry.refresh_from_exchange_info(
        sink, make_single_symbol_parsed(1'700'000'000'000, 2, 6, 1000)));
    SubmitAdapterContext ctx{nullptr, &registry};

    const SubmitPort port = make_binance_submit_port(ctx);
    EXPECT_TRUE(port.is_valid());
    EXPECT_EQ(port.current_rules_version(), registry.current_rules_version());
    EXPECT_EQ(port.current_rules_version(), 1u);
}

// --- Tier 2: Gate 1 integration through a real SubmitPort, LiveSubmitTest-style fixture ---
//
// Reuses test_live_submit_orchestrator.cpp's exact SetUp() pattern (all six earlier gates
// pre-satisfied) so a Gate 1 assertion here is never accidentally validating an earlier gate
// instead -- see this file's own header comment on the short-circuit trap this avoids.

struct FakeDurableAudit {
    bool intent_should_fail{false};
    bool prepared_should_fail{false};
    bool outcome_should_fail{false};
    std::uint64_t next_sequence{0};
    bool is_fenced{false};
};

AuditAppendResult fake_durable_append(void* user_data, const AuditRecord& rec, std::int64_t) noexcept {
    auto* f = static_cast<FakeDurableAudit*>(user_data);
    const bool is_intent = rec.event_type == AuditEventType::OrderIntentCreated;
    const bool is_prepared = rec.event_type == AuditEventType::OrderSubmitPrepared;
    const bool fail = (is_intent && f->intent_should_fail) ||
                       (is_prepared && f->prepared_should_fail) ||
                       (!is_intent && !is_prepared && f->outcome_should_fail);
    AuditAppendResult r{};
    if (fail) {
        f->is_fenced = true;
        r.status = AuditAppendResult::Status::Failed;
        return r;
    }
    r.status = AuditAppendResult::Status::Acked;
    r.sequence = f->next_sequence++;
    return r;
}

bool fake_durable_fenced(void* user_data) noexcept {
    return static_cast<FakeDurableAudit*>(user_data)->is_fenced;
}

class SubmitAdapterOrchestratorTest : public ::testing::Test {
protected:
    AuditRingSink audit_;
    KillSwitch kill_switch_;
    DryRunEvidenceChain evidence_;
    SpotRateLimitTracker rate_limiter_;
    InFlightRegistry in_flight_;
    SymbolRules rules_{};
    AccountSnapshot account_{};
    OrchestratorContext ctx_{};
    FakeDurableAudit fake_durable_audit_{};
    SymbolRegistry registry_;
    FakeSymbolRegistrySink sink_;
    SubmitAdapterContext submit_ctx_{};

    void SetUp() override {
        // Default: all gates pass -- verbatim copy of LiveSubmitTest::SetUp()'s baseline
        // (test_live_submit_orchestrator.cpp), so no earlier gate can silently swallow the
        // Gate 1 assertions below.
        audit_.set_available(true);
        kill_switch_.operator_reset();
        fill_evidence();
        rate_limiter_.configure(6000, 500, 60000, 5000, 100, 10);

        std::strncpy(rules_.symbol, "BTCUSDT", sizeof(rules_.symbol) - 1);
        rules_.is_trading = true;
        rules_.min_price_ticks = 100;
        rules_.max_price_ticks = 10000000;
        rules_.tick_size_ticks = 100;
        rules_.min_qty_ticks = 10;
        rules_.max_qty_ticks = 1000000;
        rules_.step_size_ticks = 10;
        rules_.min_notional_ticks = 1000;
        rules_.price_scale = 0;
        rules_.qty_scale = 8;

        std::strncpy(account_.assets[0].asset, "USDT", 5);
        account_.assets[0].free_ticks = 999999;
        account_.asset_count = 1;
        account_.can_trade = true;
        account_.timestamp_ms = 900;

        ctx_.audit = &audit_;
        ctx_.kill_switch = &kill_switch_;
        ctx_.evidence = &evidence_;
        ctx_.signer_ready = true;
        ctx_.depth_synced = true;
        ctx_.rate_limiter = &rate_limiter_;
        ctx_.in_flight = &in_flight_;
        ctx_.symbol_rules = &rules_;
        ctx_.account = &account_;
        ctx_.side = OrderSide::Buy;
        ctx_.order_type = OrderType::Limit;
        ctx_.base_asset = "BTC";
        ctx_.quote_asset = "USDT";
        ctx_.now_ms = 1000;
        ctx_.exposure_limits.freshness_max_age_ms = 30000;
        ctx_.exposure_limits.single_order_notional_cap = 500000;
        ctx_.exposure_limits.total_exposure_notional_cap = 1000000;
        ctx_.price_ticks = 1000;
        ctx_.qty_ticks = 10;
        ctx_.symbol_id = 1;
        ctx_.sequence = 1;
        ctx_.mode = ExecutionMode::Live;
        ctx_.order_weight = 1;

        bind_confirmation();

        // Registry starts at version 0 (never refreshed); the first refresh below publishes
        // version 1, matching rules_.rules_version so the baseline passes Gate 1 by default,
        // same "both sides matching and non-zero" discipline as LiveSubmitTest::SetUp().
        ASSERT_TRUE(registry_.refresh_from_exchange_info(
            sink_, make_single_symbol_parsed(1'700'000'000'000, 0, 8, 1000)));
        rules_.rules_version = registry_.current_rules_version();
        ASSERT_EQ(rules_.rules_version, 1u);

        // client stays nullptr: every test in this fixture either stops at Gate 1 (never
        // reaches submit_port.call()) or deliberately probes the null-client fail-closed path
        // one gate further in -- no test here needs a real network round trip.
        submit_ctx_.client = nullptr;
        submit_ctx_.registry = &registry_;
        ctx_.submit_port = make_binance_submit_port(submit_ctx_);

        ctx_.durable_audit = {&fake_durable_append, &fake_durable_fenced, &fake_durable_audit_};
    }

    void bind_confirmation() {
        ctx_.confirmation.confirmed = true;
        ctx_.confirmation.symbol_id = ctx_.symbol_id;
        ctx_.confirmation.side = ctx_.side;
        ctx_.confirmation.type = ctx_.order_type;
        ctx_.confirmation.price_ticks = ctx_.price_ticks;
        ctx_.confirmation.qty_ticks = ctx_.qty_ticks;
        ctx_.confirmation.max_notional = 1000000;
        ctx_.confirmation.valid_until_ms = ctx_.now_ms + 60000;
    }

    void fill_evidence() {
        evidence_.record(EvidencePath::SubmitSuccess, 1, 0xABCD, 1);
        evidence_.record(EvidencePath::SubmitReject, 2, 0xABCD, 1);
        evidence_.record(EvidencePath::SubmitAmbiguous, 3, 0xABCD, 1);
        evidence_.record(EvidencePath::KillSwitch, 4, 0xABCD, 1);
    }
};

TEST_F(SubmitAdapterOrchestratorTest, MatchingRulesVersionPassesGate1AndReachesSubmit) {
    ASSERT_EQ(rules_.rules_version, 1u);
    ASSERT_EQ(registry_.current_rules_version(), 1u);
    auto r = orchestrate_submit(ctx_);
    // A null submit client means the actual send fails closed one gate further in than Gate
    // 1 -- reaching SubmitNetworkError (not SubmitStaleRulesVersion) is exactly the proof that
    // Gate 1 itself passed.
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitNetworkError);
}

TEST_F(SubmitAdapterOrchestratorTest, RegistryRefreshMakesSnapshotStaleAndBlocksBeforeSubmit) {
    // The registry moves on to version 2; ctx_.symbol_rules/rules_.rules_version (the
    // captured snapshot) deliberately stays at 1 -- this is the real TOCTOU window Gate 1
    // exists to close.
    ASSERT_TRUE(registry_.refresh_from_exchange_info(
        sink_, make_single_symbol_parsed(1'700'000'002'000, 0, 8, 1000)));
    ASSERT_EQ(registry_.current_rules_version(), 2u);
    ASSERT_EQ(rules_.rules_version, 1u);

    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitStaleRulesVersion);
    // Fails closed before any state mutation: no Submitting transition, no in-flight
    // registration.
    EXPECT_EQ(r.order.state, OrderState::Intent);
    EXPECT_EQ(in_flight_.count(), 0u);
}

TEST_F(SubmitAdapterOrchestratorTest, NullRegistryInContextFailsClosedAtGate1) {
    submit_ctx_.registry = nullptr;
    ctx_.submit_port = make_binance_submit_port(submit_ctx_);
    // composite_current_rules_version_adapter(&submit_ctx_) now returns 0, which never
    // matches the real (non-zero) rules_.rules_version -- same fail-closed contract as a
    // missing current_rules_version_fn (live_submit_orchestrator.hpp's own SubmitPort
    // comment), reached here through a real registry pointer that happens to be null rather
    // than a null function pointer.
    auto r = orchestrate_submit(ctx_);
    EXPECT_EQ(r.gate, OrchestratorGate::SubmitStaleRulesVersion);
    EXPECT_EQ(in_flight_.count(), 0u);
}

// --- Tier 3: real network dispatch through the composite adapter ---

constexpr std::string_view kSyntheticApiKey =
    "TESTKEYabcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234";
constexpr std::string_view kSyntheticSecret = "synthetic-test-secret-not-real";

constexpr std::string_view kTestnetAllowedKeys[] = {
    "HENGYUAN_BINANCE_TESTNET_API_KEY",
    "HENGYUAN_BINANCE_TESTNET_SECRET",
};
constexpr EnvAllowlist kTestnetAllowlist{kTestnetAllowedKeys, 2};

std::string fixture_path(const char* filename) {
    return std::string(HY_TEST_FIXTURE_DIR) + "/" + filename;
}

class BoundCredentialsFixture : public ::testing::Test {
protected:
    std::string tmp_path_;

    void SetUp() override {
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        tmp_path_ =
            std::string(tmp) + "hy_test_submitadapter_" + std::to_string(GetCurrentProcessId()) + ".env";
#else
        tmp_path_ = "/tmp/hy_test_submitadapter_" + std::to_string(getpid()) + ".env";
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
        EXPECT_EQ(loader.load(tmp_path_.c_str(), kTestnetAllowlist).status, EnvLoadStatus::Ok);
        auto [err, creds] =
            BoundHmacCredentials::load_and_bind_credentials(EnvironmentBinding::testnet(), loader);
        EXPECT_EQ(err, QuerySigningError::Ok);
        return std::move(creds);
    }
};

TEST_F(BoundCredentialsFixture, RealSubmitDispatchThroughCompositeAdapterSignsAndParses) {
    hy::test_helpers::TlsResponseAcceptor time_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"serverTime":1700000000000})");

    hy::test_helpers::TlsResponseAcceptor order_server(
        fixture_path("test_leaf_cert_testnet_host.pem"),
        fixture_path("test_leaf_key_testnet_host.pem"), 200,
        R"({"symbol":"BTCUSDT","orderId":889,"clientOrderId":"submit-adapter-001",)"
        R"("transactTime":1700000000000,"price":"50000.12","origQty":"0.100000",)"
        R"("executedQty":"0.050000","cummulativeQuoteQty":"2500.00000000",)"
        R"("status":"PARTIALLY_FILLED","side":"BUY","type":"LIMIT","timeInForce":"GTC"})");

    PrivateRestConfig order_cfg;
    order_cfg.port = std::to_string(order_server.port());

    // 5-parameter constructor (H1/H2): rate_limiter/weight_table left at their nullptr/{}
    // defaults, order_cfg supplied as default_cfg so the no-cfg submit_order() overload the
    // composite adapter actually calls picks it up automatically.
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds(), nullptr, {},
                                     order_cfg);
    // Loopback RestTestSeam: both fixtures are reached on 127.0.0.1, the fixture certificate is trusted.
    hy::BinancePrivateRestClientTestHooks::use_loopback(client,
                                                        fixture_path("test_leaf_cert_testnet_host.pem"));

    PrivateRestConfig sync_cfg;
    sync_cfg.port = std::to_string(time_server.port());
    ASSERT_EQ(client.sync_clock(sync_cfg), PrivateRestError::None);

    SymbolRegistry registry;
    FakeSymbolRegistrySink sink;
    ASSERT_TRUE(registry.refresh_from_exchange_info(
        sink, make_single_symbol_parsed(1'700'000'000'000, 2, 6, 1000)));

    SubmitAdapterContext ctx{&client, &registry};
    const SubmitPort port = make_binance_submit_port(ctx);
    ASSERT_EQ(port.current_rules_version(), 1u);

    const auto rules = make_btcusdt_rules(1);
    const auto resp = port.call("submit-adapter-001", 1, OrderSide::Buy, OrderType::Limit,
                                 5'000'012, 100'000, rules);
    ASSERT_EQ(resp.outcome, SubmitOutcome::Accepted);
    EXPECT_EQ(resp.exchange_status, OrderState::PartialFill);
    EXPECT_EQ(resp.exchange_order_id, 889);
    EXPECT_EQ(resp.filled_qty_ticks, 50'000);
    EXPECT_EQ(resp.avg_fill_price_ticks, 5'000'000);

    const auto reqs = order_server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].api_key_header, kSyntheticApiKey);
    EXPECT_NE(reqs[0].target.find("newClientOrderId=submit-adapter-001"), std::string::npos);
}

TEST_F(BoundCredentialsFixture, RealSubmitDispatchWithoutPriorClockSyncFailsClosed) {
    // No sync_clock() call at all -- must never attempt to sign or send anything, matching
    // BinancePrivateRestClient::submit_order()'s own fail-closed contract
    // (test_binance_private_rest.cpp's SubmitOrderFailsClosedWithoutPriorClockSync).
    BinancePrivateRestClient client(EnvironmentBinding::testnet(), make_creds());
    SymbolRegistry registry;
    FakeSymbolRegistrySink sink;
    ASSERT_TRUE(registry.refresh_from_exchange_info(
        sink, make_single_symbol_parsed(1'700'000'000'000, 2, 6, 1000)));

    SubmitAdapterContext ctx{&client, &registry};
    const SubmitPort port = make_binance_submit_port(ctx);
    const auto rules = make_btcusdt_rules(1);
    const auto resp = port.call("coid-no-clock", 1, OrderSide::Buy, OrderType::Limit, 5'000'012,
                                 100'000, rules);
    EXPECT_EQ(resp.outcome, SubmitOutcome::NetworkError);
}

}  // namespace
