// P2-MD-02 / Track C: binance_rest_snapshot.hpp unit tests.
//
// All tests here are fully local -- no real network access. Connection-failure/timeout tests
// use the local TCP/TLS fixtures in test_helpers/blackhole_acceptor.hpp. HY_TEST_FIXTURE_DIR is
// a CMake-supplied macro pointing at native/tests/fixtures/ (not a relative path assumption
// about the current working directory ctest happens to run from).

#include <gtest/gtest.h>
#include <hengyuan/binance_rest_snapshot.hpp>
#include <hengyuan/snapshot_refresh_gate.hpp>

#include "binance_environment_test_hooks.hpp"
#include "test_helpers/blackhole_acceptor.hpp"
#include "test_helpers/rest_test_seam_builder.hpp"

#include <chrono>
#include <string>
#include <string_view>

using hy::EnvironmentBinding;
using hy::FetchError;
using hy::RestSnapshotConfig;
using hy::fetch_depth_snapshot;
using hy::validate_rest_config;

namespace {

std::string fixture_path(const char* filename) {
    return std::string(HY_TEST_FIXTURE_DIR) + "/" + filename;
}

// A binding for a free host name. The two factories only know the exchange hosts; these tests need a loopback
// fixture ("127.0.0.1") and the name on a fixture certificate, and the allowlist of such a binding is exactly
// that host. `host` must be a string literal (the binding keeps a view of it).
EnvironmentBinding binding_for(std::string_view host) {
    return hy::EnvironmentBindingTestHooks::with_host(EnvironmentBinding::testnet(), host);
}

}  // namespace

// --- validate_rest_config() ---

TEST(BinanceRestSnapshotConfig, AcceptsDefaultConfig) {
    RestSnapshotConfig cfg;
    EXPECT_FALSE(validate_rest_config("BTCUSDT", cfg).has_value());
}

TEST(BinanceRestSnapshotConfig, RejectsDisallowedLimit) {
    RestSnapshotConfig cfg;
    for (int bad_limit : {0, 1, 1024, 5000, -1}) {
        cfg.limit = bad_limit;
        auto err = validate_rest_config("BTCUSDT", cfg);
        ASSERT_TRUE(err.has_value()) << "limit=" << bad_limit;
        EXPECT_EQ(*err, FetchError::InvalidConfig);
    }
}

TEST(BinanceRestSnapshotConfig, AcceptsAllDocumentedLimits) {
    RestSnapshotConfig cfg;
    for (int good_limit : {5, 10, 20, 50, 100, 500, 1000}) {
        cfg.limit = good_limit;
        EXPECT_FALSE(validate_rest_config("BTCUSDT", cfg).has_value()) << "limit=" << good_limit;
    }
}

TEST(BinanceRestSnapshotConfig, RejectsInvalidSymbolChars) {
    RestSnapshotConfig cfg;
    for (const char* bad_symbol : {"BTC/USDT", "btcusdt", "BTC USDT", "BTC\r\nUSDT", ""}) {
        auto err = validate_rest_config(bad_symbol, cfg);
        ASSERT_TRUE(err.has_value()) << "symbol=" << bad_symbol;
        EXPECT_EQ(*err, FetchError::InvalidConfig);
    }
}

TEST(BinanceRestSnapshotConfig, RejectsOverlongSymbol) {
    RestSnapshotConfig cfg;
    std::string too_long(21, 'A');
    auto err = validate_rest_config(too_long, cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(*err, FetchError::InvalidConfig);
}

TEST(BinanceRestSnapshotConfig, RejectsAnEmptyPort) {
    RestSnapshotConfig cfg;
    cfg.port = "";
    auto err = validate_rest_config("BTCUSDT", cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(*err, FetchError::InvalidConfig);
}

// --- fetch_depth_snapshot(): connection failure paths (no real network) ---

TEST(BinanceRestSnapshotConnectivity, TlsHandshakeStageTimeout) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;

    RestSnapshotConfig cfg;
    cfg.port = std::to_string(blackhole.port());

    auto start = std::chrono::steady_clock::now();
    FetchError err{};
    auto result = fetch_depth_snapshot(binding_for("127.0.0.1"), "BTCUSDT", 100'000'000, 100'000'000, cfg, err);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(err, FetchError::TlsHandshake);
    // Bounded by the ~5s TLS-handshake-stage budget, not the caller's imagination -- well
    // under the total worst-case ~30s across all stages.
    EXPECT_LT(elapsed, std::chrono::seconds(15));
}

TEST(BinanceRestSnapshotConnectivity, ReadStageTimeoutAfterSuccessfulHandshake) {
    hy::test_helpers::TlsBlackholeAcceptor tls_blackhole(fixture_path("test_leaf_cert.pem"),
                                                           fixture_path("test_leaf_key.pem"));

    RestSnapshotConfig cfg;
    cfg.port = std::to_string(tls_blackhole.port());
    // The binding's host matches the fixture cert's CN/SAN. A synthetic .invalid hostname cannot be
    // relied on to fail DNS resolution -- confirmed directly in this environment, where it resolved to a
    // synthesized address instead of NXDOMAIN. The seam routes the actual connection to the local fixture
    // (and trusts the fixture CA) while hostname/SNI are still verified against the binding's host.
    const hy::RestTestSeam seam = hy::RestTestSeamBuilder::loopback(fixture_path("test_ca_cert.pem"));

    auto start = std::chrono::steady_clock::now();
    FetchError err{};
    auto result = fetch_depth_snapshot(binding_for("wrong-san.test.invalid"), "BTCUSDT", 100'000'000,
                                       100'000'000, cfg, err, seam);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(result.has_value());
    // The handshake itself must have succeeded (matching host + trusted CA) -- if this ever
    // regresses to TlsHandshake, it means the seam's extra-CA wiring or the
    // hostname-verification config broke, not that the read timeout is working.
    EXPECT_EQ(err, FetchError::Read);
    EXPECT_LT(elapsed, std::chrono::seconds(20));
}

TEST(BinanceRestSnapshotConnectivity, DefaultOverloadWithoutDiagnosticsStillFails) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    RestSnapshotConfig cfg;
    cfg.port = std::to_string(blackhole.port());

    auto result = fetch_depth_snapshot(binding_for("127.0.0.1"), "BTCUSDT", 100'000'000, 100'000'000, cfg);
    EXPECT_FALSE(result.has_value());
}

TEST(BinanceRestSnapshotConnectivity, InvalidConfigFailsWithoutTouchingNetwork) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    RestSnapshotConfig cfg;
    cfg.port = std::to_string(blackhole.port());
    cfg.limit = 42;  // not in the allowed discrete set

    FetchError err{};
    auto result = fetch_depth_snapshot(binding_for("127.0.0.1"), "BTCUSDT", 100'000'000, 100'000'000, cfg, err);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(err, FetchError::InvalidConfig);
    EXPECT_EQ(blackhole.accepted_connections(), 0u);
}

// --- the endpoint policy of the bound environment decides which host is contacted at all (SAFE-01) ---

namespace {
// A binding that names `host` but whose allowlist does not contain it: the only state in which the check
// can be proven to fire before any network attempt.
EnvironmentBinding binding_refusing(std::string_view host) {
    hy::EndpointAllowlist al{};
    al.hosts[0] = "not-the-bound-host.example";
    al.count = 1;
    return hy::EnvironmentBindingTestHooks::with_allowlist(binding_for(host), al);
}
}  // namespace

TEST(BinanceRestSnapshotEndpoint, AHostOutsideTheBindingsAllowlistIsRefusedBeforeAnyNetworkAttempt) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    RestSnapshotConfig cfg;
    cfg.port = std::to_string(blackhole.port());

    FetchError err{};
    auto result =
        fetch_depth_snapshot(binding_refusing("127.0.0.1"), "BTCUSDT", 100'000'000, 100'000'000, cfg, err);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(err, FetchError::InvalidConfig);
    EXPECT_EQ(blackhole.accepted_connections(), 0u) << "the refusal must come before the connection";
    // and the overload without diagnostics refuses the same way
    EXPECT_FALSE(fetch_depth_snapshot(binding_refusing("127.0.0.1"), "BTCUSDT", 100'000'000, 100'000'000, cfg)
                     .has_value());
    EXPECT_EQ(blackhole.accepted_connections(), 0u);
}

TEST(BinanceRestSnapshotEndpoint, AHostThatIsNoHostnameIsRefusedEvenWhenTheAllowlistListsIt) {
    for (const char* bad_host : {"", "bad host", "a/b", "host\r\nX: y", "testnet.binance.vision:443"}) {
        RestSnapshotConfig cfg;
        FetchError err{};
        auto result = fetch_depth_snapshot(binding_for(bad_host), "BTCUSDT", 100'000'000, 100'000'000, cfg, err);
        EXPECT_FALSE(result.has_value()) << "host=" << bad_host;
        EXPECT_EQ(err, FetchError::InvalidConfig) << "host=" << bad_host;
    }
}

// --- TLS hostname verification: negative + positive control ---
//
// A self-signed/untrusted-issuer certificate would fail the handshake regardless of whether
// hostname_verification is wired in at all -- that would prove nothing about hostname checking
// specifically. Both tests here explicitly trust the fixture's issuing CA via
// the seam's extra CA, so chain validation always succeeds and the *only* remaining
// variable is whether the connected host string matches the certificate's CN/SAN
// (wrong-san.test.invalid). Without the positive control, a bug that made the negative test
// fail for an unrelated reason (e.g. a broken fixture) would go unnoticed.
//
// Both tests also pass a seam that connects to 127.0.0.1: the binding's host here is deliberately a
// hostname that does NOT match the fixture cert (or, in the positive test, one that does), used
// purely for SNI/hostname-verification purposes -- it must not also be the DNS resolution
// target, since neither "api.binance.com" (a real, live hostname) nor a synthetic .invalid name
// (unreliable to fail resolution, confirmed directly in this environment) would deterministically
// route the connection to the local fixture otherwise.

TEST(BinanceRestSnapshotTlsHostnameVerification, MismatchedHostIsRejected) {
    hy::test_helpers::TlsBlackholeAcceptor tls_blackhole(fixture_path("test_leaf_cert.pem"),
                                                           fixture_path("test_leaf_key.pem"));

    RestSnapshotConfig cfg;
    cfg.port = std::to_string(tls_blackhole.port());
    // chain IS trusted, and the connection actually goes to the local fixture
    const hy::RestTestSeam seam = hy::RestTestSeamBuilder::loopback(fixture_path("test_ca_cert.pem"));

    FetchError err{};
    // the production binding: api.binance.com deliberately does NOT match the fixture cert's CN/SAN
    auto result = fetch_depth_snapshot(EnvironmentBinding::production(), "BTCUSDT", 100'000'000, 100'000'000,
                                       cfg, err, seam);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(err, FetchError::TlsHandshake);
}

TEST(BinanceRestSnapshotTlsHostnameVerification, MatchingHostWithTrustedCaSucceedsHandshake) {
    hy::test_helpers::TlsBlackholeAcceptor tls_blackhole(fixture_path("test_leaf_cert.pem"),
                                                           fixture_path("test_leaf_key.pem"));

    RestSnapshotConfig cfg;
    cfg.port = std::to_string(tls_blackhole.port());
    const hy::RestTestSeam seam = hy::RestTestSeamBuilder::loopback(fixture_path("test_ca_cert.pem"));

    FetchError err{};
    // wrong-san.test.invalid matches the fixture cert's CN/SAN
    auto result = fetch_depth_snapshot(binding_for("wrong-san.test.invalid"), "BTCUSDT", 100'000'000,
                                       100'000'000, cfg, err, seam);
    // The handshake itself must succeed; the request then fails at the read stage because the
    // fixture never sends an HTTP response (it's a handshake-only fixture, not a full server) --
    // FetchError::Read (not TlsHandshake) is exactly the proof that hostname verification
    // passed a legitimately matching certificate instead of rejecting it too.
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(err, FetchError::Read);
}

// Moved here from test_snapshot_refresh_gate.cpp (audit VERIF-TSAN-016): it is the
// only gate test that needs a real fetch_depth_snapshot(), and keeping it there kept the
// gate's threaded state machine out of the TSan job. This binary is already Boost-gated
// and already owns the blackhole fixture.
TEST(SnapshotRefreshGateRealFetch, RealFetchWiringIsNonBlocking) {
    // Light sanity check that the production wiring actually plugs a real
    // fetch_depth_snapshot() call into the gate correctly -- using the local blackhole
    // fixture (no real network). This deliberately does NOT wait out the full ~5s
    // TLS-handshake-timeout-then-cooldown cycle (covered by the tests above and by the
    // fake-fetcher tests in test_snapshot_refresh_gate.cpp); it only checks that
    // starting a real fetch through the gate does not itself block the caller.
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    hy::RestSnapshotConfig cfg;
    cfg.port = std::to_string(blackhole.port());
    const EnvironmentBinding binding = binding_for("127.0.0.1");

    hy::SnapshotRefreshGate gate([&cfg, binding](const hy::SnapshotRequest& req) {
        return hy::fetch_depth_snapshot(binding, req.symbol, req.price_multiplier, req.qty_multiplier,
                                         cfg);
    });

    auto start = std::chrono::steady_clock::now();
    auto r1 = gate.poll(true, {"BTCUSDT", 100'000'000, 100'000'000});
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(r1.has_value());
    EXPECT_LT(elapsed, std::chrono::milliseconds(500));  // must not block on the real fetch
}

TEST(SnapshotRefreshGateRealFetch, DefaultFetcherFactoryIsWired) {
    // make_default_snapshot_fetcher() is what binance_dry_run_demo.cpp now passes
    // explicitly (audit VERIF-TSAN-016 removed the constructor default), with the environment it reads
    // (SAFE-01: there is no default environment). Prove the factory produces a usable SnapshotFetcher
    // rather than only compiling.
    auto fetcher = hy::make_default_snapshot_fetcher(EnvironmentBinding::production());
    ASSERT_TRUE(static_cast<bool>(fetcher));
    // An invalid symbol is rejected by validate_rest_config() before any network I/O,
    // so this exercises the adapter shape without leaving the machine.
    EXPECT_FALSE(fetcher(hy::SnapshotRequest{"not a symbol", 100'000'000, 100'000'000}).has_value());
}

TEST(SnapshotRefreshGateRealFetch, FactoryFetcherUsesTheBindingAndTheConfigItWasGiven) {
    hy::test_helpers::PlainBlackholeAcceptor blackhole;
    hy::RestSnapshotConfig cfg;
    cfg.port = std::to_string(blackhole.port());
    const hy::SnapshotRequest request{"BTCUSDT", 100'000'000, 100'000'000};

    // A binding that refuses its own host: nothing may reach the fixture.
    auto refused = hy::make_default_snapshot_fetcher(binding_refusing("127.0.0.1"), cfg);
    EXPECT_FALSE(refused(request).has_value());
    EXPECT_EQ(blackhole.accepted_connections(), 0u);

    // Positive control: the same factory with a binding that allows the host reaches the fixture, so the
    // zero above is the policy's doing and not a fetcher that ignores its arguments.
    auto allowed = hy::make_default_snapshot_fetcher(binding_for("127.0.0.1"), cfg);
    EXPECT_FALSE(allowed(request).has_value());  // the blackhole never completes a TLS handshake
    EXPECT_GE(blackhole.accepted_connections(), 1u);
}
