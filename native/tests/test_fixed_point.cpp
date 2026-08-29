// P2-EXEC-SIM-01: fixed_point overflow-safe helpers.
#include <gtest/gtest.h>
#include <hengyuan/fixed_point.hpp>
#include <cstdint>
#include <limits>

using hy::abs_i64;
using hy::product_exceeds;
using hy::safe_mul_i64;

TEST(FixedPoint, AbsBasic) {
    EXPECT_EQ(abs_i64(5), 5);
    EXPECT_EQ(abs_i64(-5), 5);
    EXPECT_EQ(abs_i64(0), 0);
}

TEST(SafeMul, NormalCases) {
    std::int64_t out = 0;
    EXPECT_TRUE(safe_mul_i64(100, 200, out));
    EXPECT_EQ(out, 20000);
    EXPECT_TRUE(safe_mul_i64(-100, 200, out));
    EXPECT_EQ(out, -20000);
    EXPECT_TRUE(safe_mul_i64(-100, -200, out));
    EXPECT_EQ(out, 20000);
    EXPECT_TRUE(safe_mul_i64(0, 999, out));
    EXPECT_EQ(out, 0);
}

TEST(SafeMul, OverflowDetected) {
    std::int64_t out = 123;
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    EXPECT_FALSE(safe_mul_i64(kMax, 2, out));
    EXPECT_FALSE(safe_mul_i64(kMax / 2 + 1, 3, out));
    EXPECT_FALSE(safe_mul_i64(-kMax, 3, out));
    // out unchanged on overflow
    EXPECT_EQ(out, 123);
}

TEST(SafeMul, NearLimitOk) {
    std::int64_t out = 0;
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    EXPECT_TRUE(safe_mul_i64(kMax / 2, 2, out));
    EXPECT_EQ(out, (kMax / 2) * 2);
}

TEST(ProductExceeds, Basic) {
    EXPECT_TRUE(product_exceeds(100, 100, 9999));    // 10000 > 9999
    EXPECT_FALSE(product_exceeds(100, 100, 10000));  // 10000 not > 10000
    EXPECT_FALSE(product_exceeds(100, 100, 10001));
    EXPECT_FALSE(product_exceeds(0, 100, 5));
    EXPECT_FALSE(product_exceeds(100, 0, 5));
}

TEST(ProductExceeds, NoOverflowAtLargeValues) {
    constexpr std::int64_t big = 5'000'000'000LL;  // 5e9, product would be 2.5e19 (overflows i64)
    // Uses division internally so no overflow occurs.
    EXPECT_TRUE(product_exceeds(big, big, 1'000'000));
}

// --- AUDIT ARITH-ABS-039 regressions ---
//
// abs_i64(INT64_MIN) used to be signed-overflow UB (negating a value with no
// representable negation). These tests do not assert a "correct" magnitude for
// that input -- |INT64_MIN| is not representable in int64, so no correct int64
// answer exists -- they assert only that the operation is DEFINED and that the
// unsigned form, which does have a representable answer, is exact.

TEST(FixedPoint, AbsMagnitudeU64IsTotal) {
    EXPECT_EQ(hy::abs_magnitude_u64(0), 0u);
    EXPECT_EQ(hy::abs_magnitude_u64(5), 5u);
    EXPECT_EQ(hy::abs_magnitude_u64(-5), 5u);
    EXPECT_EQ(hy::abs_magnitude_u64(std::numeric_limits<std::int64_t>::max()),
              static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    // The whole point: this input has a representable answer here and only here.
    EXPECT_EQ(hy::abs_magnitude_u64(std::numeric_limits<std::int64_t>::min()),
              static_cast<std::uint64_t>(1) << 63);
}

TEST(FixedPoint, AbsI64AtInt64MinIsDefinedNotUB) {
    // Under UBSan this test is the assertion: it fails the build/run if the
    // implementation reintroduces the negation.
    const std::int64_t r = abs_i64(std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(r, std::numeric_limits<std::int64_t>::min());  // defined, still not "correct"
}

TEST(FixedPoint, SafeAddI64Boundaries) {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    std::int64_t out = 0;

    EXPECT_TRUE(hy::safe_add_i64(1, 2, out));
    EXPECT_EQ(out, 3);
    EXPECT_TRUE(hy::safe_add_i64(kMax, 0, out));
    EXPECT_EQ(out, kMax);
    EXPECT_TRUE(hy::safe_add_i64(kMin, 0, out));
    EXPECT_EQ(out, kMin);
    EXPECT_TRUE(hy::safe_add_i64(kMax - 1, 1, out));
    EXPECT_EQ(out, kMax);

    EXPECT_FALSE(hy::safe_add_i64(kMax, 1, out));
    EXPECT_FALSE(hy::safe_add_i64(kMin, -1, out));
    EXPECT_FALSE(hy::safe_add_i64(kMax, kMax, out));
    EXPECT_FALSE(hy::safe_add_i64(kMin, kMin, out));
}
