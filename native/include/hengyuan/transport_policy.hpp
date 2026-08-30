// SPDX-License-Identifier: proprietary
// transport_policy.hpp — L4 transport security policy for Binance authenticated REST.
//
// Governance: L4 — defines and enforces transport-level security gates per ADR-019 D5.
// This is a policy + validation layer. Actual HTTP calls are in binance_private_rest.hpp.
//
// ADR-019 D5 checklist enforced:
//   ✅ TLS / certificate / hostname verification (mandatory, no verify=false)
//   ✅ Endpoint allowlist (only pre-declared Binance API hosts)
//   ✅ No HTTP redirect following (3xx → error)
//   ✅ Connect + read timeout (seconds, never infinite)
//   ✅ Rate limit awareness (local request weight counter)
//   ✅ Clock skew detection (timestamp + recvWindow bound)
//   ✅ Response size limit + schema validation hooks
//   ✅ Error message sanitization (no raw Binance error in logs)

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace hy {

// --- Endpoint allowlist ---

static constexpr std::size_t kMaxEndpoints = 4;

struct EndpointAllowlist {
    std::array<std::string_view, kMaxEndpoints> hosts{};
    std::size_t count{0};

    bool contains(std::string_view host) const noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            if (hosts[i] == host) return true;
        }
        return false;
    }
};

inline constexpr EndpointAllowlist binance_default_endpoints() noexcept {
    EndpointAllowlist al{};
    al.hosts[0] = "api.binance.com";
    al.hosts[1] = "api1.binance.com";
    al.hosts[2] = "api2.binance.com";
    al.hosts[3] = "api3.binance.com";
    al.count = 4;
    return al;
}

// --- Transport policy ---

struct TransportPolicy {
    EndpointAllowlist endpoint_allowlist = binance_default_endpoints();
    bool tls_verify_peer{true};
    bool tls_verify_hostname{true};
    bool follow_redirects{false};

    std::uint32_t connect_timeout_ms{5000};
    std::uint32_t read_timeout_ms{10000};

    std::size_t max_response_bytes{1024 * 1024};  // 1 MiB

    // Binance recvWindow (ms). Signatures with timestamp outside this window are rejected.
    std::uint32_t recv_window_ms{5000};

    // Binance spot API weight limit per minute (default 6000 for most endpoints).
    std::uint32_t weight_limit_per_minute{6000};
    // Safety margin: stop sending when remaining weight drops below this.
    std::uint32_t weight_safety_margin{500};
};

// --- Transport validation results ---

enum class TransportCheck : std::uint8_t {
    Ok = 0,
    EndpointNotAllowed = 1,
    TlsVerifyDisabled = 2,
    RedirectsEnabled = 3,
    TimeoutZero = 4,
    RecvWindowZero = 5,
    ResponseTooLarge = 6,
    HttpRedirectReceived = 7,
    RateLimitExhausted = 8,
    ClockSkewTooLarge = 9,
    SchemaValidationFailed = 10,
};

// Validate policy configuration before first use (compile-time-like check).
inline TransportCheck validate_policy(const TransportPolicy& p) noexcept {
    if (p.endpoint_allowlist.count == 0) return TransportCheck::EndpointNotAllowed;
    if (!p.tls_verify_peer) return TransportCheck::TlsVerifyDisabled;
    if (!p.tls_verify_hostname) return TransportCheck::TlsVerifyDisabled;
    if (p.follow_redirects) return TransportCheck::RedirectsEnabled;
    if (p.connect_timeout_ms == 0) return TransportCheck::TimeoutZero;
    if (p.read_timeout_ms == 0) return TransportCheck::TimeoutZero;
    if (p.recv_window_ms == 0) return TransportCheck::RecvWindowZero;
    return TransportCheck::Ok;
}

// Validate that a target host is in the allowlist.
inline TransportCheck check_endpoint(const TransportPolicy& p,
                                      std::string_view host) noexcept {
    if (!p.endpoint_allowlist.contains(host)) {
        return TransportCheck::EndpointNotAllowed;
    }
    return TransportCheck::Ok;
}

// Validate HTTP status code: reject redirects (3xx).
inline TransportCheck check_http_status(unsigned int status_code) noexcept {
    if (status_code >= 300 && status_code < 400) {
        return TransportCheck::HttpRedirectReceived;
    }
    return TransportCheck::Ok;
}

// Validate response size.
inline TransportCheck check_response_size(const TransportPolicy& p,
                                           std::size_t bytes) noexcept {
    if (bytes > p.max_response_bytes) {
        return TransportCheck::ResponseTooLarge;
    }
    return TransportCheck::Ok;
}

// --- Rate limiter (local, per-minute sliding window) ---

class RequestWeightTracker {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // window_seconds: trailing default (60, matching this class's original hardcoded
    // window) so every existing 2-arg/3-arg call site keeps identical behavior.
    // Genuinely needed for spot_rate_limit_budget.hpp's RAW_REQUESTS (~300s) and
    // ORDERS (~10s) trackers — forcing either through the old hardcoded 60s window
    // would be a real fail-open (resetting far more often than Binance actually
    // allows), not a cosmetic simplification. 0 fails closed to 60 rather than
    // producing a zero-length window (which effective_used_now()'s division would
    // otherwise choke on).
    void reset(std::uint32_t limit, std::uint32_t safety_margin,
               TimePoint now = Clock::now(), std::uint32_t window_seconds = 60) noexcept {
        limit_ = limit;
        safety_ = safety_margin;
        used_current_ = 0;
        used_previous_ = 0;
        window_start_ = now;
        window_seconds_ = window_seconds > 0 ? window_seconds : 60;
    }

    // Try to consume `weight` units. Returns false if would exceed limit.
    // `now` is injectable (defaults to the real clock) purely so tests can
    // exercise sliding-window rotation without sleeping 60+ real seconds.
    bool try_consume(std::uint32_t weight, TimePoint now = Clock::now()) noexcept {
        maybe_rotate_window(now);
        auto avail = available_budget();
        auto used = effective_used_now(now);
        if (used >= avail || weight > avail - used) {
            return false;
        }
        used_current_ += weight;
        return true;
    }

    // Undoes a prior try_consume() by the same weight -- needed by
    // spot_rate_limit_budget.hpp's multi-dimension atomic reserve (weight -> raw
    // -> orders), which must unwind an earlier successful reservation when a
    // later one fails. Saturates at 0 rather than underflowing: a rollback that
    // races a window rotation (used_current_ already zeroed) is a safe no-op, not
    // a wrap-around toward UINT32_MAX.
    void rollback(std::uint32_t weight, TimePoint now = Clock::now()) noexcept {
        maybe_rotate_window(now);
        used_current_ = weight <= used_current_ ? used_current_ - weight : 0;
    }

    // Check if we can send a request with given weight (non-consuming).
    bool can_send(std::uint32_t weight, TimePoint now = Clock::now()) const noexcept {
        auto avail = available_budget();
        auto used = effective_used_now(now);
        return used < avail && weight <= (avail - used);
    }

    std::uint32_t used(TimePoint now = Clock::now()) const noexcept {
        return effective_used_now(now);
    }

    std::uint32_t remaining(TimePoint now = Clock::now()) const noexcept {
        auto avail = available_budget();
        auto used = effective_used_now(now);
        return used < avail ? avail - used : 0;
    }

private:
    // Fail-closed guard: a misconfigured safety_margin >= limit yields zero
    // available budget (nothing sendable) rather than a uint32 underflow that
    // would silently look like an enormous budget (fail-open).
    std::uint32_t available_budget() const noexcept {
        return safety_ < limit_ ? (limit_ - safety_) : 0;
    }

    // ceil(numerator / denominator) for two non-negative 64-bit operands.
    // denominator is always window_seconds_*1000 here, which reset()'s
    // zero-clamp guarantees is > 0.
    static std::uint32_t ceil_div_u64(std::uint64_t numerator, std::uint64_t denominator) noexcept {
        return static_cast<std::uint32_t>((numerator + denominator - 1) / denominator);
    }

    // True sliding-window estimate (weighted two-bucket counter): the previous
    // window's usage is discounted by how far we've slid into the current
    // window, so a burst right at a fixed-window boundary can't get ~2x the
    // intended limit (the tumbling-window bug this replaces). Pure function of
    // `now` + stored buckets — safe to call from a const method (can_send)
    // without mutating state; try_consume calls maybe_rotate_window() first so
    // the buckets it reads back here are already current.
    //
    // Pure integer fixed-point (millisecond resolution), not floating point:
    // this runs on the hot submit-thread path (Gate 8/12c, every order
    // submission) — CLAUDE.md §5's arithmetic-safety mandate applies to the
    // code path even though the values themselves aren't order economics.
    // Ceiling division (never plain rounding) so this can only ever
    // OVER-report usage relative to the exact real-valued decay, never
    // under-report it — the conservative direction the original double
    // implementation's own comment already intended but only approximated via
    // "+0.5" (round-to-nearest, not true ceiling).
    std::uint32_t effective_used_now(TimePoint now) const noexcept {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - window_start_).count();
        const std::int64_t w_ms = static_cast<std::int64_t>(window_seconds_) * 1000;

        if (elapsed_ms <= 0) [[unlikely]] {
            return used_current_;
        }
        if (elapsed_ms >= 2 * w_ms) [[unlikely]] {
            return 0;  // both buckets are stale (no traffic for 2+ windows)
        }
        if (elapsed_ms >= w_ms) {
            // 1 window <= elapsed < 2 windows: used_current_ decays to 0 as
            // elapsed approaches 2*w_ms. remain_ms == 0 at elapsed==2*w_ms,
            // == w_ms at elapsed==w_ms (fully counted, matches the boundary).
            const std::int64_t remain_ms = 2 * w_ms - elapsed_ms;
            return ceil_div_u64(static_cast<std::uint64_t>(used_current_) *
                                     static_cast<std::uint64_t>(remain_ms),
                                 static_cast<std::uint64_t>(w_ms));
        }
        // elapsed < 1 window: used_previous_ decays linearly to 0 as elapsed
        // approaches w_ms; used_current_ counts in full (it's the live bucket).
        const std::int64_t remain_ms = w_ms - elapsed_ms;
        const std::uint32_t prev_discounted = ceil_div_u64(
            static_cast<std::uint64_t>(used_previous_) * static_cast<std::uint64_t>(remain_ms),
            static_cast<std::uint64_t>(w_ms));
        return prev_discounted + used_current_;
    }

    void maybe_rotate_window(TimePoint now) noexcept {
        const auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
            now - window_start_).count();
        const auto w_s = static_cast<std::int64_t>(window_seconds_);
        if (elapsed_s < w_s) return;
        if (elapsed_s >= 2 * w_s) {
            used_previous_ = 0;
            used_current_ = 0;
        } else {
            used_previous_ = used_current_;
            used_current_ = 0;
        }
        // Advance by whole windows (not to "now") so the fractional phase
        // within the new window stays accurate instead of resetting to zero.
        const auto windows_passed = elapsed_s / w_s;
        window_start_ += std::chrono::seconds(w_s * windows_passed);
    }

    std::uint32_t limit_{6000};
    std::uint32_t safety_{500};
    std::uint32_t used_current_{0};
    std::uint32_t used_previous_{0};
    std::uint32_t window_seconds_{60};
    std::chrono::steady_clock::time_point window_start_{std::chrono::steady_clock::now()};
};

// --- Clock skew checker ---

// Check that local timestamp is within a reasonable range of server time.
// `local_ms` = our timestamp in the signed request, `server_ms` = Binance serverTime.
// Returns Ok if |local_ms - server_ms| < recv_window_ms, else ClockSkewTooLarge.
inline TransportCheck check_clock_skew(
    std::int64_t local_ms,
    std::int64_t server_ms,
    std::uint32_t recv_window_ms) noexcept {

    // AUDIT TIME-SKEW-025: `local_ms - server_ms` on two signed int64s overflows for
    // far-apart operands, and server_ms is destined to come from an untrusted
    // serverTime JSON field once the L4 REST client exists. Compute the magnitude in
    // unsigned arithmetic, where the subtraction is defined by wraparound, and let
    // any difference beyond the window fall out as ClockSkewTooLarge -- which is the
    // fail-closed answer for a nonsense server time anyway.
    const auto a = static_cast<std::uint64_t>(local_ms);
    const auto b = static_cast<std::uint64_t>(server_ms);
    const std::uint64_t diff = (local_ms > server_ms) ? (a - b) : (b - a);
    if (diff > static_cast<std::uint64_t>(recv_window_ms)) {
        return TransportCheck::ClockSkewTooLarge;
    }
    return TransportCheck::Ok;
}

// --- Error sanitization ---

// Sanitize a Binance error response body for safe logging.
// Strips anything that might contain key fragments, IPs, or tokens.
// Returns a fixed-size summary: "HTTP <status> / code=<code> / msg truncated".
struct SanitizedError {
    unsigned int http_status{0};
    std::int32_t binance_code{0};
    char summary[128]{};
};

inline SanitizedError sanitize_error(unsigned int http_status,
                                      std::string_view body) noexcept {
    SanitizedError se{};
    se.http_status = http_status;

    // Try to extract Binance error code ({"code":-XXXX,...}) without a JSON parser.
    // This is deliberately simple — we only want the numeric code, nothing else.
    auto code_pos = body.find("\"code\":");
    if (code_pos != std::string_view::npos) {
        auto num_start = code_pos + 7;
        // Skip whitespace
        while (num_start < body.size() && body[num_start] == ' ') ++num_start;
        bool negative = false;
        if (num_start < body.size() && body[num_start] == '-') {
            negative = true;
            ++num_start;
        }
        // Overflow-safe accumulation: a malformed/adversarial body with a very
        // long digit run must not hit signed-overflow UB via code*10+digit.
        // Saturate at INT32_MAX instead — this is a sanitized display value,
        // not a value anything decides on, so saturation is the right failure
        // mode (still advances num_start past the full digit run below).
        std::int32_t code = 0;
        constexpr std::int32_t kCodeMax = std::numeric_limits<std::int32_t>::max();
        while (num_start < body.size() && body[num_start] >= '0' && body[num_start] <= '9') {
            int digit = body[num_start] - '0';
            if (code > (kCodeMax - digit) / 10) {
                code = kCodeMax;
            } else {
                code = code * 10 + digit;
            }
            ++num_start;
        }
        se.binance_code = negative ? -code : code;
    }

    std::snprintf(se.summary, sizeof(se.summary),
                  "HTTP %u / binance_code=%d / body_len=%zu (sanitized)",
                  http_status, se.binance_code,
                  body.size());

    return se;
}

}  // namespace hy
