// P2-EXEC-LIVE-01: BinanceSigner HMAC-SHA256 tests.
// Verifies signing against known Binance API test vectors.
// SIMULATION ONLY — no real API keys, no network.
#include <gtest/gtest.h>
#include <hengyuan/binance_signer.hpp>
#include <string>
#include <string_view>

using hy::BinanceSigner;

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
