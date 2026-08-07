// P2-EXEC-LIVE-01 D12-3: TransportPolicy unit tests — L4 transport security gates.
// All tests are local (no network). Validates policy enforcement logic.
#include <gtest/gtest.h>
#include <hengyuan/transport_policy.hpp>

using hy::EndpointAllowlist;
using hy::RequestWeightTracker;
using hy::SanitizedError;
using hy::TransportCheck;
using hy::TransportPolicy;
using hy::binance_default_endpoints;
using hy::check_clock_skew;
using hy::check_endpoint;
using hy::check_http_status;
using hy::check_response_size;
using hy::sanitize_error;
using hy::validate_policy;

// --- Default endpoint allowlist ---

TEST(EndpointAllowlist, DefaultContainsBinanceHosts) {
    auto al = binance_default_endpoints();
    EXPECT_TRUE(al.contains("api.binance.com"));
    EXPECT_TRUE(al.contains("api1.binance.com"));
    EXPECT_TRUE(al.contains("api2.binance.com"));
    EXPECT_TRUE(al.contains("api3.binance.com"));
}

TEST(EndpointAllowlist, RejectsUnknownHost) {
    auto al = binance_default_endpoints();
    EXPECT_FALSE(al.contains("evil.example.com"));
    EXPECT_FALSE(al.contains("api.kraken.com"));
    EXPECT_FALSE(al.contains(""));
}

// --- Policy validation ---

TEST(TransportPolicy, DefaultPolicyIsValid) {
    TransportPolicy p{};
    EXPECT_EQ(validate_policy(p), TransportCheck::Ok);
}

TEST(TransportPolicy, RejectsTlsVerifyDisabled) {
    TransportPolicy p{};
    p.tls_verify_peer = false;
    EXPECT_EQ(validate_policy(p), TransportCheck::TlsVerifyDisabled);
}

TEST(TransportPolicy, RejectsHostnameVerifyDisabled) {
    TransportPolicy p{};
    p.tls_verify_hostname = false;
    EXPECT_EQ(validate_policy(p), TransportCheck::TlsVerifyDisabled);
}

TEST(TransportPolicy, RejectsRedirectsEnabled) {
    TransportPolicy p{};
    p.follow_redirects = true;
    EXPECT_EQ(validate_policy(p), TransportCheck::RedirectsEnabled);
}

TEST(TransportPolicy, RejectsZeroConnectTimeout) {
    TransportPolicy p{};
    p.connect_timeout_ms = 0;
    EXPECT_EQ(validate_policy(p), TransportCheck::TimeoutZero);
}

TEST(TransportPolicy, RejectsZeroReadTimeout) {
    TransportPolicy p{};
    p.read_timeout_ms = 0;
    EXPECT_EQ(validate_policy(p), TransportCheck::TimeoutZero);
}

TEST(TransportPolicy, RejectsZeroRecvWindow) {
    TransportPolicy p{};
    p.recv_window_ms = 0;
    EXPECT_EQ(validate_policy(p), TransportCheck::RecvWindowZero);
}

TEST(TransportPolicy, RejectsEmptyEndpointList) {
    TransportPolicy p{};
    p.endpoint_allowlist.count = 0;
    EXPECT_EQ(validate_policy(p), TransportCheck::EndpointNotAllowed);
}

// --- Endpoint check ---

TEST(TransportPolicy, EndpointCheckAllowsValidHost) {
    TransportPolicy p{};
    EXPECT_EQ(check_endpoint(p, "api.binance.com"), TransportCheck::Ok);
}

TEST(TransportPolicy, EndpointCheckRejectsInvalidHost) {
    TransportPolicy p{};
    EXPECT_EQ(check_endpoint(p, "evil.com"), TransportCheck::EndpointNotAllowed);
}

// --- HTTP status check ---

TEST(TransportPolicy, HttpStatusOkPasses) {
    EXPECT_EQ(check_http_status(200), TransportCheck::Ok);
}

TEST(TransportPolicy, HttpStatus4xxPasses) {
    EXPECT_EQ(check_http_status(400), TransportCheck::Ok);
    EXPECT_EQ(check_http_status(403), TransportCheck::Ok);
    EXPECT_EQ(check_http_status(429), TransportCheck::Ok);
}

TEST(TransportPolicy, HttpStatus3xxIsRedirect) {
    EXPECT_EQ(check_http_status(301), TransportCheck::HttpRedirectReceived);
    EXPECT_EQ(check_http_status(302), TransportCheck::HttpRedirectReceived);
    EXPECT_EQ(check_http_status(307), TransportCheck::HttpRedirectReceived);
}

TEST(TransportPolicy, HttpStatus5xxPasses) {
    EXPECT_EQ(check_http_status(500), TransportCheck::Ok);
    EXPECT_EQ(check_http_status(503), TransportCheck::Ok);
}

// --- Response size check ---

TEST(TransportPolicy, ResponseSizeWithinLimit) {
    TransportPolicy p{};
    EXPECT_EQ(check_response_size(p, 1024), TransportCheck::Ok);
    EXPECT_EQ(check_response_size(p, p.max_response_bytes), TransportCheck::Ok);
}

TEST(TransportPolicy, ResponseSizeExceedsLimit) {
    TransportPolicy p{};
    EXPECT_EQ(check_response_size(p, p.max_response_bytes + 1),
              TransportCheck::ResponseTooLarge);
}

// --- Rate limiter ---

TEST(RequestWeightTracker, InitialCanSend) {
    RequestWeightTracker t;
    t.reset(6000, 500);
    EXPECT_TRUE(t.can_send(10));
    EXPECT_EQ(t.remaining(), 5500u);
}

TEST(RequestWeightTracker, ConsumeReducesRemaining) {
    RequestWeightTracker t;
    t.reset(6000, 500);
    EXPECT_TRUE(t.try_consume(1000));
    EXPECT_EQ(t.used(), 1000u);
    EXPECT_EQ(t.remaining(), 4500u);
}

TEST(RequestWeightTracker, RejectsWhenExhausted) {
    RequestWeightTracker t;
    t.reset(100, 10);  // limit=100, safety=10 → effective=90
    EXPECT_TRUE(t.try_consume(85));
    EXPECT_FALSE(t.try_consume(10));  // 85+10=95 > 90
    EXPECT_EQ(t.used(), 85u);
}

TEST(RequestWeightTracker, CanSendReturnsFalseNearLimit) {
    RequestWeightTracker t;
    t.reset(100, 10);
    EXPECT_TRUE(t.try_consume(85));
    EXPECT_FALSE(t.can_send(10));
    EXPECT_TRUE(t.can_send(5));
}

// --- F7 regression: misconfigured safety_margin >= limit fails closed, not open ---

TEST(RequestWeightTracker, SafetyEqualToLimitYieldsZeroBudget) {
    RequestWeightTracker t;
    t.reset(100, 100);  // safety == limit -> zero available, not a uint32 underflow
    EXPECT_FALSE(t.can_send(1));
    EXPECT_EQ(t.remaining(), 0u);
    EXPECT_FALSE(t.try_consume(1));
}

TEST(RequestWeightTracker, SafetyGreaterThanLimitYieldsZeroBudget) {
    RequestWeightTracker t;
    t.reset(100, 500);  // safety > limit -> must NOT underflow into a huge budget
    EXPECT_FALSE(t.can_send(1));
    EXPECT_EQ(t.remaining(), 0u);
    EXPECT_FALSE(t.try_consume(1));
}

// --- F8 regression: true sliding window, not a tumbling one (no boundary 2x burst) ---

TEST(RequestWeightTracker, SlidingWindowDiscountsPreviousUsageGradually) {
    using Clock = RequestWeightTracker::Clock;
    RequestWeightTracker t;
    auto t0 = Clock::now();
    t.reset(100, 0, t0);
    ASSERT_TRUE(t.try_consume(100, t0));  // fully use the first window

    // Immediately after the window "rolls over" (60s later), a naive tumbling
    // window would reset used to 0 and allow another full 100 — a 2x burst at
    // the boundary. The sliding window must still consider the prior window's
    // usage as still (mostly) counting right at the boundary.
    auto t_60 = t0 + std::chrono::seconds(60);
    EXPECT_FALSE(t.can_send(100, t_60))
        << "sliding window allowed a full second burst immediately at the boundary";
    EXPECT_GT(t.used(t_60), 90u)
        << "previous window usage should barely be discounted right at rotation";

    // Halfway through the new window, roughly half of the previous usage
    // should have "slid out", freeing up meaningful (but not full) budget.
    auto t_90 = t0 + std::chrono::seconds(90);
    auto used_at_90 = t.used(t_90);
    EXPECT_LT(used_at_90, 90u);
    EXPECT_GT(used_at_90, 10u);

    // A full window after rotation, the old usage should be fully gone.
    auto t_120 = t0 + std::chrono::seconds(120);
    EXPECT_TRUE(t.can_send(100, t_120));
    EXPECT_EQ(t.used(t_120), 0u);
}

TEST(RequestWeightTracker, NoTrafficForTwoWindowsResetsCleanly) {
    using Clock = RequestWeightTracker::Clock;
    RequestWeightTracker t;
    auto t0 = Clock::now();
    t.reset(100, 0, t0);
    ASSERT_TRUE(t.try_consume(50, t0));

    auto t_far = t0 + std::chrono::seconds(200);  // well past 2 windows, no traffic in between
    EXPECT_EQ(t.used(t_far), 0u);
    EXPECT_TRUE(t.can_send(100, t_far));
}

// --- Clock skew ---

TEST(ClockSkew, WithinWindowPasses) {
    EXPECT_EQ(check_clock_skew(1000000, 1000100, 5000), TransportCheck::Ok);
    EXPECT_EQ(check_clock_skew(1000100, 1000000, 5000), TransportCheck::Ok);
}

TEST(ClockSkew, ExactBoundaryPasses) {
    EXPECT_EQ(check_clock_skew(1000000, 1005000, 5000), TransportCheck::Ok);
}

TEST(ClockSkew, BeyondWindowFails) {
    EXPECT_EQ(check_clock_skew(1000000, 1006000, 5000),
              TransportCheck::ClockSkewTooLarge);
    EXPECT_EQ(check_clock_skew(1006000, 1000000, 5000),
              TransportCheck::ClockSkewTooLarge);
}

TEST(ClockSkew, ZeroSkewPasses) {
    EXPECT_EQ(check_clock_skew(1000000, 1000000, 5000), TransportCheck::Ok);
}

// --- Error sanitization ---

TEST(SanitizeError, ExtractsBinanceCode) {
    auto se = sanitize_error(400, R"({"code":-1013,"msg":"Invalid quantity."})");
    EXPECT_EQ(se.http_status, 400u);
    EXPECT_EQ(se.binance_code, -1013);
    EXPECT_NE(std::strstr(se.summary, "HTTP 400"), nullptr);
    EXPECT_NE(std::strstr(se.summary, "-1013"), nullptr);
    EXPECT_NE(std::strstr(se.summary, "sanitized"), nullptr);
}

TEST(SanitizeError, HandlesNoCodeField) {
    auto se = sanitize_error(500, "Internal Server Error");
    EXPECT_EQ(se.http_status, 500u);
    EXPECT_EQ(se.binance_code, 0);
}

TEST(SanitizeError, DoesNotLeakRawBody) {
    std::string body = R"({"code":-2015,"msg":"Invalid API-key, IP, or permissions for action. API_KEY=abc123secret"})";
    auto se = sanitize_error(403, body);
    // The summary must NOT contain the raw API key
    EXPECT_EQ(std::strstr(se.summary, "abc123secret"), nullptr);
    EXPECT_NE(std::strstr(se.summary, "sanitized"), nullptr);
}

TEST(SanitizeError, HandlesEmptyBody) {
    auto se = sanitize_error(418, "");
    EXPECT_EQ(se.http_status, 418u);
    EXPECT_EQ(se.binance_code, 0);
}

TEST(SanitizeError, PositiveCodeExtracted) {
    auto se = sanitize_error(200, R"({"code": 0, "msg":"OK"})");
    EXPECT_EQ(se.binance_code, 0);
}

// --- F9 regression: a very long digit run must not hit signed-overflow UB ---

TEST(SanitizeError, VeryLongDigitRunSaturatesInsteadOfOverflowing) {
    // 40 digits — far beyond int32 range. code*10+digit without a guard is UB.
    auto se = sanitize_error(400, R"({"code":99999999999999999999999999999999999999,"msg":"x"})");
    EXPECT_EQ(se.binance_code, std::numeric_limits<std::int32_t>::max());
    EXPECT_NE(std::strstr(se.summary, "sanitized"), nullptr);
}

TEST(SanitizeError, VeryLongNegativeDigitRunSaturates) {
    auto se = sanitize_error(400, R"({"code":-99999999999999999999999999999999999999,"msg":"x"})");
    EXPECT_EQ(se.binance_code, -std::numeric_limits<std::int32_t>::max());
}

TEST(SanitizeError, DigitRunAtExactInt32MaxBoundary) {
    // INT32_MAX == 2147483647
    auto se = sanitize_error(400, R"({"code":2147483647,"msg":"x"})");
    EXPECT_EQ(se.binance_code, std::numeric_limits<std::int32_t>::max());
}

// --- check_clock_skew() overflow safety (audit TIME-SKEW-025) ---
//
// `local_ms - server_ms` on two signed int64s is UB for far-apart operands, and
// server_ms is destined to come from an untrusted serverTime field once the L4 REST
// client exists. The magnitude is computed in unsigned arithmetic now; UBSan is what
// actually vets this.

TEST(ClockSkew, ExtremeOperandsDoNotOverflow) {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

    EXPECT_EQ(check_clock_skew(kMax, kMin, 5000), TransportCheck::ClockSkewTooLarge);
    EXPECT_EQ(check_clock_skew(kMin, kMax, 5000), TransportCheck::ClockSkewTooLarge);
    EXPECT_EQ(check_clock_skew(1'700'000'000'000LL, kMin, 5000), TransportCheck::ClockSkewTooLarge);
    EXPECT_EQ(check_clock_skew(kMax, 1'700'000'000'000LL, 5000), TransportCheck::ClockSkewTooLarge);
}

TEST(ClockSkew, OrdinaryValuesStillBehave) {
    const std::int64_t now = 1'700'000'000'000LL;
    EXPECT_EQ(check_clock_skew(now, now, 5000), TransportCheck::Ok);
    EXPECT_EQ(check_clock_skew(now, now + 4999, 5000), TransportCheck::Ok);
    EXPECT_EQ(check_clock_skew(now, now - 4999, 5000), TransportCheck::Ok);
    EXPECT_EQ(check_clock_skew(now, now + 5000, 5000), TransportCheck::Ok) << "boundary is inclusive";
    EXPECT_EQ(check_clock_skew(now, now + 5001, 5000), TransportCheck::ClockSkewTooLarge);
    EXPECT_EQ(check_clock_skew(now, now - 5001, 5000), TransportCheck::ClockSkewTooLarge);
}
