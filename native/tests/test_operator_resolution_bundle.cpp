// SPDX-License-Identifier: proprietary
// test_operator_resolution_bundle.cpp -- D5 slice R3a: the evidence file (operator_resolution_bundle.hpp).
//
// What these pin: the digest is a real SHA-256 (known answers), the document is always valid JSON whatever bytes the exchange
// sent (every byte value survives a round trip through the escaping), the content is what the design promises (tool version,
// fence and read-lag parameters, every serverTime reading, the raw answers in the order they were made, the verdicts), and the
// bytes are a pure function of the inputs -- the same evidence always has the same digest, and any change in it changes the digest.

#include <gtest/gtest.h>
#include <hengyuan/operator_resolution_bundle.hpp>

#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace hy;
using namespace hy::operator_resolution;

namespace {

BundleHeader good_header() {
    BundleHeader h;
    h.tool_version = 1;
    h.client_order_id = "HY-1790000000000-7-0A1B";
    h.symbol = "BTCUSDT";
    h.environment = OperatorEnvironment::Testnet;
    h.account_uid_claimed = 123456789;
    h.account_uid_reported = 987654321;  // a different number: a value written under the other one's name shows
    // four different values, so that a parameter written under another one's name shows
    h.fence.recv_window_ms = 61'000;
    h.fence.clock_skew_ms = 62'000;
    h.fence.margin_ms = 63'000;
    h.fence.read_lag_ms = 124'000;
    h.lock_ref = ExchangeMs{1'790'000'000'000};
    h.read_ref = ExchangeMs{1'790'000'270'000};
    h.frame_anchor = ExchangeMs{1'789'999'940'000};
    h.range_start = ExchangeMs{1'789'999'700'000};
    h.range_end = ExchangeMs{1'790'000'270'000};
    h.time_readings = {{"lock", 1'790'000'000'000}, {"wait", 1'790'000'030'000}, {"read", 1'790'000'270'000}};
    h.account_summary = "USDT free 1000.0";
    return h;
}

BundleVerdicts good_verdicts() {
    BundleVerdicts v;
    v.point_query = PointQueryVerdict::NoSuchOrder;
    v.all_orders = {PageVerdict::Clear, PageVerdict::Clear};
    v.open_orders = PageVerdict::Clear;
    return v;
}

std::vector<BundleCall> good_calls() {
    return {{"order", "symbol=BTCUSDT&origClientOrderId=HY-1790000000000-7-0A1B", true, 400, "{\"code\":-2013,\"msg\":\"Order does not exist.\"}"},
            {"allOrders", "symbol=BTCUSDT&startTime=1&endTime=2&limit=1000", true, 200, "[]"},
            {"openOrders", "", true, 200, "[]"}};
}

// The document, parsed (simdjson keeps its views into the padded copy, so both live in the struct).
struct Parsed {
    simdjson::padded_string text;
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    bool ok{false};

    explicit Parsed(const std::string& s) : text(s) { ok = parser.parse(text).get(root) == simdjson::SUCCESS; }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
};

template <class Container>
std::string str(const Container& e, std::string_view key) {
    std::string_view v;
    EXPECT_EQ(e[key].get(v), simdjson::SUCCESS) << key;
    return std::string(v);
}

template <class Container>
std::int64_t num(const Container& e, std::string_view key) {
    std::int64_t v = 0;
    EXPECT_EQ(e[key].get(v), simdjson::SUCCESS) << key;
    return v;
}

simdjson::dom::element at(simdjson::dom::array a, std::size_t i) {
    simdjson::dom::element e;
    EXPECT_EQ(a.at(i).get(e), simdjson::SUCCESS) << i;
    return e;
}

simdjson::dom::object object_at(const simdjson::dom::element& root, std::string_view key) {
    simdjson::dom::object o;
    EXPECT_EQ(root[key].get(o), simdjson::SUCCESS) << key;
    return o;
}

// The Latin-1 view of a JSON string: every code point 0..255 is one byte.
bool latin1_bytes(std::string_view utf8, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < utf8.size();) {
        const auto b0 = static_cast<unsigned char>(utf8[i]);
        if (b0 < 0x80) {
            out.push_back(static_cast<char>(b0));
            ++i;
        } else if ((b0 == 0xC2 || b0 == 0xC3) && i + 1 < utf8.size()) {
            const auto b1 = static_cast<unsigned char>(utf8[i + 1]);
            if ((b1 & 0xC0) != 0x80) return false;
            out.push_back(static_cast<char>(((b0 & 0x03) << 6) | (b1 & 0x3F)));
            i += 2;
        } else {
            return false;  // a code point above 255 cannot have come from one byte
        }
    }
    return true;
}

}  // namespace

// --- the digest ---------------------------------------------------------------------------------------------------------------------

TEST(OperatorResolutionBundle, Sha256HexIsASha256) {
    EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string d = sha256_hex(std::string(1000, 'a'));
    EXPECT_EQ(d.size(), kEvidenceDigestHexChars);
    EXPECT_TRUE(is_lower_hex(d, kEvidenceDigestHexChars));
    EXPECT_NE(d, sha256_hex(std::string(1001, 'a')));
}

// --- the document ------------------------------------------------------------------------------------------------------------------

TEST(OperatorResolutionBundle, TheDocumentIsValidJsonWithWhatTheDesignPromises) {
    const std::string bundle = build_evidence_bundle(good_header(), good_calls(), good_verdicts());
    const Parsed p(bundle);
    ASSERT_TRUE(p.ok) << bundle;
    EXPECT_EQ(str(p.root, "format"), "hy-operator-evidence/1");
    EXPECT_EQ(num(p.root, "tool_version"), 1);
    EXPECT_EQ(str(p.root, "client_order_id"), "HY-1790000000000-7-0A1B");
    EXPECT_EQ(str(p.root, "symbol"), "BTCUSDT");
    EXPECT_EQ(str(p.root, "environment"), "TESTNET");
    EXPECT_EQ(num(p.root, "account_uid_claimed"), 123456789);
    EXPECT_EQ(num(p.root, "account_uid_reported"), 987654321);
    EXPECT_EQ(num(p.root, "lock_ref_ms"), 1'790'000'000'000);
    EXPECT_EQ(num(p.root, "read_ref_ms"), 1'790'000'270'000);
    EXPECT_EQ(num(p.root, "frame_anchor_ms"), 1'789'999'940'000);
    EXPECT_EQ(str(p.root, "account_summary"), "USDT free 1000.0");

    simdjson::dom::object fence;
    ASSERT_EQ(p.root["fence"].get(fence), simdjson::SUCCESS);
    EXPECT_EQ(num(fence, "recv_window_ms"), 61'000);
    EXPECT_EQ(num(fence, "clock_skew_ms"), 62'000);
    EXPECT_EQ(num(fence, "margin_ms"), 63'000);
    EXPECT_EQ(num(fence, "read_lag_ms"), 124'000);

    simdjson::dom::object range;
    ASSERT_EQ(p.root["all_orders_range"].get(range), simdjson::SUCCESS);
    EXPECT_EQ(num(range, "start_ms"), 1'789'999'700'000);
    EXPECT_EQ(num(range, "end_ms"), 1'790'000'270'000);

    simdjson::dom::array readings;
    ASSERT_EQ(p.root["server_time_readings"].get(readings), simdjson::SUCCESS);
    ASSERT_EQ(readings.size(), 3u);
    const char* roles[] = {"lock", "wait", "read"};
    std::size_t i = 0;
    for (const simdjson::dom::element r : readings) {
        EXPECT_EQ(str(r, "role"), roles[i]);
        ++i;
    }
    EXPECT_EQ(num(at(readings, 2), "ms"), 1'790'000'270'000);

    simdjson::dom::object verdicts;
    ASSERT_EQ(p.root["verdicts"].get(verdicts), simdjson::SUCCESS);
    EXPECT_EQ(str(verdicts, "point_query"), "NoSuchOrder");
    EXPECT_EQ(str(verdicts, "open_orders"), "Clear");
    simdjson::dom::array pages;
    ASSERT_EQ(verdicts["all_orders"].get(pages), simdjson::SUCCESS);
    EXPECT_EQ(pages.size(), 2u);

    simdjson::dom::array calls;
    ASSERT_EQ(p.root["calls"].get(calls), simdjson::SUCCESS);
    ASSERT_EQ(calls.size(), 3u);
    const char* names[] = {"order", "allOrders", "openOrders"};
    i = 0;
    for (const simdjson::dom::element c : calls) {
        EXPECT_EQ(str(c, "name"), names[i]) << "in the order the calls were made";
        ++i;
    }
    EXPECT_EQ(num(at(calls, 0), "http_status"), 400);
    EXPECT_EQ(str(at(calls, 0), "body"), "{\"code\":-2013,\"msg\":\"Order does not exist.\"}") << "the raw answer, as received";
    bool ok = false;
    ASSERT_EQ(at(calls, 0)["transport_ok"].get(ok), simdjson::SUCCESS);
    EXPECT_TRUE(ok);
}

// The whole document, byte for byte: the operator types a piece of its digest, so the format is part of the contract and a change
// to it (a key, a separator, the trailing newline) must be a deliberate one that updates this text and the format name.
TEST(OperatorResolutionBundle, TheDocumentIsExactlyTheseBytes) {
    const std::string golden =
        "{\n"
        "\"format\":\"hy-operator-evidence/1\",\n"
        "\"tool_version\":1,\n"
        "\"client_order_id\":\"HY-1790000000000-7-0A1B\",\n"
        "\"symbol\":\"BTCUSDT\",\n"
        "\"environment\":\"TESTNET\",\n"
        "\"account_uid_claimed\":123456789,\n"
        "\"account_uid_reported\":987654321,\n"
        "\"fence\":{\"recv_window_ms\":61000,\"clock_skew_ms\":62000,\"margin_ms\":63000,\"read_lag_ms\":124000},\n"
        "\"server_time_readings\":[{\"role\":\"lock\",\"ms\":1790000000000},{\"role\":\"wait\",\"ms\":1790000030000},"
        "{\"role\":\"read\",\"ms\":1790000270000}],\n"
        "\"lock_ref_ms\":1790000000000,\n"
        "\"read_ref_ms\":1790000270000,\n"
        "\"frame_anchor_ms\":1789999940000,\n"
        "\"all_orders_range\":{\"start_ms\":1789999700000,\"end_ms\":1790000270000},\n"
        "\"account_summary\":\"USDT free 1000.0\",\n"
        "\"verdicts\":{\"point_query\":\"NoSuchOrder\",\"all_orders\":[\"Clear\",\"Clear\"],\"open_orders\":\"Clear\"},\n"
        "\"calls\":[\n"
        "{\"name\":\"order\",\"params\":\"symbol=BTCUSDT&origClientOrderId=HY-1790000000000-7-0A1B\",\"transport_ok\":true,"
        "\"http_status\":400,\"body\":\"{\\\"code\\\":-2013,\\\"msg\\\":\\\"Order does not exist.\\\"}\"},\n"
        "{\"name\":\"allOrders\",\"params\":\"symbol=BTCUSDT&startTime=1&endTime=2&limit=1000\",\"transport_ok\":true,"
        "\"http_status\":200,\"body\":\"[]\"},\n"
        "{\"name\":\"openOrders\",\"params\":\"\",\"transport_ok\":true,\"http_status\":200,\"body\":\"[]\"}\n"
        "]\n"
        "}\n";
    EXPECT_EQ(build_evidence_bundle(good_header(), good_calls(), good_verdicts()), golden);

    BundleHeader no_anchor = good_header();
    no_anchor.frame_anchor.reset();
    const std::string without = build_evidence_bundle(no_anchor, good_calls(), good_verdicts());
    EXPECT_NE(without.find("\"frame_anchor_ms\":null,\n"), std::string::npos);

    BundleHeader no_uid = good_header();
    no_uid.account_uid_reported.reset();
    const std::string unreported = build_evidence_bundle(no_uid, good_calls(), good_verdicts());
    EXPECT_NE(unreported.find("\"account_uid_reported\":null,\n"), std::string::npos);
}

// One byte at a time against a literal oracle: printable ASCII is written as itself, the five escapes JSON names are those, and
// everything else is \u00xx in lower case.
TEST(OperatorResolutionBundle, EveryByteIsWrittenAsItsOneDocumentedForm) {
    const std::string open = "\"body\":\"<";
    for (int b = 0; b < 256; ++b) {
        std::vector<BundleCall> calls = good_calls();
        calls[0].body = std::string("<") + static_cast<char>(b) + ">";
        const std::string bundle = build_evidence_bundle(good_header(), calls, good_verdicts());
        const std::size_t start = bundle.find(open);
        ASSERT_NE(start, std::string::npos) << b;
        const std::size_t stop = bundle.find(">\"}", start);
        ASSERT_NE(stop, std::string::npos) << b;
        const std::string written = bundle.substr(start + open.size(), stop - (start + open.size()));
        std::string expected;
        if (b == '"') {
            expected = "\\\"";
        } else if (b == '\\') {
            expected = "\\\\";
        } else if (b == '\n') {
            expected = "\\n";
        } else if (b == '\r') {
            expected = "\\r";
        } else if (b == '\t') {
            expected = "\\t";
        } else if (b < 0x20 || b >= 0x7F) {
            const char* hex = "0123456789abcdef";
            expected = std::string("\\u00") + hex[b >> 4] + hex[b & 0x0F];
        } else {
            expected = std::string(1, static_cast<char>(b));
        }
        EXPECT_EQ(written, expected) << "byte " << b;
    }
}

TEST(OperatorResolutionBundle, ANullFrameAnchorIsWrittenAsNullNotAsZero) {
    BundleHeader h = good_header();
    h.frame_anchor.reset();
    const Parsed p(build_evidence_bundle(h, good_calls(), good_verdicts()));
    ASSERT_TRUE(p.ok);
    simdjson::dom::element anchor;
    ASSERT_EQ(p.root["frame_anchor_ms"].get(anchor), simdjson::SUCCESS);
    EXPECT_TRUE(anchor.is_null());
}

TEST(OperatorResolutionBundle, AnEmptyEvidenceSetIsStillAValidDocument) {
    BundleHeader h = good_header();
    h.time_readings.clear();
    h.account_summary = {};
    const BundleVerdicts none;
    const Parsed p(build_evidence_bundle(h, {}, none));
    ASSERT_TRUE(p.ok);
    simdjson::dom::array calls;
    ASSERT_EQ(p.root["calls"].get(calls), simdjson::SUCCESS);
    EXPECT_EQ(calls.size(), 0u);
    EXPECT_EQ(str(object_at(p.root, "verdicts"), "point_query"), "Unusable") << "the zero verdict is 'no evidence'";
    EXPECT_EQ(str(object_at(p.root, "verdicts"), "open_orders"), "Malformed");
    EXPECT_EQ(str(p.root, "account_summary"), "");
}

TEST(OperatorResolutionBundle, EveryByteValueSurvivesTheEscapingAndTheDocumentStaysValid) {
    std::string hostile;
    for (int b = 0; b < 256; ++b) hostile.push_back(static_cast<char>(b));
    hostile += "\"}],\"calls\":[{\"injected\":true}]//\\u0041\\";  // an attempt to break out of the string
    std::vector<BundleCall> calls = good_calls();
    calls[1].body = hostile;
    calls[1].params = hostile;
    BundleHeader h = good_header();
    h.account_summary = hostile;
    const std::string bundle = build_evidence_bundle(h, calls, good_verdicts());
    const Parsed p(bundle);
    ASSERT_TRUE(p.ok) << "a hostile body must not break the document";

    simdjson::dom::array parsed_calls;
    ASSERT_EQ(p.root["calls"].get(parsed_calls), simdjson::SUCCESS);
    EXPECT_EQ(parsed_calls.size(), 3u) << "and must not add a call";
    std::string back;
    ASSERT_TRUE(latin1_bytes(str(at(parsed_calls, 1), "body"), back));
    EXPECT_EQ(back, hostile) << "every byte comes back as it was";
    ASSERT_TRUE(latin1_bytes(str(at(parsed_calls, 1), "params"), back));
    EXPECT_EQ(back, hostile);
    ASSERT_TRUE(latin1_bytes(str(p.root, "account_summary"), back));
    EXPECT_EQ(back, hostile);
    EXPECT_FALSE(p.root["injected"].error() == simdjson::SUCCESS);

    // and in the text itself nothing but printable ASCII and the newlines the layout adds
    for (const char ch : bundle) {
        const auto c = static_cast<unsigned char>(ch);
        EXPECT_TRUE(c == '\n' || (c >= 0x20 && c < 0x7F)) << static_cast<int>(c);
    }
}

TEST(OperatorResolutionBundle, TheBytesArePureAndEveryInputChangesTheDigest) {
    const std::string a = build_evidence_bundle(good_header(), good_calls(), good_verdicts());
    EXPECT_EQ(a, build_evidence_bundle(good_header(), good_calls(), good_verdicts())) << "the same inputs, the same bytes";
    const std::string digest = sha256_hex(a);

    const auto differs = [&](const std::string& other, const char* what) { EXPECT_NE(sha256_hex(other), digest) << what; };
    {
        auto calls = good_calls();
        calls[0].body.back() = ' ';
        differs(build_evidence_bundle(good_header(), calls, good_verdicts()), "one byte of one raw answer");
    }
    {
        auto calls = good_calls();
        calls[2].http_status = 500;
        differs(build_evidence_bundle(good_header(), calls, good_verdicts()), "an HTTP status");
    }
    {
        auto calls = good_calls();
        calls[1].transport_ok = false;
        differs(build_evidence_bundle(good_header(), calls, good_verdicts()), "transport_ok");
    }
    {
        auto calls = good_calls();
        std::swap(calls[1], calls[2]);
        differs(build_evidence_bundle(good_header(), calls, good_verdicts()), "the order of the calls");
    }
    {
        BundleHeader h = good_header();
        h.read_ref = ExchangeMs{1'790'000'270'001};
        differs(build_evidence_bundle(h, good_calls(), good_verdicts()), "read_ref");
    }
    {
        BundleHeader h = good_header();
        h.fence.read_lag_ms = 120'001;
        differs(build_evidence_bundle(h, good_calls(), good_verdicts()), "a fence parameter");
    }
    {
        BundleHeader h = good_header();
        h.account_uid_claimed = 123456790;
        differs(build_evidence_bundle(h, good_calls(), good_verdicts()), "the claimed account uid");
    }
    {
        BundleHeader h = good_header();
        h.environment = OperatorEnvironment::Production;
        differs(build_evidence_bundle(h, good_calls(), good_verdicts()), "the environment");
    }
    {
        BundleHeader h = good_header();
        h.time_readings.pop_back();
        differs(build_evidence_bundle(h, good_calls(), good_verdicts()), "a serverTime reading");
    }
    {
        BundleVerdicts v = good_verdicts();
        v.all_orders[1] = PageVerdict::PossiblyTruncated;
        differs(build_evidence_bundle(good_header(), good_calls(), v), "a verdict");
    }
    {
        BundleHeader h = good_header();
        h.tool_version = 2;
        differs(build_evidence_bundle(h, good_calls(), good_verdicts()), "the tool version");
    }
}

TEST(OperatorResolutionBundle, VerdictNamesCoverEveryValueAndAnOutOfRangeOneIsUnknown) {
    // the names are what the evidence file and the operator's screen say: each value has its own, spelled as the enumerator
    EXPECT_EQ(verdict_name(PointQueryVerdict::Unusable), "Unusable");
    EXPECT_EQ(verdict_name(PointQueryVerdict::NoSuchOrder), "NoSuchOrder");
    EXPECT_EQ(verdict_name(PointQueryVerdict::OrderExists), "OrderExists");
    EXPECT_EQ(verdict_name(PointQueryVerdict::Archived), "Archived");
    EXPECT_EQ(verdict_name(PointQueryVerdict::OtherError), "OtherError");
    EXPECT_EQ(verdict_name(PageVerdict::Malformed), "Malformed");
    EXPECT_EQ(verdict_name(PageVerdict::Clear), "Clear");
    EXPECT_EQ(verdict_name(PageVerdict::Listed), "Listed");
    EXPECT_EQ(verdict_name(PageVerdict::PossiblyTruncated), "PossiblyTruncated");
    EXPECT_EQ(verdict_name(static_cast<PointQueryVerdict>(99)), "Unknown");
    EXPECT_EQ(verdict_name(static_cast<PageVerdict>(99)), "Unknown");
}
