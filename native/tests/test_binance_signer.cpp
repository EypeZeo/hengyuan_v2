// P2-EXEC-LIVE-01: BinanceSigner HMAC-SHA256 tests.
// Verifies signing against known Binance API test vectors.
// SIMULATION ONLY — no real API keys, no network.
#include <gtest/gtest.h>
#include <hengyuan/binance_signer.hpp>
#include "binance_signer_test_hooks.hpp"
#include <cstddef>
#include <string>
#include <string_view>

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
