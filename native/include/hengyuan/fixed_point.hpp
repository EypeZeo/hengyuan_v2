// SPDX-License-Identifier: proprietary
// fixed_point.hpp — portable overflow-safe int64 helpers (no __int128, MSVC-safe).
// Governance: L1, pure arithmetic. No network/token/order.

#pragma once

#include <cstdint>
#include <limits>

namespace hy {

// Magnitude of a signed value as an unsigned type. Total function: |INT64_MIN|
// is 2^63, which does not fit in int64 but does fit here, so there is no input
// for which this is undefined.
//
// AUDIT ARITH-ABS-039: this is the idiom binance_clock_sync.hpp already
// documents under AUDIT TIME-SKEW-025 ("Negating INT64_MIN is signed-overflow
// UB ... take the magnitude of a signed difference via unsigned arithmetic").
// Prefer this over abs_i64() whenever the result is only compared against a
// bound -- comparison works fine in the unsigned domain and cannot overflow.
inline std::uint64_t abs_magnitude_u64(std::int64_t x) noexcept {
    return x < 0 ? (std::uint64_t{0} - static_cast<std::uint64_t>(x))
                 : static_cast<std::uint64_t>(x);
}

// AUDIT ARITH-ABS-039: previously `return x < 0 ? -x : x;`, which is
// signed-overflow UB for INT64_MIN (negating it has no representable result).
// The comment "Caller guarantees x != INT64_MIN" pushed an obligation onto
// callers that none of them actually checked.
//
// Now computed through the unsigned domain, so the operation itself is always
// well-defined. INT64_MIN still cannot produce a correct int64 answer -- no
// implementation can, |INT64_MIN| is not representable -- and this returns
// INT64_MIN for it (the C++20-defined result of the narrowing conversion).
// That value is still mathematically wrong, but it is now *defined* wrong
// rather than UB, which is the difference between "the optimizer may do
// anything" and "a wrong number a test can observe".
//
// If the result is only compared against a bound, use abs_magnitude_u64()
// instead -- it has no unrepresentable case at all.
inline std::int64_t abs_i64(std::int64_t x) noexcept {
    return static_cast<std::int64_t>(abs_magnitude_u64(x));
}

// Overflow-checked signed addition. Returns false (leaving out untouched) when
// a+b would overflow int64. General-purpose: unlike account_truth.hpp's
// checked_add(), this accepts negative operands, which position arithmetic
// needs.
inline bool safe_add_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (b >= 0) {
        if (a > std::numeric_limits<std::int64_t>::max() - b) return false;
    } else {
        if (a < std::numeric_limits<std::int64_t>::min() - b) return false;
    }
    out = a + b;
    return true;
}

// Overflow-checked signed multiply. Returns false (and leaves out untouched)
// when a*b would overflow int64. Checks BEFORE multiplying to avoid signed UB.
inline bool safe_mul_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > kMax / b) return false;
        } else {
            if (b < kMin / a) return false;
        }
    } else {
        if (b > 0) {
            if (a < kMin / b) return false;
        } else {
            if (a < kMax / b) return false;
        }
    }
    out = a * b;
    return true;
}

// True when (a * b) > limit, without overflow. Requires a > 0 and b > 0.
//
// AUDIT RISK-REFPRICE-033: the `a <= 0 || b <= 0 -> false` branch below reads
// as a harmless guard but means "no violation" -- which is fail-OPEN when the
// caller is a risk cap. It is kept (this is a pure predicate, and "0 * b does
// not exceed a non-negative limit" is arithmetically true), but callers using
// it as a *gate* must reject non-positive operands themselves BEFORE asking,
// rather than reading a `false` here as permission. risk_gate.hpp does exactly
// that now; see its BlockInvalidPrice path.
inline bool product_exceeds(std::int64_t a, std::int64_t b, std::int64_t limit) noexcept {
    if (a <= 0 || b <= 0) return false;
    // a*b > limit  <=>  a > limit / b  (floor division, exact for positive ints)
    return a > limit / b;
}

}  // namespace hy
