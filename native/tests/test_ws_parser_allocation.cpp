// UserDataJsonParser::parse() and KlineJsonParser::parse() allocate nothing once the parser exists -- for any frame a WebSocket session can
// deliver, in any order (Appendix D L-38). Same story as BinanceJsonParser (test_binance_json_parser_allocation.cpp, which carries the
// long version): simdjson's on-demand parser sizes itself lazily inside iterate(), and both of these started with a 4 KiB padded copy
// buffer, so they allocated 4 to 5 times (up to ~28 MB at the 4 MiB limit) on every frame longer than any seen before.
//
// These two parsers live in the session headers (Boost.Beast), which is why this is a separate, Boost-gated executable.
// The probe is test_helpers/alloc_probe.hpp: a window is open only around parse() calls, never around gtest's own bookkeeping, and every
// input is built before its window opens.
#include <gtest/gtest.h>
#include <hengyuan/binance_kline_ws_session.hpp>
#include <hengyuan/binance_user_data_ws_session.hpp>
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
using hy::UserDataEventKind;
using hy::UserDataJsonParser;
using hy::UserDataParseResult;
using hy::UserDataWsEvent;

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

// --- user data frames ---

const std::string kExecutionReport =
    R"({"e":"executionReport","E":1700000000123,"s":"BTCUSDT","c":"HY-1700000000000-7-42","S":"BUY","o":"LIMIT","f":"GTC",)"
    R"("q":"1.00000000","p":"50000.00000000","x":"TRADE","X":"PARTIALLY_FILLED","i":998877,"l":"0.40000000","z":"0.40000000",)"
    R"("L":"50000.00000000","T":1700000000120,"Z":"20000.00000000"})";
const std::string kOutboundAccountPosition =
    R"({"e":"outboundAccountPosition","E":1700000000123,"u":1700000000122,"B":[{"a":"BTC","f":"1.0","l":"0.0"}]})";
const std::string kListenKeyExpired = R"({"e":"listenKeyExpired","E":1})";
const std::string kUnknownUserEvent = R"({"e":"balanceUpdate","E":1,"a":"BTC","d":"0.1"})";
const std::string kNoEventType = R"({"E":1})";

struct UserDataCase {
    std::string name;
    std::string body;
    UserDataParseResult expected;
    UserDataEventKind expected_kind;
    bool any_failure;  // garbage: expect "not Ok" instead of one exact code
};

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
// UserDataJsonParser
// ---------------------------------------------------------------------------------------------------------------------------------

// The parser is built and its very first frame is the largest one a session can deliver: nothing is warmed up.
TEST(UserDataJsonParserAllocation, TheVeryFirstFrameMayBeTheLargestOneAndStillAllocatesNothing) {
    UserDataJsonParser parser;
    const std::string frame = padded_to(kListenKeyExpired, kLimit);
    ASSERT_EQ(frame.size(), kLimit);
    UserDataWsEvent ev{};
    UserDataParseResult result = UserDataParseResult::MalformedJson;
    std::size_t calls = 0;
    std::size_t bytes = 0;
    {
        const alloc_probe::Window window;
        result = parser.parse(frame, ev);
        calls = window.calls();
        bytes = window.bytes();
    }
    EXPECT_EQ(result, UserDataParseResult::Ok) << "a frame of exactly the limit is a frame";
    EXPECT_EQ(ev.kind, UserDataEventKind::ListenKeyExpired);
    EXPECT_EQ(calls, 0u) << "operator new was called " << calls << " time(s), " << bytes << " byte(s), by the first parse of a " << kLimit
                         << "-byte frame";
}

// Every size is a new high-water mark when taken in this order (the old copy buffer started at 4 KiB, so 10,000 already regrew it);
// then back down and up again -- the buffers must neither shrink nor regrow.
TEST(UserDataJsonParserAllocation, AscendingThenDescendingSizesAllocateNothing) {
    UserDataJsonParser parser;
    const std::array<std::size_t, 11> ladder{0, 339, 2'700, 10'000, 52'000, 65'536, 104'000, 700'000, 1'048'576, kLimit - 1, kLimit};
    std::vector<std::string> frames;
    for (const std::size_t size : ladder) frames.push_back(padded_to(kExecutionReport, size));
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < frames.size(); ++i) order.push_back(i);
    for (std::size_t i = frames.size(); i-- > 0;) order.push_back(i);
    for (std::size_t i = 0; i < frames.size(); ++i) order.push_back(i);
    std::vector<std::size_t> sizes;
    for (const std::size_t i : order) sizes.push_back(frames[i].size());
    std::vector<UserDataParseResult> results(order.size(), UserDataParseResult::MalformedJson);
    std::vector<UserDataEventKind> kinds(order.size(), UserDataEventKind::Unknown);
    std::vector<std::size_t> calls_after(order.size(), 0);
    UserDataWsEvent ev{};
    std::size_t total = 0;
    {
        const alloc_probe::Window window;
        for (std::size_t n = 0; n < order.size(); ++n) {
            results[n] = parser.parse(frames[order[n]], ev);
            kinds[n] = ev.kind;
            calls_after[n] = window.calls();
        }
        total = window.calls();
    }
    std::size_t wrong = 0;
    for (std::size_t n = 0; n < order.size(); ++n) {
        if (results[n] != UserDataParseResult::Ok || kinds[n] != UserDataEventKind::ExecutionReport) ++wrong;
    }
    EXPECT_EQ(wrong, 0u) << "every frame is a valid executionReport followed by whitespace";
    EXPECT_EQ(frames.back().size(), kLimit);
    EXPECT_EQ(total, 0u) << total << " allocation(s) while parsing " << order.size() << " frames:" << describe(sizes, calls_after);
}

// Every way parse() can end, at its natural size and padded to the limit; plus the two frames that load the buffers hardest.
TEST(UserDataJsonParserAllocation, EveryOutcomeAllocatesNothingAtAnySize) {
    UserDataJsonParser parser;
    // The longest tape: 2 M numbers; the parser only reads "e" and "E", but stage 1 indexes every structural character of the 4 MiB.
    const std::string dense_numbers =
        repeated_to(R"({"e":"outboundAccountPosition","E":1,"B":[)", "1,", "1", "]}", kLimit);
    // The client order id is a ~4 MiB string, unescaped into simdjson's string buffer in full, and then ignored for being too long.
    const std::string coid_prefix = R"({"e":"executionReport","E":1,"c":")";
    const std::string coid_suffix = R"(","i":42})";
    const std::string huge_coid = coid_prefix + std::string(kLimit - coid_prefix.size() - coid_suffix.size(), 'A') + coid_suffix;
    ASSERT_EQ(dense_numbers.size(), kLimit);
    ASSERT_EQ(huge_coid.size(), kLimit);

    std::vector<UserDataCase> cases;
    const auto add_both = [&cases](const char* name, const std::string& body, UserDataParseResult expected, UserDataEventKind kind) {
        cases.push_back({name, body, expected, kind, false});
        cases.push_back({name, padded_to(body, 104'000), expected, kind, false});
        cases.push_back({name, padded_to(body, kLimit), expected, kind, false});
    };
    add_both("executionReport", kExecutionReport, UserDataParseResult::Ok, UserDataEventKind::ExecutionReport);
    add_both("outboundAccountPosition", kOutboundAccountPosition, UserDataParseResult::Ok, UserDataEventKind::OutboundAccountPosition);
    add_both("listenKeyExpired", kListenKeyExpired, UserDataParseResult::Ok, UserDataEventKind::ListenKeyExpired);
    add_both("unknown event type", kUnknownUserEvent, UserDataParseResult::Ok, UserDataEventKind::Unknown);
    add_both("no event type", kNoEventType, UserDataParseResult::MissingEventType, UserDataEventKind::Unknown);
    cases.push_back({"dense numbers", dense_numbers, UserDataParseResult::Ok, UserDataEventKind::OutboundAccountPosition, false});
    cases.push_back({"huge client order id", huge_coid, UserDataParseResult::Ok, UserDataEventKind::ExecutionReport, false});
    cases.push_back({"empty", "", UserDataParseResult::MalformedJson, UserDataEventKind::Unknown, false});
    cases.push_back({"not json", "this is not json at all", UserDataParseResult::MalformedJson, UserDataEventKind::Unknown, true});
    cases.push_back({"not json, padded to the limit", padded_to("this is not json at all", kLimit), UserDataParseResult::MalformedJson,
                     UserDataEventKind::Unknown, true});

    std::vector<UserDataParseResult> results(cases.size(), UserDataParseResult::Ok);
    std::vector<UserDataEventKind> kinds(cases.size(), UserDataEventKind::Unknown);
    std::vector<std::size_t> sizes;
    for (const UserDataCase& c : cases) sizes.push_back(c.body.size());
    std::vector<std::size_t> calls_after(cases.size(), 0);
    UserDataWsEvent ev{};
    std::size_t total = 0;
    {
        const alloc_probe::Window window;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            results[n] = parser.parse(cases[n].body, ev);
            kinds[n] = ev.kind;
            calls_after[n] = window.calls();
        }
        total = window.calls();
    }
    for (std::size_t n = 0; n < cases.size(); ++n) {
        if (cases[n].any_failure) {
            EXPECT_NE(results[n], UserDataParseResult::Ok) << cases[n].name;
        } else {
            EXPECT_EQ(results[n], cases[n].expected) << cases[n].name << " (" << cases[n].body.size() << " B)";
            EXPECT_EQ(kinds[n], cases[n].expected_kind) << cases[n].name << " (" << cases[n].body.size() << " B)";
        }
    }
    EXPECT_EQ(total, 0u) << total << " allocation(s) while parsing " << cases.size() << " frames:" << describe(sizes, calls_after);
}

// A body above the limit cannot come from a session (Beast refuses the message first); a direct caller gets it refused BEFORE it is copied,
// the output left empty, and the parser exactly as usable afterwards.
TEST(UserDataJsonParserAllocation, AFrameAboveTheLimitIsRefusedWithoutAllocatingAndTheParserSurvivesIt) {
    UserDataJsonParser parser;
    const std::string oversize = padded_to(kListenKeyExpired, kLimit + 1);
    const std::string at_limit = padded_to(kListenKeyExpired, kLimit);
    ASSERT_EQ(oversize.size(), kLimit + 1);
    UserDataWsEvent ev{};
    ev.kind = UserDataEventKind::ExecutionReport;  // dirty on purpose: a refusal must leave it empty, not as it was
    ev.event_time_ms = 5;
    UserDataParseResult refused = UserDataParseResult::Ok;
    UserDataEventKind kind_after_refusal = UserDataEventKind::ExecutionReport;
    std::int64_t time_after_refusal = 5;
    UserDataParseResult accepted = UserDataParseResult::MalformedJson;
    std::size_t calls = 0;
    {
        const alloc_probe::Window window;
        refused = parser.parse(oversize, ev);
        kind_after_refusal = ev.kind;
        time_after_refusal = ev.event_time_ms;
        accepted = parser.parse(at_limit, ev);
        calls = window.calls();
    }
    EXPECT_EQ(refused, UserDataParseResult::MalformedJson);
    EXPECT_EQ(kind_after_refusal, UserDataEventKind::Unknown);
    EXPECT_EQ(time_after_refusal, 0);
    EXPECT_EQ(accepted, UserDataParseResult::Ok) << "the parser is as usable after the refusal as before it";
    EXPECT_EQ(ev.kind, UserDataEventKind::ListenKeyExpired);
    EXPECT_EQ(calls, 0u) << calls << " allocation(s) around a refused frame and the next accepted one";
}

// "Refused before it is copied", checked the hard way: the bodies below are views over address space that cannot be read at all, so a parser
// that touched even the first byte of one would fault (access violation / SIGSEGV), where one that looks at the length first simply returns.
// A body one byte over the limit, and one twice the limit.
TEST(UserDataJsonParserAllocation, AFrameAboveTheLimitIsRefusedBeforeAnyByteOfItIsRead) {
    UserDataJsonParser parser;
    const hy_test::UnreadableRegion region(2 * kLimit);
    ASSERT_TRUE(region.ok());
    UserDataWsEvent ev{};
    const UserDataParseResult just_over = parser.parse(region.view(kLimit + 1), ev);
    const UserDataParseResult far_over = parser.parse(region.view(2 * kLimit), ev);
    EXPECT_EQ(just_over, UserDataParseResult::MalformedJson);
    EXPECT_EQ(far_over, UserDataParseResult::MalformedJson);
    EXPECT_EQ(ev.kind, UserDataEventKind::Unknown);
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

// "Refused before it is copied", checked the hard way: see UserDataJsonParserAllocation.AFrameAboveTheLimitIsRefusedBeforeAnyByteOfItIsRead.
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
