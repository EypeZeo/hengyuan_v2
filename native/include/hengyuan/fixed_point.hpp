// SPDX-License-Identifier: proprietary
// fixed_point.hpp — portable overflow-safe int64 helpers (no __int128, MSVC-safe).
// Governance: L1, pure arithmetic. No network/token/order.

#pragma once

#include <cstdint>
#include <limits>

namespace hy {

inline std::int64_t abs_i64(std::int64_t x) noexcept {
    // Caller guarantees x != INT64_MIN for simulation-scale values.
    return x < 0 ? -x : x;
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

// True when (a * b) > limit for non-negative a, b, limit, without overflow.
inline bool product_exceeds(std::int64_t a, std::int64_t b, std::int64_t limit) noexcept {
    if (a <= 0 || b <= 0) return false;
    // a*b > limit  <=>  a > limit / b  (floor division, exact for positive ints)
    return a > limit / b;
}

}  // namespace hy
