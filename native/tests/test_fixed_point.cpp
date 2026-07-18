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
