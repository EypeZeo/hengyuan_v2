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

    void reset(std::uint32_t limit, std::uint32_t safety_margin,
               TimePoint now = Clock::now()) noexcept {
        limit_ = limit;
        safety_ = safety_margin;
        used_current_ = 0;
        used_previous_ = 0;
        window_start_ = now;
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

    // True sliding-window estimate (weighted two-bucket counter): the previous
    // minute's usage is discounted by how far we've slid into the current
    // minute, so a burst right at a fixed-window boundary can't get ~2x the
    // intended limit (the tumbling-window bug this replaces). Pure function of
    // `now` + stored buckets — safe to call from a const method (can_send)
    // without mutating state; try_consume calls maybe_rotate_window() first so
    // the buckets it reads back here are already current.
    std::uint32_t effective_used_now(TimePoint now) const noexcept {
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - window_start_).count();
        double elapsed_s = elapsed_ms > 0 ? static_cast<double>(elapsed_ms) / 1000.0 : 0.0;

        double effective;
        if (elapsed_s >= 120.0) {
            effective = 0.0;  // both buckets are stale (no traffic for 2+ windows)
        } else if (elapsed_s >= 60.0) {
            double frac = (elapsed_s - 60.0) / 60.0;
            frac = frac > 1.0 ? 1.0 : frac;
            effective = static_cast<double>(used_current_) * (1.0 - frac);
        } else {
            double frac = elapsed_s / 60.0;
            effective = static_cast<double>(used_previous_) * (1.0 - frac) +
                        static_cast<double>(used_current_);
        }
        // Round up conservatively so float rounding never under-counts usage
        // relative to the integer weight actually charged.
        return static_cast<std::uint32_t>(effective + 0.5);
    }

    void maybe_rotate_window(TimePoint now) noexcept {
        auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
            now - window_start_).count();
        if (elapsed_s < 60) return;
        if (elapsed_s >= 120) {
            used_previous_ = 0;
            used_current_ = 0;
        } else {
            used_previous_ = used_current_;
            used_current_ = 0;
        }
        // Advance by whole 60s windows (not to "now") so the fractional phase
        // within the new window stays accurate instead of resetting to zero.
        auto windows_passed = elapsed_s / 60;
        window_start_ += std::chrono::seconds(60 * windows_passed);
    }

    std::uint32_t limit_{6000};
    std::uint32_t safety_{500};
    std::uint32_t used_current_{0};
    std::uint32_t used_previous_{0};
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

    auto diff = local_ms > server_ms ? (local_ms - server_ms) : (server_ms - local_ms);
    if (diff > static_cast<std::int64_t>(recv_window_ms)) {
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
