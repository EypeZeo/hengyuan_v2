// Private-stream migration M1: binance_ws_api_codec.hpp -- the response and event side of the spot WebSocket API.
//
// The fixtures are the official docs' own examples (ws_api_official_samples.hpp, generated from web-socket-api.md and
// user-data-stream.md); the variants built from them (a fill, a missing field, a long id, ...) say what they change. Pure parsing: no
// socket, no clock, no files.
#include <gtest/gtest.h>
#include <hengyuan/binance_ws_api_codec.hpp>

#include "ws_api_official_samples.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

using hy::ExecutionType;
using hy::ListOrderStatus;
using hy::OrderSide;
using hy::OrderState;
using hy::UserDataEventKind;
using hy::UserDataWsEvent;
using hy::WsApiCodec;
using hy::WsApiFrame;
using hy::WsApiFrameKind;
using hy::WsApiParseResult;
namespace samples = hy::test_samples;

// --- allocation probe ---------------------------------------------------------------------------------------------------------------
// The codec's design is "no heap after construction" (its simdjson parser memory and its padded copy buffer are sized once, in the
// constructor). WsApiCodecAllocation below turns that sentence into a measurement: this binary's global allocation functions are replaced
// by counting ones, and a window is open only around parse() calls -- never around gtest's own bookkeeping. What is counted is operator
// new / new[] in every form, which is how this codebase defines "heap" on a hot path (CLAUDE.md, Engineering Mandate 1); a raw malloc()
// called from inside a library would not be seen.
namespace alloc_probe {

std::atomic<bool> g_open{false};
std::atomic<std::size_t> g_calls{0};

inline void note() noexcept {
    if (g_open.load(std::memory_order_relaxed)) g_calls.fetch_add(1, std::memory_order_relaxed);
}

inline void* plain_alloc(std::size_t n) noexcept { return std::malloc(n != 0 ? n : 1); }

#if defined(_MSC_VER)
inline void* aligned_alloc_impl(std::size_t n, std::size_t align) noexcept { return _aligned_malloc(n != 0 ? n : 1, align); }
inline void aligned_free_impl(void* p) noexcept { _aligned_free(p); }
#else
inline void* aligned_alloc_impl(std::size_t n, std::size_t align) noexcept {
    void* p = nullptr;
    return ::posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, n != 0 ? n : 1) == 0 ? p : nullptr;
}
inline void aligned_free_impl(void* p) noexcept { std::free(p); }
#endif

class Window {
public:
    Window() noexcept {
        g_calls.store(0, std::memory_order_relaxed);
        g_open.store(true, std::memory_order_relaxed);
    }
    ~Window() { g_open.store(false, std::memory_order_relaxed); }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    [[nodiscard]] std::size_t calls() const noexcept { return g_calls.load(std::memory_order_relaxed); }
};

}  // namespace alloc_probe

// GCC 14's -Wmismatched-new-delete cannot see that BOTH halves of this pair are replaced: when a replacement operator delete is inlined into
// code whose pointer came from operator new, it reports the std::free() inside as a mismatch. So the replacements are never inlined
// (a standalone delete body sees a pointer of unknown origin), and the block carries a diagnostic fence as well.
#if defined(_MSC_VER)
#define HY_PROBE_NOINLINE
#else
#define HY_PROBE_NOINLINE __attribute__((noinline))
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

HY_PROBE_NOINLINE void* operator new(std::size_t n) {
    alloc_probe::note();
    if (void* p = alloc_probe::plain_alloc(n)) return p;
    throw std::bad_alloc();
}
HY_PROBE_NOINLINE void* operator new[](std::size_t n) { return ::operator new(n); }
HY_PROBE_NOINLINE void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    alloc_probe::note();
    return alloc_probe::plain_alloc(n);
}
HY_PROBE_NOINLINE void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return ::operator new(n, std::nothrow); }
HY_PROBE_NOINLINE void* operator new(std::size_t n, std::align_val_t align) {
    alloc_probe::note();
    if (void* p = alloc_probe::aligned_alloc_impl(n, static_cast<std::size_t>(align))) return p;
    throw std::bad_alloc();
}
HY_PROBE_NOINLINE void* operator new[](std::size_t n, std::align_val_t align) { return ::operator new(n, align); }
HY_PROBE_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
HY_PROBE_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
HY_PROBE_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
HY_PROBE_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
HY_PROBE_NOINLINE void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
HY_PROBE_NOINLINE void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
HY_PROBE_NOINLINE void operator delete(void* p, std::align_val_t) noexcept { alloc_probe::aligned_free_impl(p); }
HY_PROBE_NOINLINE void operator delete[](void* p, std::align_val_t) noexcept { alloc_probe::aligned_free_impl(p); }
HY_PROBE_NOINLINE void operator delete(void* p, std::size_t, std::align_val_t) noexcept { alloc_probe::aligned_free_impl(p); }
HY_PROBE_NOINLINE void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { alloc_probe::aligned_free_impl(p); }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

// `text` with exactly one occurrence of `from` replaced by `to` (the test fails if there is not exactly one).
std::string replace_once(std::string_view text, std::string_view from, std::string_view to) {
    std::string out(text);
    const auto pos = out.find(from);
    EXPECT_NE(pos, std::string::npos) << "pattern not found: " << from;
    EXPECT_EQ(out.find(from, pos == std::string::npos ? 0 : pos + 1), std::string::npos) << "pattern found twice: " << from;
    if (pos != std::string::npos) out.replace(pos, from.size(), to);
    return out;
}

struct Parsed {
    WsApiParseResult result;
    WsApiFrame frame;
};

Parsed parse_with(WsApiCodec& codec, std::string_view body) {
    Parsed p{};
    p.result = codec.parse(body, p.frame);
    return p;
}

Parsed parse_fresh(std::string_view body) {
    WsApiCodec codec;
    return parse_with(codec, body);
}

// A compact, comparable rendering of the parts of a frame the tests care about.
std::string summary(const Parsed& p) {
    std::string s = "result=" + std::to_string(static_cast<int>(p.result)) + " kind=" + std::to_string(static_cast<int>(p.frame.kind));
    if (p.frame.kind == WsApiFrameKind::Event) {
        const UserDataWsEvent& e = p.frame.event;
        s += " ek=" + std::to_string(static_cast<int>(e.kind)) + " sub=" + std::to_string(e.subscription_id) +
             " E=" + std::to_string(e.event_time_ms) + " T=" + std::to_string(e.transaction_time_ms) +
             " i=" + std::to_string(e.exchange_order_id) + " t=" + std::to_string(e.trade_id) + " g=" + std::to_string(e.order_list_id) +
             " coid=" + std::string(e.coid.view()) + " z=" + e.cumulative_filled_qty_raw + " n=" + e.commission_raw +
             " N=" + e.commission_asset + " a=" + e.asset + " d=" + e.delta_raw;
    } else if (p.frame.kind == WsApiFrameKind::Response) {
        const hy::WsApiResponse& r = p.frame.response;
        s += " id=" + std::to_string(r.id) + (r.id_known ? "k" : "u") + " st=" + std::to_string(r.status) +
             " res=" + std::to_string(r.has_result) + " err=" + std::to_string(r.has_error) + " code=" + std::to_string(r.error_code) +
             " retry=" + std::to_string(r.retry_after_ms) + " sub=" + std::to_string(r.has_subscription_id) + ":" +
             std::to_string(r.subscription_id) + " list=" + std::to_string(r.has_subscription_list) + ":" +
             std::to_string(r.subscription_count) + (r.subscription_list_truncated ? "T" : "");
    }
    return s;
}

}  // namespace

// --- the official examples ---

TEST(WsApiCodecEvents, TheOfficialExecutionReportExampleIsReadFieldByField) {
    const Parsed p = parse_fresh(samples::kEventExecutionReport);
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    ASSERT_EQ(p.frame.kind, WsApiFrameKind::Event);
    const UserDataWsEvent& e = p.frame.event;
    EXPECT_EQ(e.kind, UserDataEventKind::ExecutionReport);
    EXPECT_EQ(e.subscription_id, 0);
    EXPECT_EQ(e.generation, 0u) << "the generation is stamped by the session, never by the parser";
    EXPECT_EQ(e.event_time_ms, 1499405658658LL);
    EXPECT_EQ(e.coid.view(), "mUvoqJxFIILMdfAW5iGSOW");
    EXPECT_EQ(e.exchange_order_id, 4293153);
    EXPECT_TRUE(e.side_known);
    EXPECT_EQ(e.side, OrderSide::Buy);
    EXPECT_TRUE(e.tif_is_gtc);
    EXPECT_STREQ(e.order_qty_raw, "1.00000000");
    EXPECT_STREQ(e.order_price_raw, "0.10264410");
    EXPECT_EQ(e.exec_type, ExecutionType::New);
    EXPECT_TRUE(e.order_status_known);
    EXPECT_EQ(e.order_status, OrderState::Accepted);
    EXPECT_STREQ(e.last_qty_raw, "0.00000000");
    EXPECT_STREQ(e.cumulative_filled_qty_raw, "0.00000000");
    EXPECT_STREQ(e.last_price_raw, "0.00000000");
    EXPECT_STREQ(e.cumulative_quote_qty_raw, "0.00000000");
    EXPECT_STREQ(e.commission_raw, "0");
    EXPECT_STREQ(e.commission_asset, "") << "\"N\":null means no commission asset";
    EXPECT_EQ(e.transaction_time_ms, 1499405658657LL);
    EXPECT_EQ(e.trade_id, -1) << "the exchange's own -1 for an event that is not a fill";
}

TEST(WsApiCodecEvents, AFillCarriesItsCommissionAssetAndTradeId) {
    // The official example turned into a partial fill: only the values that a fill changes.
    std::string body(samples::kEventExecutionReport);
    body = replace_once(body, R"("x":"NEW","X":"NEW")", R"("x":"TRADE","X":"PARTIALLY_FILLED")");
    body = replace_once(body, R"("l":"0.00000000","z":"0.00000000","L":"0.00000000")", R"("l":"0.50000000","z":"0.50000000","L":"0.10264410")");
    body = replace_once(body, R"("n":"0","N":null)", R"("n":"0.00000100","N":"BNB")");
    body = replace_once(body, R"("t":-1)", R"("t":987654)");
    body = replace_once(body, R"("Z":"0.00000000")", R"("Z":"0.05132205")");
    const Parsed p = parse_fresh(body);
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    const UserDataWsEvent& e = p.frame.event;
    EXPECT_EQ(e.kind, UserDataEventKind::ExecutionReport);
    EXPECT_EQ(e.exec_type, ExecutionType::Trade);
    EXPECT_EQ(e.order_status, OrderState::PartialFill);
    EXPECT_STREQ(e.last_qty_raw, "0.50000000");
    EXPECT_STREQ(e.cumulative_filled_qty_raw, "0.50000000");
    EXPECT_STREQ(e.last_price_raw, "0.10264410");
    EXPECT_STREQ(e.cumulative_quote_qty_raw, "0.05132205");
    EXPECT_STREQ(e.commission_raw, "0.00000100");
    EXPECT_STREQ(e.commission_asset, "BNB");
    EXPECT_EQ(e.trade_id, 987654);
}

TEST(WsApiCodecEvents, TheOtherOfficialEventExamplesAreRecognizedWithTheirFields) {
    {
        const Parsed p = parse_fresh(samples::kEventOutboundAccountPosition);
        ASSERT_EQ(p.result, WsApiParseResult::Ok);
        EXPECT_EQ(p.frame.event.kind, UserDataEventKind::OutboundAccountPosition);
        EXPECT_EQ(p.frame.event.event_time_ms, 1564034571105LL);
        EXPECT_EQ(p.frame.event.transaction_time_ms, 1564034571073LL) << "\"u\", the time of the last account update";
        EXPECT_EQ(p.frame.event.subscription_id, 0);
    }
    {
        const Parsed p = parse_fresh(samples::kEventTwoBalances);
        ASSERT_EQ(p.result, WsApiParseResult::Ok);
        EXPECT_EQ(p.frame.event.kind, UserDataEventKind::OutboundAccountPosition);
        EXPECT_EQ(p.frame.event.event_time_ms, 1728972148778LL);
    }
    {
        const Parsed p = parse_fresh(samples::kEventBalanceUpdate);
        ASSERT_EQ(p.result, WsApiParseResult::Ok);
        EXPECT_EQ(p.frame.event.kind, UserDataEventKind::BalanceUpdate);
        EXPECT_STREQ(p.frame.event.asset, "BTC");
        EXPECT_STREQ(p.frame.event.delta_raw, "100.00000000");
        EXPECT_EQ(p.frame.event.event_time_ms, 1573200697110LL);
        EXPECT_EQ(p.frame.event.transaction_time_ms, 1573200697068LL) << "\"T\", the clear time";
    }
    {
        const Parsed p = parse_fresh(samples::kEventExternalLockUpdate);
        ASSERT_EQ(p.result, WsApiParseResult::Ok);
        EXPECT_EQ(p.frame.event.kind, UserDataEventKind::ExternalLockUpdate);
        EXPECT_STREQ(p.frame.event.asset, "NEO");
        EXPECT_STREQ(p.frame.event.delta_raw, "10.00000000");
        EXPECT_EQ(p.frame.event.transaction_time_ms, 1581557507268LL);
    }
    {
        const Parsed p = parse_fresh(samples::kEventListStatus);
        ASSERT_EQ(p.result, WsApiParseResult::Ok);
        EXPECT_EQ(p.frame.event.kind, UserDataEventKind::ListStatus);
        EXPECT_EQ(p.frame.event.order_list_id, 2);
        EXPECT_EQ(p.frame.event.list_order_status, ListOrderStatus::Executing);
        EXPECT_EQ(p.frame.event.transaction_time_ms, 1564035303625LL);
    }
    {
        const Parsed p = parse_fresh(samples::kEventEventStreamTerminated);
        ASSERT_EQ(p.result, WsApiParseResult::Ok);
        EXPECT_EQ(p.frame.event.kind, UserDataEventKind::EventStreamTerminated);
        EXPECT_EQ(p.frame.event.subscription_id, 0) << "it names the subscription that ended";
        EXPECT_EQ(p.frame.event.event_time_ms, 1728973001334LL);
    }
}

TEST(WsApiCodecEvents, ServerShutdownIsAConnectionEventWithNoSubscription) {
    const Parsed p = parse_fresh(samples::kEventServerShutdown);
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    ASSERT_EQ(p.frame.kind, WsApiFrameKind::Event);
    EXPECT_EQ(p.frame.event.kind, UserDataEventKind::ServerShutdown);
    EXPECT_EQ(p.frame.event.subscription_id, -1) << "blueprint 5.7 P-29: this frame has no subscriptionId, and that is not an error";
    EXPECT_EQ(p.frame.event.event_time_ms, 1770123456789LL);
}

TEST(WsApiCodecEvents, ASubscriptionIdIsReadWhenThereIsOneAndIgnoredWhenItIsNotAValidId) {
    const Parsed with = parse_fresh(replace_once(samples::kEventServerShutdown, R"({"event")", R"({"subscriptionId":7,"event")"));
    ASSERT_EQ(with.result, WsApiParseResult::Ok);
    EXPECT_EQ(with.frame.event.subscription_id, 7) << "if the exchange ever puts one on it, it is kept";

    const Parsed none = parse_fresh(replace_once(samples::kEventExecutionReport, R"("subscriptionId":0,)", ""));
    ASSERT_EQ(none.result, WsApiParseResult::Ok);
    EXPECT_EQ(none.frame.event.subscription_id, -1);
    EXPECT_EQ(none.frame.event.kind, UserDataEventKind::ExecutionReport);

    const Parsed negative = parse_fresh(replace_once(samples::kEventExecutionReport, R"("subscriptionId":0)", R"("subscriptionId":-5)"));
    ASSERT_EQ(negative.result, WsApiParseResult::Ok);
    EXPECT_EQ(negative.frame.event.subscription_id, -1) << "subscription ids are non-negative; anything else is no id";

    const Parsed text = parse_fresh(replace_once(samples::kEventExecutionReport, R"("subscriptionId":0)", R"("subscriptionId":"0")"));
    ASSERT_EQ(text.result, WsApiParseResult::Ok);
    EXPECT_EQ(text.frame.event.subscription_id, -1);
}

TEST(WsApiCodecEvents, ASubscriptionIdAfterTheEventIsStillFound) {
    // JSON objects are unordered; the parser must not depend on the exchange's field order.
    std::string_view original = samples::kEventBalanceUpdate;
    const std::string swapped = replace_once(replace_once(original, R"("subscriptionId":0,)", ""), R"(}})", R"(},"subscriptionId":3})");
    const Parsed p = parse_fresh(swapped);
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    EXPECT_EQ(p.frame.event.kind, UserDataEventKind::BalanceUpdate);
    EXPECT_EQ(p.frame.event.subscription_id, 3);
}

TEST(WsApiCodecEvents, AnUnknownEventTypeIsAcceptedAndNotActedOn) {
    const Parsed p = parse_fresh(R"({"subscriptionId":0,"event":{"e":"somethingNew","E":42}})");
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    EXPECT_EQ(p.frame.kind, WsApiFrameKind::Event);
    EXPECT_EQ(p.frame.event.kind, UserDataEventKind::Unknown);
    EXPECT_EQ(p.frame.event.event_time_ms, 42);
}

TEST(WsApiCodecEvents, AnEventWithoutATypeIsRefused) {
    EXPECT_EQ(parse_fresh(R"({"subscriptionId":0,"event":{"E":42}})").result, WsApiParseResult::MissingEventType);
    EXPECT_EQ(parse_fresh(R"({"subscriptionId":0,"event":{"e":5,"E":42}})").result, WsApiParseResult::MissingEventType);
    EXPECT_EQ(parse_fresh(R"({"subscriptionId":0,"event":{}})").result, WsApiParseResult::MissingEventType);
}

// --- an executionReport that cannot be trusted is not returned, it is reported as lost ---

TEST(WsApiCodecEvents, AnExecutionReportWithoutAUsableOrderIdOrCumulativeQuantityIsReportedIncomplete) {
    const std::string_view base = samples::kEventExecutionReport;
    const std::string cases[] = {
        replace_once(base, R"("c":"mUvoqJxFIILMdfAW5iGSOW",)", ""),                            // no "c"
        replace_once(base, R"("c":"mUvoqJxFIILMdfAW5iGSOW")", R"("c":"")"),                    // an empty id
        replace_once(base, R"("c":"mUvoqJxFIILMdfAW5iGSOW")", R"("c":123)"),                   // not a string
        replace_once(base, R"("c":"mUvoqJxFIILMdfAW5iGSOW")", R"("c":null)"),
        replace_once(base, R"("c":"mUvoqJxFIILMdfAW5iGSOW")", R"("c":"abcdefghijklmnopqrstuvwxyzABCDEFGHIJK")"),  // 37 characters
        replace_once(base, R"("c":"mUvoqJxFIILMdfAW5iGSOW")", R"("c":"abc\u0000def")"),        // an embedded NUL: view() would cut it
        replace_once(base, R"("c":"mUvoqJxFIILMdfAW5iGSOW")", R"("c":"abc def")"),             // a character no order id has
        replace_once(base, R"("c":"mUvoqJxFIILMdfAW5iGSOW")", R"("c":"abc\"def")"),
        replace_once(base, R"("z":"0.00000000",)", ""),                                        // no "z"
        replace_once(base, R"("z":"0.00000000")", R"("z":"")"),
        replace_once(base, R"("z":"0.00000000")", R"("z":0)"),
        replace_once(base, R"("z":"0.00000000")", R"("z":"123456789012345678901234")"),        // 24 characters: does not fit
    };
    for (const std::string& body : cases) {
        const Parsed p = parse_fresh(body);
        EXPECT_EQ(p.result, WsApiParseResult::ExecutionReportIncomplete) << body;
        EXPECT_EQ(p.frame.kind, WsApiFrameKind::Unknown) << "never a partially filled frame: " << body;
        EXPECT_EQ(p.frame.event.kind, UserDataEventKind::Unknown) << body;
        EXPECT_TRUE(p.frame.event.coid.empty()) << body;
    }
}

TEST(WsApiCodecEvents, AnOrderIdAtTheLengthLimitAndWithEveryAllowedCharacterIsAccepted) {
    const std::string id36 = "A.b_c:d/e-fGHIJKLMNOPQRSTUVWXYZ01234";  // 36 characters from [A-Za-z0-9._:/-]
    ASSERT_EQ(id36.size(), 36u);
    const Parsed p = parse_fresh(replace_once(samples::kEventExecutionReport, "mUvoqJxFIILMdfAW5iGSOW", id36));
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    EXPECT_EQ(p.frame.event.coid.view(), id36);

    const Parsed ours = parse_fresh(replace_once(samples::kEventExecutionReport, "mUvoqJxFIILMdfAW5iGSOW", "HY-1760000000000-12-00AB"));
    ASSERT_EQ(ours.result, WsApiParseResult::Ok);
    EXPECT_EQ(ours.frame.event.coid.view(), "HY-1760000000000-12-00AB");
}

TEST(WsApiCodecEvents, OptionalExecutionReportFieldsThatAreMissingStayAtTheirDefaults) {
    // Only "c" and "z" are load-bearing; everything else is best effort, exactly as in the old parser.
    const Parsed p = parse_fresh(R"({"subscriptionId":2,"event":{"e":"executionReport","c":"X1","z":"1.5"}})");
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    const UserDataWsEvent& e = p.frame.event;
    EXPECT_EQ(e.kind, UserDataEventKind::ExecutionReport);
    EXPECT_EQ(e.subscription_id, 2);
    EXPECT_FALSE(e.side_known);
    EXPECT_FALSE(e.order_status_known);
    EXPECT_EQ(e.exec_type, ExecutionType::Unknown);
    EXPECT_EQ(e.exchange_order_id, 0);
    EXPECT_EQ(e.trade_id, -1);
    EXPECT_STREQ(e.commission_raw, "");
    EXPECT_STREQ(e.commission_asset, "");
}

TEST(WsApiCodecEvents, ADecimalThatDoesNotFitItsBufferIsLeftEmptyNotTruncated) {
    const Parsed p = parse_fresh(replace_once(samples::kEventExecutionReport, R"("l":"0.00000000")", R"("l":"1234567890123456789012345")"));
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    EXPECT_STREQ(p.frame.event.last_qty_raw, "");
    const Parsed fits = parse_fresh(replace_once(samples::kEventExecutionReport, R"("l":"0.00000000")", R"("l":"12345678901234567890123")"));
    ASSERT_EQ(fits.result, WsApiParseResult::Ok);
    EXPECT_STREQ(fits.frame.event.last_qty_raw, "12345678901234567890123") << "23 characters + the NUL is the largest that fits";
}

// --- responses ---

TEST(WsApiCodecResponses, TheOfficialSubscribeResponseGivesTheSubscriptionId) {
    const Parsed p = parse_fresh(samples::kResponseSubscribeOk);
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    ASSERT_EQ(p.frame.kind, WsApiFrameKind::Response);
    const hy::WsApiResponse& r = p.frame.response;
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.has_result);
    EXPECT_FALSE(r.has_error);
    EXPECT_TRUE(r.has_subscription_id);
    EXPECT_EQ(r.subscription_id, 0);
    EXPECT_FALSE(r.has_subscription_list);
    EXPECT_FALSE(r.id_known) << "the example's id is a UUID string; ours are integers, and a string is not one";
    EXPECT_TRUE(r.ok());
}

TEST(WsApiCodecResponses, TheOfficialSessionSubscriptionsResponseGivesTheList) {
    const Parsed p = parse_fresh(samples::kResponseSubscriptionsList);
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    const hy::WsApiResponse& r = p.frame.response;
    EXPECT_TRUE(r.has_result);
    EXPECT_TRUE(r.has_subscription_list);
    EXPECT_FALSE(r.has_subscription_id);
    ASSERT_EQ(r.subscription_count, 2u);
    EXPECT_EQ(r.subscription_ids[0], 0);
    EXPECT_EQ(r.subscription_ids[1], 1);
    EXPECT_FALSE(r.subscription_list_truncated);
    EXPECT_TRUE(r.ok());
}

TEST(WsApiCodecResponses, AnEmptySubscriptionListIsAListWithNothingInIt) {
    const Parsed p = parse_fresh(R"({"id":9,"status":200,"result":[]})");
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    EXPECT_TRUE(p.frame.response.has_subscription_list);
    EXPECT_EQ(p.frame.response.subscription_count, 0u);
    EXPECT_TRUE(p.frame.response.id_known);
    EXPECT_EQ(p.frame.response.id, 9u);
}

TEST(WsApiCodecResponses, AListLongerThanTheCapacityIsFlaggedNotStored) {
    std::string body = R"({"id":1,"status":200,"result":[)";
    for (int i = 0; i < 20; ++i) {
        if (i > 0) body += ',';
        body += R"({"subscriptionId":)" + std::to_string(i) + "}";
    }
    body += "]}";
    const Parsed p = parse_fresh(body);
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    EXPECT_TRUE(p.frame.response.subscription_list_truncated);
    EXPECT_EQ(p.frame.response.subscription_count, hy::kMaxListedSubscriptions);
    EXPECT_EQ(p.frame.response.subscription_ids[hy::kMaxListedSubscriptions - 1], static_cast<std::int64_t>(hy::kMaxListedSubscriptions) - 1);
}

TEST(WsApiCodecResponses, ElementsThatAreNotSubscriptionsAreSkipped) {
    const Parsed p = parse_fresh(R"({"id":1,"status":200,"result":[5,{"x":1},{"subscriptionId":4},"s",{"subscriptionId":"no"},{"subscriptionId":6}]})");
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    ASSERT_EQ(p.frame.response.subscription_count, 2u);
    EXPECT_EQ(p.frame.response.subscription_ids[0], 4);
    EXPECT_EQ(p.frame.response.subscription_ids[1], 6);
}

TEST(WsApiCodecResponses, TheOfficialErrorResponseGivesStatusAndCodeAndNoMessage) {
    const Parsed p = parse_fresh(samples::kResponseError400);
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    const hy::WsApiResponse& r = p.frame.response;
    EXPECT_EQ(r.status, 400);
    EXPECT_TRUE(r.has_error);
    EXPECT_FALSE(r.has_result);
    EXPECT_EQ(r.error_code, -2010);
    EXPECT_EQ(r.retry_after_ms, 0);
    EXPECT_FALSE(r.ok());
}

TEST(WsApiCodecResponses, ABanCarriesItsAbsoluteRetryAfterTimestamp) {
    const Parsed p = parse_fresh(samples::kResponseBanned418);
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    EXPECT_EQ(p.frame.response.status, 418);
    EXPECT_EQ(p.frame.response.error_code, -1003);
    EXPECT_EQ(p.frame.response.retry_after_ms, 1659146400000LL) << "an absolute epoch-ms timestamp, not a duration";
}

TEST(WsApiCodecResponses, AnIntegerIdIsEchoedBackAndAnythingElseIsUnknown) {
    EXPECT_TRUE(parse_fresh(R"({"id":7,"status":200,"result":{}})").frame.response.id_known);
    EXPECT_EQ(parse_fresh(R"({"id":7,"status":200,"result":{}})").frame.response.id, 7u);
    EXPECT_FALSE(parse_fresh(R"({"id":null,"status":200,"result":{}})").frame.response.id_known);
    EXPECT_FALSE(parse_fresh(R"({"id":"7","status":200,"result":{}})").frame.response.id_known);
    EXPECT_FALSE(parse_fresh(R"({"id":-7,"status":200,"result":{}})").frame.response.id_known);
    EXPECT_FALSE(parse_fresh(R"({"id":7.5,"status":200,"result":{}})").frame.response.id_known);
    EXPECT_FALSE(parse_fresh(R"({"status":200,"result":{}})").frame.response.id_known);
    EXPECT_TRUE(parse_fresh(R"({"id":9007199254740991,"status":200,"result":{}})").frame.response.id_known);
}

TEST(WsApiCodecResponses, AStatusThatIsNotAnHttpStatusMakesTheFrameMalformed) {
    EXPECT_EQ(parse_fresh(R"({"id":1,"status":99,"result":{}})").result, WsApiParseResult::MalformedJson);
    EXPECT_EQ(parse_fresh(R"({"id":1,"status":600,"result":{}})").result, WsApiParseResult::MalformedJson);
    EXPECT_EQ(parse_fresh(R"({"id":1,"status":-1,"result":{}})").result, WsApiParseResult::MalformedJson);
    EXPECT_EQ(parse_fresh(R"({"id":1,"status":100,"result":{}})").result, WsApiParseResult::Ok);
    EXPECT_EQ(parse_fresh(R"({"id":1,"status":599,"result":{}})").result, WsApiParseResult::Ok);
}

TEST(WsApiCodecResponses, AResponseNeedsBothAStatusOfTheRightTypeAndOkIsMoreThan200) {
    EXPECT_EQ(parse_fresh(R"({"id":1,"status":"200","result":{}})").result, WsApiParseResult::UnrecognizedFrame);
    EXPECT_FALSE(parse_fresh(R"({"id":1,"status":200})").frame.response.ok()) << "no result";
    EXPECT_FALSE(parse_fresh(R"({"id":1,"status":200,"result":{},"error":{"code":-1}})").frame.response.ok()) << "an error beside the result";
    EXPECT_FALSE(parse_fresh(R"({"id":1,"status":503,"result":{}})").frame.response.ok());
}

// --- what is not a frame ---

TEST(WsApiCodecFraming, NothingThatIsNotAJsonObjectIsAFrame) {
    for (const std::string_view body : {std::string_view(""), std::string_view("   "), std::string_view("[]"), std::string_view("[{\"event\":{}}]"),
                                        std::string_view("123"), std::string_view("null"), std::string_view("\"x\""), std::string_view("{"),
                                        std::string_view("{\"event\":"), std::string_view("not json"), std::string_view("{\"a\":1,}"),
                                        std::string_view("{'a':1}")}) {
        const Parsed p = parse_fresh(body);
        EXPECT_EQ(p.result, WsApiParseResult::MalformedJson) << "[" << body << "]";
        EXPECT_EQ(p.frame.kind, WsApiFrameKind::Unknown) << "[" << body << "]";
    }
}

TEST(WsApiCodecFraming, AnObjectThatIsNeitherAnEventNorAResponseIsUnrecognized) {
    for (const std::string_view body :
         {std::string_view("{}"), std::string_view(R"({"foo":1})"), std::string_view(R"({"event":5})"), std::string_view(R"({"event":[]})"),
          std::string_view(R"({"event":null,"result":{}})"), std::string_view(R"({"id":1,"result":{}})"), std::string_view(R"({"subscriptionId":1})")}) {
        const Parsed p = parse_fresh(body);
        EXPECT_EQ(p.result, WsApiParseResult::UnrecognizedFrame) << body;
        EXPECT_EQ(p.frame.kind, WsApiFrameKind::Unknown) << body;
    }
}

TEST(WsApiCodecFraming, AnEventObjectWinsOverAStatus) {
    const Parsed p = parse_fresh(R"({"status":200,"event":{"e":"serverShutdown","E":1}})");
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    EXPECT_EQ(p.frame.kind, WsApiFrameKind::Event);
}

TEST(WsApiCodecFraming, TheFrameCapIsExactAndOversizeIsRefusedBeforeAnythingIsCopied) {
    EXPECT_EQ(hy::kWsApiMaxMessageBytes, 64u * 1024u);
    std::string body(samples::kEventServerShutdown);
    body.resize(hy::kWsApiMaxMessageBytes, ' ');  // valid JSON followed by whitespace, exactly at the cap
    EXPECT_EQ(parse_fresh(body).result, WsApiParseResult::Ok);
    body.push_back(' ');
    const Parsed over = parse_fresh(body);
    EXPECT_EQ(over.result, WsApiParseResult::FrameTooLarge);
    EXPECT_EQ(over.frame.kind, WsApiFrameKind::Unknown);
}

TEST(WsApiCodecFraming, AFailedParseLeavesTheFrameAsItFoundItEmpty) {
    WsApiCodec codec;
    WsApiFrame frame{};
    ASSERT_EQ(codec.parse(samples::kEventExecutionReport, frame), WsApiParseResult::Ok);
    ASSERT_EQ(frame.kind, WsApiFrameKind::Event);
    // The next frame is garbage: the previous contents must not survive into the failure.
    EXPECT_EQ(codec.parse("not json", frame), WsApiParseResult::MalformedJson);
    EXPECT_EQ(frame.kind, WsApiFrameKind::Unknown);
    EXPECT_EQ(frame.event.kind, UserDataEventKind::Unknown);
    EXPECT_TRUE(frame.event.coid.empty());
    EXPECT_EQ(frame.response.status, 0);
}

// --- robustness ---

namespace {
const std::vector<std::string_view>& all_samples() {
    static const std::vector<std::string_view> v = {
        samples::kEventOutboundAccountPosition, samples::kEventBalanceUpdate,    samples::kEventExecutionReport,
        samples::kEventListStatus,              samples::kEventEventStreamTerminated, samples::kEventExternalLockUpdate,
        samples::kEventServerShutdown,          samples::kEventTwoBalances,      samples::kResponseSubscribeOk,
        samples::kResponseSubscriptionsList,    samples::kResponseError400,      samples::kResponseBanned418};
    return v;
}
}  // namespace

TEST(WsApiCodecRobustness, NoStrictPrefixOfAnOfficialFrameIsAFrame) {
    // A complete JSON object has its closing brace last, so every shorter prefix is invalid. Run under ASan, this also proves the parser
    // never reads past the bytes it was given.
    WsApiCodec codec;
    for (const std::string_view sample : all_samples()) {
        for (std::size_t len = 0; len < sample.size(); ++len) {
            const Parsed p = parse_with(codec, sample.substr(0, len));
            ASSERT_NE(p.result, WsApiParseResult::Ok) << "prefix of " << len << " bytes of " << sample;
            ASSERT_EQ(p.frame.kind, WsApiFrameKind::Unknown) << len;
        }
        EXPECT_EQ(parse_with(codec, sample).result, WsApiParseResult::Ok) << sample;
    }
}

TEST(WsApiCodecRobustness, EveryByteReplacedByHostileOnesNeverBreaksTheContractOfAFrame) {
    // For every position of every official frame, put each of a few hostile bytes there. Whatever comes out, it is deterministic (a second
    // codec agrees with the reused one), and an accepted executionReport still has a valid order id and a cumulative quantity.
    const char hostile[] = {'\0', '"', '\\', '{', '}', '[', ']', ':', ',', 'x', '\xFF', '\n'};
    WsApiCodec reused;
    for (const std::string_view sample : all_samples()) {
        std::string mutated(sample);
        for (std::size_t pos = 0; pos < sample.size(); ++pos) {
            const char original = mutated[pos];
            for (const char b : hostile) {
                mutated[pos] = b;
                const Parsed a = parse_with(reused, mutated);
                const Parsed fresh = parse_fresh(mutated);
                ASSERT_EQ(summary(a), summary(fresh)) << "position " << pos << " byte " << static_cast<int>(static_cast<unsigned char>(b));
                if (a.result == WsApiParseResult::Ok && a.frame.kind == WsApiFrameKind::Event &&
                    a.frame.event.kind == UserDataEventKind::ExecutionReport) {
                    ASSERT_TRUE(hy::detail::is_valid_ws_client_order_id(a.frame.event.coid.view())) << mutated;
                    ASSERT_NE(a.frame.event.cumulative_filled_qty_raw[0], '\0') << mutated;
                }
                if (a.result != WsApiParseResult::Ok) {
                    ASSERT_EQ(a.frame.kind, WsApiFrameKind::Unknown) << mutated;
                }
            }
            mutated[pos] = original;
        }
    }
}

TEST(WsApiCodecRobustness, OneCodecReusedForManyFramesGivesWhatAFreshOneWould) {
    WsApiCodec reused;
    const auto& v = all_samples();
    for (int round = 0; round < 3; ++round) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            const std::string_view body = v[(i * 5 + static_cast<std::size_t>(round)) % v.size()];
            EXPECT_EQ(summary(parse_with(reused, body)), summary(parse_fresh(body))) << body;
            // and a failure in between must not poison the next success
            EXPECT_EQ(parse_with(reused, "{\"event\":").result, WsApiParseResult::MalformedJson);
        }
    }
}

TEST(WsApiCodecRobustness, DeeplyNestedAndHugeNumbersAreHandledWithoutIncident) {
    std::string deep = R"({"subscriptionId":0,"event":{"e":"balanceUpdate","x":)";
    for (int i = 0; i < 500; ++i) deep += "[";
    for (int i = 0; i < 500; ++i) deep += "]";
    deep += "}}";
    const Parsed p = parse_fresh(deep);
    EXPECT_TRUE(p.result == WsApiParseResult::Ok || p.result == WsApiParseResult::MalformedJson);

    // A number that fits no 64-bit integer makes the whole document invalid for a DOM parse: the frame is refused, not read around.
    const Parsed big = parse_fresh(R"({"subscriptionId":99999999999999999999,"event":{"e":"serverShutdown","E":99999999999999999999}})");
    EXPECT_EQ(big.result, WsApiParseResult::MalformedJson);
    EXPECT_EQ(big.frame.kind, WsApiFrameKind::Unknown);

    // ... while an int64 maximum is read as it is.
    const Parsed max = parse_fresh(R"({"subscriptionId":9223372036854775807,"event":{"e":"serverShutdown","E":9223372036854775807}})");
    ASSERT_EQ(max.result, WsApiParseResult::Ok);
    EXPECT_EQ(max.frame.event.subscription_id, 9223372036854775807LL);
    EXPECT_EQ(max.frame.event.event_time_ms, 9223372036854775807LL);
}

// --- the allocation claim ---

namespace {
volatile char* volatile g_probe_sink = nullptr;  // volatile: the compiler may not drop the block it holds
}  // namespace

TEST(WsApiCodecAllocation, TheProbeSeesTheAllocationsItIsMeantToSee) {
    // The positive control. If the counting functions were not the ones this binary links, or the window never opened, the test below
    // would pass for the wrong reason. (simdjson allocates through the nothrow array form.)
    std::size_t after_array = 0;
    std::size_t after_nothrow = 0;
    {
        const alloc_probe::Window window;
        g_probe_sink = new char[100];
        g_probe_sink[0] = 1;
        after_array = window.calls();
        delete[] const_cast<char*>(g_probe_sink);
        g_probe_sink = new (std::nothrow) char[100];
        g_probe_sink[0] = 1;
        after_nothrow = window.calls();
        delete[] const_cast<char*>(g_probe_sink);
    }
    g_probe_sink = nullptr;
    EXPECT_EQ(after_array, 1u) << "new char[N] must be counted";
    EXPECT_EQ(after_nothrow, 2u) << "new (std::nothrow) char[N] must be counted";
}

TEST(WsApiCodecAllocation, ParsingAllocatesNothingOnceTheCodecExists) {
    WsApiCodec codec;  // its parser memory and its padded buffer: the allocations the design allows, made here and nowhere else
    WsApiFrame frame{};

    // Every input is built BEFORE the window opens; inside it there is no gtest macro, no container and no string construction.
    const std::vector<std::string_view>& samples_list = all_samples();
    std::vector<std::string> mutable_copies;
    for (const std::string_view s : samples_list) mutable_copies.emplace_back(s);
    const std::string oversize(hy::kWsApiMaxMessageBytes + 1, ' ');
    const std::string no_order_id = replace_once(samples::kEventExecutionReport, R"("c":"mUvoqJxFIILMdfAW5iGSOW",)", "");
    const std::string no_cumulative = replace_once(samples::kEventExecutionReport, R"("z":"0.00000000",)", "");
    const std::string unusable_order_id = replace_once(samples::kEventExecutionReport, "mUvoqJxFIILMdfAW5iGSOW", "not a valid id");
    const char hostile[] = {'\0', '"', '\\', '{', '}', '[', ']', ':', ',', 'x', '\xFF', '\n'};
    // The two largest documents the codec accepts (the official samples are all under 600 bytes): a real frame followed by whitespace up
    // to exactly kWsApiMaxMessageBytes, and a frame that is nearly all array -- the longest tape one can get.
    std::string padded_to_max(samples::kEventOutboundAccountPosition);
    padded_to_max.resize(hy::kWsApiMaxMessageBytes, ' ');
    std::string many_balances = R"({"subscriptionId":0,"event":{"e":"outboundAccountPosition","E":1,"u":2,"B":[)";
    while (many_balances.size() + 64 < hy::kWsApiMaxMessageBytes) many_balances += R"({"a":"ETH","f":"1","l":"0"},)";
    many_balances += R"({"a":"ETH","f":"1","l":"0"}]}})";
    ASSERT_EQ(padded_to_max.size(), hy::kWsApiMaxMessageBytes);
    ASSERT_GT(many_balances.size(), hy::kWsApiMaxMessageBytes - 100);
    ASSERT_LE(many_balances.size(), hy::kWsApiMaxMessageBytes);

    std::size_t parses = 0;
    std::size_t accepted = 0;
    std::size_t refused = 0;
    std::size_t calls = 0;
    bool largest_accepted = false;
    bool longest_tape_accepted = false;
    {
        const alloc_probe::Window window;
        const auto count = [&](std::string_view body) noexcept {
            ++parses;
            if (codec.parse(body, frame) == WsApiParseResult::Ok) {
                ++accepted;
            } else {
                ++refused;
            }
        };
        for (int round = 0; round < 3; ++round) {
            for (const std::string_view s : samples_list) count(s);
        }
        for (const std::string_view s : samples_list) {
            for (std::size_t len = 0; len < s.size(); ++len) count(s.substr(0, len));
        }
        for (std::string& m : mutable_copies) {
            for (std::size_t pos = 0; pos < m.size(); pos += 3) {
                const char original = m[pos];
                for (const char b : hostile) {
                    m[pos] = b;
                    count(m);
                }
                m[pos] = original;
            }
        }
        largest_accepted = codec.parse(padded_to_max, frame) == WsApiParseResult::Ok;
        longest_tape_accepted = codec.parse(many_balances, frame) == WsApiParseResult::Ok;
        parses += 2;
        count(oversize);
        count(no_order_id);
        count(no_cumulative);
        count(unusable_order_id);
        calls = window.calls();
    }
    EXPECT_EQ(calls, 0u) << "operator new was called " << calls << " time(s) while parsing " << parses << " frames";
    EXPECT_TRUE(largest_accepted) << "a real frame padded to exactly kWsApiMaxMessageBytes is a frame";
    EXPECT_TRUE(longest_tape_accepted) << "so is one that is nearly all array";
    EXPECT_GT(accepted, 30u) << "the loop must really exercise the accepting paths";
    EXPECT_GT(refused, 1000u) << "... and the refusing ones";
}

// --- the event model ---

TEST(UserDataWsEventModel, TheEventStaysSmallEnoughForItsRingAndTrivialToCopy) {
    EXPECT_TRUE(std::is_trivially_copyable_v<UserDataWsEvent>);
    EXPECT_LE(sizeof(UserDataWsEvent), 512u) << "64 of them sit in the SPSC ring: ~32 KiB at most";
    const UserDataWsEvent fresh{};
    EXPECT_EQ(fresh.subscription_id, -1);
    EXPECT_EQ(fresh.trade_id, -1);
    EXPECT_EQ(fresh.order_list_id, -1);
    EXPECT_EQ(fresh.generation, 0u);
    EXPECT_EQ(fresh.kind, UserDataEventKind::Unknown);
    EXPECT_EQ(fresh.list_order_status, ListOrderStatus::Unknown);
}

TEST(UserDataWsEventModel, ListOrderStatusParsesTheThreeDocumentedValuesOnly) {
    ListOrderStatus s = ListOrderStatus::Unknown;
    EXPECT_TRUE(hy::parse_binance_list_order_status("EXECUTING", s));
    EXPECT_EQ(s, ListOrderStatus::Executing);
    EXPECT_TRUE(hy::parse_binance_list_order_status("ALL_DONE", s));
    EXPECT_EQ(s, ListOrderStatus::AllDone);
    EXPECT_TRUE(hy::parse_binance_list_order_status("REJECT", s));
    EXPECT_EQ(s, ListOrderStatus::Reject);
    s = ListOrderStatus::Unknown;
    EXPECT_FALSE(hy::parse_binance_list_order_status("executing", s));
    EXPECT_FALSE(hy::parse_binance_list_order_status("", s));
    EXPECT_FALSE(hy::parse_binance_list_order_status("RESPONSE", s));
    EXPECT_EQ(s, ListOrderStatus::Unknown);
}

TEST(WsApiCodecListStatus, AnUnrecognizedListStatusLeavesItUnknown) {
    const Parsed p = parse_fresh(replace_once(samples::kEventListStatus, R"("L":"EXECUTING")", R"("L":"SOMETHING_NEW")"));
    ASSERT_EQ(p.result, WsApiParseResult::Ok);
    EXPECT_EQ(p.frame.event.kind, UserDataEventKind::ListStatus);
    EXPECT_EQ(p.frame.event.list_order_status, ListOrderStatus::Unknown);
}
