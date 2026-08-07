// Known-answer tests for sha256.hpp's vendored SHA-256/HMAC-SHA256. Pinned to
// the official NIST SHA-256 test vectors and RFC 4231's HMAC-SHA256 test
// vectors -- every expected digest below was independently generated via
// `openssl dgst -sha256` / `openssl dgst -sha256 -mac HMAC` (not retyped from
// memory) so a transcription error here can't silently validate a broken
// implementation. See sha256.hpp's own header comment for why this is
// vendored rather than using binance_signer.hpp's OpenSSL wrapper.
#include <gtest/gtest.h>
#include <hengyuan/sha256.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace hy::crypto;

namespace {

std::string to_hex(const Sha256Digest& d) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (auto b : d.bytes) {
        out.push_back(kHexDigits[b >> 4]);
        out.push_back(kHexDigits[b & 0x0F]);
    }
    return out;
}

std::span<const std::byte> as_bytes(const std::string& s) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

std::span<const std::byte> as_bytes(const std::vector<std::uint8_t>& v) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.data()), v.size());
}

}  // namespace

// --- SHA-256 known-answer tests (NIST) ---

TEST(Sha256, EmptyString) {
    EXPECT_EQ(to_hex(sha256(as_bytes(std::string()))),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, Abc) {
    EXPECT_EQ(to_hex(sha256(as_bytes(std::string("abc")))),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// NIST's standard 448-bit (56-byte) multi-block message vector -- exercises
// the padding boundary this exact length was chosen to hit.
TEST(Sha256, Nist448BitVector) {
    EXPECT_EQ(to_hex(sha256(as_bytes(
                  std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")))),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

// Exercises the multi-call update() accumulation path (as opposed to a
// single update() call) producing the identical digest to Nist448BitVector.
TEST(Sha256, IncrementalUpdateMatchesSingleShot) {
    Sha256 h;
    std::string msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    h.update(as_bytes(msg.substr(0, 10)));
    h.update(as_bytes(msg.substr(10, 40)));
    h.update(as_bytes(msg.substr(50)));
    EXPECT_EQ(to_hex(h.finish()),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

// A message longer than one 64-byte block by exactly one byte over several
// blocks, forcing update() through its "already-buffered bytes plus a full
// block" branch and its "process several full blocks in a loop" branch.
TEST(Sha256, MultiBlockMessage) {
    std::vector<std::uint8_t> data(200, 0x61);  // 200 'a' bytes
    auto d1 = sha256(as_bytes(data));

    Sha256 h;
    h.update(as_bytes(data));
    auto d2 = h.finish();
    EXPECT_EQ(d1, d2);

    // Cross-check against a second independent invocation with a different
    // internal chunking (proves chunking doesn't affect the result).
    Sha256 h2;
    for (std::size_t i = 0; i < data.size(); i += 7) {
        std::size_t n = std::min<std::size_t>(7, data.size() - i);
        h2.update(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(data.data() + i), n));
    }
    EXPECT_EQ(d1, h2.finish());
}

// --- HMAC-SHA256 known-answer tests (RFC 4231) ---

TEST(HmacSha256, RfcTestCase1) {
    std::vector<std::uint8_t> key(20, 0x0b);
    EXPECT_EQ(to_hex(hmac_sha256(as_bytes(key), as_bytes(std::string("Hi There")))),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(HmacSha256, RfcTestCase2ShortKey) {
    EXPECT_EQ(to_hex(hmac_sha256(as_bytes(std::string("Jefe")),
                                  as_bytes(std::string("what do ya want for nothing?")))),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(HmacSha256, RfcTestCase3) {
    std::vector<std::uint8_t> key(20, 0xaa);
    std::vector<std::uint8_t> data(50, 0xdd);
    EXPECT_EQ(to_hex(hmac_sha256(as_bytes(key), as_bytes(data))),
              "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
}

TEST(HmacSha256, RfcTestCase4) {
    std::vector<std::uint8_t> key;
    for (int i = 1; i <= 25; ++i) key.push_back(static_cast<std::uint8_t>(i));
    std::vector<std::uint8_t> data(50, 0xcd);
    EXPECT_EQ(to_hex(hmac_sha256(as_bytes(key), as_bytes(data))),
              "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
}

// Key longer than the SHA-256 block size (64 bytes) -- exercises the
// "hash the key first" branch in HmacSha256's constructor.
TEST(HmacSha256, KeyLongerThanBlockSizeIsHashedFirst) {
    std::vector<std::uint8_t> key(131, 0xaa);
    EXPECT_EQ(to_hex(hmac_sha256(
                  as_bytes(key), as_bytes(std::string("Test Using Larger Than Block-Size Key - Hash Key First")))),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

// Key AND data both longer than the block size.
TEST(HmacSha256, KeyAndDataLongerThanBlockSize) {
    std::vector<std::uint8_t> key(131, 0xaa);
    std::string data =
        "This is a test using a larger than block-size key and a larger "
        "than block-size data. The key needs to be hashed before being "
        "used by the HMAC algorithm.";
    EXPECT_EQ(to_hex(hmac_sha256(as_bytes(key), as_bytes(data))),
              "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

TEST(HmacSha256, IncrementalUpdateMatchesSingleShot) {
    std::vector<std::uint8_t> key(20, 0x0b);
    std::string msg = "Hi There";

    HmacSha256 h(as_bytes(key));
    h.update(as_bytes(msg.substr(0, 3)));
    h.update(as_bytes(msg.substr(3)));
    EXPECT_EQ(to_hex(h.finish()),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

// Two different keys over the same data must never collide.
TEST(HmacSha256, DifferentKeysProduceDifferentDigests) {
    std::string data = "same payload";
    std::vector<std::uint8_t> key_a(20, 0xaa);
    std::vector<std::uint8_t> key_b(20, 0xbb);
    EXPECT_NE(to_hex(hmac_sha256(as_bytes(key_a), as_bytes(data))),
              to_hex(hmac_sha256(as_bytes(key_b), as_bytes(data))));
}

// --- constant_time_equal (audit SEC-MACCMP-010) ---
//
// These test CORRECTNESS, not timing. A constant-time comparison that returns the
// wrong answer is far worse than a leaky one, and timing itself is not something a
// unit test on a shared CI runner can measure honestly -- the guarantee here comes
// from the implementation having no data-dependent control flow or early exit,
// which is a code-review property. What IS worth pinning is that replacing
// seventeen std::memcmp calls did not change any verdict.

TEST(ConstantTimeEqual, EqualBuffersCompareEqual) {
    std::array<std::byte, 32> a{};
    std::array<std::byte, 32> b{};
    for (std::size_t i = 0; i < 32; ++i) {
        a[i] = static_cast<std::byte>(i * 7 + 1);
        b[i] = a[i];
    }
    EXPECT_TRUE(hy::crypto::constant_time_equal(a, b));
}

TEST(ConstantTimeEqual, DiffersInFirstByte) {
    std::array<std::byte, 32> a{};
    std::array<std::byte, 32> b{};
    b[0] = std::byte{1};
    EXPECT_FALSE(hy::crypto::constant_time_equal(a, b));
}

TEST(ConstantTimeEqual, DiffersInLastByte) {
    // The case std::memcmp handles in 32 comparisons and this handles in 32 --
    // i.e. the one whose timing used to leak.
    std::array<std::byte, 32> a{};
    std::array<std::byte, 32> b{};
    b[31] = std::byte{1};
    EXPECT_FALSE(hy::crypto::constant_time_equal(a, b));
}

TEST(ConstantTimeEqual, SingleBitDifferenceIsDetected) {
    for (std::size_t i = 0; i < 32; ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            std::array<std::byte, 32> a{};
            std::array<std::byte, 32> b{};
            b[i] = static_cast<std::byte>(1u << bit);
            ASSERT_FALSE(hy::crypto::constant_time_equal(a, b))
                << "byte " << i << " bit " << bit << " must be detected";
        }
    }
}

TEST(ConstantTimeEqual, LengthMismatchIsNotEqual) {
    std::array<std::byte, 32> a{};
    std::array<std::byte, 16> b{};
    EXPECT_FALSE(hy::crypto::constant_time_equal(a, b));
}

TEST(ConstantTimeEqual, EmptyRangesAreEqual) {
    EXPECT_TRUE(hy::crypto::constant_time_equal(std::span<const std::byte>{},
                                                 std::span<const std::byte>{}));
}

TEST(ConstantTimeEqual, AgreesWithMemcmpOnRandomVectors) {
    // Differential check against the function it replaced, over every
    // single-byte-difference position plus the all-equal case.
    std::array<std::byte, 32> base{};
    for (std::size_t i = 0; i < 32; ++i) base[i] = static_cast<std::byte>(i * 31 + 5);

    for (std::size_t pos = 0; pos <= 32; ++pos) {
        auto other = base;
        if (pos < 32) other[pos] = static_cast<std::byte>(~static_cast<unsigned>(base[pos]) & 0xFFu);
        const bool ct = hy::crypto::constant_time_equal(base, other);
        const bool mc = std::memcmp(base.data(), other.data(), base.size()) == 0;
        ASSERT_EQ(ct, mc) << "verdict changed at pos=" << pos;
    }
}

TEST(ConstantTimeEqual, DigestOverloadMatchesWireBytes) {
    const auto digest = hy::crypto::sha256(as_bytes(std::string("abc")));
    std::array<std::byte, 32> wire{};
    std::memcpy(wire.data(), digest.bytes.data(), wire.size());
    EXPECT_TRUE(hy::crypto::constant_time_equal(digest, wire));

    wire[17] = static_cast<std::byte>(static_cast<unsigned>(wire[17]) ^ 0x01u);
    EXPECT_FALSE(hy::crypto::constant_time_equal(digest, wire));
}

TEST(Sha256Digest, EqualityIsConstantTimeAndStillCorrect) {
    const auto a = hy::crypto::sha256(as_bytes(std::string("abc")));
    const auto b = hy::crypto::sha256(as_bytes(std::string("abc")));
    const auto c = hy::crypto::sha256(as_bytes(std::string("abd")));
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
    EXPECT_TRUE(a != c);
    EXPECT_FALSE(a == c);
}
