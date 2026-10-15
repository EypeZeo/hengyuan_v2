// Private-stream migration M1: binance_ws_api_signing.hpp -- the request side of the spot WebSocket API.
//
// Synthetic credentials only: the apiKey and secretKey are the ones the official docs print for their signing examples (they are already
// in test_binance_signer.cpp). The known answers are of three kinds, none of them derived from the code under test:
//   * the two HMAC examples of the official web-socket-api.md ("SIGNED request example (HMAC)"), digests aa1b5712... and b33892ae...,
//     also recomputed with Python's hmac on 2026-10-14 (blueprint 5.7 P-31);
//   * golden digests for userDataStream.subscribe.signature, computed with Python's hmac;
//   * an independent verifier in this file, which parses a frame like the exchange (a real JSON parser, the parameters sorted by name,
//     "name=value" joined with '&', NO percent-encoding) and recomputes the signature with OpenSSL's own HMAC -- used on the known
//     answers and on hundreds of random parameter sets, so "signed bytes == sent bytes" is checked rather than asserted.
#include <gtest/gtest.h>
#include <hengyuan/binance_environment.hpp>
#include <hengyuan/binance_ws_api_signing.hpp>
#include <hengyuan/spot_rate_limit_budget.hpp>
#include "binance_clock_sync_test_hooks.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#endif
#include <simdjson.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>
#endif

using hy::BoundHmacCredentials;
using hy::ClockReadyTokenTestHooks;
using hy::EnvAllowlist;
using hy::EnvironmentBinding;
using hy::SecureEnvLoader;
using hy::WsApiParam;
using hy::WsApiParamKind;
using hy::WsApiRequestKind;
using hy::WsApiSignedRequest;
using hy::WsApiSigningError;
using hy::WsApiUnsignedRequest;

namespace {

constexpr std::string_view kApiKey = "vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A";
constexpr std::string_view kSecret = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";

constexpr std::string_view kTestnetAllowedKeys[] = {"HENGYUAN_BINANCE_TESTNET_API_KEY", "HENGYUAN_BINANCE_TESTNET_SECRET"};
constexpr EnvAllowlist kTestnetAllowlist{kTestnetAllowedKeys, 2};

hy::ClockReadyToken token_at(std::int64_t server_ms) { return ClockReadyTokenTestHooks::make(server_ms); }

WsApiParam text(std::string_view name, std::string_view value) { return {name, value, WsApiParamKind::Text}; }
WsApiParam number(std::string_view name, std::string_view value) { return {name, value, WsApiParamKind::Number}; }

// --- the independent verifier ---

std::string hmac_sha256_hex(std::string_view key, std::string_view data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char*>(data.data()), data.size(), md, &len);
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(hex[md[i] >> 4]);
        out.push_back(hex[md[i] & 0x0F]);
    }
    return out;
}

struct Verified {
    bool json_ok{false};
    bool signature_ok{false};
    std::string method;
    std::uint64_t id{0};
    std::string signature;
    std::string payload;
    std::vector<std::pair<std::string, std::string>> params;  // without the signature, in the order of the frame
};

// What the exchange does with a frame: parse it (all of it), take the params, drop the signature, sort by name, join "name=value" with '&',
// HMAC with the secret and compare. Strings are taken after JSON unescaping, numbers as the token was written.
Verified verify_frame(std::string_view frame, std::string_view secret) {
    Verified v;
    const simdjson::padded_string padded(frame);

    simdjson::dom::parser dom_parser;
    simdjson::dom::element root;
    if (dom_parser.parse(padded).get(root)) return v;  // not entirely valid JSON
    simdjson::dom::object top;
    if (root.get_object().get(top)) return v;
    std::uint64_t id = 0;
    std::string_view method;
    simdjson::dom::element e;
    if (top.at_key("id").get(e) || e.get_uint64().get(id)) return v;
    if (top.at_key("method").get(e) || e.get_string().get(method)) return v;
    simdjson::dom::object params_dom;
    if (top.at_key("params").get(e) || e.get_object().get(params_dom)) return v;
    v.id = id;
    v.method = std::string(method);

    simdjson::ondemand::parser od_parser;
    simdjson::ondemand::document doc;
    if (od_parser.iterate(padded).get(doc)) return v;
    simdjson::ondemand::object od_top;
    if (doc.get_object().get(od_top)) return v;
    simdjson::ondemand::object od_params;
    if (od_top.find_field_unordered("params").get_object().get(od_params)) return v;
    for (auto field : od_params) {
        std::string_view key;
        if (field.unescaped_key().get(key)) return v;
        simdjson::ondemand::value val = field.value();
        simdjson::ondemand::json_type t{};
        if (val.type().get(t)) return v;
        std::string text_value;
        if (t == simdjson::ondemand::json_type::string) {
            std::string_view s;
            if (val.get_string().get(s)) return v;
            text_value = std::string(s);
        } else if (t == simdjson::ondemand::json_type::number) {
            text_value = std::string(val.raw_json_token());
            while (!text_value.empty() && (text_value.back() == ' ' || text_value.back() == '\n')) text_value.pop_back();
        } else {
            return v;  // the policy writes strings and numbers only
        }
        if (key == "signature") {
            v.signature = text_value;
        } else {
            v.params.emplace_back(std::string(key), text_value);
        }
    }
    v.json_ok = true;

    auto sorted = v.params;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0) v.payload += '&';
        v.payload += sorted[i].first + "=" + sorted[i].second;
    }
    v.signature_ok = !v.signature.empty() && hmac_sha256_hex(secret, v.payload) == v.signature;
    return v;
}

class WsApiSigningTest : public ::testing::Test {
protected:
    void SetUp() override {
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        env_path_ = std::string(tmp) + "hy_test_wsapisig_" + std::to_string(GetCurrentProcessId()) + ".env";
#else
        env_path_ = "/tmp/hy_test_wsapisig_" + std::to_string(getpid()) + ".env";
#endif
        {
            std::ofstream f(env_path_, std::ios::binary);
            f << "HENGYUAN_BINANCE_TESTNET_API_KEY=" << kApiKey << "\n"
              << "HENGYUAN_BINANCE_TESTNET_SECRET=" << kSecret << "\n";
        }
#ifdef __linux__
        chmod(env_path_.c_str(), 0600);  // the Linux loader refuses a file other users can read
#endif
        loader_ = std::make_unique<SecureEnvLoader>();
        const auto r = loader_->load(env_path_.c_str(), kTestnetAllowlist);
        ASSERT_EQ(r.status, hy::EnvLoadStatus::Ok);
        const auto binding = EnvironmentBinding::testnet();
        auto [err, creds] = BoundHmacCredentials::load_and_bind_credentials(binding, *loader_);
        ASSERT_EQ(err, hy::QuerySigningError::Ok);
        creds_ = std::move(creds);
    }

    void TearDown() override { std::remove(env_path_.c_str()); }

    // The generic builder with the official examples' method name; the typed entry points only differ in the method they pass.
    std::pair<WsApiSigningError, WsApiSignedRequest> sign(std::string_view method, std::uint64_t id, std::vector<WsApiParam> params,
                                                         std::int64_t ts) {
        return hy::detail::build_ws_api_signed_request(*creds_, method, id, params, token_at(ts));
    }

    std::string env_path_;
    std::unique_ptr<SecureEnvLoader> loader_;
    std::unique_ptr<BoundHmacCredentials> creds_;
};

}  // namespace

// --- the official examples ---

TEST_F(WsApiSigningTest, TheOfficialAsciiHmacExampleSignsToTheDocumentedDigest) {
    // web-socket-api.md, "SIGNED request example (HMAC)", first set: payload 197 bytes, digest aa1b5712....
    const auto [err, req] = sign("order.place", 1,
                                 {text("symbol", "BTCUSDT"), text("side", "SELL"), text("type", "LIMIT"), text("timeInForce", "GTC"),
                                  text("quantity", "0.01000000"), text("price", "52000.00"), number("recvWindow", "100")},
                                 1645423376532LL);
    ASSERT_EQ(err, WsApiSigningError::Ok);
    EXPECT_EQ(req.frame(),
              R"({"id":1,"method":"order.place","params":{"apiKey":"vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A",)"
              R"("price":"52000.00","quantity":"0.01000000","recvWindow":100,"side":"SELL","symbol":"BTCUSDT","timeInForce":"GTC",)"
              R"("timestamp":1645423376532,"type":"LIMIT",)"
              R"("signature":"aa1b5712c094bc4e57c05a1a5c1fd8d88dcd628338ea863fec7b88e59fe2db24"}})");
    EXPECT_EQ(req.request_id(), 1u);
    const Verified v = verify_frame(req.frame(), kSecret);
    EXPECT_TRUE(v.json_ok);
    EXPECT_TRUE(v.signature_ok);
    EXPECT_EQ(v.payload.size(), 197u) << "the docs give the payload length of this example";
}

TEST_F(WsApiSigningTest, TheOfficialNonAsciiHmacExampleKeepsTheSymbolUnencoded) {
    // Second set: the symbol is six full-width digits U+FF11..U+FF16. The payload carries their UTF-8 bytes AS THEY ARE -- no percent-encoding --
    // and so does the frame. Digest b33892ae....
    const std::string symbol = "\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93\xEF\xBC\x94\xEF\xBC\x95\xEF\xBC\x96";
    const auto [err, req] = sign("order.place", 2,
                                 {text("symbol", symbol), text("side", "BUY"), text("type", "LIMIT"), text("timeInForce", "GTC"),
                                  text("quantity", "1.00000000"), text("price", "0.10000000"), number("recvWindow", "5000")},
                                 1645423376532LL);
    ASSERT_EQ(err, WsApiSigningError::Ok);
    EXPECT_NE(req.frame().find("\"symbol\":\"" + symbol + "\""), std::string_view::npos);
    EXPECT_NE(req.frame().find("\"signature\":\"b33892ae8e687c939f4468c6268ddd4c40ac1af18ad19a064864c47bae0752cd\""), std::string_view::npos);
    const Verified v = verify_frame(req.frame(), kSecret);
    EXPECT_TRUE(v.json_ok);
    EXPECT_TRUE(v.signature_ok);
    EXPECT_EQ(v.payload.size(), 210u);
    EXPECT_EQ(v.payload.find('%'), std::string::npos);
}

// --- userDataStream.subscribe.signature ---

TEST_F(WsApiSigningTest, TheSubscribeSignatureRequestMatchesTheGoldenDigests) {
    // Golden digests: Python hmac over "apiKey=<key>[&recvWindow=N]&timestamp=<ts>" with the docs' secret.
    struct Case {
        std::uint32_t recv_window;
        std::int64_t ts;
        std::string_view frame;
    };
    const Case cases[] = {
        {0, 1747385641636LL,
         R"({"id":11,"method":"userDataStream.subscribe.signature","params":{"apiKey":"vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A",)"
         R"("timestamp":1747385641636,"signature":"681110a6057ad58657af22f5640fab9dd17418834cdfccdc81be8b0f7b2cde3e"}})"},
        {5000, 1747385641636LL,
         R"({"id":11,"method":"userDataStream.subscribe.signature","params":{"apiKey":"vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A",)"
         R"("recvWindow":5000,"timestamp":1747385641636,"signature":"49601ffc5b0122c3c7ee3ea85ef7d6cd76256f5191a57c976418faa32e917653"}})"},
        {60000, 1747385641636LL,
         R"({"id":11,"method":"userDataStream.subscribe.signature","params":{"apiKey":"vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A",)"
         R"("recvWindow":60000,"timestamp":1747385641636,"signature":"3345cd0e553e8e0f2b143972359b622bf21c7564ecb2b94f9981cf8b385d8afe"}})"},
        {0, 0,
         R"({"id":11,"method":"userDataStream.subscribe.signature","params":{"apiKey":"vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A",)"
         R"("timestamp":0,"signature":"e3a69706338216aa3c660530dd1fd2df13a3eceb91452be5ce6b8e879a891ee0"}})"},
        {0, 9999999999999LL,
         R"({"id":11,"method":"userDataStream.subscribe.signature","params":{"apiKey":"vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A",)"
         R"("timestamp":9999999999999,"signature":"504d4169864b0b77f3f858422d1510d99b1cc6bf1a589db1a5aa46a066bcb566"}})"},
    };
    for (const Case& c : cases) {
        const auto [err, req] = hy::build_subscribe_signature_request(*creds_, 11, token_at(c.ts), c.recv_window);
        ASSERT_EQ(err, WsApiSigningError::Ok) << "recvWindow " << c.recv_window << " ts " << c.ts;
        EXPECT_EQ(req.frame(), c.frame);
        EXPECT_EQ(req.request_id(), 11u);
        EXPECT_TRUE(verify_frame(req.frame(), kSecret).signature_ok);
    }
}

TEST_F(WsApiSigningTest, TheSubscribeSignatureRequestRefusesAnImpossibleRecvWindowAndRequestId) {
    EXPECT_EQ(hy::build_subscribe_signature_request(*creds_, 1, token_at(1), 60001).first, WsApiSigningError::BadRecvWindow);
    EXPECT_EQ(hy::build_subscribe_signature_request(*creds_, 1, token_at(1), 60000).first, WsApiSigningError::Ok);
    EXPECT_EQ(hy::build_subscribe_signature_request(*creds_, 1, token_at(1), 1).first, WsApiSigningError::Ok);
    EXPECT_EQ(hy::build_subscribe_signature_request(*creds_, hy::kMaxWsApiRequestId + 1, token_at(1)).first, WsApiSigningError::BadRequestId);
    EXPECT_EQ(hy::build_subscribe_signature_request(*creds_, hy::kMaxWsApiRequestId, token_at(1)).first, WsApiSigningError::Ok);
    EXPECT_EQ(hy::build_subscribe_signature_request(*creds_, 0, token_at(1)).first, WsApiSigningError::Ok) << "id 0 is an id";
}

TEST_F(WsApiSigningTest, TheTimestampComesFromTheTokenAndIsRangeChecked) {
    EXPECT_EQ(hy::build_subscribe_signature_request(*creds_, 1, token_at(-1)).first, WsApiSigningError::TimestampOutOfRange);
    EXPECT_EQ(hy::build_subscribe_signature_request(*creds_, 1, token_at(10'000'000'000'000LL)).first, WsApiSigningError::TimestampOutOfRange);
    EXPECT_EQ(hy::build_subscribe_signature_request(*creds_, 1, token_at(9'999'999'999'999LL)).first, WsApiSigningError::Ok);
    EXPECT_EQ(hy::build_subscribe_signature_request(*creds_, 1, token_at(0)).first, WsApiSigningError::Ok);
}

TEST_F(WsApiSigningTest, ABuilderOnAnotherThreadCannotSign) {
    // The credentials are pinned to the thread that bound them: every other thread fails closed, and says why.
    WsApiSigningError err = WsApiSigningError::Ok;
    std::thread t([&] { err = hy::build_subscribe_signature_request(*creds_, 1, token_at(1747385641636LL)).first; });
    t.join();
    EXPECT_EQ(err, WsApiSigningError::ApiKeyUnavailable);
}

// --- tamper: one changed byte must fail the independent verifier ---

TEST_F(WsApiSigningTest, AnyChangedByteOfASignedFrameFailsVerification) {
    const auto [err, req] = sign("order.place", 5, {text("symbol", "BTCUSDT"), text("side", "BUY"), number("recvWindow", "5000")}, 1645423376532LL);
    ASSERT_EQ(err, WsApiSigningError::Ok);
    const std::string frame(req.frame());
    ASSERT_TRUE(verify_frame(frame, kSecret).signature_ok);

    const std::size_t params_at = frame.find("\"params\":{") + std::string_view("\"params\":{").size();
    ASSERT_NE(params_at, std::string::npos);
    std::size_t changed_and_still_json = 0;
    for (std::size_t pos = params_at; pos + 2 < frame.size(); ++pos) {
        for (const char replacement : {'0', 'a', 'Z', '1'}) {
            if (frame[pos] == replacement) continue;
            std::string tampered = frame;
            tampered[pos] = replacement;
            const Verified v = verify_frame(tampered, kSecret);
            // A change either breaks the JSON or breaks the signature -- never both fine.
            EXPECT_FALSE(v.json_ok && v.signature_ok) << "position " << pos << " -> " << replacement << ": " << tampered;
            if (v.json_ok) ++changed_and_still_json;
        }
    }
    EXPECT_GT(changed_and_still_json, 100u) << "the sweep must include many edits that leave valid JSON, or it proves nothing";
}

TEST_F(WsApiSigningTest, AChangedSecretFailsVerification) {
    const auto [err, req] = hy::build_subscribe_signature_request(*creds_, 1, token_at(1747385641636LL));
    ASSERT_EQ(err, WsApiSigningError::Ok);
    EXPECT_TRUE(verify_frame(req.frame(), kSecret).signature_ok);
    std::string wrong(kSecret);
    wrong[0] = wrong[0] == 'N' ? 'M' : 'N';
    EXPECT_FALSE(verify_frame(req.frame(), wrong).signature_ok);
}

// --- the parameter rules ---

TEST_F(WsApiSigningTest, ReservedAndMalformedParameterNamesAreRefused) {
    EXPECT_EQ(sign("m", 1, {text("apiKey", "x")}, 1).first, WsApiSigningError::ReservedParamName);
    EXPECT_EQ(sign("m", 1, {number("timestamp", "1")}, 1).first, WsApiSigningError::ReservedParamName);
    EXPECT_EQ(sign("m", 1, {text("signature", "x")}, 1).first, WsApiSigningError::ReservedParamName);
    // a name must be a lower-case letter followed by letters and digits
    for (const std::string_view bad : {std::string_view(""), std::string_view("Symbol"), std::string_view("1abc"), std::string_view("a b"),
                                        std::string_view("a-b"), std::string_view("a_b"), std::string_view("a.b"), std::string_view("a=b"),
                                        std::string_view("a&b"), std::string_view("a\"b"), std::string_view("a\\b"), std::string_view("é")}) {
        EXPECT_EQ(sign("m", 1, {text(bad, "x")}, 1).first, WsApiSigningError::BadParamName) << "[" << bad << "]";
    }
    EXPECT_EQ(sign("m", 1, {text(std::string(hy::kMaxWsApiNameLen + 1, 'a'), "x")}, 1).first, WsApiSigningError::BadParamName);
    EXPECT_EQ(sign("m", 1, {text(std::string(hy::kMaxWsApiNameLen, 'a'), "x")}, 1).first, WsApiSigningError::Ok);
    EXPECT_EQ(sign("m", 1, {text("a", "x"), text("b", "y"), text("a", "z")}, 1).first, WsApiSigningError::DuplicateParam);
    EXPECT_EQ(sign("m", 1, {text("a", "x"), number("a", "1")}, 1).first, WsApiSigningError::DuplicateParam);
}

TEST_F(WsApiSigningTest, AMethodNameIsASafeToken) {
    EXPECT_EQ(sign("order.place", 1, {}, 1).first, WsApiSigningError::Ok);
    EXPECT_EQ(sign("v3/order.place", 1, {}, 1).first, WsApiSigningError::Ok);
    EXPECT_EQ(sign("userDataStream.subscribe.signature", 1, {}, 1).first, WsApiSigningError::Ok);
    for (const std::string_view bad : {std::string_view(""), std::string_view("a b"), std::string_view("a\"b"), std::string_view("a\\b"),
                                        std::string_view("a,b"), std::string_view("a:b"), std::string_view("é"), std::string_view("a\nb")}) {
        EXPECT_EQ(sign(bad, 1, {}, 1).first, WsApiSigningError::BadMethodName) << "[" << bad << "]";
    }
    EXPECT_EQ(sign(std::string(hy::kMaxWsApiMethodLen + 1, 'a'), 1, {}, 1).first, WsApiSigningError::BadMethodName);
    EXPECT_EQ(sign(std::string(hy::kMaxWsApiMethodLen, 'a'), 1, {}, 1).first, WsApiSigningError::Ok);
}

TEST_F(WsApiSigningTest, ATextValueThatJsonWouldHaveToEscapeOrThatBlursThePayloadIsRefusedNotEscaped) {
    EXPECT_EQ(sign("m", 1, {text("a", "x\"y")}, 1).first, WsApiSigningError::UnsafeParamValue);
    EXPECT_EQ(sign("m", 1, {text("a", "x\\y")}, 1).first, WsApiSigningError::UnsafeParamValue);
    EXPECT_EQ(sign("m", 1, {text("a", "x&y=z")}, 1).first, WsApiSigningError::UnsafeParamValue);
    EXPECT_EQ(sign("m", 1, {text("a", "x&y")}, 1).first, WsApiSigningError::UnsafeParamValue);
    EXPECT_EQ(sign("m", 1, {text("a", "x=y")}, 1).first, WsApiSigningError::UnsafeParamValue);
    for (int c = 0; c < 0x20; ++c) {
        const std::string v = std::string("x") + static_cast<char>(c) + "y";
        EXPECT_EQ(sign("m", 1, {text("a", v)}, 1).first, WsApiSigningError::UnsafeParamValue) << "control character " << c;
    }
    EXPECT_EQ(sign("m", 1, {text("a", "x\x7Fy")}, 1).first, WsApiSigningError::UnsafeParamValue);
    // invalid UTF-8 would make the frame invalid JSON
    for (const std::string_view bad : {std::string_view("\xC0\x80"), std::string_view("\xC1\xBF"), std::string_view("\xE0\x80\x80"),
                                        std::string_view("\xED\xA0\x80"), std::string_view("\xF4\x90\x80\x80"), std::string_view("\xF5\x80\x80\x80"),
                                        std::string_view("\x80"), std::string_view("\xFF"), std::string_view("\xC3"), std::string_view("\xE6\x97"),
                                        std::string_view("\xF0\x9F\x98")}) {
        EXPECT_EQ(sign("m", 1, {text("a", bad)}, 1).first, WsApiSigningError::UnsafeParamValue) << "bytes starting " << static_cast<int>(static_cast<unsigned char>(bad[0]));
    }
    // ... and well-formed multi-byte text, spaces and ordinary punctuation are fine
    for (const std::string_view good : {std::string_view("\xC3\xA9"), std::string_view("\xE6\x97\xA5\xE6\x9C\xAC"), std::string_view("\xF0\x9F\x98\x80"),
                                         std::string_view("\xEF\xBF\xBD"), std::string_view("a b"), std::string_view("!#$%()*+,-./:;<>?@[]^_{|}~"),
                                         std::string_view("")}) {
        EXPECT_EQ(sign("m", 1, {text("a", good)}, 1).first, WsApiSigningError::Ok) << "[" << good << "]";
    }
    EXPECT_EQ(sign("m", 1, {text("a", std::string(hy::kMaxWsApiValueLen, 'x'))}, 1).first, WsApiSigningError::Ok);
    EXPECT_EQ(sign("m", 1, {text("a", std::string(hy::kMaxWsApiValueLen + 1, 'x'))}, 1).first, WsApiSigningError::UnsafeParamValue);
}

TEST_F(WsApiSigningTest, AnUnknownParameterKindIsRefused) {
    const WsApiParam odd{"a", "1", static_cast<WsApiParamKind>(7)};
    EXPECT_EQ(hy::detail::build_ws_api_signed_request(*creds_, "m", 1, std::span<const WsApiParam>(&odd, 1), token_at(1)).first,
              WsApiSigningError::UnsafeParamValue);
}

TEST_F(WsApiSigningTest, ANumberValueIsDigitsWithAnOptionalShortFractionAndNothingElse) {
    for (const std::string_view good : {std::string_view("0"), std::string_view("5"), std::string_view("5000"), std::string_view("60000"),
                                         std::string_view("6000.346"), std::string_view("0.5"), std::string_view("0.000000001"),
                                         std::string_view("1645423376532"), std::string_view("9223372036854775807"), std::string_view("9999999999999999999"),
                                         std::string_view("10.123456789"), std::string_view("1234567890123456.123") /* 20 characters */}) {
        EXPECT_EQ(sign("m", 1, {number("a", good)}, 1).first, WsApiSigningError::Ok) << good;
    }
    for (const std::string_view bad : {std::string_view(""), std::string_view("-1"), std::string_view("+1"), std::string_view("01"), std::string_view("00"),
                                        std::string_view(".5"), std::string_view("5."), std::string_view("1e3"), std::string_view("1E3"), std::string_view("1.2.3"),
                                        std::string_view("1,5"), std::string_view(" 1"), std::string_view("1 "), std::string_view("0x10"), std::string_view("abc"),
                                        std::string_view("NaN"), std::string_view("1.0000000001"), std::string_view("10000000000000000000"),
                                        std::string_view("1\n")}) {
        EXPECT_EQ(sign("m", 1, {number("a", bad)}, 1).first, WsApiSigningError::BadNumberValue) << "[" << bad << "]";
    }
}

TEST_F(WsApiSigningTest, TheParameterCountAndThePayloadSizeAreBounded) {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < hy::kMaxWsApiParams + 1; ++i) names.push_back("p" + std::to_string(i));
    std::vector<WsApiParam> params;
    for (std::size_t i = 0; i < hy::kMaxWsApiParams; ++i) params.push_back(text(names[i], "v"));
    EXPECT_EQ(sign("m", 1, params, 1).first, WsApiSigningError::Ok) << "exactly kMaxWsApiParams";
    params.push_back(text(names[hy::kMaxWsApiParams], "v"));
    EXPECT_EQ(sign("m", 1, params, 1).first, WsApiSigningError::TooManyParams);

    // 16 parameters of 100 bytes each overflow the 1 KiB signing buffer; 5 of 150 bytes do not.
    std::vector<std::string> values(hy::kMaxWsApiParams, std::string(100, 'v'));
    params.clear();
    for (std::size_t i = 0; i < hy::kMaxWsApiParams; ++i) params.push_back(text(names[i], values[i]));
    EXPECT_EQ(sign("m", 1, params, 1).first, WsApiSigningError::PayloadTooLarge);
    params.resize(5);
    const std::string long_value(150, 'v');
    for (auto& p : params) p.value = long_value;
    const auto [err, req] = sign("m", 1, params, 1);
    ASSERT_EQ(err, WsApiSigningError::Ok);
    EXPECT_LE(req.frame().size(), hy::kMaxWsApiFrameBytes);
    EXPECT_TRUE(verify_frame(req.frame(), kSecret).signature_ok);
}

TEST_F(WsApiSigningTest, ParametersAreSortedByByteOrderOfTheirNamesWhateverOrderTheCallerGaveThem) {
    const auto [err, a] = sign("m", 1, {text("zeta", "1"), text("alpha", "2"), text("timeInForce", "GTC")}, 7);
    ASSERT_EQ(err, WsApiSigningError::Ok);
    const auto [err2, b] = sign("m", 1, {text("timeInForce", "GTC"), text("alpha", "2"), text("zeta", "1")}, 7);
    ASSERT_EQ(err2, WsApiSigningError::Ok);
    EXPECT_EQ(a.frame(), b.frame()) << "the same parameters, however they were listed, give the same frame";
    const Verified v = verify_frame(a.frame(), kSecret);
    ASSERT_TRUE(v.signature_ok);
    ASSERT_EQ(v.params.size(), 5u);
    EXPECT_EQ(v.params[0].first, "alpha");
    EXPECT_EQ(v.params[1].first, "apiKey");
    EXPECT_EQ(v.params[2].first, "timeInForce") << "'I' sorts before 's' in byte order, so timeInForce precedes timestamp";
    EXPECT_EQ(v.params[3].first, "timestamp");
    EXPECT_EQ(v.params[4].first, "zeta");
}

// --- signed bytes == sent bytes, over random parameter sets ---

TEST_F(WsApiSigningTest, RandomParameterSetsAlwaysVerifyAndComeBackAsGiven) {
    std::mt19937_64 rng(0xC0FFEE);
    const auto rnd = [&](std::uint64_t n) { return static_cast<std::size_t>(rng() % n); };
    const std::vector<std::string> fragments = {"a", "Z", "0", "9", " ", "-", "_", ".", ":", "/", "~", "!", "#", "$", "%", "(", ")", "*", "+", ",",
                                                ";", "<", ">", "?", "@", "[", "]", "^", "{", "|", "}", "\xC3\xA9", "\xE6\x97\xA5\xE6\x9C\xAC",
                                                "\xEF\xBC\x91\xEF\xBC\x92", "\xF0\x9F\x98\x80", "\xEF\xBF\xBD"};
    const std::string name_head = "abcdefghijklmnopqrstuvwxyz";
    const std::string name_tail = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    for (int round = 0; round < 400; ++round) {
        const std::size_t count = rnd(hy::kMaxWsApiParams + 1);
        std::vector<std::string> names;
        std::vector<std::string> values;
        std::vector<WsApiParamKind> kinds;
        while (names.size() < count) {
            std::string name(1, name_head[rnd(name_head.size())]);
            const std::size_t extra = rnd(8);
            for (std::size_t i = 0; i < extra; ++i) name.push_back(name_tail[rnd(name_tail.size())]);
            if (hy::detail::is_reserved_ws_api_param_name(name) || std::find(names.begin(), names.end(), name) != names.end()) continue;
            std::string value;
            const bool is_number = rnd(3) == 0;
            if (is_number) {
                // digits with an optional fraction of 1..9 digits, 20 characters at most (the grammar of is_valid_ws_api_number)
                const std::size_t frac = rnd(2) == 0 ? 0 : 1 + rnd(9);
                const std::size_t max_int = std::min<std::size_t>(19, hy::kMaxWsApiNumberLen - (frac == 0 ? 0 : frac + 1));
                if (rnd(4) == 0) {
                    value = "0";
                } else {
                    value = std::string(1, static_cast<char>('1' + rnd(9)));
                    const std::size_t more = rnd(max_int);  // 0 .. max_int - 1 more digits
                    for (std::size_t i = 0; i < more; ++i) value.push_back(static_cast<char>('0' + rnd(10)));
                }
                if (frac != 0) {
                    value.push_back('.');
                    for (std::size_t i = 0; i < frac; ++i) value.push_back(static_cast<char>('0' + rnd(10)));
                }
            } else {
                const std::size_t pieces = rnd(10);
                for (std::size_t i = 0; i < pieces; ++i) value += fragments[rnd(fragments.size())];
            }
            names.push_back(name);
            values.push_back(value);
            kinds.push_back(is_number ? WsApiParamKind::Number : WsApiParamKind::Text);
        }
        std::vector<WsApiParam> params;
        for (std::size_t i = 0; i < count; ++i) params.push_back({names[i], values[i], kinds[i]});
        const std::int64_t ts = static_cast<std::int64_t>(rng() % 10'000'000'000'000ULL);
        const std::uint64_t id = rng() % (hy::kMaxWsApiRequestId + 1);

        const auto [err, req] = sign("order.place", id, params, ts);
        ASSERT_EQ(err, WsApiSigningError::Ok) << "round " << round;
        const Verified v = verify_frame(req.frame(), kSecret);
        ASSERT_TRUE(v.json_ok) << req.frame();
        ASSERT_TRUE(v.signature_ok) << "round " << round << ": " << req.frame();
        EXPECT_EQ(v.id, id);
        EXPECT_EQ(v.method, "order.place");
        // the server sees exactly the given parameters plus apiKey and timestamp
        auto got = v.params;
        std::sort(got.begin(), got.end());
        std::vector<std::pair<std::string, std::string>> want;
        for (std::size_t i = 0; i < count; ++i) want.emplace_back(names[i], values[i]);
        want.emplace_back("apiKey", std::string(kApiKey));
        want.emplace_back("timestamp", std::to_string(ts));
        std::sort(want.begin(), want.end());
        ASSERT_EQ(got, want) << "round " << round;
        EXPECT_LE(req.frame().size(), hy::kMaxWsApiFrameBytes);
    }
}

// --- the unsigned requests ---

TEST(WsApiUnsignedRequests, TheirFramesAreExactAndValidJson) {
    {
        const auto [err, req] = hy::build_session_subscriptions_request(5);
        ASSERT_EQ(err, WsApiSigningError::Ok);
        EXPECT_EQ(req.frame(), R"({"id":5,"method":"session.subscriptions","params":{}})");
        EXPECT_EQ(req.request_id(), 5u);
    }
    {
        const auto [err, req] = hy::build_user_data_unsubscribe_request(6, 3);
        ASSERT_EQ(err, WsApiSigningError::Ok);
        EXPECT_EQ(req.frame(), R"({"id":6,"method":"userDataStream.unsubscribe","params":{"subscriptionId":3}})");
    }
    {
        const auto [err, req] = hy::build_user_data_unsubscribe_all_request(7);
        ASSERT_EQ(err, WsApiSigningError::Ok);
        EXPECT_EQ(req.frame(), R"({"id":7,"method":"userDataStream.unsubscribe"})");
    }
    {
        const auto [err, req] = hy::build_user_data_unsubscribe_request(hy::kMaxWsApiRequestId, 4294967295U);
        ASSERT_EQ(err, WsApiSigningError::Ok);
        EXPECT_EQ(req.frame(), R"({"id":9007199254740991,"method":"userDataStream.unsubscribe","params":{"subscriptionId":4294967295}})");
        simdjson::dom::parser p;
        const simdjson::padded_string padded(req.frame());
        simdjson::dom::element root;
        EXPECT_EQ(p.parse(padded).get(root), simdjson::SUCCESS);
    }
}

TEST(WsApiUnsignedRequests, AnImpossibleRequestIdIsRefused) {
    EXPECT_EQ(hy::build_session_subscriptions_request(hy::kMaxWsApiRequestId + 1).first, WsApiSigningError::BadRequestId);
    EXPECT_EQ(hy::build_user_data_unsubscribe_request(hy::kMaxWsApiRequestId + 1, 1).first, WsApiSigningError::BadRequestId);
    EXPECT_EQ(hy::build_user_data_unsubscribe_all_request(hy::kMaxWsApiRequestId + 1).first, WsApiSigningError::BadRequestId);
    EXPECT_EQ(hy::build_session_subscriptions_request(0).first, WsApiSigningError::Ok);
}

// --- types and costs ---

TEST(WsApiRequestTypes, ARequestCanOnlyBeObtainedFromABuilder) {
    EXPECT_FALSE(std::is_default_constructible_v<WsApiSignedRequest>);
    EXPECT_FALSE(std::is_default_constructible_v<WsApiUnsignedRequest>);
    EXPECT_FALSE((std::is_constructible_v<WsApiSignedRequest, std::string_view>));
    EXPECT_FALSE((std::is_constructible_v<WsApiUnsignedRequest, std::string_view>));
    EXPECT_TRUE(std::is_copy_constructible_v<WsApiSignedRequest>) << "a session moves requests around";
}

TEST(WsApiRequestCosts, EveryRequestKindWeighsTwoAndAnUnknownKindIsRefused) {
    // blueprint 5.7 P-26: subscribe, unsubscribe, session.subscriptions, subscribe.signature and connecting all cost 2.
    for (const WsApiRequestKind kind : {WsApiRequestKind::Connect, WsApiRequestKind::SubscribeSignature, WsApiRequestKind::SessionSubscriptions,
                                        WsApiRequestKind::UserDataUnsubscribe}) {
        EXPECT_EQ(hy::ws_api_weight(kind), 2u) << static_cast<int>(kind);
    }
    EXPECT_EQ(hy::ws_api_weight(static_cast<WsApiRequestKind>(hy::kWsApiRequestKindCount)), std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(hy::ws_api_weight(static_cast<WsApiRequestKind>(255)), std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(hy::ws_api_method_name(WsApiRequestKind::SubscribeSignature), "userDataStream.subscribe.signature");
    EXPECT_EQ(hy::ws_api_method_name(WsApiRequestKind::SessionSubscriptions), "session.subscriptions");
    EXPECT_EQ(hy::ws_api_method_name(WsApiRequestKind::UserDataUnsubscribe), "userDataStream.unsubscribe");
    EXPECT_EQ(hy::ws_api_method_name(WsApiRequestKind::Connect), "") << "connecting is not a method";
    EXPECT_EQ(hy::ws_api_method_name(static_cast<WsApiRequestKind>(77)), "");
}

TEST(WsApiRequestCosts, TheWeightIsReservedOnTheReconciliationLaneThroughTheExplicitReservation) {
    hy::SpotRateLimitTracker tracker;
    ASSERT_TRUE(tracker.configure(6000, 500, 61000, 5000, 50, 5));
    for (const WsApiRequestKind kind : {WsApiRequestKind::Connect, WsApiRequestKind::SubscribeSignature, WsApiRequestKind::SessionSubscriptions,
                                        WsApiRequestKind::UserDataUnsubscribe}) {
        const auto permit = tracker.reserve_weight_only_explicit(hy::RateLimitLane::Reconciliation, hy::ws_api_weight(kind));
        ASSERT_TRUE(permit.has_value()) << static_cast<int>(kind);
        EXPECT_EQ(permit->lane(), hy::RateLimitLane::Reconciliation);
        EXPECT_EQ(permit->weight(), 2u);
        EXPECT_EQ(permit->basis(), hy::RateLimitPermit::Basis::Reserved);
    }
    // an unknown kind asks for UINT32_MAX, which no budget grants
    EXPECT_FALSE(tracker.reserve_weight_only_explicit(hy::RateLimitLane::Reconciliation, hy::ws_api_weight(static_cast<WsApiRequestKind>(200))).has_value());
}

TEST(WsApiRequestCosts, ALaneWithoutBudgetRefusesTheReservation) {
    hy::SpotRateLimitTracker tracker;
    // 100 weight, no margin: the Reconciliation lane gets 10% = 10, five requests of weight 2, then nothing.
    ASSERT_TRUE(tracker.configure(100, 0, 61000, 5000, 50, 5));
    int granted = 0;
    for (int i = 0; i < 10; ++i) {
        if (tracker.reserve_weight_only_explicit(hy::RateLimitLane::Reconciliation, hy::ws_api_weight(WsApiRequestKind::SubscribeSignature)).has_value()) ++granted;
    }
    EXPECT_EQ(granted, 5);
}
