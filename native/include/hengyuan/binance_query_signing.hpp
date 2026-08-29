// SPDX-License-Identifier: proprietary
// binance_query_signing.hpp — Binance L4 §2.1 query string construction +
// signing (CanonicalUnsignedQuery/SignedQuery/build_canonical_query()/
// build_signed_query()).
//
// QuerySigningError itself is defined in binance_environment.hpp (which
// this header depends on) — see that header's comment for why.
//
// Governance: L4 (offline logic only — this header performs no network
// I/O; it only assembles and signs the bytes a caller will later send).

#pragma once

#include <hengyuan/binance_environment.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

namespace hy {

inline constexpr std::size_t kMaxQueryParams = 32;
inline constexpr std::size_t kMaxQueryLen = 2048;   // headroom for a typical order's params

namespace detail {

inline bool is_unreserved(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

// Percent-encodes `value` into buf starting at *len, bounded by buf.size().
// Returns false (no partial write beyond what was already there) if it
// would not fit.
inline bool append_percent_encoded(std::span<char> buf, std::size_t& len,
                                    std::string_view value) noexcept {
    static constexpr char hex[] = "0123456789ABCDEF";
    for (char raw_c : value) {
        const auto c = static_cast<unsigned char>(raw_c);
        if (is_unreserved(static_cast<char>(c))) {
            if (len >= buf.size()) return false;
            buf[len++] = static_cast<char>(c);
        } else {
            if (len + 3 > buf.size()) return false;
            buf[len++] = '%';
            buf[len++] = hex[c >> 4];
            buf[len++] = hex[c & 0x0F];
        }
    }
    return true;
}

inline bool append_literal(std::span<char> buf, std::size_t& len,
                            std::string_view lit) noexcept {
    if (len + lit.size() > buf.size()) return false;
    std::memcpy(buf.data() + len, lit.data(), lit.size());
    len += lit.size();
    return true;
}

// AUDIT L4-RESERVED-PARAM-004: "timestamp"/"signature" are appended by
// build_signed_query() itself, always last, after this file's own
// canonicalization runs. A caller-supplied param using either name would
// produce a wire string with two occurrences of the same key — not a
// forgery surface (the signature covers both), but the server's choice of
// which one "wins" would diverge from what the caller meant to send.
inline bool is_reserved_query_key(std::string_view key) noexcept {
    return key == "timestamp" || key == "signature";
}

}  // namespace detail

// §2.1: fixed lexicographic-by-key order, reject duplicate keys (->
// DuplicateParam) and reject "timestamp"/"signature" keys (->
// ReservedParamName — those two are appended by build_signed_query()
// itself, see AUDIT L4-RESERVED-PARAM-004 below), percent-encode anything
// outside the RFC3986 unreserved set (A-Za-z0-9-._~). recvWindow is an
// ordinary business param the caller includes here — it's a fixed policy
// value, not something needing "last-moment" freshness, so
// build_signed_query() does not append it (unlike timestamp).
//
// params.size() < 1 or > kMaxQueryParams -> QueryTooLarge (same branch;
// "count invalid" doesn't distinguish "too few" from "too many"). Empty
// input is never a legitimate call in practice — recvWindow alone always
// contributes at least one entry — so this can't reject a real caller.
class CanonicalUnsignedQuery {
public:
    std::string_view bytes() const noexcept { return {buf_.data(), len_}; }

private:
    friend std::pair<QuerySigningError, CanonicalUnsignedQuery> build_canonical_query(
        std::span<const std::pair<std::string_view, std::string_view>> params) noexcept;
    CanonicalUnsignedQuery() noexcept = default;
    std::array<char, kMaxQueryLen> buf_{};
    std::size_t len_{0};

    // build_canonical_query() rejects empty input above, so the production
    // path can never produce a bytes().empty() instance — but
    // build_signed_query()'s defensive empty-input check needs exactly
    // that instance to be testable. Unconditional declaration; literal-
    // identical across every TU.
    friend class CanonicalUnsignedQueryTestHooks;
};

inline std::pair<QuerySigningError, CanonicalUnsignedQuery> build_canonical_query(
    std::span<const std::pair<std::string_view, std::string_view>> params) noexcept {
    if (params.size() < 1 || params.size() > kMaxQueryParams) {
        return {QuerySigningError::QueryTooLarge, CanonicalUnsignedQuery{}};
    }

    for (const auto& kv : params) {
        if (detail::is_reserved_query_key(kv.first)) {
            return {QuerySigningError::ReservedParamName, CanonicalUnsignedQuery{}};
        }
    }

    std::array<std::size_t, kMaxQueryParams> order{};
    for (std::size_t i = 0; i < params.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(params.size()),
              [&](std::size_t a, std::size_t b) noexcept {
                  return params[a].first < params[b].first;
              });

    for (std::size_t i = 1; i < params.size(); ++i) {
        if (params[order[i]].first == params[order[i - 1]].first) {
            return {QuerySigningError::DuplicateParam, CanonicalUnsignedQuery{}};
        }
    }

    CanonicalUnsignedQuery q{};
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto& kv = params[order[i]];
        if (i > 0 && !detail::append_literal(q.buf_, q.len_, "&")) {
            return {QuerySigningError::QueryTooLarge, CanonicalUnsignedQuery{}};
        }
        if (!detail::append_percent_encoded(q.buf_, q.len_, kv.first)) {
            return {QuerySigningError::QueryTooLarge, CanonicalUnsignedQuery{}};
        }
        if (!detail::append_literal(q.buf_, q.len_, "=")) {
            return {QuerySigningError::QueryTooLarge, CanonicalUnsignedQuery{}};
        }
        if (!detail::append_percent_encoded(q.buf_, q.len_, kv.second)) {
            return {QuerySigningError::QueryTooLarge, CanonicalUnsignedQuery{}};
        }
    }

    return {QuerySigningError::Ok, q};
}

// build_signed_query() appends "&timestamp=" + up to 13 decimal digits (ms
// since epoch — 13 digits through year 2286) + "&signature=" + 64 hex
// digits (HMAC-SHA256, matches BinanceSigner::kHexLen) onto
// unsigned_query.bytes().
inline constexpr std::size_t kTimestampLiteralLen = sizeof("&timestamp=") - 1;  // 11
inline constexpr std::size_t kMaxTimestampDigits = 13;
inline constexpr std::size_t kSignatureLiteralLen = sizeof("&signature=") - 1;  // 11
inline constexpr std::size_t kHexSignatureLen = 64;
inline constexpr std::size_t kSignatureParamLen =
    kTimestampLiteralLen + kMaxTimestampDigits + kSignatureLiteralLen + kHexSignatureLen;
static_assert(kSignatureParamLen == 99);

// Unforgeable: only build_signed_query() can produce one, and its mere
// existence proves the "signed bytes == sent bytes" invariant held (buf_
// is exactly the bytes that were HMAC'd, plus the signature suffix that
// was appended only after signing completed).
class SignedQuery {
public:
    std::string_view wire_bytes() const noexcept { return {buf_.data(), len_}; }

private:
    friend std::pair<QuerySigningError, SignedQuery> build_signed_query(
        BoundHmacCredentials&, const CanonicalUnsignedQuery&, std::int64_t) noexcept;
    SignedQuery() noexcept = default;
    std::array<char, kMaxQueryLen + kSignatureParamLen> buf_{};
    std::size_t len_{0};
};

// fresh_ts_ms: the caller must obtain this via try_get_signing_timestamp_ms()
// at the last possible moment before this call, never an earlier-cached
// value — "no caching" is enforced by this parameter-passing path itself,
// there is no cross-call slot for a caller to misuse.
//
// Signature byte contract (must be followed literally):
//   signing input = unsigned_query.bytes() + "&timestamp=" + <fresh_ts_ms>
//                   (up to, not including, "&signature=")
//   signature: computed over the signing input above, appended only AFTER
//              HMAC-SHA256 completes — never itself part of the HMAC input
//              (folding "&signature=" or the whole concatenated string
//              back into the signature computation is spec §2.1's own
//              named "most common real-world Binance signature mismatch
//              (-1022) bug source").
//   wire_bytes() = signing input + "&signature=" + 64 hex digits
//
// CanonicalUnsignedQuery is lexicographically ordered and "&timestamp="/
// "&signature=" are always appended last regardless of where "timestamp"
// would sort among the business param keys — so the final wire byte order
// is not strictly lexicographic across the whole string once a key sorts
// after "timestamp" (e.g. "type"). Spec §2.1 only requires the order be
// fixed/deterministic/testable (Binance's server does not itself check
// param order), and appending these two fixed suffixes last is exactly
// that: a fixed, deterministic construction, so this is compliant, not a
// bug.
inline std::pair<QuerySigningError, SignedQuery> build_signed_query(
    BoundHmacCredentials& creds,
    const CanonicalUnsignedQuery& unsigned_query,
    std::int64_t fresh_ts_ms) noexcept {
    // Defense in depth: unreachable from build_canonical_query()'s normal
    // path (it already rejects empty input), but zero-cost and consistent
    // with this design's fail-closed discipline throughout.
    if (unsigned_query.bytes().empty()) {
        return {QuerySigningError::QueryTooLarge, SignedQuery{}};
    }
    // kMaxTimestampDigits is a reserved-capacity budget, not a self-
    // enforcing bound — an out-of-range fresh_ts_ms (negative, or >= 10^13)
    // would format to more characters than that budget without this check,
    // which is exactly the buffer-overflow entry point this guards.
    if (fresh_ts_ms < 0 || fresh_ts_ms >= 10'000'000'000'000LL) {
        return {QuerySigningError::QueryTooLarge, SignedQuery{}};
    }

    SignedQuery q{};
    if (!detail::append_literal(q.buf_, q.len_, unsigned_query.bytes())) {
        return {QuerySigningError::QueryTooLarge, SignedQuery{}};
    }
    if (!detail::append_literal(q.buf_, q.len_, "&timestamp=")) {
        return {QuerySigningError::QueryTooLarge, SignedQuery{}};
    }
    {
        char digits[kMaxTimestampDigits];
        int n = 0;
        auto v = static_cast<std::uint64_t>(fresh_ts_ms);
        if (v == 0) {
            digits[n++] = '0';
        } else {
            while (v > 0 && n < static_cast<int>(kMaxTimestampDigits)) {
                digits[n++] = static_cast<char>('0' + (v % 10));
                v /= 10;
            }
            if (v != 0) {
                // Unreachable given the range check above; second capacity
                // line of defense rather than a format-loop overrun.
                return {QuerySigningError::QueryTooLarge, SignedQuery{}};
            }
        }
        if (q.len_ + static_cast<std::size_t>(n) > q.buf_.size()) {
            return {QuerySigningError::QueryTooLarge, SignedQuery{}};
        }
        for (int i = n - 1; i >= 0; --i) {
            q.buf_[q.len_++] = digits[i];
        }
    }

    std::string_view signing_input(q.buf_.data(), q.len_);
    std::span<const char> sig = creds.sign(signing_input);
    if (sig.empty()) {
        return {QuerySigningError::SigningFailed, SignedQuery{}};
    }

    if (!detail::append_literal(q.buf_, q.len_, "&signature=")) {
        return {QuerySigningError::QueryTooLarge, SignedQuery{}};
    }
    if (q.len_ + sig.size() > q.buf_.size()) {
        return {QuerySigningError::QueryTooLarge, SignedQuery{}};
    }
    std::memcpy(q.buf_.data() + q.len_, sig.data(), sig.size());
    q.len_ += sig.size();

    return {QuerySigningError::Ok, q};
}

}  // namespace hy
