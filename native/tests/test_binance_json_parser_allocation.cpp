// BinanceJsonParser::parse() allocates nothing once the parser exists -- for any frame a WebSocket session can deliver, in any order
// (Appendix D L-38). It used to allocate 4 to 5 times (up to ~28 MB at the 4 MiB limit) on every frame longer than any seen before,
// because simdjson's on-demand parser sizes itself lazily inside iterate(); the old comment (AUDIT PERF-ALLOC-012) said "after the first few
// messages ... zero mallocs", which only held after the largest frame had been seen.
//
// The probe is test_helpers/alloc_probe.hpp: this binary's global allocation functions are replaced by counting ones, and a window is open
// only around parse() calls -- never around gtest's own bookkeeping, so no gtest macro, container growth or string construction happens
// inside it. Every input is built before its window opens. Each test asserts that its frames really took the path it claims (the result
// codes), so a zero cannot come from parse() bailing out early.
#include <gtest/gtest.h>
#include <hengyuan/binance_json_parser.hpp>
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

using hy::BinanceJsonParser;
using hy::BinanceMarketEvent;
using hy::ParseResult;

namespace {

constexpr std::size_t kLimit = hy::kWsReadMessageMaxBytes;  // the largest frame a session can deliver: exactly 4 MiB
constexpr std::size_t kMaxEvents = 256;

const std::string kTrade = R"({"e":"trade","E":7,"s":"BTCUSDT","t":42,"p":"100.5","q":"2.25","m":false})";
const std::string kAggTrade = R"({"e":"aggTrade","E":8,"s":"ETHUSDT","a":43,"p":"2000.5","q":"1.5","m":true})";
const std::string kDepth =
    R"({"e":"depthUpdate","E":123,"s":"BTCUSDT","U":10,"u":15,"b":[["50000.0","1.0"],["49999.0","2.0"]],"a":[["50010.0","1.0"]]})";
const std::string kSubscribeAck = R"({"result":null,"id":1})";  // no "e": parse() re-iterates the document to look for "id"
const std::string kUnknownSymbol = R"({"e":"trade","E":7,"s":"DOGEUSDT","t":42,"p":"1.0","q":"2.0","m":false})";
const std::string kUnknownEvent = R"({"e":"kline","E":7,"s":"BTCUSDT"})";
const std::string kPriceOverflow = R"({"e":"trade","E":7,"s":"BTCUSDT","t":42,"p":"99999999999999999999.0","q":"2.0","m":false})";
const std::string kQtyOverflow = R"({"e":"trade","E":7,"s":"BTCUSDT","t":42,"p":"1.0","q":"99999999999999999999.0","m":false})";
const std::string kNotJson = "this is not json at all";

// `body` followed by spaces up to exactly `size` bytes: still ONE valid JSON document, so a frame of that length takes every path a real frame
// of that length would. (A body already longer than `size` is returned unchanged; the tests assert the exact length they rely on.)
std::string padded_to(std::string body, std::size_t size) {
    if (body.size() < size) body.resize(size, ' ');
    return body;
}

// prefix + unit x k + last + suffix, then spaces, to exactly `size` bytes. With unit "1," and last "1" inside an array this is the longest
// tape one can get (one structural character per two bytes); with a depth level as the unit it is a dense book update.
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

struct Case {
    std::string name;
    std::string body;
    ParseResult expected;
    bool any_failure;  // the body is garbage: expect "not Ok" instead of one exact code
};

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

}  // namespace

// The number is a decision, not an implementation detail: it is the largest WebSocket message any session accepts AND, in each of the three
// parsers, ~28 MB of address space (4 MiB of it touched) allocated once per instance. These tests follow the constant wherever it goes;
// changing it should be a deliberate edit of this line too.
TEST(WsFrameLimits, TheReadLimitIsFourMiBAndEveryParserIsSizedFromIt) {
    EXPECT_EQ(hy::kWsReadMessageMaxBytes, std::size_t{4} * 1024 * 1024);
}

// The positive control: without it, "0 allocations" proves nothing.
TEST(BinanceJsonParserAllocation, TheProbeSeesTheAllocationsItIsMeantToSee) {
    const alloc_probe::SelfCheck c = alloc_probe::selfcheck();
    EXPECT_EQ(c.after_array_new, 1u) << "new char[N] must be counted";
    EXPECT_EQ(c.after_nothrow_array_new, 2u) << "new (std::nothrow) char[N] must be counted (simdjson allocates through it)";
    EXPECT_EQ(c.after_string_growth, 3u) << "a std::string growing past its small buffer must be counted";
    EXPECT_EQ(c.outside_window, 0u) << "nothing outside a window may be counted";
}

// The strongest form of the claim: the parser is built, and its very first frame is the largest one a session can deliver. Nothing is
// warmed up, so every byte of memory the parse needs must already exist.
TEST(BinanceJsonParserAllocation, TheVeryFirstFrameMayBeTheLargestOneAndStillAllocatesNothing) {
    BinanceJsonParser parser;
    ASSERT_TRUE(parser.register_symbol("BTCUSDT", 0));
    const std::string frame = padded_to(kTrade, kLimit);
    ASSERT_EQ(frame.size(), kLimit);
    BinanceMarketEvent events[kMaxEvents]{};
    std::size_t count = 0;
    ParseResult result = ParseResult::MalformedJson;
    std::size_t calls = 0;
    std::size_t bytes = 0;
    {
        const alloc_probe::Window window;
        result = parser.parse(frame, 1, events, kMaxEvents, count);
        calls = window.calls();
        bytes = window.bytes();
    }
    EXPECT_EQ(result, ParseResult::Ok) << "a frame of exactly the limit is a frame";
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(calls, 0u) << "operator new was called " << calls << " time(s), " << bytes << " byte(s), by the first parse of a "
                         << kLimit << "-byte frame";
}

// What the old parser went through: every size below is a new high-water mark when taken in this order (the 65,536 step crosses the old
// 64 KiB padded buffer). Then the sizes back down and up again -- the buffers must neither shrink nor regrow.
TEST(BinanceJsonParserAllocation, AscendingThenDescendingSizesAllocateNothing) {
    BinanceJsonParser parser;
    ASSERT_TRUE(parser.register_symbol("BTCUSDT", 0));

    const std::array<std::size_t, 11> ladder{0, 339, 2'700, 10'000, 52'000, 65'536, 104'000, 700'000, 1'048'576, kLimit - 1, kLimit};
    std::vector<std::string> frames;
    for (const std::size_t size : ladder) frames.push_back(padded_to(kTrade, size));
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < frames.size(); ++i) order.push_back(i);
    for (std::size_t i = frames.size(); i-- > 0;) order.push_back(i);
    for (std::size_t i = 0; i < frames.size(); ++i) order.push_back(i);
    std::vector<std::size_t> sizes;
    for (const std::size_t i : order) sizes.push_back(frames[i].size());
    std::vector<ParseResult> results(order.size(), ParseResult::MalformedJson);
    std::vector<std::size_t> calls_after(order.size(), 0);
    BinanceMarketEvent events[kMaxEvents]{};
    std::size_t count = 0;
    std::size_t total = 0;
    {
        const alloc_probe::Window window;
        for (std::size_t n = 0; n < order.size(); ++n) {
            results[n] = parser.parse(frames[order[n]], 1, events, kMaxEvents, count);
            calls_after[n] = window.calls();
        }
        total = window.calls();
    }
    std::size_t not_ok = 0;
    for (const ParseResult r : results) {
        if (r != ParseResult::Ok) ++not_ok;
    }
    EXPECT_EQ(not_ok, 0u) << "every frame is a valid trade followed by whitespace";
    EXPECT_EQ(frames.back().size(), kLimit);
    EXPECT_EQ(total, 0u) << total << " allocation(s) while parsing " << order.size() << " frames:" << describe(sizes, calls_after);
}

// Every way parse() can end, at its natural size and padded to the limit: the second iterate() of the subscribe-ack path, the error returns,
// the truncated-resync path. A path that allocated only on its own would be invisible to the ladder above.
TEST(BinanceJsonParserAllocation, EveryOutcomeAllocatesNothingAtAnySize) {
    BinanceJsonParser parser;
    ASSERT_TRUE(parser.register_symbol("BTCUSDT", 0));
    ASSERT_TRUE(parser.register_symbol("ETHUSDT", 1));

    // A dense book update that does not fit kMaxEvents levels: TruncatedResync, after a scan of the whole 4 MiB.
    const std::string dense_levels =
        repeated_to(R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":1,"u":2,"b":[)", R"(["50000.0","1.0"],)", R"(["50000.0","1.0"])",
                    R"(],"a":[["60000.0","1.0"]]})", kLimit);
    // The longest tape: an array of 2 M numbers -- no level is an array, so nothing is produced (MalformedJson) but every structural
    // character of the 4 MiB is indexed and visited.
    const std::string dense_numbers =
        repeated_to(R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":1,"u":2,"b":[)", "1,", "1", R"(],"a":[]})", kLimit);
    // The symbol is a ~4 MiB string, unescaped into simdjson's string buffer in full, and matches nothing.
    const std::string huge_symbol_prefix = R"({"e":"trade","E":7,"s":")";
    const std::string huge_symbol_suffix = R"("})";
    const std::string huge_symbol =
        huge_symbol_prefix + std::string(kLimit - huge_symbol_prefix.size() - huge_symbol_suffix.size(), 'A') + huge_symbol_suffix;
    ASSERT_EQ(dense_levels.size(), kLimit);
    ASSERT_EQ(dense_numbers.size(), kLimit);
    ASSERT_EQ(huge_symbol.size(), kLimit);

    std::vector<Case> cases;
    const auto add = [&cases](const char* name, const std::string& body, ParseResult expected) {
        cases.push_back({name, body, expected, false});
    };
    const auto add_both = [&](const char* name, const std::string& body, ParseResult expected) {
        add(name, body, expected);
        add(name, padded_to(body, 104'000), expected);
        add(name, padded_to(body, kLimit), expected);
    };
    add_both("trade", kTrade, ParseResult::Ok);
    add_both("aggTrade", kAggTrade, ParseResult::Ok);
    add_both("depthUpdate", kDepth, ParseResult::Ok);
    add_both("subscribe ack", kSubscribeAck, ParseResult::EventIgnored);
    add_both("unknown symbol", kUnknownSymbol, ParseResult::UnknownSymbol);
    add_both("unknown event", kUnknownEvent, ParseResult::UnknownEventType);
    add_both("price overflow", kPriceOverflow, ParseResult::PriceOverflow);
    add_both("qty overflow", kQtyOverflow, ParseResult::QtyOverflow);
    add("dense levels", dense_levels, ParseResult::TruncatedResync);
    add("dense numbers", dense_numbers, ParseResult::MalformedJson);
    add("huge symbol", huge_symbol, ParseResult::UnknownSymbol);
    cases.push_back({"empty", "", ParseResult::MalformedJson, false});
    cases.push_back({"not json", kNotJson, ParseResult::MalformedJson, true});
    cases.push_back({"not json, padded to the limit", padded_to(kNotJson, kLimit), ParseResult::MalformedJson, true});
    cases.push_back({"unclosed object", R"({"e":"trade","s":"BTCUSDT")", ParseResult::MalformedJson, true});

    std::vector<ParseResult> results(cases.size(), ParseResult::Ok);
    std::vector<std::size_t> sizes;
    for (const Case& c : cases) sizes.push_back(c.body.size());
    std::vector<std::size_t> calls_after(cases.size(), 0);
    BinanceMarketEvent events[kMaxEvents]{};
    std::size_t count = 0;
    std::size_t total = 0;
    {
        const alloc_probe::Window window;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            results[n] = parser.parse(cases[n].body, 1, events, kMaxEvents, count);
            calls_after[n] = window.calls();
        }
        total = window.calls();
    }
    for (std::size_t n = 0; n < cases.size(); ++n) {
        if (cases[n].any_failure) {
            EXPECT_NE(results[n], ParseResult::Ok) << cases[n].name;
        } else {
            EXPECT_EQ(results[n], cases[n].expected) << cases[n].name << " (" << cases[n].body.size() << " B)";
        }
    }
    EXPECT_EQ(total, 0u) << total << " allocation(s) while parsing " << cases.size() << " frames:" << describe(sizes, calls_after);
}

// A body above the limit cannot come from a session (Beast refuses the message first); a direct caller gets it refused BEFORE it is copied,
// counted as malformed, and the parser is exactly as usable afterwards.
TEST(BinanceJsonParserAllocation, AFrameAboveTheLimitIsRefusedWithoutAllocatingAndTheParserSurvivesIt) {
    BinanceJsonParser parser;
    ASSERT_TRUE(parser.register_symbol("BTCUSDT", 0));
    const std::string oversize = padded_to(kTrade, kLimit + 1);
    const std::string at_limit = padded_to(kTrade, kLimit);
    ASSERT_EQ(oversize.size(), kLimit + 1);
    BinanceMarketEvent events[kMaxEvents]{};
    std::size_t count = 99;
    ParseResult refused = ParseResult::Ok;
    ParseResult accepted = ParseResult::MalformedJson;
    std::size_t count_after_refusal = 99;
    const std::uint64_t malformed_before = parser.counters().malformed;
    std::uint64_t malformed_after = 0;
    std::size_t calls = 0;
    {
        const alloc_probe::Window window;
        refused = parser.parse(oversize, 1, events, kMaxEvents, count);
        count_after_refusal = count;
        malformed_after = parser.counters().malformed;
        accepted = parser.parse(at_limit, 2, events, kMaxEvents, count);
        calls = window.calls();
    }
    EXPECT_EQ(refused, ParseResult::MalformedJson);
    EXPECT_EQ(count_after_refusal, 0u) << "out_count is zeroed before anything else";
    EXPECT_EQ(malformed_after, malformed_before + 1) << "a refused frame is a counted one";
    EXPECT_EQ(accepted, ParseResult::Ok) << "the parser is as usable after the refusal as before it";
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(calls, 0u) << calls << " allocation(s) around a refused frame and the next accepted one";
}

// "Refused before it is copied", checked the hard way: the bodies below are views over address space that cannot be read at all, so a parser
// that touched even the first byte of one would fault (access violation / SIGSEGV), where one that looks at the length first simply returns.
// A body one byte over the limit, and one twice the limit.
TEST(BinanceJsonParserAllocation, AFrameAboveTheLimitIsRefusedBeforeAnyByteOfItIsRead) {
    BinanceJsonParser parser;
    ASSERT_TRUE(parser.register_symbol("BTCUSDT", 0));
    const hy_test::UnreadableRegion region(2 * kLimit);
    ASSERT_TRUE(region.ok());
    BinanceMarketEvent events[kMaxEvents]{};
    std::size_t count = 99;
    const ParseResult just_over = parser.parse(region.view(kLimit + 1), 1, events, kMaxEvents, count);
    const ParseResult far_over = parser.parse(region.view(2 * kLimit), 1, events, kMaxEvents, count);
    EXPECT_EQ(just_over, ParseResult::MalformedJson);
    EXPECT_EQ(far_over, ParseResult::MalformedJson);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(parser.counters().malformed, 2u);
}
