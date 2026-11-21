// P2-EXEC-LIVE-01: BinanceSigner HMAC-SHA256 tests.
// Verifies signing against known Binance API test vectors.
// SIMULATION ONLY — no real API keys, no network.
#include <gtest/gtest.h>
#include <hengyuan/binance_signer.hpp>
#include <hengyuan/owner_cancel_approval.hpp>
#include "binance_clock_sync_test_hooks.hpp"
#include "binance_signer_test_hooks.hpp"
#include <cstddef>
#include <cstdint>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using hy::BinanceSigner;
using hy::BinanceSignerTestHooks;

namespace {

// Counting stub MemoryLockBehavior — deterministic, not tied to real system
// lock quota. Each test gets its own pair of counters via a small fixture
// struct rather than shared globals (tests may run in parallel).
struct LockStub {
    int lock_calls = 0;
    int unlock_calls = 0;
    bool lock_result = true;
    // AUDIT L4-LOCK-ORDER-003 regression: true iff every buffer handed to
    // stub_lock() across the whole test was still all-zero at the moment
    // lock() was called — i.e. the secret had not been memcpy'd in yet.
    bool saw_all_zero_at_lock = true;
};

LockStub* g_active_stub = nullptr;  // one at a time — tests are sequential
                                     // within this TU (gtest default runner).

bool stub_lock(void* ptr, std::size_t size) noexcept {
    ++g_active_stub->lock_calls;
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != 0) {
            g_active_stub->saw_all_zero_at_lock = false;
            break;
        }
    }
    return g_active_stub->lock_result;
}
void stub_unlock(void*, std::size_t) noexcept {
    ++g_active_stub->unlock_calls;
}

}  // namespace

namespace {

struct P256Fixture {
    EVP_PKEY* key{nullptr};
    std::vector<unsigned char> public_der;

    P256Fixture() {
        key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1");
        if (!key) return;
        const int size = i2d_PUBKEY(key, nullptr);
        if (size <= 0) return;
        public_der.resize(static_cast<std::size_t>(size));
        unsigned char* cursor = public_der.data();
        if (i2d_PUBKEY(key, &cursor) != size) public_der.clear();
    }

    ~P256Fixture() { EVP_PKEY_free(key); }
    P256Fixture(const P256Fixture&) = delete;
    P256Fixture& operator=(const P256Fixture&) = delete;

    std::vector<unsigned char> sign(std::span<const std::byte> payload) const {
        std::vector<unsigned char> out;
        if (!key) return out;
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        if (!context) return out;
        if (EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key) != 1 ||
            EVP_DigestSignUpdate(context, payload.data(), payload.size()) != 1) {
            EVP_MD_CTX_free(context);
            return out;
        }
        std::size_t size = 0;
        if (EVP_DigestSignFinal(context, nullptr, &size) != 1) {
            EVP_MD_CTX_free(context);
            return out;
        }
        out.resize(size);
        if (EVP_DigestSignFinal(context, out.data(), &size) != 1) out.clear();
        else out.resize(size);
        EVP_MD_CTX_free(context);
        return out;
    }
};

void fresh_clock(hy::ClockOffsetPublisher& publisher) {
    hy::ClockOffsetSnapshot snapshot{};
    snapshot.system_at_fetch_ms = 1'000'000;
    snapshot.steady_at_fetch_ms = 2'000'000;
    snapshot.error_bound_ms = 0;
    EXPECT_TRUE(publisher.publish(snapshot));
}

hy::ClockPairSample clock_now() {
    return hy::ClockPairSampleTestHooks::make(1'000'000, 2'000'000);
}

hy::CancelApproval approval_for(const hy::CancelSubject& subject, std::uint64_t serial,
                                std::int64_t not_after) {
    hy::CancelApproval approval{};
    approval.subject = subject;
    std::strncpy(approval.symbol, "BTCUSDT", sizeof(approval.symbol) - 1);
    approval.serial = serial;
    approval.not_after_ms = not_after;
    return approval;
}

hy::AutomaticCancelRequest request_for(const hy::CancelApproval& approval) {
    hy::AutomaticCancelRequest request{};
    request.subject = approval.subject;
    std::memcpy(request.symbol, approval.symbol, sizeof(request.symbol));
    request.approval_serial = approval.serial;
    request.approval_not_after_ms = approval.not_after_ms;
    return request;
}

std::span<const std::byte> grant_bytes(const hy::OwnerCancelGrantBytes& grant) {
    return std::span<const std::byte>(grant.data(), grant.size());
}

}  // namespace

TEST(OwnerCancelApproval, InstallsSignedCanonicalGrantAndPermitsMatchingRequest) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    ASSERT_FALSE(fixture.public_der.empty());
    const hy::StoreBinding binding{hy::StoreEnvironment::Testnet, 42, 100};
    auto authority = hy::OwnerCancelAuthority::create(binding, 7, 8, 11, fixture.public_der, 60'000);
    ASSERT_NE(authority, nullptr);
    const auto approval = approval_for(authority->subject(), 1, 1'030'000);
    const auto grant = authority->signing_payload("BTCUSDT", approval.serial, approval.not_after_ms);
    ASSERT_TRUE(grant.has_value());
    const auto signature = fixture.sign(grant_bytes(*grant));
    ASSERT_FALSE(signature.empty());
    hy::ClockOffsetPublisher publisher;
    fresh_clock(publisher);
    EXPECT_EQ(authority->install(grant_bytes(*grant), signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::Installed);
    EXPECT_TRUE(authority->permits(request_for(approval), publisher, clock_now()));
    auto config = authority->config();
    const auto installed = config.approvals.read("BTCUSDT");
    ASSERT_TRUE(installed.has_value());
    EXPECT_EQ(installed->serial, approval.serial);
}

TEST(OwnerCancelApproval, RejectsPayloadTamperingAndWrongKeyId) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    const hy::StoreBinding binding{hy::StoreEnvironment::Testnet, 42, 100};
    auto authority = hy::OwnerCancelAuthority::create(binding, 7, 8, 11, fixture.public_der, 60'000);
    ASSERT_NE(authority, nullptr);
    const auto approval = approval_for(authority->subject(), 1, 1'030'000);
    auto grant = hy::encode_owner_cancel_grant(approval, 11);
    ASSERT_TRUE(grant.has_value());
    const auto signature = fixture.sign(grant_bytes(*grant));
    ASSERT_FALSE(signature.empty());
    hy::ClockOffsetPublisher publisher;
    fresh_clock(publisher);
    auto tampered = *grant;
    tampered[0] ^= std::byte{1};
    EXPECT_EQ(authority->install(grant_bytes(tampered), signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::MalformedGrant);
    const auto modified_grant = authority->signing_payload("ETHUSDT", approval.serial, approval.not_after_ms);
    ASSERT_TRUE(modified_grant.has_value());
    EXPECT_EQ(authority->install(*modified_grant, signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::InvalidSignature);
    P256Fixture other_key;
    ASSERT_NE(other_key.key, nullptr);
    const auto wrong_key_signature = other_key.sign(*grant);
    ASSERT_FALSE(wrong_key_signature.empty());
    EXPECT_EQ(authority->install(*grant, wrong_key_signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::InvalidSignature);
    hy::ClockOffsetPublisher unsynchronized;
    EXPECT_EQ(authority->install(*grant, signature, unsynchronized, clock_now()),
              hy::OwnerCancelInstallResult::ClockUnavailable);
    ASSERT_FALSE(authority->config().approvals.read("BTCUSDT").has_value());
    auto wrong_key = hy::encode_owner_cancel_grant(approval, 12);
    ASSERT_TRUE(wrong_key.has_value());
    const auto wrong_signature = fixture.sign(grant_bytes(*wrong_key));
    EXPECT_EQ(authority->install(grant_bytes(*wrong_key), wrong_signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::WrongScope);
}

TEST(OwnerCancelApproval, EnforcesSerialWatermarkRevocationAndExpiration) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    const hy::StoreBinding binding{hy::StoreEnvironment::Testnet, 42, 100};
    auto authority = hy::OwnerCancelAuthority::create(binding, 7, 8, 11, fixture.public_der, 60'000);
    ASSERT_NE(authority, nullptr);
    hy::ClockOffsetPublisher publisher;
    fresh_clock(publisher);
    const auto first = approval_for(authority->subject(), 2, 1'030'000);
    auto first_grant = hy::encode_owner_cancel_grant(first, 11);
    ASSERT_TRUE(first_grant.has_value());
    const auto first_signature = fixture.sign(grant_bytes(*first_grant));
    EXPECT_EQ(authority->install(grant_bytes(*first_grant), first_signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::Installed);
    authority->revoke(2);
    EXPECT_FALSE(authority->permits(request_for(first), publisher, clock_now()));
    EXPECT_EQ(authority->install(grant_bytes(*first_grant), first_signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::StaleSerial);
    const auto expired = approval_for(authority->subject(), 3, 999'999);
    auto expired_grant = hy::encode_owner_cancel_grant(expired, 11);
    ASSERT_TRUE(expired_grant.has_value());
    const auto expired_signature = fixture.sign(grant_bytes(*expired_grant));
    EXPECT_EQ(authority->install(grant_bytes(*expired_grant), expired_signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::InvalidDeadline);
}

TEST(OwnerCancelApproval, FreshAuthorityUsesDifferentActivationScope) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    const hy::StoreBinding binding{hy::StoreEnvironment::Testnet, 42, 100};
    auto first = hy::OwnerCancelAuthority::create(binding, 7, 8, 11, fixture.public_der, 60'000);
    auto second = hy::OwnerCancelAuthority::create(binding, 7, 8, 11, fixture.public_der, 60'000);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first->subject().activation_id, second->subject().activation_id);
    const auto approval = approval_for(first->subject(), 1, 1'030'000);
    auto grant = hy::encode_owner_cancel_grant(approval, 11);
    ASSERT_TRUE(grant.has_value());
    const auto signature = fixture.sign(grant_bytes(*grant));
    hy::ClockOffsetPublisher publisher;
    fresh_clock(publisher);
    EXPECT_EQ(second->install(grant_bytes(*grant), signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::WrongScope);
}

TEST(BinanceSigner, UninitializedRefusesSigning) {
    BinanceSigner signer;
    auto sig = signer.sign("test");
    EXPECT_TRUE(sig.empty());
    EXPECT_EQ(signer.stats().failures, 1u);
}

TEST(BinanceSigner, EmptySecretFails) {
    BinanceSigner signer;
    EXPECT_FALSE(signer.init(""));
    EXPECT_FALSE(signer.is_initialized());
}

TEST(BinanceSigner, InitSucceeds) {
    BinanceSigner signer;
    EXPECT_TRUE(signer.init("NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j"));
    EXPECT_TRUE(signer.is_initialized());
}

TEST(BinanceSigner, KnownTestVector) {
    // Binance API docs test vector:
    // secret = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j"
    // payload = "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559"
    // expected HMAC = "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71"
    BinanceSigner signer;
    EXPECT_TRUE(signer.init("NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j"));

    auto sig = signer.sign(
        "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC"
        "&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559");

    ASSERT_EQ(sig.size(), 64u);
    std::string hex(sig.data(), sig.size());
    EXPECT_EQ(hex, "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71");
}

TEST(BinanceSigner, MultipleSignsConsistent) {
    BinanceSigner signer;
    signer.init("testsecret123");

    auto sig1 = signer.sign("payload1");
    std::string hex1(sig1.data(), sig1.size());

    auto sig2 = signer.sign("payload1");
    std::string hex2(sig2.data(), sig2.size());

    EXPECT_EQ(hex1, hex2);
    EXPECT_EQ(signer.stats().signed_ok, 2u);
}

TEST(BinanceSigner, DifferentPayloadsDifferentSigs) {
    BinanceSigner signer;
    signer.init("testsecret123");

    auto sig1 = signer.sign("payload_a");
    std::string hex1(sig1.data(), sig1.size());

    auto sig2 = signer.sign("payload_b");
    std::string hex2(sig2.data(), sig2.size());

    EXPECT_NE(hex1, hex2);
}

TEST(BinanceSigner, EmptyPayloadRefused) {
    BinanceSigner signer;
    signer.init("secret");
    auto sig = signer.sign("");
    EXPECT_TRUE(sig.empty());
    EXPECT_EQ(signer.stats().failures, 1u);
}

TEST(BinanceSigner, ZeroAllocSpanOutput) {
    BinanceSigner signer;
    signer.init("key");
    auto sig = signer.sign("data");
    ASSERT_EQ(sig.size(), 64u);
    // Verify all chars are valid hex
    for (char c : sig) {
        bool valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        EXPECT_TRUE(valid) << "Invalid hex char: " << c;
    }
}

// --- Plan v17 §"BinanceSigner 锁注入" regressions ---

TEST(BinanceSignerLockInjection, StubPairingFailurePath) {
    LockStub stub;
    stub.lock_result = false;
    g_active_stub = &stub;

    BinanceSigner signer;
    bool ok = BinanceSignerTestHooks::init_with_lock_behavior(
        signer, "secretvalue", {&stub_lock, &stub_unlock});

    EXPECT_FALSE(ok);
    EXPECT_EQ(stub.lock_calls, 1);
    EXPECT_EQ(stub.unlock_calls, 0);   // never locked, so destroy() must not
                                        // call unlock — precise pairing, not
                                        // "unlocking unlocked memory is
                                        // harmless" reasoning.
    EXPECT_FALSE(BinanceSignerTestHooks::is_locked(signer));
    EXPECT_EQ(BinanceSignerTestHooks::last_failure(signer),
              BinanceSigner::SignFailure::LockFailed);
    EXPECT_FALSE(signer.is_initialized());
}

TEST(BinanceSignerLockInjection, StubPairingSuccessPath) {
    LockStub stub;
    stub.lock_result = true;
    g_active_stub = &stub;

    {
        BinanceSigner signer;
        bool ok = BinanceSignerTestHooks::init_with_lock_behavior(
            signer, "secretvalue", {&stub_lock, &stub_unlock});
        EXPECT_TRUE(ok);
        EXPECT_EQ(stub.lock_calls, 1);
        EXPECT_EQ(stub.unlock_calls, 0);   // not unlocked yet — destructor
                                             // hasn't run.
        EXPECT_TRUE(BinanceSignerTestHooks::is_locked(signer));
    }  // ~BinanceSigner() -> destroy() runs here

    EXPECT_EQ(stub.unlock_calls, 1);   // exactly one paired unlock call
}

TEST(BinanceSignerLockInjection, NullBehaviorRefusedNotCalled) {
    BinanceSigner signer;
    bool ok = BinanceSignerTestHooks::init_with_lock_behavior(
        signer, "secretvalue", {nullptr, nullptr});

    EXPECT_FALSE(ok);
    EXPECT_EQ(BinanceSignerTestHooks::last_failure(signer),
              BinanceSigner::SignFailure::LockFailed);
    EXPECT_FALSE(signer.is_initialized());
    // No crash: the null function pointers were never invoked.
}

TEST(BinanceSignerLockInjection, RealSyscallSmoke) {
    // Independent of the stub tests above — exercises the actual default
    // MemoryLockBehavior{} (hy::try_lock_memory/unlock_memory). Does not
    // assert the lock necessarily succeeds (depends on the machine's
    // RLIMIT_MEMLOCK/working-set quota) — only that internal state stays
    // consistent and nothing crashes.
    BinanceSigner signer;
    bool ok = signer.init("secretvalue");
    if (ok) {
        EXPECT_TRUE(BinanceSignerTestHooks::is_locked(signer));
    } else {
        EXPECT_FALSE(BinanceSignerTestHooks::is_locked(signer));
    }
}

TEST(BinanceSignerLockInjection, LockSeesEmptyBufferBeforeSecretIsWritten) {
    LockStub stub;
    stub.lock_result = true;
    g_active_stub = &stub;

    BinanceSigner signer;
    bool ok = BinanceSignerTestHooks::init_with_lock_behavior(
        signer, "secretvalue", {&stub_lock, &stub_unlock});

    EXPECT_TRUE(ok);
    EXPECT_EQ(stub.lock_calls, 1);
    // Proves the ordering directly (not just the final signing result):
    // secret_buf_ was still all-zero at the instant lock() ran, i.e. the
    // memcpy of the real secret happened strictly after locking succeeded.
    EXPECT_TRUE(stub.saw_all_zero_at_lock);
}

TEST(BinanceSignerLockInjection, InvalidSecretSetsFailureAndResetsOnRetry) {
    LockStub stub;
    g_active_stub = &stub;
    BinanceSigner signer;

    bool empty_ok = BinanceSignerTestHooks::init_with_lock_behavior(
        signer, "", {&stub_lock, &stub_unlock});
    EXPECT_FALSE(empty_ok);
    EXPECT_EQ(BinanceSignerTestHooks::last_failure(signer),
              BinanceSigner::SignFailure::InvalidSecret);

    std::string too_long(257, 'x');
    bool long_ok = BinanceSignerTestHooks::init_with_lock_behavior(
        signer, too_long, {&stub_lock, &stub_unlock});
    EXPECT_FALSE(long_ok);
    EXPECT_EQ(BinanceSignerTestHooks::last_failure(signer),
              BinanceSigner::SignFailure::InvalidSecret);

    // Reset timing lives at init_impl() entry, not inside destroy() — a
    // subsequent successful attempt must clear the stale diagnostic.
    bool retry_ok = BinanceSignerTestHooks::init_with_lock_behavior(
        signer, "validsecret", {&stub_lock, &stub_unlock});
    EXPECT_TRUE(retry_ok);
    EXPECT_EQ(BinanceSignerTestHooks::last_failure(signer),
              BinanceSigner::SignFailure::None);
}
