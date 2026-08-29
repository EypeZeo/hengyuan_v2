// Plan v17 §"binance_query_signing.hpp": QuerySigningError/
// CanonicalUnsignedQuery/SignedQuery/build_canonical_query()/
// build_signed_query(). Synthetic test credentials only.
//
// P2-15-1 (must read before touching the known-answer vector below):
// build_canonical_query() sorts params lexicographically by key. The
// well-known Binance API-docs signing example uses the param order
// symbol,side,type,timeInForce,quantity,price,recvWindow — that is NOT
// lexicographic, so its published expected signature
// ("c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71",
// reused as-is in test_binance_signer.cpp, which signs a hand-built raw
// string and never goes through this file's sorting builder) does NOT
// apply to the sorted byte sequence this builder produces for the same
// business params. The expected hex below was computed independently, on
// the SORTED byte sequence, using two separate external tools (Python's
// hmac/hashlib and `openssl dgst -hmac`) that agree with each other — not
// derived from this codebase's own BinanceSigner. It is intentionally
// different from the well-known c8db5682... value; that is expected, not
// a bug, and nobody should ever "fix" this test to match that other hash.
#include <gtest/gtest.h>
#include <hengyuan/binance_environment.hpp>
#include <hengyuan/binance_query_signing.hpp>
#include "binance_environment_test_hooks.hpp"
#include "binance_query_signing_test_hooks.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <sys/stat.h>
#endif

using hy::BoundHmacCredentials;
using hy::CanonicalUnsignedQuery;
using hy::CanonicalUnsignedQueryTestHooks;
using hy::EnvAllowlist;
using hy::EnvironmentBinding;
using hy::QuerySigningError;
using hy::SecureEnvLoader;
using hy::SignedQuery;
using hy::build_canonical_query;
using hy::build_signed_query;
using hy::kMaxQueryLen;
using hy::kMaxQueryParams;
using hy::kSignatureParamLen;

namespace {

using ParamList = std::vector<std::pair<std::string_view, std::string_view>>;

constexpr std::string_view kTestnetAllowedKeys[] = {
    "HENGYUAN_BINANCE_TESTNET_API_KEY",
    "HENGYUAN_BINANCE_TESTNET_SECRET",
};
constexpr EnvAllowlist kTestnetAllowlist{kTestnetAllowedKeys, 2};

}  // namespace

// --- build_canonical_query ---

TEST(BuildCanonicalQuery, SortsByKeyLexicographically) {
    ParamList params = {{"symbol", "LTCBTC"}, {"side", "BUY"}, {"type", "LIMIT"}};
    auto [err, q] = build_canonical_query(params);
    ASSERT_EQ(err, QuerySigningError::Ok);
    EXPECT_EQ(q.bytes(), "side=BUY&symbol=LTCBTC&type=LIMIT");
}

TEST(BuildCanonicalQuery, RejectsEmptyParamSpan) {
    ParamList params;
    auto [err, q] = build_canonical_query(params);
    EXPECT_EQ(err, QuerySigningError::QueryTooLarge);
}

TEST(BuildCanonicalQuery, RejectsTooManyParams) {
    ParamList params;
    for (std::size_t i = 0; i <= kMaxQueryParams; ++i) {
        params.push_back({"k", "v"});  // duplicate keys, but count check fires first
    }
    auto [err, q] = build_canonical_query(params);
    EXPECT_EQ(err, QuerySigningError::QueryTooLarge);
}

TEST(BuildCanonicalQuery, RejectsDuplicateKey) {
    ParamList params = {{"symbol", "LTCBTC"}, {"symbol", "ETHBTC"}};
    auto [err, q] = build_canonical_query(params);
    EXPECT_EQ(err, QuerySigningError::DuplicateParam);
}

// --- AUDIT L4-RESERVED-PARAM-004 ---

TEST(BuildCanonicalQuery, RejectsReservedTimestampKey) {
    ParamList params = {{"symbol", "LTCBTC"}, {"timestamp", "1499827319559"}};
    auto [err, q] = build_canonical_query(params);
    EXPECT_EQ(err, QuerySigningError::ReservedParamName);
}

TEST(BuildCanonicalQuery, RejectsReservedSignatureKey) {
    ParamList params = {{"symbol", "LTCBTC"}, {"signature", "deadbeef"}};
    auto [err, q] = build_canonical_query(params);
    EXPECT_EQ(err, QuerySigningError::ReservedParamName);
}

TEST(BuildCanonicalQuery, AcceptsRecvWindowAsOrdinaryParam) {
    // recvWindow is an ordinary business param, not reserved -- must not be
    // caught by the same check that rejects timestamp/signature.
    ParamList params = {{"recvWindow", "5000"}};
    auto [err, q] = build_canonical_query(params);
    EXPECT_EQ(err, QuerySigningError::Ok);
}

TEST(BuildCanonicalQuery, PercentEncodesReservedCharacters) {
    ParamList params = {{"a", "1+1"}, {"b", "hello world"}};
    auto [err, q] = build_canonical_query(params);
    ASSERT_EQ(err, QuerySigningError::Ok);
    EXPECT_EQ(q.bytes(), "a=1%2B1&b=hello%20world");
}

TEST(BuildCanonicalQuery, TildeIsNotEncoded) {
    ParamList params = {{"a", "abc~123-_.XYZ"}};
    auto [err, q] = build_canonical_query(params);
    ASSERT_EQ(err, QuerySigningError::Ok);
    EXPECT_EQ(q.bytes(), "a=abc~123-_.XYZ");  // fully unreserved, untouched
}

// AUDIT TEST-GAP-L4-007: percent-encoding was only ever exercised on the
// value side (above) even though append_percent_encoded() is applied to
// keys identically -- this closes that gap on the key side.
TEST(BuildCanonicalQuery, PercentEncodesKeyToo) {
    ParamList params = {{"a b", "1"}, {"c+d", "2"}};
    auto [err, q] = build_canonical_query(params);
    ASSERT_EQ(err, QuerySigningError::Ok);
    // Sort order is by the RAW (pre-encoding) key: "a b" (0x61...) < "c+d"
    // (0x63...).
    EXPECT_EQ(q.bytes(), "a%20b=1&c%2Bd=2");
}

// AUDIT TEST-GAP-L4-007: the real injection vector this file's percent-
// encoding exists to close -- a value containing '&' or '=' must not be
// able to smuggle in an extra param or corrupt the kv boundary. The
// implementation was already safe (append_percent_encoded() has no
// special-case for these characters), but nothing proved it until now.
TEST(BuildCanonicalQuery, ValueContainingAmpersandEqualsStaysSafe) {
    ParamList params = {{"a", "x&b=y"}};
    auto [err, q] = build_canonical_query(params);
    ASSERT_EQ(err, QuerySigningError::Ok);
    EXPECT_EQ(q.bytes(), "a=x%26b%3Dy");  // single kv, not split into a=x, b=y
}

TEST(BuildCanonicalQuery, MaxLengthBoundarySucceeds) {
    // key "k" (1) + "=" (1) + value -> total == kMaxQueryLen exactly.
    std::string value(kMaxQueryLen - 2, 'A');
    ParamList params = {{"k", value}};
    auto [err, q] = build_canonical_query(params);
    ASSERT_EQ(err, QuerySigningError::Ok);
    EXPECT_EQ(q.bytes().size(), kMaxQueryLen);
}

TEST(BuildCanonicalQuery, OneByteOverMaxLengthFails) {
    std::string value(kMaxQueryLen - 1, 'A');  // one byte over budget
    ParamList params = {{"k", value}};
    auto [err, q] = build_canonical_query(params);
    EXPECT_EQ(err, QuerySigningError::QueryTooLarge);
}

// --- build_signed_query: fresh_ts_ms boundary ---

class BuildSignedQueryFixture : public ::testing::Test {
protected:
    std::string tmp_path_;

    void SetUp() override {
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        tmp_path_ = std::string(tmp) + "hy_test_binqs_" +
                    std::to_string(GetCurrentProcessId()) + ".env";
#else
        tmp_path_ = "/tmp/hy_test_binqs_" + std::to_string(getpid()) + ".env";
#endif
        std::ofstream f(tmp_path_, std::ios::binary);
        f << "HENGYUAN_BINANCE_TESTNET_API_KEY="
             "TESTKEYabcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234\n"
          << "HENGYUAN_BINANCE_TESTNET_SECRET="
             "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j\n";
        f.close();
#ifdef __linux__
        // env_loader.hpp's Linux path rejects (PermissionTooWide) any file
        // readable/writable by group or other -- a freshly created temp
        // file's mode depends on the process umask, not guaranteed to
        // already satisfy this (Windows has no equivalent check).
        chmod(tmp_path_.c_str(), 0600);
#endif
    }

    void TearDown() override { std::remove(tmp_path_.c_str()); }

    std::unique_ptr<BoundHmacCredentials> load_creds() {
        loader_ = std::make_unique<SecureEnvLoader>();
        auto r = loader_->load(tmp_path_.c_str(), kTestnetAllowlist);
        EXPECT_EQ(r.status, hy::EnvLoadStatus::Ok);
        auto binding = EnvironmentBinding::testnet();
        auto [err, creds] = BoundHmacCredentials::load_and_bind_credentials(binding, *loader_);
        EXPECT_EQ(err, QuerySigningError::Ok);
        return std::move(creds);
    }

private:
    std::unique_ptr<SecureEnvLoader> loader_;
};

TEST_F(BuildSignedQueryFixture, KnownAnswerVectorEndToEnd) {
    auto creds = load_creds();
    ASSERT_NE(creds, nullptr);

    ParamList params = {
        {"symbol", "LTCBTC"}, {"side", "BUY"}, {"type", "LIMIT"},
        {"timeInForce", "GTC"}, {"quantity", "1"}, {"price", "0.1"},
        {"recvWindow", "5000"},
    };
    auto [q_err, unsigned_query] = build_canonical_query(params);
    ASSERT_EQ(q_err, QuerySigningError::Ok);
    // Sorted order, confirmed above by BuildCanonicalQuery.SortsByKeyLexicographically
    // and manually re-derivable: price,quantity,recvWindow,side,symbol,timeInForce,type.
    EXPECT_EQ(unsigned_query.bytes(),
              "price=0.1&quantity=1&recvWindow=5000&side=BUY&symbol=LTCBTC"
              "&timeInForce=GTC&type=LIMIT");

    auto [s_err, signed_query] = build_signed_query(*creds, unsigned_query, 1499827319559LL);
    ASSERT_EQ(s_err, QuerySigningError::Ok);

    const std::string expected =
        "price=0.1&quantity=1&recvWindow=5000&side=BUY&symbol=LTCBTC"
        "&timeInForce=GTC&type=LIMIT&timestamp=1499827319559"
        "&signature=100b7c0e855e5a970d9d44e297b2db5670764f73cddfd8a9d1cf4a464b3f9b4d";
    EXPECT_EQ(signed_query.wire_bytes(), expected);
}

TEST_F(BuildSignedQueryFixture, NegativeTimestampRejected) {
    auto creds = load_creds();
    ParamList params = {{"recvWindow", "5000"}};
    auto [q_err, unsigned_query] = build_canonical_query(params);
    ASSERT_EQ(q_err, QuerySigningError::Ok);

    auto [err, sq] = build_signed_query(*creds, unsigned_query, -1);
    EXPECT_EQ(err, QuerySigningError::QueryTooLarge);
}

TEST_F(BuildSignedQueryFixture, TimestampAtUpperBoundaryRejected) {
    auto creds = load_creds();
    ParamList params = {{"recvWindow", "5000"}};
    auto [q_err, unsigned_query] = build_canonical_query(params);
    ASSERT_EQ(q_err, QuerySigningError::Ok);

    auto [err, sq] = build_signed_query(*creds, unsigned_query, 10'000'000'000'000LL);
    EXPECT_EQ(err, QuerySigningError::QueryTooLarge);
}

TEST_F(BuildSignedQueryFixture, TimestampJustBelowUpperBoundarySucceeds) {
    auto creds = load_creds();
    ParamList params = {{"recvWindow", "5000"}};
    auto [q_err, unsigned_query] = build_canonical_query(params);
    ASSERT_EQ(q_err, QuerySigningError::Ok);

    auto [err, sq] = build_signed_query(*creds, unsigned_query, 9'999'999'999'999LL);
    EXPECT_EQ(err, QuerySigningError::Ok);
    EXPECT_NE(sq.wire_bytes().find("&timestamp=9999999999999&signature="),
              std::string_view::npos);
}

TEST_F(BuildSignedQueryFixture, TimestampZeroSucceeds) {
    auto creds = load_creds();
    ParamList params = {{"recvWindow", "5000"}};
    auto [q_err, unsigned_query] = build_canonical_query(params);
    ASSERT_EQ(q_err, QuerySigningError::Ok);

    auto [err, sq] = build_signed_query(*creds, unsigned_query, 0);
    EXPECT_EQ(err, QuerySigningError::Ok);
    EXPECT_NE(sq.wire_bytes().find("&timestamp=0&signature="), std::string_view::npos);
}

// --- P2-15-2 / P1-16-1: empty canonical query defense ---

TEST_F(BuildSignedQueryFixture, EmptyCanonicalQueryRejected) {
    auto creds = load_creds();
    CanonicalUnsignedQuery empty = CanonicalUnsignedQueryTestHooks::make_empty_for_test();
    ASSERT_TRUE(empty.bytes().empty());

    auto [err, sq] = build_signed_query(*creds, empty, 1000);
    EXPECT_EQ(err, QuerySigningError::QueryTooLarge);
}

// --- kSignatureParamLen boundary: max-length canonical query + signature ---

TEST_F(BuildSignedQueryFixture, MaxLengthCanonicalQueryDoesNotOverflowSignedQuery) {
    auto creds = load_creds();
    std::string value(kMaxQueryLen - 2, 'A');  // canonical bytes == kMaxQueryLen exactly
    ParamList params = {{"k", value}};
    auto [q_err, unsigned_query] = build_canonical_query(params);
    ASSERT_EQ(q_err, QuerySigningError::Ok);
    ASSERT_EQ(unsigned_query.bytes().size(), kMaxQueryLen);

    auto [err, sq] = build_signed_query(*creds, unsigned_query, 9'999'999'999'999LL);
    ASSERT_EQ(err, QuerySigningError::Ok);
    // canonical (kMaxQueryLen) + "&timestamp=" (11) + 13 digits + "&signature="
    // (11) + 64 hex == kMaxQueryLen + kSignatureParamLen exactly.
    EXPECT_EQ(sq.wire_bytes().size(), kMaxQueryLen + kSignatureParamLen);
}
