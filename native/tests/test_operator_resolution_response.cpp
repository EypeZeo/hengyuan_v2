// SPDX-License-Identifier: proprietary
// test_operator_resolution_response.cpp -- D5 slice R2: reading the exchange's answers to the operator tool's evidence queries
// (operator_resolution_response.hpp). The rule under test is one-sided: only an exact, well-formed "HTTP 400 and error -2013"
// answer is evidence of absence, and only a well-formed page of order objects that does not name the order (and is not full)
// is a clear page. Everything else -- a truncated body, a duplicate key, a number where a string belongs, a proxy's HTML, an
// unexpected status -- must come out as "no evidence", so most of this file is hostile input.

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <hengyuan/operator_resolution_evidence.hpp>
#include <hengyuan/operator_resolution_response.hpp>
#include <hengyuan/order_lifecycle.hpp>

namespace {

using namespace hy::operator_resolution;

constexpr std::string_view kCoid = "HY-1790000000000-7-0042";
constexpr std::string_view kSymbol = "BTCUSDT";

const std::string kNoSuchOrder = R"({"code":-2013,"msg":"Order does not exist."})";

std::string error_body(std::string_view code) { return "{\"code\":" + std::string(code) + ",\"msg\":\"x\"}"; }

// A realistic order object (the field set of the official docs' allOrders example).
std::string order_json(std::string_view coid, std::string_view symbol = kSymbol, std::int64_t order_id = 1) {
    return "{\"symbol\":\"" + std::string(symbol) + "\",\"orderId\":" + std::to_string(order_id) +
           ",\"orderListId\":-1,\"clientOrderId\":\"" + std::string(coid) +
           "\",\"price\":\"0.1\",\"origQty\":\"1.0\",\"executedQty\":\"0.0\",\"cummulativeQuoteQty\":\"0.0\","
           "\"status\":\"NEW\",\"timeInForce\":\"GTC\",\"type\":\"LIMIT\",\"side\":\"BUY\",\"stopPrice\":\"0.00000000\","
           "\"icebergQty\":\"0.00000000\",\"time\":1499827319559,\"updateTime\":1499827319559,\"isWorking\":true,"
           "\"origQuoteOrderQty\":\"0.000000\",\"workingTime\":1499827319559,\"selfTradePreventionMode\":\"NONE\"}";
}

std::string other_coid(std::size_t i) { return "HY-1790000000001-" + std::to_string(i) + "-0042"; }

// A page of `n` orders that are not ours; `ours_at` (if < n) is replaced by ours.
std::string page(std::size_t n, std::size_t ours_at = static_cast<std::size_t>(-1), std::string_view symbol = kSymbol) {
    std::string out = "[";
    for (std::size_t i = 0; i < n; ++i) {
        if (i != 0) out += ',';
        out += order_json(i == ours_at ? kCoid : std::string_view(other_coid(i)), symbol, static_cast<std::int64_t>(i) + 1);
    }
    out += ']';
    return out;
}

// --- GET /api/v3/order -----------------------------------------------------------------------------------------------------------

TEST(OperatorResolutionResponsePoint, TheDocumentedErrorIsTheEvidenceOfAbsence) {
    EXPECT_EQ(classify_point_query(400, kNoSuchOrder), PointQueryVerdict::NoSuchOrder);
    // JSON is JSON: whitespace, key order, extra fields and a different message do not matter
    EXPECT_EQ(classify_point_query(400, "{\"msg\":\"Order does not exist.\",\"code\":-2013}"), PointQueryVerdict::NoSuchOrder);
    EXPECT_EQ(classify_point_query(400, "  {\n  \"code\" : -2013 ,\n  \"msg\" : \"Order does not exist.\"\n}\r\n"),
              PointQueryVerdict::NoSuchOrder);
    EXPECT_EQ(classify_point_query(400, "{\"code\":-2013}"), PointQueryVerdict::NoSuchOrder);
    EXPECT_EQ(classify_point_query(400, "{\"code\":-2013,\"msg\":\"Unknown order sent.\",\"extra\":{\"a\":[1,2,{\"b\":null}]}}"),
              PointQueryVerdict::NoSuchOrder);
    EXPECT_EQ(classify_point_query(400, "{\"code\":-2013,\"msg\":5}"), PointQueryVerdict::NoSuchOrder) << "msg is never read";
}

TEST(OperatorResolutionResponsePoint, TheStatusIsPartOfTheClaim) {
    for (const std::uint16_t status : {std::uint16_t{0}, std::uint16_t{200}, std::uint16_t{201}, std::uint16_t{204}, std::uint16_t{301},
                                       std::uint16_t{304}, std::uint16_t{399}, std::uint16_t{401}, std::uint16_t{403}, std::uint16_t{404},
                                       std::uint16_t{418}, std::uint16_t{429}, std::uint16_t{451}, std::uint16_t{499}, std::uint16_t{500},
                                       std::uint16_t{502}, std::uint16_t{503}, std::uint16_t{504}, std::uint16_t{599},
                                       std::uint16_t{65535}}) {
        EXPECT_EQ(classify_point_query(status, kNoSuchOrder), PointQueryVerdict::Unusable) << status;
    }
    EXPECT_EQ(classify_point_query(399, kNoSuchOrder), PointQueryVerdict::Unusable);
    EXPECT_EQ(classify_point_query(400, kNoSuchOrder), PointQueryVerdict::NoSuchOrder);
}

TEST(OperatorResolutionResponsePoint, AnArchivedOrderExisted) {
    EXPECT_EQ(classify_point_query(400, error_body("-2026")), PointQueryVerdict::Archived);
    EXPECT_EQ(classify_point_query(404, error_body("-2026")), PointQueryVerdict::Archived) << "any 4xx";
    EXPECT_EQ(classify_point_query(499, error_body("-2026")), PointQueryVerdict::Archived);
    EXPECT_EQ(classify_point_query(200, error_body("-2026")), PointQueryVerdict::Unusable) << "a success status with an error body";
    EXPECT_EQ(classify_point_query(500, error_body("-2026")), PointQueryVerdict::Unusable);
    EXPECT_EQ(classify_point_query(399, error_body("-2026")), PointQueryVerdict::Unusable) << "not a 4xx";
    EXPECT_EQ(classify_point_query(300, error_body("-1000")), PointQueryVerdict::Unusable);
    EXPECT_EQ(classify_point_query(399, error_body("-1000")), PointQueryVerdict::Unusable);
    EXPECT_EQ(classify_point_query(500, error_body("-1000")), PointQueryVerdict::Unusable);
    EXPECT_EQ(classify_point_query(400, error_body("-1000")), PointQueryVerdict::OtherError);
    EXPECT_EQ(classify_point_query(499, error_body("-1000")), PointQueryVerdict::OtherError);
    EXPECT_NE(classify_point_query(400, error_body("-2026")), PointQueryVerdict::NoSuchOrder)
        << "ORDER_ARCHIVED is not NO_SUCH_ORDER: it says the order was there";
}

TEST(OperatorResolutionResponsePoint, AnyOtherErrorSaysNothingAboutTheOrder) {
    for (const char* const code : {"-1021", "-1022", "-1003", "-1007", "-1013", "-1000", "-2011", "-2015", "-2014", "0", "1", "2013",
                                   "-20130", "-201", "-2012", "-2027", "9223372036854775807", "-9223372036854775808"}) {
        EXPECT_EQ(classify_point_query(400, error_body(code)), PointQueryVerdict::OtherError) << code;
        EXPECT_EQ(classify_point_query(429, error_body(code)), PointQueryVerdict::OtherError) << code;
    }
}

TEST(OperatorResolutionResponsePoint, AnExistingOrderIsNeverAbsent) {
    EXPECT_EQ(classify_point_query(200, order_json(kCoid)), PointQueryVerdict::OrderExists);
    EXPECT_EQ(classify_point_query(200, "{\"orderId\":1}"), PointQueryVerdict::OrderExists);
    EXPECT_EQ(classify_point_query(200, "{\"orderId\":\"1\"}"), PointQueryVerdict::Unusable) << "orderId must be a number";
    EXPECT_EQ(classify_point_query(200, "{\"orderId\":1,\"orderId\":2}"), PointQueryVerdict::Unusable);
    EXPECT_EQ(classify_point_query(200, "{}"), PointQueryVerdict::Unusable);
    EXPECT_EQ(classify_point_query(200, "[]"), PointQueryVerdict::Unusable);
    EXPECT_EQ(classify_point_query(200, ""), PointQueryVerdict::Unusable);
    EXPECT_EQ(classify_point_query(200, kNoSuchOrder), PointQueryVerdict::Unusable) << "a -2013 body under a 200 status contradicts itself";
}

TEST(OperatorResolutionResponsePoint, WhateverIsNotAnExactErrorObjectIsUnusable) {
    const std::string bom = "\xEF\xBB\xBF" + kNoSuchOrder;
    const std::string bad_utf8 = std::string("{\"code\":-2013,\"msg\":\"") + "\xFF\xFE" + "\"}";
    const std::string raw_nul = std::string("{\"code\":-2013,\"msg\":\"a") + std::string(1, '\0') + "b\"}";
    const std::string raw_newline = "{\"code\":-2013,\"msg\":\"a\nb\"}";
    const std::string deep = "{\"code\":-2013,\"x\":" + std::string(5000, '[') + std::string(5000, ']') + "}";
    const std::vector<std::string> bodies = {
        "",
        " ",
        "{",
        "}",
        "{\"code\":-2013",
        "{\"code\":-2013,",
        "{\"code\":-2013,\"msg\":\"x\"",
        "{\"code\":-2013,\"msg\":\"x\"}}",
        "null",
        "true",
        "-2013",
        "\"-2013\"",
        "[]",
        "[{\"code\":-2013}]",
        "{}",
        "{\"msg\":\"Order does not exist.\"}",
        "{\"code\":\"-2013\"}",
        "{\"code\":-2013.0}",
        "{\"code\":-2.013e3}",
        "{\"code\":-2013e0}",
        "{\"code\":null}",
        "{\"code\":true}",
        "{\"code\":[-2013]}",
        "{\"code\":{\"code\":-2013}}",
        "{\"code\":-2013,\"code\":-2013}",
        "{\"code\":-2013,\"code\":-1000}",
        "{\"code\":-1000,\"code\":-2013}",
        "{\"Code\":-2013}",
        "{\"code \":-2013}",
        "{\"cod\\u0065\":-2013,\"cod\\u0065\":-1}",
        "{'code':-2013}",
        "{code:-2013}",
        "{\"code\":-2013,}",
        "{\"code\":-2013} x",
        "{\"code\":-2013}{\"code\":-2013}",
        "{\"code\":-2013},",
        "{\"code\":+2013}",
        "{\"code\":--2013}",
        "{\"code\":-02013}",
        "{\"code\":-}",
        "{\"code\":0x7DD}",
        "{\"code\":1e400}",
        "{\"code\":9223372036854775808}",
        "{\"code\":-9223372036854775809}",
        "{\"code\":18446744073709551615}",
        "<html><body>400 Bad Request</body></html>",
        "Order does not exist.",
        bom,
        bad_utf8,
        raw_nul,
        raw_newline,
        deep,
    };
    for (const std::string& body : bodies) {
        EXPECT_EQ(classify_point_query(400, body), PointQueryVerdict::Unusable) << "[" << body.substr(0, 60) << "]";
        EXPECT_EQ(classify_point_query(404, body), PointQueryVerdict::Unusable) << "[" << body.substr(0, 60) << "]";
    }
    // by contrast, a well-formed error object with another integer code is a real answer that says nothing: OtherError
    EXPECT_EQ(classify_point_query(400, "{\"code\":9223372036854775807}"), PointQueryVerdict::OtherError);
    EXPECT_EQ(classify_point_query(400, "{\"code\":-1021,\"msg\":\"Timestamp outside of the recvWindow.\"}"),
              PointQueryVerdict::OtherError);
}

TEST(OperatorResolutionResponsePoint, EveryStrictPrefixOfTheEvidenceIsUnusable) {
    for (std::size_t len = 0; len < kNoSuchOrder.size(); ++len) {
        EXPECT_NE(classify_point_query(400, std::string_view(kNoSuchOrder).substr(0, len)), PointQueryVerdict::NoSuchOrder)
            << "prefix of " << len << " bytes";
    }
    EXPECT_EQ(classify_point_query(400, kNoSuchOrder), PointQueryVerdict::NoSuchOrder);
    // and the same for the body of an existing order and for the archived error
    const std::string archived = error_body("-2026");
    for (std::size_t len = 0; len < archived.size(); ++len) {
        EXPECT_NE(classify_point_query(400, std::string_view(archived).substr(0, len)), PointQueryVerdict::Archived) << len;
    }
}

TEST(OperatorResolutionResponsePoint, TheSizeCapIsExactAndAnOversizeBodyIsNotParsed) {
    ASSERT_EQ(kMaxEvidenceBodyBytes, 4u * 1024u * 1024u);
    std::string at_cap = R"({"code":-2013,"msg":")";
    const std::size_t tail = 2;  // "}
    at_cap.append(kMaxEvidenceBodyBytes - at_cap.size() - tail, 'A');
    at_cap += "\"}";
    ASSERT_EQ(at_cap.size(), kMaxEvidenceBodyBytes);
    EXPECT_EQ(classify_point_query(400, at_cap), PointQueryVerdict::NoSuchOrder);
    std::string over = at_cap;
    over.insert(over.size() - 2, "A");
    ASSERT_EQ(over.size(), kMaxEvidenceBodyBytes + 1);
    EXPECT_EQ(classify_point_query(400, over), PointQueryVerdict::Unusable) << "valid JSON, but over the cap";
}

TEST(OperatorResolutionResponsePoint, RandomBytesAreNeverEvidence) {
    std::mt19937_64 rng(0x0D5'0005'5EEDULL);
    for (int i = 0; i < 20'000; ++i) {
        std::string body(rng() % 200, '\0');
        for (char& c : body) c = static_cast<char>(rng());
        const PointQueryVerdict v = classify_point_query(400, body);
        ASSERT_TRUE(v == PointQueryVerdict::Unusable || v == PointQueryVerdict::OtherError) << i;
    }
}

TEST(OperatorResolutionResponsePoint, RandomlyDamagedEvidenceStaysEvidenceOnlyWhileTheCodeIsIntact) {
    // One byte of the evidence replaced by a random printable character: the result is either still exactly the evidence (the
    // byte landed in the message text, or was replaced by itself) or something that is not NoSuchOrder. The independent check for
    // "still the evidence" is that the damaged body still contains the digits of the code at the same place.
    std::mt19937_64 rng(0x0D5'0006'5EEDULL);
    const std::size_t code_pos = kNoSuchOrder.find("-2013");
    ASSERT_NE(code_pos, std::string::npos);
    for (int i = 0; i < 20'000; ++i) {
        std::string body = kNoSuchOrder;
        const std::size_t pos = rng() % body.size();
        body[pos] = static_cast<char>(' ' + rng() % 95);
        if (classify_point_query(400, body) == PointQueryVerdict::NoSuchOrder) {
            ASSERT_EQ(body.compare(code_pos, 5, "-2013"), 0) << body;
            ASSERT_EQ(body.find("\"code\""), kNoSuchOrder.find("\"code\"")) << body;
        }
    }
}

// --- lists of orders --------------------------------------------------------------------------------------------------------------

TEST(OperatorResolutionResponseScan, CountsTheItemsAndFindsTheOrder) {
    OrdersScan s = scan_orders_array("[]", kCoid, kSymbol);
    EXPECT_TRUE(s.ok);
    EXPECT_FALSE(s.coid_listed);
    EXPECT_EQ(s.item_count, 0u);
    s = scan_orders_array(page(5), kCoid, kSymbol);
    EXPECT_TRUE(s.ok);
    EXPECT_FALSE(s.coid_listed);
    EXPECT_EQ(s.item_count, 5u);
    s = scan_orders_array(page(5, 3), kCoid, kSymbol);
    EXPECT_TRUE(s.ok);
    EXPECT_TRUE(s.coid_listed);
    EXPECT_EQ(s.item_count, 5u);
}

TEST(OperatorResolutionResponsePages, AnEmptyPageIsClear) {
    EXPECT_EQ(classify_all_orders_page(200, "[]", kCoid, kSymbol), PageVerdict::Clear);
    EXPECT_EQ(classify_all_orders_page(200, " [ ] \n", kCoid, kSymbol), PageVerdict::Clear);
    EXPECT_EQ(classify_open_orders_page(200, "[]", kCoid), PageVerdict::Clear);
}

TEST(OperatorResolutionResponsePages, OurOrderAnywhereInThePageIsListed) {
    for (const std::size_t at_index : {std::size_t{0}, std::size_t{1}, std::size_t{25}, std::size_t{49}}) {
        EXPECT_EQ(classify_all_orders_page(200, page(50, at_index), kCoid, kSymbol), PageVerdict::Listed) << at_index;
        EXPECT_EQ(classify_open_orders_page(200, page(50, at_index), kCoid), PageVerdict::Listed) << at_index;
    }
    EXPECT_EQ(classify_all_orders_page(200, page(1, 0), kCoid, kSymbol), PageVerdict::Listed);
    EXPECT_EQ(classify_all_orders_page(200, page(50), kCoid, kSymbol), PageVerdict::Clear);
}

TEST(OperatorResolutionResponsePages, OnlyTheWholeClientOrderIdMatches) {
    for (const char* const near_miss : {"HY-1790000000000-7-004", "HY-1790000000000-7-00420", "hy-1790000000000-7-0042",
                                        "HY-1790000000000-7-0042 ", " HY-1790000000000-7-0042", "HY-1790000000000-8-0042",
                                        "HY-1790000000001-7-0042", "HY-1790000000000-7-0043", "HY-1790000000000-7-004\\u0000",
                                        "HY-1790000000000-7-0042\\u0000", "", "H", "HY-1790000000000-7-0042HY-1790000000000-7-0042"}) {
        const std::string body = "[" + order_json(near_miss) + "]";
        EXPECT_EQ(classify_all_orders_page(200, body, kCoid, kSymbol), PageVerdict::Clear) << near_miss;
        EXPECT_EQ(classify_open_orders_page(200, body, kCoid), PageVerdict::Clear) << near_miss;
    }
    // JSON escapes are decoded before the comparison
    for (const char* const same : {"HY-1790000000000-7-00\\u0034\\u0032", "HY\\u002d1790000000000-7-0042", "\\u0048Y-1790000000000-7-0042",
                                   "HY-1790000000000-7-0042"}) {
        const std::string body = "[" + order_json(same) + "]";
        EXPECT_EQ(classify_all_orders_page(200, body, kCoid, kSymbol), PageVerdict::Listed) << same;
    }
    // a client order id nested somewhere inside an order is not that order's clientOrderId
    const std::string nested = "[{\"symbol\":\"BTCUSDT\",\"clientOrderId\":\"" + other_coid(1) + "\",\"fills\":[{\"clientOrderId\":\"" +
                               std::string(kCoid) + "\"}]}]";
    EXPECT_EQ(classify_all_orders_page(200, nested, kCoid, kSymbol), PageVerdict::Clear);
}

TEST(OperatorResolutionResponsePages, AnItemThatCannotBeReadMakesThePageUnusable) {
    const std::string good = order_json(other_coid(1));
    const std::vector<std::string> bodies = {
        "[{\"symbol\":\"BTCUSDT\"}]",                                                  // no clientOrderId
        "[{\"symbol\":\"BTCUSDT\",\"clientOrderId\":5}]",                              // not a string
        "[{\"symbol\":\"BTCUSDT\",\"clientOrderId\":null}]",
        "[{\"symbol\":\"BTCUSDT\",\"clientOrderId\":[\"HY-1790000000000-7-0042\"]}]",
        "[{\"symbol\":\"BTCUSDT\",\"clientOrderId\":{\"a\":1}}]",
        "[{\"symbol\":\"BTCUSDT\",\"clientOrderId\":\"a\",\"clientOrderId\":\"b\"}]",   // twice
        "[{\"symbol\":\"BTCUSDT\",\"clientOrderId\":\"a\",\"clientOrderId\":\"a\"}]",   // twice, equal
        "[\"HY-1790000000000-7-0042\"]",                                               // an item that is a string
        "[5]",
        "[null]",
        "[[]]",
        "[{}]",
        "[" + good + ",{}]",                                                           // one good, one bad
        "[{}," + good + "]",
        "[" + good + ",5," + good + "]",
        "[" + good + "," + good,                                                       // cut off
        "[" + good + "," + good + "]]",
        "[" + good + "] []",
        "{\"code\":-1121,\"msg\":\"Invalid symbol.\"}",                                // an error object, not a page
        "{\"orders\":[]}",
        "null",
        "\"[]\"",
        "5",
        "",
        "[",
        "]",
        "[,]",
        "[" + good + ",]",
        "[1 2]",
        "<html></html>",
    };
    for (const std::string& body : bodies) {
        EXPECT_EQ(classify_all_orders_page(200, body, kCoid, kSymbol), PageVerdict::Malformed) << "[" << body.substr(0, 70) << "]";
        EXPECT_EQ(classify_open_orders_page(200, body, kCoid), PageVerdict::Malformed) << "[" << body.substr(0, 70) << "]";
        EXPECT_FALSE(scan_orders_array(body, kCoid, kSymbol).ok) << "[" << body.substr(0, 70) << "]";
    }
    // a bad item after the order itself does not rescue the page: the scan is all or nothing
    const std::string ours_then_bad = "[" + order_json(kCoid) + ",{}]";
    EXPECT_NE(classify_all_orders_page(200, ours_then_bad, kCoid, kSymbol), PageVerdict::Clear);
}

TEST(OperatorResolutionResponsePages, TheSymbolMustBeTheOneAskedFor) {
    EXPECT_EQ(classify_all_orders_page(200, page(3, 99, "ETHUSDT"), kCoid, kSymbol), PageVerdict::Malformed) << "another symbol's orders";
    EXPECT_EQ(classify_all_orders_page(200, page(3, 99, "btcusdt"), kCoid, kSymbol), PageVerdict::Malformed) << "case matters";
    EXPECT_EQ(classify_all_orders_page(200, page(3, 99, "BTCUSDT "), kCoid, kSymbol), PageVerdict::Malformed);
    EXPECT_EQ(classify_all_orders_page(200, "[{\"clientOrderId\":\"x\"}]", kCoid, kSymbol), PageVerdict::Malformed) << "no symbol field";
    EXPECT_EQ(classify_all_orders_page(200, "[{\"symbol\":5,\"clientOrderId\":\"x\"}]", kCoid, kSymbol), PageVerdict::Malformed);
    EXPECT_EQ(classify_all_orders_page(200, "[{\"symbol\":\"BTCUSDT\",\"symbol\":\"BTCUSDT\",\"clientOrderId\":\"x\"}]", kCoid, kSymbol),
              PageVerdict::Malformed);
    // one wrong symbol among right ones
    std::string mixed = page(2);
    mixed.insert(mixed.size() - 1, "," + order_json(other_coid(9), "ETHUSDT", 9));
    EXPECT_EQ(classify_all_orders_page(200, mixed, kCoid, kSymbol), PageVerdict::Malformed);
    // the symbol of the request must exist
    EXPECT_EQ(classify_all_orders_page(200, page(3), kCoid, ""), PageVerdict::Malformed);
    EXPECT_EQ(classify_all_orders_page(200, "[]", kCoid, ""), PageVerdict::Malformed);
    // openOrders is across all symbols and checks none
    EXPECT_EQ(classify_open_orders_page(200, page(3, 99, "ETHUSDT"), kCoid), PageVerdict::Clear);
    EXPECT_EQ(classify_open_orders_page(200, page(3, 1, "ETHUSDT"), kCoid), PageVerdict::Listed);
}

TEST(OperatorResolutionResponsePages, AFullPageMayHaveBeenTruncated) {
    ASSERT_EQ(kAllOrdersPageLimit, 1000u);
    EXPECT_EQ(classify_all_orders_page(200, page(999), kCoid, kSymbol), PageVerdict::Clear);
    EXPECT_EQ(classify_all_orders_page(200, page(1000), kCoid, kSymbol), PageVerdict::PossiblyTruncated);
    EXPECT_EQ(classify_all_orders_page(200, page(1001), kCoid, kSymbol), PageVerdict::PossiblyTruncated);
    EXPECT_EQ(classify_all_orders_page(200, page(1000, 999), kCoid, kSymbol), PageVerdict::Listed) << "naming the order beats truncation";
    EXPECT_EQ(classify_all_orders_page(200, page(999, 0), kCoid, kSymbol), PageVerdict::Listed);
    // openOrders has no limit to hit
    EXPECT_EQ(classify_open_orders_page(200, page(1500), kCoid), PageVerdict::Clear);
    EXPECT_EQ(classify_open_orders_page(200, page(1500, 1499), kCoid), PageVerdict::Listed);
}

TEST(OperatorResolutionResponsePages, OnlyHttp200Counts) {
    const std::string clear_page = page(3);
    EXPECT_EQ(classify_all_orders_page(200, clear_page, kCoid, kSymbol), PageVerdict::Clear);
    for (const std::uint16_t status : {std::uint16_t{0}, std::uint16_t{199}, std::uint16_t{201}, std::uint16_t{204}, std::uint16_t{206},
                                       std::uint16_t{304}, std::uint16_t{400}, std::uint16_t{401}, std::uint16_t{403}, std::uint16_t{418},
                                       std::uint16_t{429}, std::uint16_t{500}, std::uint16_t{503}}) {
        EXPECT_EQ(classify_all_orders_page(status, clear_page, kCoid, kSymbol), PageVerdict::Malformed) << status;
        EXPECT_EQ(classify_open_orders_page(status, clear_page, kCoid), PageVerdict::Malformed) << status;
        EXPECT_EQ(classify_all_orders_page(status, page(3, 1), kCoid, kSymbol), PageVerdict::Malformed) << status;
    }
}

TEST(OperatorResolutionResponsePages, AnEmptyOrImpossibleClientOrderIdIsNeverClear) {
    EXPECT_EQ(classify_all_orders_page(200, "[]", "", kSymbol), PageVerdict::Malformed);
    EXPECT_EQ(classify_open_orders_page(200, "[]", ""), PageVerdict::Malformed);
    const std::string too_long(hy::kClientOrderIdLen + 1, 'A');
    EXPECT_EQ(classify_all_orders_page(200, "[]", too_long, kSymbol), PageVerdict::Malformed);
    const std::string longest(hy::kClientOrderIdLen, 'A');
    EXPECT_EQ(classify_all_orders_page(200, "[]", longest, kSymbol), PageVerdict::Clear);
}

TEST(OperatorResolutionResponsePages, ACutOffPageIsNeverClear) {
    const std::string body = page(3);
    for (std::size_t len = 0; len < body.size(); ++len) {
        ASSERT_EQ(classify_all_orders_page(200, std::string_view(body).substr(0, len), kCoid, kSymbol), PageVerdict::Malformed)
            << "prefix of " << len << " bytes";
        ASSERT_EQ(classify_open_orders_page(200, std::string_view(body).substr(0, len), kCoid), PageVerdict::Malformed) << len;
    }
    // a full-size page, cut at scattered points (every cut is a distinct way for a transfer to die)
    const std::string big = page(1000);
    std::mt19937_64 rng(0x0D5'0007'5EEDULL);
    for (int i = 0; i < 300; ++i) {
        const std::size_t len = rng() % big.size();
        ASSERT_EQ(classify_all_orders_page(200, std::string_view(big).substr(0, len), kCoid, kSymbol), PageVerdict::Malformed) << len;
    }
}

TEST(OperatorResolutionResponsePages, AnOversizePageIsNotRead) {
    std::string big = "[";
    while (big.size() <= kMaxEvidenceBodyBytes) {
        if (big.size() > 1) big += ',';
        big += order_json(other_coid(big.size()));
    }
    big += ']';
    ASSERT_GT(big.size(), kMaxEvidenceBodyBytes);
    EXPECT_EQ(classify_all_orders_page(200, big, kCoid, kSymbol), PageVerdict::Malformed);
    EXPECT_EQ(classify_open_orders_page(200, big, kCoid), PageVerdict::Malformed);
}

TEST(OperatorResolutionResponsePages, RandomBytesAreNeverAClearPage) {
    std::mt19937_64 rng(0x0D5'0008'5EEDULL);
    for (int i = 0; i < 20'000; ++i) {
        std::string body(rng() % 200, '\0');
        for (char& c : body) c = static_cast<char>(rng());
        ASSERT_EQ(classify_all_orders_page(200, body, kCoid, kSymbol), PageVerdict::Malformed) << i;
        ASSERT_EQ(classify_open_orders_page(200, body, kCoid), PageVerdict::Malformed) << i;
    }
}

TEST(OperatorResolutionResponsePages, EscapesInOtherFieldsAreFineAndARawControlCharacterIsNotJson) {
    // valid escapes (a surrogate pair included) inside other fields are fine; a raw control character in a string is not JSON
    const std::string ok = "[{\"symbol\":\"BTCUSDT\",\"clientOrderId\":\"x\",\"note\":\"\\ud83d\\ude00 \\n \\t \\\\ \\\"\"}]";
    EXPECT_EQ(classify_all_orders_page(200, ok, kCoid, kSymbol), PageVerdict::Clear);
    const std::string control = std::string("[{\"symbol\":\"BTCUSDT\",\"clientOrderId\":\"x\",\"note\":\"a") + '\x01' + "b\"}]";
    EXPECT_EQ(classify_all_orders_page(200, control, kCoid, kSymbol), PageVerdict::Malformed);
}

TEST(OperatorResolutionResponsePages, AByteOrderMarkInFrontIsRefusedLikeEverywhereElse) {
    const std::string bom = "\xEF\xBB\xBF";
    EXPECT_EQ(classify_all_orders_page(200, bom + "[]", kCoid, kSymbol), PageVerdict::Malformed);
    EXPECT_EQ(classify_open_orders_page(200, bom + page(2), kCoid), PageVerdict::Malformed);
    EXPECT_EQ(classify_point_query(400, bom + kNoSuchOrder), PointQueryVerdict::Unusable);
    EXPECT_EQ(classify_point_query(200, bom + order_json(kCoid)), PointQueryVerdict::Unusable);
}

}  // namespace
