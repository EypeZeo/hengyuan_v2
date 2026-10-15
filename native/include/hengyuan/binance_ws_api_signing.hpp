// SPDX-License-Identifier: proprietary
// binance_ws_api_signing.hpp — private-stream migration M1 (docs/PRIVATE_STREAM_MIGRATION_DESIGN.md §4.3): the REQUEST side of
// the spot WebSocket API. What a request is called and costs, and the one place a signed request is built.
//
// A WS API request is a JSON text frame: {"id":N,"method":"...","params":{...,"signature":"<hex>"}}. The signature is the
// HMAC-SHA-256 of the parameters with the signature removed, sorted by name, written "name=value" and joined with '&' --
// NOT percent-encoded (blueprint 5.2, 5.7 P-31; the REST policy in binance_query_signing.hpp is the opposite: it encodes).
// Binance recomputes that string from the parsed JSON, so what is signed and what is sent are two renderings of ONE
// parameter list. Here both come from the same sorted list in the same function, and a value that would need JSON escaping,
// or that could blur the "name=value&..." structure, is refused instead of escaped -- so the text that was signed is, byte for
// byte, the text the server will see after parsing the frame (the "signed bytes == sent bytes" rule of the REST builder,
// audit E-03). tests/test_binance_ws_api_signing.cpp checks that with an independent verifier that parses the frame and
// recomputes the signature the way the exchange does.
//
// Unforgeable like SignedQuery: WsApiSignedRequest and WsApiUnsignedRequest have private constructors, so the only way to
// hold one is to have gone through a builder here; a session (M2) that accepts only these two types cannot be handed a
// hand-written frame. The timestamp is a ClockReadyToken (binance_clock_sync.hpp), the API key comes from the bound
// credentials, both are appended by the policy itself and are therefore RESERVED parameter names for callers.
//
// Governance: L4 (offline logic only -- this header performs no network I/O; it assembles and signs the bytes a session
// will later send). No heap allocation: fixed buffers, bounded by the constants below.

#pragma once

#include <hengyuan/binance_clock_sync.hpp>
#include <hengyuan/binance_environment.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace hy {

// --- what a request is called and what it costs (blueprint 5.7 P-26) ---

enum class WsApiRequestKind : std::uint8_t {
    Connect = 0,               // opening the connection itself: it costs weight but has no method name
    SubscribeSignature = 1,    // userDataStream.subscribe.signature
    SessionSubscriptions = 2,  // session.subscriptions
    UserDataUnsubscribe = 3,   // userDataStream.unsubscribe
};
inline constexpr std::size_t kWsApiRequestKindCount = 4;

[[nodiscard]] constexpr std::string_view ws_api_method_name(WsApiRequestKind kind) noexcept {
    switch (kind) {
        case WsApiRequestKind::Connect: return {};
        case WsApiRequestKind::SubscribeSignature: return "userDataStream.subscribe.signature";
        case WsApiRequestKind::SessionSubscriptions: return "session.subscriptions";
        case WsApiRequestKind::UserDataUnsubscribe: return "userDataStream.unsubscribe";
    }
    return {};
}

// The REQUEST_WEIGHT a request costs: per IP address, shared with REST (P-26). An unknown kind -> UINT32_MAX, which no
// budget can grant: never a guessed weight (the rule endpoint_weight() in spot_rate_limit_budget.hpp follows too).
// PrivateRestEndpoint is pinned to the L4 spec by tools/spec_enum_diff.py and must not grow, so the WS API has its own kind;
// the permit comes from SpotRateLimitTracker::reserve_weight_only_explicit(lane, ws_api_weight(kind)), on the
// Reconciliation lane (design D7).
[[nodiscard]] constexpr std::uint32_t ws_api_weight(WsApiRequestKind kind) noexcept {
    switch (kind) {
        case WsApiRequestKind::Connect:
        case WsApiRequestKind::SubscribeSignature:
        case WsApiRequestKind::SessionSubscriptions:
        case WsApiRequestKind::UserDataUnsubscribe: return 2;
    }
    return std::numeric_limits<std::uint32_t>::max();
}

// --- limits ---

inline constexpr std::size_t kMaxWsApiParams = 16;        // caller parameters; apiKey and timestamp come on top
inline constexpr std::size_t kMaxWsApiNameLen = 32;       // a parameter name
inline constexpr std::size_t kMaxWsApiMethodLen = 48;     // a method name
inline constexpr std::size_t kMaxWsApiValueLen = 256;     // one parameter value
inline constexpr std::size_t kMaxWsApiNumberLen = 20;     // one numeric value ("18446744073709551615" is 20 digits)
inline constexpr std::size_t kMaxWsApiPayloadBytes = 1024;  // the signing input
inline constexpr std::size_t kMaxWsApiFrameBytes = 1536;    // one request frame
inline constexpr std::size_t kWsApiSignatureHexLen = 64;
inline constexpr std::uint64_t kMaxWsApiRequestId = (std::uint64_t{1} << 53) - 1;  // a JSON integer every parser keeps exact
inline constexpr std::uint32_t kMaxWsApiRecvWindowMs = 60'000;  // the documented ceiling (P-25, P-30)

static_assert(kWsApiSignatureHexLen == kHexLen, "the signer's hex digest is what the frame carries");

// Worst case of a frame: the id (up to 16 digits below 2^53, 20 as a bound), the method, the longest payload with every one of
// the kMaxWsApiParams + 2 entries quoted in JSON (4 more bytes per entry than in the payload), and the signature member.
static_assert(6 + 20 + 11 + kMaxWsApiMethodLen + 12 + kMaxWsApiPayloadBytes + 4 * (kMaxWsApiParams + 2) + 14 +
                      kWsApiSignatureHexLen + 3 <=
                  kMaxWsApiFrameBytes,
              "a request that fits the payload buffer must fit the frame buffer");

// --- parameters ---

enum class WsApiParamKind : std::uint8_t {
    Text = 0,    // a JSON string
    Number = 1,  // a JSON number: unsigned digits with an optional fraction (an INT, LONG or DECIMAL parameter)
};

struct WsApiParam {
    std::string_view name;
    std::string_view value;
    WsApiParamKind kind{WsApiParamKind::Text};
};

enum class WsApiSigningError : std::uint8_t {
    Ok = 0,
    BadMethodName = 1,         // empty, too long, or a character outside [A-Za-z0-9._/]
    TooManyParams = 2,
    BadParamName = 3,          // not a lower-case letter followed by letters and digits, or too long
    DuplicateParam = 4,
    ReservedParamName = 5,     // apiKey, timestamp and signature are appended by the policy itself
    UnsafeParamValue = 6,      // a Text value that would need JSON escaping, would blur the payload, or is not UTF-8
    BadNumberValue = 7,        // a Number value that is not digits with an optional short fraction
    PayloadTooLarge = 8,
    FrameTooLarge = 9,
    TimestampOutOfRange = 10,  // outside [0, 10^13)
    ApiKeyUnavailable = 11,    // not readable from this thread, or not a well-formed API key
    SigningFailed = 12,        // the signer returned no digest (wrong thread, or the crypto layer failed)
    BadRequestId = 13,         // above 2^53 - 1
    BadRecvWindow = 14,        // above kMaxWsApiRecvWindowMs
};

class WsApiSignedRequest;
class WsApiUnsignedRequest;

namespace detail {

[[nodiscard]] constexpr bool is_ws_api_lower(char c) noexcept { return c >= 'a' && c <= 'z'; }
[[nodiscard]] constexpr bool is_ws_api_alnum(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// A parameter name: a lower-case letter, then letters and digits. Binance's names all look like that, and a lower-case first
// letter keeps the sort order independent of how a server compares upper and lower case.
[[nodiscard]] constexpr bool is_valid_ws_api_param_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxWsApiNameLen || !is_ws_api_lower(name.front())) return false;
    for (const char c : name) {
        if (!is_ws_api_alnum(c)) return false;
    }
    return true;
}

[[nodiscard]] constexpr bool is_reserved_ws_api_param_name(std::string_view name) noexcept {
    return name == "apiKey" || name == "timestamp" || name == "signature";
}

// A method name: letters, digits, '.', '_' and '/' (the explicit "v3/" version prefix the request format allows).
[[nodiscard]] constexpr bool is_valid_ws_api_method_name(std::string_view method) noexcept {
    if (method.empty() || method.size() > kMaxWsApiMethodLen) return false;
    for (const char c : method) {
        if (!(is_ws_api_alnum(c) || c == '.' || c == '_' || c == '/')) return false;
    }
    return true;
}

// Well-formed UTF-8 (RFC 3629): no overlong forms, no surrogates, nothing above U+10FFFF.
[[nodiscard]] constexpr bool is_well_formed_utf8(std::string_view s) noexcept {
    std::size_t i = 0;
    const std::size_t n = s.size();
    const auto byte = [&](std::size_t k) noexcept { return static_cast<unsigned char>(s[k]); };
    const auto cont = [&](std::size_t k) noexcept { return k < n && (byte(k) & 0xC0U) == 0x80U; };
    while (i < n) {
        const unsigned char b = byte(i);
        if (b < 0x80U) {
            ++i;
        } else if (b >= 0xC2U && b <= 0xDFU) {
            if (!cont(i + 1)) return false;
            i += 2;
        } else if (b >= 0xE0U && b <= 0xEFU) {
            if (!cont(i + 1) || !cont(i + 2)) return false;
            const unsigned char b1 = byte(i + 1);
            if (b == 0xE0U && b1 < 0xA0U) return false;  // overlong
            if (b == 0xEDU && b1 > 0x9FU) return false;  // surrogate
            i += 3;
        } else if (b >= 0xF0U && b <= 0xF4U) {
            if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) return false;
            const unsigned char b1 = byte(i + 1);
            if (b == 0xF0U && b1 < 0x90U) return false;  // overlong
            if (b == 0xF4U && b1 > 0x8FU) return false;  // above U+10FFFF
            i += 4;
        } else {
            return false;  // a continuation byte on its own, 0xC0, 0xC1, or 0xF5 and up
        }
    }
    return true;
}

// A Text value goes into the payload verbatim AND into a JSON string verbatim. So it must hold nothing JSON would have to
// escape (a quote, a backslash, a control character) and nothing that could make "name=value&name=value" ambiguous
// ('&' and '=') -- those are refused, not escaped, so that the two renderings can never differ -- and it must be UTF-8,
// because the frame is. An empty value is legal (the server decides what to do with it).
[[nodiscard]] constexpr bool is_safe_ws_api_text_value(std::string_view v) noexcept {
    if (v.size() > kMaxWsApiValueLen) return false;
    for (const char c : v) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20U || u == 0x7FU || c == '"' || c == '\\' || c == '&' || c == '=') return false;
    }
    return is_well_formed_utf8(v);
}

// A Number value is written into the frame as a bare JSON number, and the payload carries the same text: unsigned digits, no
// leading zero unless the integer part is exactly "0", and an optional fraction of 1 to 9 digits.
[[nodiscard]] constexpr bool is_valid_ws_api_number(std::string_view v) noexcept {
    if (v.empty() || v.size() > kMaxWsApiNumberLen) return false;
    std::size_t i = 0;
    while (i < v.size() && v[i] >= '0' && v[i] <= '9') ++i;
    if (i == 0 || i > 19) return false;
    if (v[0] == '0' && i > 1) return false;
    if (i == v.size()) return true;
    if (v[i] != '.') return false;
    const std::size_t frac = v.size() - i - 1;
    if (frac < 1 || frac > 9) return false;
    for (std::size_t k = i + 1; k < v.size(); ++k) {
        if (v[k] < '0' || v[k] > '9') return false;
    }
    return true;
}

// Appends `lit` at buf[len...); false (nothing written) when it does not fit.
inline bool ws_append(std::span<char> buf, std::size_t& len, std::string_view lit) noexcept {
    if (lit.size() > buf.size() - len) return false;
    if (!lit.empty()) std::memcpy(buf.data() + len, lit.data(), lit.size());
    len += lit.size();
    return true;
}

// Appends the decimal digits of `value`.
inline bool ws_append_u64(std::span<char> buf, std::size_t& len, std::uint64_t value) noexcept {
    char digits[20];
    int n = 0;
    if (value == 0) {
        digits[n++] = '0';
    } else {
        while (value > 0) {
            digits[n++] = static_cast<char>('0' + (value % 10U));
            value /= 10U;
        }
    }
    if (static_cast<std::size_t>(n) > buf.size() - len) return false;
    for (int i = n - 1; i >= 0; --i) buf[len++] = digits[i];
    return true;
}

// The one place a signed request is built. `method` is any well-formed method name so the official signing examples
// ("order.place") can be checked against it; the typed entry points below pass the method of their WsApiRequestKind.
// Declared before the classes so they can name it as a friend; defined below.
[[nodiscard]] std::pair<WsApiSigningError, WsApiSignedRequest> build_ws_api_signed_request(
    BoundHmacCredentials& creds, std::string_view method, std::uint64_t request_id, std::span<const WsApiParam> params,
    ClockReadyToken clock_token) noexcept;

}  // namespace detail

// The frame to send (an exact JSON text), proof that it was built and signed by detail::build_ws_api_signed_request().
class WsApiSignedRequest {
public:
    [[nodiscard]] std::string_view frame() const noexcept { return {buf_.data(), len_}; }
    [[nodiscard]] std::uint64_t request_id() const noexcept { return id_; }

private:
    friend std::pair<WsApiSigningError, WsApiSignedRequest> detail::build_ws_api_signed_request(
        BoundHmacCredentials&, std::string_view, std::uint64_t, std::span<const WsApiParam>, ClockReadyToken) noexcept;
    friend std::pair<WsApiSigningError, WsApiSignedRequest> build_subscribe_signature_request(
        BoundHmacCredentials&, std::uint64_t, ClockReadyToken, std::uint32_t) noexcept;
    WsApiSignedRequest() noexcept = default;

    std::array<char, kMaxWsApiFrameBytes> buf_{};
    std::size_t len_{0};
    std::uint64_t id_{0};
};

// A request that carries no signature (session.subscriptions, userDataStream.unsubscribe). Same rule: no other way to make one.
class WsApiUnsignedRequest {
public:
    [[nodiscard]] std::string_view frame() const noexcept { return {buf_.data(), len_}; }
    [[nodiscard]] std::uint64_t request_id() const noexcept { return id_; }

private:
    friend std::pair<WsApiSigningError, WsApiUnsignedRequest> build_session_subscriptions_request(std::uint64_t) noexcept;
    friend std::pair<WsApiSigningError, WsApiUnsignedRequest> build_user_data_unsubscribe_request(
        std::uint64_t, std::uint32_t) noexcept;
    friend std::pair<WsApiSigningError, WsApiUnsignedRequest> build_user_data_unsubscribe_all_request(std::uint64_t) noexcept;
    WsApiUnsignedRequest() noexcept = default;

    std::array<char, 256> buf_{};
    std::size_t len_{0};
    std::uint64_t id_{0};
};

namespace detail {

inline std::pair<WsApiSigningError, WsApiSignedRequest> build_ws_api_signed_request(
    BoundHmacCredentials& creds, std::string_view method, std::uint64_t request_id, std::span<const WsApiParam> params,
    ClockReadyToken clock_token) noexcept {
    using E = WsApiSigningError;
    if (request_id > kMaxWsApiRequestId) return {E::BadRequestId, WsApiSignedRequest{}};
    if (!is_valid_ws_api_method_name(method)) return {E::BadMethodName, WsApiSignedRequest{}};
    if (params.size() > kMaxWsApiParams) return {E::TooManyParams, WsApiSignedRequest{}};

    struct Entry {
        std::string_view name;
        std::string_view value;
        bool number{false};
    };
    std::array<Entry, kMaxWsApiParams + 2> entries{};
    std::size_t n = 0;
    for (const WsApiParam& p : params) {
        if (!is_valid_ws_api_param_name(p.name)) return {E::BadParamName, WsApiSignedRequest{}};
        if (is_reserved_ws_api_param_name(p.name)) return {E::ReservedParamName, WsApiSignedRequest{}};
        if (p.kind == WsApiParamKind::Text) {
            if (!is_safe_ws_api_text_value(p.value)) return {E::UnsafeParamValue, WsApiSignedRequest{}};
        } else if (p.kind == WsApiParamKind::Number) {
            if (!is_valid_ws_api_number(p.value)) return {E::BadNumberValue, WsApiSignedRequest{}};
        } else {
            return {E::UnsafeParamValue, WsApiSignedRequest{}};  // not a kind this build knows
        }
        entries[n++] = Entry{p.name, p.value, p.kind == WsApiParamKind::Number};
    }

    // apiKey: read from the bound credentials (this thread only) and re-checked here, because it is written into the frame
    // without escaping. A key that load_and_bind_credentials() accepted always passes.
    std::array<char, 128> key_buf{};
    std::size_t key_len = 0;
    if (!creds.copy_api_key(key_buf, key_len) || key_len == 0 ||
        !is_valid_api_key(std::string_view(key_buf.data(), key_len))) {
        return {E::ApiKeyUnavailable, WsApiSignedRequest{}};
    }

    // timestamp: only ever the exchange-corrected, fresh value a ClockReadyToken carries.
    const std::int64_t ts = clock_token.server_ms();
    if (ts < 0 || ts >= 10'000'000'000'000LL) return {E::TimestampOutOfRange, WsApiSignedRequest{}};
    std::array<char, 16> ts_buf{};
    std::size_t ts_len = 0;
    if (!ws_append_u64(ts_buf, ts_len, static_cast<std::uint64_t>(ts))) return {E::TimestampOutOfRange, WsApiSignedRequest{}};

    entries[n++] = Entry{"apiKey", std::string_view(key_buf.data(), key_len), false};
    entries[n++] = Entry{"timestamp", std::string_view(ts_buf.data(), ts_len), true};

    std::sort(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(n),
              [](const Entry& a, const Entry& b) noexcept { return a.name < b.name; });
    for (std::size_t i = 1; i < n; ++i) {
        if (entries[i].name == entries[i - 1].name) return {E::DuplicateParam, WsApiSignedRequest{}};
    }

    // The signing input: every entry, sorted, "name=value", joined with '&', no percent-encoding.
    std::array<char, kMaxWsApiPayloadBytes> payload{};
    std::size_t plen = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0 && !ws_append(payload, plen, "&")) return {E::PayloadTooLarge, WsApiSignedRequest{}};
        if (!ws_append(payload, plen, entries[i].name) || !ws_append(payload, plen, "=") ||
            !ws_append(payload, plen, entries[i].value)) {
            return {E::PayloadTooLarge, WsApiSignedRequest{}};
        }
    }
    const std::span<const char> sig = creds.sign(std::string_view(payload.data(), plen));
    if (sig.size() != kWsApiSignatureHexLen) return {E::SigningFailed, WsApiSignedRequest{}};

    // The frame, from the SAME sorted list, signature last.
    WsApiSignedRequest req{};
    std::span<char> out(req.buf_);
    if (!ws_append(out, req.len_, "{\"id\":") || !ws_append_u64(out, req.len_, request_id) ||
        !ws_append(out, req.len_, ",\"method\":\"") || !ws_append(out, req.len_, method) ||
        !ws_append(out, req.len_, "\",\"params\":{")) {
        return {E::FrameTooLarge, WsApiSignedRequest{}};
    }
    for (std::size_t i = 0; i < n; ++i) {
        const Entry& e = entries[i];
        if (i > 0 && !ws_append(out, req.len_, ",")) return {E::FrameTooLarge, WsApiSignedRequest{}};
        if (!ws_append(out, req.len_, "\"") || !ws_append(out, req.len_, e.name) || !ws_append(out, req.len_, "\":") ||
            (!e.number && !ws_append(out, req.len_, "\"")) || !ws_append(out, req.len_, e.value) ||
            (!e.number && !ws_append(out, req.len_, "\""))) {
            return {E::FrameTooLarge, WsApiSignedRequest{}};
        }
    }
    if (!ws_append(out, req.len_, ",\"signature\":\"") ||
        !ws_append(out, req.len_, std::string_view(sig.data(), sig.size())) || !ws_append(out, req.len_, "\"}}")) {
        return {E::FrameTooLarge, WsApiSignedRequest{}};
    }
    req.id_ = request_id;
    return {E::Ok, req};
}

}  // namespace detail

// userDataStream.subscribe.signature (P-25): apiKey, timestamp and signature, plus recvWindow when `recv_window_ms` is not 0.
// Any API key type works with it; it is the subscription an HMAC-only build can use (blueprint 5.2).
[[nodiscard]] inline std::pair<WsApiSigningError, WsApiSignedRequest> build_subscribe_signature_request(
    BoundHmacCredentials& creds, std::uint64_t request_id, ClockReadyToken clock_token,
    std::uint32_t recv_window_ms = 0) noexcept {
    if (recv_window_ms > kMaxWsApiRecvWindowMs) return {WsApiSigningError::BadRecvWindow, WsApiSignedRequest{}};
    const std::string_view method = ws_api_method_name(WsApiRequestKind::SubscribeSignature);
    if (recv_window_ms == 0) {
        return detail::build_ws_api_signed_request(creds, method, request_id, {}, clock_token);
    }
    char digits[12];
    std::size_t dlen = 0;
    detail::ws_append_u64(std::span<char>(digits), dlen, recv_window_ms);
    const WsApiParam params[] = {{"recvWindow", std::string_view(digits, dlen), WsApiParamKind::Number}};
    return detail::build_ws_api_signed_request(creds, method, request_id, params, clock_token);
}

// session.subscriptions: {"id":N,"method":"session.subscriptions","params":{}} -- the shape the official example shows.
[[nodiscard]] inline std::pair<WsApiSigningError, WsApiUnsignedRequest> build_session_subscriptions_request(
    std::uint64_t request_id) noexcept {
    if (request_id > kMaxWsApiRequestId) return {WsApiSigningError::BadRequestId, WsApiUnsignedRequest{}};
    WsApiUnsignedRequest req{};
    std::span<char> out(req.buf_);
    if (!detail::ws_append(out, req.len_, "{\"id\":") || !detail::ws_append_u64(out, req.len_, request_id) ||
        !detail::ws_append(out, req.len_, ",\"method\":\"") ||
        !detail::ws_append(out, req.len_, ws_api_method_name(WsApiRequestKind::SessionSubscriptions)) ||
        !detail::ws_append(out, req.len_, "\",\"params\":{}}")) {
        return {WsApiSigningError::FrameTooLarge, WsApiUnsignedRequest{}};
    }
    req.id_ = request_id;
    return {WsApiSigningError::Ok, req};
}

// userDataStream.unsubscribe with a subscriptionId: closes that subscription only.
[[nodiscard]] inline std::pair<WsApiSigningError, WsApiUnsignedRequest> build_user_data_unsubscribe_request(
    std::uint64_t request_id, std::uint32_t subscription_id) noexcept {
    if (request_id > kMaxWsApiRequestId) return {WsApiSigningError::BadRequestId, WsApiUnsignedRequest{}};
    WsApiUnsignedRequest req{};
    std::span<char> out(req.buf_);
    if (!detail::ws_append(out, req.len_, "{\"id\":") || !detail::ws_append_u64(out, req.len_, request_id) ||
        !detail::ws_append(out, req.len_, ",\"method\":\"") ||
        !detail::ws_append(out, req.len_, ws_api_method_name(WsApiRequestKind::UserDataUnsubscribe)) ||
        !detail::ws_append(out, req.len_, "\",\"params\":{\"subscriptionId\":") ||
        !detail::ws_append_u64(out, req.len_, subscription_id) || !detail::ws_append(out, req.len_, "}}")) {
        return {WsApiSigningError::FrameTooLarge, WsApiUnsignedRequest{}};
    }
    req.id_ = request_id;
    return {WsApiSigningError::Ok, req};
}

// userDataStream.unsubscribe without a parameter: closes EVERY subscription of the session (the official behavior).
[[nodiscard]] inline std::pair<WsApiSigningError, WsApiUnsignedRequest> build_user_data_unsubscribe_all_request(
    std::uint64_t request_id) noexcept {
    if (request_id > kMaxWsApiRequestId) return {WsApiSigningError::BadRequestId, WsApiUnsignedRequest{}};
    WsApiUnsignedRequest req{};
    std::span<char> out(req.buf_);
    if (!detail::ws_append(out, req.len_, "{\"id\":") || !detail::ws_append_u64(out, req.len_, request_id) ||
        !detail::ws_append(out, req.len_, ",\"method\":\"") ||
        !detail::ws_append(out, req.len_, ws_api_method_name(WsApiRequestKind::UserDataUnsubscribe)) ||
        !detail::ws_append(out, req.len_, "\"}")) {
        return {WsApiSigningError::FrameTooLarge, WsApiUnsignedRequest{}};
    }
    req.id_ = request_id;
    return {WsApiSigningError::Ok, req};
}

}  // namespace hy
