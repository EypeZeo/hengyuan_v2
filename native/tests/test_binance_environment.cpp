// Plan v17 §"binance_environment.hpp": EnvironmentBinding + BoundHmacCredentials.
// Synthetic test credentials only, written to a temp .env file. No network,
// no real API keys.
#include <gtest/gtest.h>
#include <hengyuan/binance_environment.hpp>
#include "binance_environment_test_hooks.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <sys/stat.h>
#endif

using hy::BinanceEnvironment;
using hy::BinanceSigner;
using hy::BoundHmacCredentials;
using hy::BoundHmacCredentialsTestHooks;
using hy::EnvAllowlist;
using hy::EnvironmentBinding;
using hy::QuerySigningError;
using hy::SecureEnvLoader;
using hy::is_valid_api_key;

namespace {

// A syntactically valid (but obviously synthetic) 64-char alphanumeric key.
constexpr std::string_view kSyntheticApiKey =
    "TESTKEYabcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234";

constexpr std::string_view kTestnetAllowedKeys[] = {
    "HENGYUAN_BINANCE_TESTNET_API_KEY",
    "HENGYUAN_BINANCE_TESTNET_SECRET",
};
constexpr EnvAllowlist kTestnetAllowlist{kTestnetAllowedKeys, 2};

int g_lock_calls = 0;
int g_unlock_calls = 0;
bool g_lock_result = true;
// AUDIT L4-LOCK-ORDER-003 regression: true iff every buffer handed to
// stub_lock() (across both the api_key_buf_ lock and the forwarded
// signer_.init_impl() secret_buf_ lock) was still all-zero at the moment
// lock() was called -- i.e. neither credential had been memcpy'd in yet.
bool g_saw_all_zero_at_lock = true;

bool stub_lock(void* ptr, std::size_t size) noexcept {
    ++g_lock_calls;
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != 0) {
            g_saw_all_zero_at_lock = false;
            break;
        }
    }
    return g_lock_result;
}
void stub_unlock(void*, std::size_t) noexcept {
    ++g_unlock_calls;
}

class BoundHmacCredentialsTest : public ::testing::Test {
protected:
    std::string tmp_path_;

    void SetUp() override {
        g_lock_calls = 0;
        g_unlock_calls = 0;
        g_lock_result = true;
        g_saw_all_zero_at_lock = true;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        tmp_path_ = std::string(tmp) + "hy_test_binenv_" +
                    std::to_string(GetCurrentProcessId()) + ".env";
#else
        tmp_path_ = "/tmp/hy_test_binenv_" + std::to_string(getpid()) + ".env";
#endif
    }

    void TearDown() override { std::remove(tmp_path_.c_str()); }

    void write_valid_env() {
        std::ofstream f(tmp_path_, std::ios::binary);
        f << "HENGYUAN_BINANCE_TESTNET_API_KEY=" << kSyntheticApiKey << "\n"
          << "HENGYUAN_BINANCE_TESTNET_SECRET=synthetic_test_secret_value\n";
        f.close();
        chmod_owner_only();
    }

    void write_bad_api_key_env() {
        std::ofstream f(tmp_path_, std::ios::binary);
        f << "HENGYUAN_BINANCE_TESTNET_API_KEY=too_short\n"
          << "HENGYUAN_BINANCE_TESTNET_SECRET=synthetic_test_secret_value\n";
        f.close();
        chmod_owner_only();
    }

    // env_loader.hpp's Linux path rejects (PermissionTooWide) any file
    // readable/writable by group or other -- Windows has no equivalent
    // check. A freshly created temp file's mode depends on the process
    // umask, which is not guaranteed to already satisfy this.
    void chmod_owner_only() {
#ifdef __linux__
        chmod(tmp_path_.c_str(), 0600);
#endif
    }
};

}  // namespace

// --- EnvironmentBinding ---

TEST(EnvironmentBinding, TestnetFactoryFieldsAreCorrect) {
    auto b = EnvironmentBinding::testnet();
    EXPECT_EQ(b.environment(), BinanceEnvironment::Testnet);
    EXPECT_EQ(b.base_host(), "testnet.binance.vision");
    EXPECT_EQ(b.api_key_env_key(), "HENGYUAN_BINANCE_TESTNET_API_KEY");
    EXPECT_EQ(b.secret_env_key(), "HENGYUAN_BINANCE_TESTNET_SECRET");
    EXPECT_TRUE(b.transport_policy().endpoint_allowlist.contains("testnet.binance.vision"));
    EXPECT_FALSE(b.transport_policy().endpoint_allowlist.contains("api.binance.com"));
}

TEST(EnvironmentBinding, ProductionFactoryFieldsAreCorrect) {
    auto b = EnvironmentBinding::production();
    EXPECT_EQ(b.environment(), BinanceEnvironment::Production);
    EXPECT_EQ(b.base_host(), "api.binance.com");
    EXPECT_EQ(b.api_key_env_key(), "HENGYUAN_BINANCE_LIVE_API_KEY");
    EXPECT_EQ(b.secret_env_key(), "HENGYUAN_BINANCE_LIVE_SECRET");
    EXPECT_TRUE(b.transport_policy().endpoint_allowlist.contains("api.binance.com"));
    EXPECT_FALSE(b.transport_policy().endpoint_allowlist.contains("testnet.binance.vision"));
}

// --- Batch H, H4: ws_host()/ws_port() ---

TEST(EnvironmentBinding, TestnetWsHostAndPortAreCorrect) {
    auto b = EnvironmentBinding::testnet();
    EXPECT_EQ(b.ws_host(), "stream.testnet.binance.vision");
    EXPECT_EQ(b.ws_port(), "443");
}

TEST(EnvironmentBinding, ProductionWsHostAndPortAreCorrect) {
    auto b = EnvironmentBinding::production();
    EXPECT_EQ(b.ws_host(), "stream.binance.com");
    EXPECT_EQ(b.ws_port(), "9443");
}

// Only ws_host() is asserted distinct across environments -- environment isolation is the
// job of ws_host()/base_host()/the credential env-var names, never the port number. Both
// environments happening to use the same port (or even swapped ports, hypothetically) would
// not itself indicate a cross-environment bug, so no EXPECT_NE on ws_port() here.
TEST(EnvironmentBinding, WsHostDiffersAcrossEnvironments) {
    EXPECT_NE(EnvironmentBinding::testnet().ws_host(), EnvironmentBinding::production().ws_host());
}

// ws_host_/ws_port_ must never be folded into transport_policy().endpoint_allowlist --
// BinanceUserDataWsSession connects directly on the raw host, never consults this allowlist,
// and production's allowlist already uses all 4 of its fixed slots for REST hosts (see this
// class's own header comment). This is a regression guard against a future "helpful" edit
// that tries to add ws_host_ there.
TEST(EnvironmentBinding, WsHostNeverInTransportPolicyAllowlist) {
    auto testnet = EnvironmentBinding::testnet();
    EXPECT_FALSE(testnet.transport_policy().endpoint_allowlist.contains(testnet.ws_host()));
    auto production = EnvironmentBinding::production();
    EXPECT_FALSE(production.transport_policy().endpoint_allowlist.contains(production.ws_host()));
}

// static_assert(is_trivially_copyable_v<EnvironmentBinding>) already proves this at compile
// time; this is a cheap runtime double-check that a copy is genuinely independent (not, say,
// accidentally aliasing shared state through some field this class might grow later).
TEST(EnvironmentBinding, TriviallyCopyableValueSemantics) {
    static_assert(std::is_trivially_copyable_v<EnvironmentBinding>);
    auto b1 = EnvironmentBinding::testnet();
    auto b2 = b1;  // explicit value copy
    EXPECT_EQ(b2.ws_host(), b1.ws_host());
    EXPECT_EQ(b2.ws_port(), b1.ws_port());
    EXPECT_EQ(b2.base_host(), b1.base_host());
}

// --- is_valid_api_key ---

TEST(IsValidApiKey, AcceptsWellFormedKey) {
    EXPECT_TRUE(is_valid_api_key(kSyntheticApiKey));
}

TEST(IsValidApiKey, RejectsWrongLength) {
    EXPECT_FALSE(is_valid_api_key("tooshort"));
    EXPECT_FALSE(is_valid_api_key(""));
}

TEST(IsValidApiKey, RejectsNonAlphanumericChars) {
    std::string key(64, 'a');
    key[10] = '/';  // not in Binance's key alphabet
    EXPECT_FALSE(is_valid_api_key(key));
}

// --- BoundHmacCredentials::load_and_bind_credentials ---

TEST_F(BoundHmacCredentialsTest, SucceedsWithValidEnv) {
    write_valid_env();
    SecureEnvLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str(), kTestnetAllowlist).status,
              hy::EnvLoadStatus::Ok);

    auto binding = EnvironmentBinding::testnet();
    auto [err, creds] = BoundHmacCredentials::load_and_bind_credentials(binding, loader);
    EXPECT_EQ(err, QuerySigningError::Ok);
    ASSERT_NE(creds, nullptr);

    std::array<char, 128> out{};
    std::size_t written = 0;
    EXPECT_TRUE(creds->copy_api_key(out, written));
    EXPECT_EQ(written, kSyntheticApiKey.size());
    EXPECT_EQ(std::string_view(out.data(), written), kSyntheticApiKey);
}

TEST_F(BoundHmacCredentialsTest, InvalidApiKeyFormatRejected) {
    write_bad_api_key_env();
    SecureEnvLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str(), kTestnetAllowlist).status,
              hy::EnvLoadStatus::Ok);

    auto binding = EnvironmentBinding::testnet();
    auto [err, creds] = BoundHmacCredentials::load_and_bind_credentials(binding, loader);
    EXPECT_EQ(err, QuerySigningError::InvalidApiKeyFormat);
    EXPECT_EQ(creds, nullptr);
}

TEST_F(BoundHmacCredentialsTest, LockUnavailableWhenBehaviorNull) {
    write_valid_env();
    SecureEnvLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str(), kTestnetAllowlist).status,
              hy::EnvLoadStatus::Ok);

    auto binding = EnvironmentBinding::testnet();
    auto [err, creds] = BoundHmacCredentialsTestHooks::load_and_bind_credentials_with_lock_behavior(
        binding, loader, {nullptr, nullptr});
    EXPECT_EQ(err, QuerySigningError::LockUnavailable);
    EXPECT_EQ(creds, nullptr);
}

TEST_F(BoundHmacCredentialsTest, LockFailurePathWipesAndReportsError) {
    write_valid_env();
    SecureEnvLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str(), kTestnetAllowlist).status,
              hy::EnvLoadStatus::Ok);

    g_lock_result = false;
    auto binding = EnvironmentBinding::testnet();
    auto [err, creds] = BoundHmacCredentialsTestHooks::load_and_bind_credentials_with_lock_behavior(
        binding, loader, {&stub_lock, &stub_unlock});

    EXPECT_EQ(err, QuerySigningError::LockFailed);
    EXPECT_EQ(creds, nullptr);
    EXPECT_GE(g_lock_calls, 1);
    EXPECT_EQ(g_unlock_calls, 0);  // never locked, so never unlocked
}

TEST_F(BoundHmacCredentialsTest, StubLockBehaviorLocksApiKeyOnSuccess) {
    write_valid_env();
    SecureEnvLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str(), kTestnetAllowlist).status,
              hy::EnvLoadStatus::Ok);

    auto binding = EnvironmentBinding::testnet();
    auto [err, creds] = BoundHmacCredentialsTestHooks::load_and_bind_credentials_with_lock_behavior(
        binding, loader, {&stub_lock, &stub_unlock});

    ASSERT_EQ(err, QuerySigningError::Ok);
    ASSERT_NE(creds, nullptr);
    // Two separate buffers get locked with this one shared injected
    // behavior: api_key_buf_ (BoundHmacCredentials' own) and, via the same
    // `b` forwarded into signer_.init_impl(), the internal BinanceSigner's
    // secret_buf_.
    EXPECT_EQ(g_lock_calls, 2);
    EXPECT_EQ(g_unlock_calls, 0);

    creds.reset();  // ~BoundHmacCredentials() -> wipe_api_key(); the
                     // BinanceSigner subobject's own destructor also runs
                     // as part of ~BoundHmacCredentials(), unlocking its
                     // secret_buf_ too -- two unlocks total, one per
                     // buffer, precisely paired with the two locks above.
    EXPECT_EQ(g_unlock_calls, 2);
}

TEST_F(BoundHmacCredentialsTest, LockSeesEmptyBuffersBeforeCredentialsAreWritten) {
    write_valid_env();
    SecureEnvLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str(), kTestnetAllowlist).status,
              hy::EnvLoadStatus::Ok);

    auto binding = EnvironmentBinding::testnet();
    auto [err, creds] = BoundHmacCredentialsTestHooks::load_and_bind_credentials_with_lock_behavior(
        binding, loader, {&stub_lock, &stub_unlock});

    ASSERT_EQ(err, QuerySigningError::Ok);
    ASSERT_NE(creds, nullptr);
    EXPECT_EQ(g_lock_calls, 2);  // api_key_buf_ + signer_'s own secret_buf_
    // Proves the ordering directly (not just the final result): both
    // buffers were still all-zero at the instant their respective lock()
    // call ran, i.e. each memcpy happened strictly after locking succeeded.
    EXPECT_TRUE(g_saw_all_zero_at_lock);
}

// --- AUDIT L4-BINDING-REF-005 ---

TEST_F(BoundHmacCredentialsTest, AcceptsTemporaryEnvironmentBinding) {
    // Before the fix, BoundHmacCredentials stored a `const EnvironmentBinding&`
    // for its whole lifetime, so a deleted && overload had to reject a
    // temporary at compile time to prevent a dangling reference. Nothing
    // is stored anymore, so passing a temporary directly is safe and must
    // now compile and succeed -- this line would not have compiled before
    // the fix.
    write_valid_env();
    SecureEnvLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str(), kTestnetAllowlist).status,
              hy::EnvLoadStatus::Ok);

    auto [err, creds] =
        BoundHmacCredentials::load_and_bind_credentials(EnvironmentBinding::testnet(), loader);
    EXPECT_EQ(err, QuerySigningError::Ok);
    ASSERT_NE(creds, nullptr);
}

// --- single-owning-thread constraint ---

TEST_F(BoundHmacCredentialsTest, SignAndCopyApiKeySucceedOnOwningThread) {
    write_valid_env();
    SecureEnvLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str(), kTestnetAllowlist).status,
              hy::EnvLoadStatus::Ok);

    auto binding = EnvironmentBinding::testnet();
    auto [err, creds] = BoundHmacCredentials::load_and_bind_credentials(binding, loader);
    ASSERT_EQ(err, QuerySigningError::Ok);
    ASSERT_NE(creds, nullptr);

    auto sig = BoundHmacCredentialsTestHooks::sign(*creds, "payload");
    EXPECT_FALSE(sig.empty());

    std::array<char, 128> out{};
    std::size_t written = 0;
    EXPECT_TRUE(creds->copy_api_key(out, written));
}

TEST_F(BoundHmacCredentialsTest, SignAndCopyApiKeyFailOnOtherThread) {
    write_valid_env();
    SecureEnvLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str(), kTestnetAllowlist).status,
              hy::EnvLoadStatus::Ok);

    auto binding = EnvironmentBinding::testnet();
    auto [err, creds] = BoundHmacCredentials::load_and_bind_credentials(binding, loader);
    ASSERT_EQ(err, QuerySigningError::Ok);
    ASSERT_NE(creds, nullptr);

    bool sign_empty = false;
    bool copy_failed = false;
    std::thread other([&] {
        auto sig = BoundHmacCredentialsTestHooks::sign(*creds, "payload");
        sign_empty = sig.empty();
        std::array<char, 128> out{};
        std::size_t written = 0;
        copy_failed = !creds->copy_api_key(out, written);
    });
    other.join();

    EXPECT_TRUE(sign_empty);
    EXPECT_TRUE(copy_failed);
}

// --- copy_api_key() disambiguation ---

TEST_F(BoundHmacCredentialsTest, CopyApiKeyBufferTooSmallFails) {
    write_valid_env();
    SecureEnvLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str(), kTestnetAllowlist).status,
              hy::EnvLoadStatus::Ok);

    auto binding = EnvironmentBinding::testnet();
    auto [err, creds] = BoundHmacCredentials::load_and_bind_credentials(binding, loader);
    ASSERT_EQ(err, QuerySigningError::Ok);

    std::array<char, 4> too_small{};
    std::size_t written = 999;
    EXPECT_FALSE(creds->copy_api_key(too_small, written));
}

// --- wipe / UAF-free erasure verification ---

TEST_F(BoundHmacCredentialsTest, WipeApiKeyZeroesFullBufferWhileObjectStillAlive) {
    write_valid_env();
    SecureEnvLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str(), kTestnetAllowlist).status,
              hy::EnvLoadStatus::Ok);

    auto binding = EnvironmentBinding::testnet();
    auto [err, creds] = BoundHmacCredentials::load_and_bind_credentials(binding, loader);
    ASSERT_EQ(err, QuerySigningError::Ok);

    std::array<char, 128> before{};
    std::size_t written_before = 0;
    ASSERT_TRUE(creds->copy_api_key(before, written_before));
    EXPECT_GT(written_before, 0u);

    BoundHmacCredentialsTestHooks::wipe_api_key(*creds);  // object still alive

    std::array<char, 128> after{};
    std::size_t written_after = 0;
    ASSERT_TRUE(creds->copy_api_key(after, written_after));
    EXPECT_EQ(written_after, 0u);  // api_key_len_ reset to 0
}

// --- private-stream migration M1: the WebSocket API host of each environment (blueprint 5.7 P-24) ---

TEST(EnvironmentBindingWsApi, EachEnvironmentNamesItsOwnWsApiHostPortAndTarget) {
    const auto testnet = EnvironmentBinding::testnet();
    EXPECT_EQ(testnet.ws_api_host(), "ws-api.testnet.binance.vision");
    EXPECT_EQ(testnet.ws_api_port(), "443");
    const auto production = EnvironmentBinding::production();
    EXPECT_EQ(production.ws_api_host(), "ws-api.binance.com");
    EXPECT_EQ(production.ws_api_port(), "443");
    EXPECT_EQ(EnvironmentBinding::ws_api_target(), "/ws-api/v3");
    EXPECT_NE(testnet.ws_api_host(), production.ws_api_host());
    // not the market-data stream host either: a different service with its own host
    EXPECT_NE(testnet.ws_api_host(), testnet.ws_host());
    EXPECT_NE(production.ws_api_host(), production.ws_host());
}

TEST(EnvironmentBindingWsApi, AnEnvironmentsPolicyPermitsItsOwnWsApiHostAndNeverTheOtherOnes) {
    const auto testnet = EnvironmentBinding::testnet();
    const auto production = EnvironmentBinding::production();

    const auto own_testnet = hy::issue_ws_endpoint_permit(testnet.transport_policy(), testnet.ws_api_host());
    ASSERT_TRUE(own_testnet.has_value());
    EXPECT_EQ(own_testnet->host(), "ws-api.testnet.binance.vision");
    const auto own_production = hy::issue_ws_endpoint_permit(production.transport_policy(), production.ws_api_host());
    ASSERT_TRUE(own_production.has_value());
    EXPECT_EQ(own_production->host(), "ws-api.binance.com");

    // the environments never cross
    EXPECT_FALSE(hy::issue_ws_endpoint_permit(testnet.transport_policy(), production.ws_api_host()).has_value());
    EXPECT_FALSE(hy::issue_ws_endpoint_permit(production.transport_policy(), testnet.ws_api_host()).has_value());
    // and neither policy permits a REST or market-data host for a WS API session
    for (const auto* b : {&testnet, &production}) {
        EXPECT_FALSE(hy::issue_ws_endpoint_permit(b->transport_policy(), b->base_host()).has_value());
        EXPECT_FALSE(hy::issue_ws_endpoint_permit(b->transport_policy(), b->ws_host()).has_value());
    }
}

TEST(EnvironmentBindingWsApi, TheWsApiHostIsOnNeitherEnvironmentsRestAllowlist) {
    for (const auto& b : {EnvironmentBinding::testnet(), EnvironmentBinding::production()}) {
        EXPECT_FALSE(b.transport_policy().endpoint_allowlist.contains(b.ws_api_host()));
        EXPECT_FALSE(hy::issue_endpoint_permit(b.transport_policy(), b.ws_api_host(), hy::ResponseKind::SignedJson).has_value());
    }
}

TEST(EnvironmentBindingWsApi, TheTestnetWsApiAllowlistHoldsItsHostAndNothingElse) {
    const auto testnet = EnvironmentBinding::testnet();
    const auto& al = testnet.transport_policy().ws_api_allowlist;
    EXPECT_EQ(al.count, 1u);
    EXPECT_TRUE(al.contains("ws-api.testnet.binance.vision"));
    EXPECT_FALSE(al.contains("ws-api.binance.com"));
    const auto production = EnvironmentBinding::production();
    EXPECT_EQ(production.transport_policy().ws_api_allowlist.count, 1u);
    EXPECT_TRUE(production.transport_policy().ws_api_allowlist.contains("ws-api.binance.com"));
    EXPECT_FALSE(production.transport_policy().ws_api_allowlist.contains("ws-api.testnet.binance.vision"));
}

// --- D5 slice R3b-2: the offline operator tool's bindings (a read-only key of its own) ---

namespace {

struct BindingPair {
    EnvironmentBinding trading;
    EnvironmentBinding tool;
};

std::array<BindingPair, 2> binding_pairs() {
    return {BindingPair{EnvironmentBinding::testnet(), EnvironmentBinding::operator_testnet()},
            BindingPair{EnvironmentBinding::production(), EnvironmentBinding::operator_production()}};
}

void expect_same_allowlist(const hy::EndpointAllowlist& a, const hy::EndpointAllowlist& b) {
    ASSERT_EQ(a.count, b.count);
    for (std::size_t i = 0; i < a.count; ++i) EXPECT_EQ(a.hosts[i], b.hosts[i]) << i;
}

constexpr std::string_view kOperatorTestnetAllowedKeys[] = {
    "HENGYUAN_OPERATOR_TESTNET_API_KEY",
    "HENGYUAN_OPERATOR_TESTNET_API_SECRET",
};
constexpr EnvAllowlist kOperatorTestnetAllowlist{kOperatorTestnetAllowedKeys, 2};

}  // namespace

TEST(OperatorBindings, AreTheirTradingTwinsInEverythingButTheTwoCredentialNames) {
    for (const BindingPair& p : binding_pairs()) {
        EXPECT_EQ(p.tool.environment(), p.trading.environment());
        EXPECT_EQ(p.tool.base_host(), p.trading.base_host());
        EXPECT_EQ(p.tool.ws_host(), p.trading.ws_host());
        EXPECT_EQ(p.tool.ws_port(), p.trading.ws_port());
        EXPECT_EQ(p.tool.ws_api_host(), p.trading.ws_api_host());
        EXPECT_EQ(p.tool.ws_api_port(), p.trading.ws_api_port());
        const hy::TransportPolicy& a = p.trading.transport_policy();
        const hy::TransportPolicy& b = p.tool.transport_policy();
        expect_same_allowlist(a.endpoint_allowlist, b.endpoint_allowlist);
        expect_same_allowlist(a.ws_api_allowlist, b.ws_api_allowlist);
        EXPECT_EQ(b.tls_verify_peer, a.tls_verify_peer);
        EXPECT_EQ(b.tls_verify_hostname, a.tls_verify_hostname);
        EXPECT_EQ(b.follow_redirects, a.follow_redirects);
        EXPECT_EQ(b.connect_timeout_ms, a.connect_timeout_ms);
        EXPECT_EQ(b.read_timeout_ms, a.read_timeout_ms);
        EXPECT_EQ(b.max_response_bytes, a.max_response_bytes);
        EXPECT_EQ(b.recv_window_ms, a.recv_window_ms);
        EXPECT_EQ(b.weight_limit_per_minute, a.weight_limit_per_minute);
        EXPECT_EQ(b.weight_safety_margin, a.weight_safety_margin);
        // and, like every binding, a plain value
        EXPECT_TRUE(std::is_trivially_copyable_v<EnvironmentBinding>);
    }
}

TEST(OperatorBindings, NameTheCredentialsOfTheOperatorToolAndNoOtherOnes) {
    EXPECT_EQ(EnvironmentBinding::operator_testnet().api_key_env_key(), "HENGYUAN_OPERATOR_TESTNET_API_KEY");
    EXPECT_EQ(EnvironmentBinding::operator_testnet().secret_env_key(), "HENGYUAN_OPERATOR_TESTNET_API_SECRET");
    EXPECT_EQ(EnvironmentBinding::operator_production().api_key_env_key(), "HENGYUAN_OPERATOR_LIVE_API_KEY");
    EXPECT_EQ(EnvironmentBinding::operator_production().secret_env_key(), "HENGYUAN_OPERATOR_LIVE_API_SECRET");
    // the trading bindings keep theirs: adding the tool's did not move them
    EXPECT_EQ(EnvironmentBinding::testnet().api_key_env_key(), "HENGYUAN_BINANCE_TESTNET_API_KEY");
    EXPECT_EQ(EnvironmentBinding::testnet().secret_env_key(), "HENGYUAN_BINANCE_TESTNET_SECRET");
    EXPECT_EQ(EnvironmentBinding::production().api_key_env_key(), "HENGYUAN_BINANCE_LIVE_API_KEY");
    EXPECT_EQ(EnvironmentBinding::production().secret_env_key(), "HENGYUAN_BINANCE_LIVE_SECRET");
}

TEST(OperatorBindings, ShareNoCredentialNameWithTradingOrWithEachOther) {
    std::vector<std::string_view> names;
    for (const EnvironmentBinding& b : {EnvironmentBinding::testnet(), EnvironmentBinding::production(),
                                        EnvironmentBinding::operator_testnet(), EnvironmentBinding::operator_production()}) {
        names.push_back(b.api_key_env_key());
        names.push_back(b.secret_env_key());
    }
    ASSERT_EQ(names.size(), 8u);
    for (std::size_t i = 0; i < names.size(); ++i) {
        for (std::size_t j = i + 1; j < names.size(); ++j) EXPECT_NE(names[i], names[j]) << names[i];
    }
}

TEST_F(BoundHmacCredentialsTest, TheOperatorBindingReadsTheOperatorNamesAndTheTradingBindingFindsNothingThere) {
    {
        std::ofstream f(tmp_path_, std::ios::binary);
        f << "HENGYUAN_OPERATOR_TESTNET_API_KEY=" << kSyntheticApiKey << "\n"
          << "HENGYUAN_OPERATOR_TESTNET_API_SECRET=synthetic_operator_secret_value\n";
    }
    chmod_owner_only();
    SecureEnvLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str(), kOperatorTestnetAllowlist).status, hy::EnvLoadStatus::Ok);

    auto [err, creds] = BoundHmacCredentials::load_and_bind_credentials(EnvironmentBinding::operator_testnet(), loader);
    EXPECT_EQ(err, QuerySigningError::Ok);
    ASSERT_NE(creds, nullptr);
    std::array<char, 128> out{};
    std::size_t written = 0;
    EXPECT_TRUE(creds->copy_api_key(out, written));
    EXPECT_EQ(std::string_view(out.data(), written), kSyntheticApiKey);

    // the trading binding asks the same loader for the trading names, which the file does not hold
    auto [trading_err, trading_creds] = BoundHmacCredentials::load_and_bind_credentials(EnvironmentBinding::testnet(), loader);
    EXPECT_EQ(trading_err, QuerySigningError::InvalidApiKeyFormat);
    EXPECT_EQ(trading_creds, nullptr);
}

TEST_F(BoundHmacCredentialsTest, AFileThatHoldsTheTradingNamesIsNotLoadedByTheOperatorToolsAllowlist) {
    write_valid_env();  // HENGYUAN_BINANCE_TESTNET_API_KEY / _SECRET
    SecureEnvLoader loader;
    const hy::EnvLoadResult r = loader.load(tmp_path_.c_str(), kOperatorTestnetAllowlist);
    EXPECT_EQ(r.status, hy::EnvLoadStatus::ParseError);
    EXPECT_EQ(r.parse_result.status, hy::EnvParseStatus::UnknownKey) << "a credentials file with the trading key in it is refused outright";
    EXPECT_TRUE(loader.get("HENGYUAN_BINANCE_TESTNET_API_KEY").empty());
}
