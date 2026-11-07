// KlineJsonParser::parse() allocates nothing once the parser exists -- for any frame a WebSocket session can deliver, in any order
// (Appendix D L-38). Same story as BinanceJsonParser (test_binance_json_parser_allocation.cpp, which carries the long version):
// simdjson's on-demand parser sizes itself lazily inside iterate(), and this one started with a 4 KiB padded copy buffer, so it allocated
// 4 to 5 times (up to ~28 MB at the 4 MiB limit) on every frame longer than any seen before.
//
// The parser lives in the session header (Boost.Beast), which is why this is a separate, Boost-gated executable. (UserDataJsonParser, the
// listenKey stream's parser, was measured here too until it was retired in M5b-2; the WebSocket API codec that replaced it is measured in
// test_binance_ws_api_codec.cpp, WsApiCodecAllocation.)
// The probe is test_helpers/alloc_probe.hpp: a window is open only around parse() calls, never around gtest's own bookkeeping, and every
// input is built before its window opens.
#include <gtest/gtest.h>
#include <hengyuan/binance_kline_ws_session.hpp>
#include <hengyuan/ws_frame_limits.hpp>

#include "test_helpers/alloc_probe.hpp"
#include "test_helpers/unreadable_region.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using hy::KlineJsonParser;
using hy::KlineParseResult;
using hy::KlineWsEvent;

namespace {

constexpr std::size_t kLimit = hy::kWsReadMessageMaxBytes;  // the largest frame a session can deliver: exactly 4 MiB

// `body` followed by spaces up to exactly `size` bytes: still ONE valid JSON document (a body already longer than `size` is unchanged).
std::string padded_to(std::string body, std::size_t size) {
    if (body.size() < size) body.resize(size, ' ');
    return body;
}

// prefix + unit x k + last + suffix, then spaces, to exactly `size` bytes ("1," / "1" in an array is the longest tape one can get).
std::string repeated_to(const std::string& prefix, const std::string& unit, const std::string& last, const std::string& suffix,
                        std::size_t size) {
    const std::size_t fixed = prefix.size() + last.size() + suffix.size();
    const std::size_t k = size > fixed ? (size - fixed) / unit.size() : 0;
    std::string out;
    out.reserve(size);
    out += prefix;
    for (std::size_t i = 0; i < k; ++i) out += unit;
    out += last;
    out += suffix;
    return padded_to(std::move(out), size);
}

// Text for a failed assertion: one line per frame that allocated, so the log shows WHICH frame size did.
std::string describe(const std::vector<std::size_t>& sizes, const std::vector<std::size_t>& calls_after) {
    std::ostringstream os;
    std::size_t before = 0;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const std::size_t delta = calls_after[i] - before;
        before = calls_after[i];
        if (delta != 0) os << "\n  frame #" << i << " (" << sizes[i] << " B): " << delta << " allocation(s)";
    }
    return os.str();
}

// --- kline frames ---

std::string kline_body(bool closed, const std::string& before_k = "") {
    return std::string(R"({"e":"kline","E":123456789,"s":"BNBBTC",)") + before_k +
           R"("k":{"t":123400000,"T":123459999,"s":"BNBBTC","i":"1m","f":100,"L":200,)"
           R"("o":"0.0010","c":"0.0020","h":"0.0025","l":"0.0015","v":"1000","n":100,"x":)" +
           (closed ? "true" : "false") + R"(,"q":"1.0000","V":"500","Q":"0.500","B":"123456"}})";
}

struct KlineCase {
    std::string name;
    std::string body;
    KlineParseResult expected;
    bool any_failure;
};

}  // namespace

// The positive control: without it, "0 allocations" proves nothing.
TEST(WsParserAllocation, TheProbeSeesTheAllocationsItIsMeantToSee) {
    const alloc_probe::SelfCheck c = alloc_probe::selfcheck();
    EXPECT_EQ(c.after_array_new, 1u) << "new char[N] must be counted";
    EXPECT_EQ(c.after_nothrow_array_new, 2u) << "new (std::nothrow) char[N] must be counted (simdjson allocates through it)";
    EXPECT_EQ(c.after_string_growth, 3u) << "a std::string growing past its small buffer must be counted";
    EXPECT_EQ(c.outside_window, 0u) << "nothing outside a window may be counted";
}

// ---------------------------------------------------------------------------------------------------------------------------------
// KlineJsonParser
// ---------------------------------------------------------------------------------------------------------------------------------

TEST(KlineJsonParserAllocation, TheVeryFirstFrameMayBeTheLargestOneAndStillAllocatesNothing) {
    KlineJsonParser parser;
    const std::string frame = padded_to(kline_body(true), kLimit);
    ASSERT_EQ(frame.size(), kLimit);
    KlineWsEvent ev{};
    KlineParseResult result = KlineParseResult::MalformedJson;
    std::size_t calls = 0;
    std::size_t bytes = 0;
    {
        const alloc_probe::Window window;
        result = parser.parse(frame, ev);
        calls = window.calls();
        bytes = window.bytes();
    }
    EXPECT_EQ(result, KlineParseResult::Ok) << "a frame of exactly the limit is a frame";
    EXPECT_TRUE(ev.is_closed);
    EXPECT_EQ(calls, 0u) << "operator new was called " << calls << " time(s), " << bytes << " byte(s), by the first parse of a " << kLimit
                         << "-byte frame";
}

TEST(KlineJsonParserAllocation, AscendingThenDescendingSizesAllocateNothing) {
    KlineJsonParser parser;
    const std::array<std::size_t, 11> ladder{0, 339, 2'700, 10'000, 52'000, 65'536, 104'000, 700'000, 1'048'576, kLimit - 1, kLimit};
    std::vector<std::string> frames;
    for (const std::size_t size : ladder) frames.push_back(padded_to(kline_body(true), size));
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < frames.size(); ++i) order.push_back(i);
    for (std::size_t i = frames.size(); i-- > 0;) order.push_back(i);
    for (std::size_t i = 0; i < frames.size(); ++i) order.push_back(i);
    std::vector<std::size_t> sizes;
    for (const std::size_t i : order) sizes.push_back(frames[i].size());
    std::vector<KlineParseResult> results(order.size(), KlineParseResult::MalformedJson);
    std::vector<std::size_t> calls_after(order.size(), 0);
    KlineWsEvent ev{};
    std::size_t total = 0;
    {
        const alloc_probe::Window window;
        for (std::size_t n = 0; n < order.size(); ++n) {
            results[n] = parser.parse(frames[order[n]], ev);
            calls_after[n] = window.calls();
        }
        total = window.calls();
    }
    std::size_t not_ok = 0;
    for (const KlineParseResult r : results) {
        if (r != KlineParseResult::Ok) ++not_ok;
    }
    EXPECT_EQ(not_ok, 0u) << "every frame is a valid closed kline followed by whitespace";
    EXPECT_EQ(frames.back().size(), kLimit);
    EXPECT_EQ(total, 0u) << total << " allocation(s) while parsing " << order.size() << " frames:" << describe(sizes, calls_after);
}

TEST(KlineJsonParserAllocation, EveryOutcomeAllocatesNothingAtAnySize) {
    KlineJsonParser parser;
    // The longest tape, in front of "k": looking "k" up skips 2 M numbers, and stage 1 indexes every structural character of the 4 MiB.
    const std::string closed_head = R"({"e":"kline","E":123456789,"s":"BNBBTC",)";  // what kline_body() puts in front of "k"
    const std::string closed_k = kline_body(true).substr(closed_head.size());       // "k":{...}}
    const std::string dense_numbers = repeated_to(closed_head + R"("pad":[)", "1,", "1", "]," + closed_k, kLimit);
    // "o" is a ~4 MiB string, unescaped into simdjson's string buffer in full, and then rejected as a number.
    const std::string open_prefix = R"({"e":"kline","E":1,"k":{"t":1,"T":2,"o":")";
    const std::string open_suffix = R"(","h":"1","l":"1","c":"1","v":"1","x":true}})";
    const std::string huge_open = open_prefix + std::string(kLimit - open_prefix.size() - open_suffix.size(), 'A') + open_suffix;
    ASSERT_EQ(dense_numbers.size(), kLimit);
    ASSERT_EQ(huge_open.size(), kLimit);

    std::vector<KlineCase> cases;
    const auto add_both = [&cases](const char* name, const std::string& body, KlineParseResult expected) {
        cases.push_back({name, body, expected, false});
        cases.push_back({name, padded_to(body, 104'000), expected, false});
        cases.push_back({name, padded_to(body, kLimit), expected, false});
    };
    add_both("closed kline", kline_body(true), KlineParseResult::Ok);
    add_both("unclosed kline", kline_body(false), KlineParseResult::Unclosed);
    add_both("wrong event type", R"({"e":"trade","E":1})", KlineParseResult::MissingFields);
    add_both("no k object", R"({"e":"kline","E":1})", KlineParseResult::MissingFields);
    cases.push_back({"dense numbers", dense_numbers, KlineParseResult::Ok, false});
    cases.push_back({"huge open price", huge_open, KlineParseResult::MissingFields, false});
    cases.push_back({"empty", "", KlineParseResult::MalformedJson, false});
    cases.push_back({"not json", "this is not json at all", KlineParseResult::MalformedJson, true});
    cases.push_back({"not json, padded to the limit", padded_to("this is not json at all", kLimit), KlineParseResult::MalformedJson, true});

    std::vector<KlineParseResult> results(cases.size(), KlineParseResult::Ok);
    std::vector<std::size_t> sizes;
    for (const KlineCase& c : cases) sizes.push_back(c.body.size());
    std::vector<std::size_t> calls_after(cases.size(), 0);
    KlineWsEvent ev{};
    std::size_t total = 0;
    {
        const alloc_probe::Window window;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            results[n] = parser.parse(cases[n].body, ev);
            calls_after[n] = window.calls();
        }
        total = window.calls();
    }
    for (std::size_t n = 0; n < cases.size(); ++n) {
        if (cases[n].any_failure) {
            EXPECT_NE(results[n], KlineParseResult::Ok) << cases[n].name;
        } else {
            EXPECT_EQ(results[n], cases[n].expected) << cases[n].name << " (" << cases[n].body.size() << " B)";
        }
    }
    EXPECT_EQ(total, 0u) << total << " allocation(s) while parsing " << cases.size() << " frames:" << describe(sizes, calls_after);
}

TEST(KlineJsonParserAllocation, AFrameAboveTheLimitIsRefusedWithoutAllocatingAndTheParserSurvivesIt) {
    KlineJsonParser parser;
    const std::string oversize = padded_to(kline_body(true), kLimit + 1);
    const std::string at_limit = padded_to(kline_body(true), kLimit);
    ASSERT_EQ(oversize.size(), kLimit + 1);
    KlineWsEvent ev{};
    ev.is_closed = true;  // dirty on purpose: a refusal must leave it empty, not as it was
    ev.open_time_ms = 5;
    KlineParseResult refused = KlineParseResult::Ok;
    bool closed_after_refusal = true;
    std::int64_t open_time_after_refusal = 5;
    KlineParseResult accepted = KlineParseResult::MalformedJson;
    std::size_t calls = 0;
    {
        const alloc_probe::Window window;
        refused = parser.parse(oversize, ev);
        closed_after_refusal = ev.is_closed;
        open_time_after_refusal = ev.open_time_ms;
        accepted = parser.parse(at_limit, ev);
        calls = window.calls();
    }
    EXPECT_EQ(refused, KlineParseResult::MalformedJson);
    EXPECT_FALSE(closed_after_refusal);
    EXPECT_EQ(open_time_after_refusal, 0);
    EXPECT_EQ(accepted, KlineParseResult::Ok) << "the parser is as usable after the refusal as before it";
    EXPECT_TRUE(ev.is_closed);
    EXPECT_EQ(calls, 0u) << calls << " allocation(s) around a refused frame and the next accepted one";
}

// "Refused before it is copied", checked the hard way: the bodies below are views over address space that cannot be read at all, so a parser
// that touched even the first byte of one would fault (access violation / SIGSEGV), where one that looks at the length first simply returns.
// A body one byte over the limit, and one twice the limit.
TEST(KlineJsonParserAllocation, AFrameAboveTheLimitIsRefusedBeforeAnyByteOfItIsRead) {
    KlineJsonParser parser;
    const hy_test::UnreadableRegion region(2 * kLimit);
    ASSERT_TRUE(region.ok());
    KlineWsEvent ev{};
    const KlineParseResult just_over = parser.parse(region.view(kLimit + 1), ev);
    const KlineParseResult far_over = parser.parse(region.view(2 * kLimit), ev);
    EXPECT_EQ(just_over, KlineParseResult::MalformedJson);
    EXPECT_EQ(far_over, KlineParseResult::MalformedJson);
    EXPECT_FALSE(ev.is_closed);
}
