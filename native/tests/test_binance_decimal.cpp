// L4 §5.1 (PR 5b): derive_scale_from_decimal_string() / parse_decimal_to_ticks_with_scale()
// tests. Pure logic, no network — parse_balance_decimal_to_ticks()'s own tests remain in
// test_binance_private_rest.cpp where they already lived; this file only covers the two
// functions binance_decimal.hpp adds new for L4 §5.
#include <gtest/gtest.h>
#include <hengyuan/binance_decimal.hpp>

using hy::derive_scale_from_decimal_string;
using hy::parse_decimal_to_ticks_with_scale;

// --- derive_scale_from_decimal_string() ---

TEST(DeriveScaleFromDecimalString, FourFractionalDigitsAfterStrippingTrailingZeros) {
    std::uint8_t scale = 0xFF;
    ASSERT_TRUE(derive_scale_from_decimal_string("0.00010000", scale));
    EXPECT_EQ(scale, 4);
}

TEST(DeriveScaleFromDecimalString, IntegerValuedTickSizeHasScaleZero) {
    std::uint8_t scale = 0xFF;
    ASSERT_TRUE(derive_scale_from_decimal_string("1.00000000", scale));
    EXPECT_EQ(scale, 0);
}

TEST(DeriveScaleFromDecimalString, EightFractionalDigitsNoTrailingZerosToStrip) {
    std::uint8_t scale = 0xFF;
    ASSERT_TRUE(derive_scale_from_decimal_string("0.00000001", scale));
    EXPECT_EQ(scale, 8);
}

TEST(DeriveScaleFromDecimalString, BareIntegerNoDecimalPointHasScaleZero) {
    std::uint8_t scale = 0xFF;
    ASSERT_TRUE(derive_scale_from_decimal_string("5", scale));
    EXPECT_EQ(scale, 0);
}

TEST(DeriveScaleFromDecimalString, ZeroVariantsAreRejectedNotGivenScaleZero) {
    std::uint8_t scale = 0xFF;
    EXPECT_FALSE(derive_scale_from_decimal_string("0", scale));
    EXPECT_FALSE(derive_scale_from_decimal_string("0.0", scale));
    EXPECT_FALSE(derive_scale_from_decimal_string("0.00000000", scale));
}

TEST(DeriveScaleFromDecimalString, EmptyStringIsRejected) {
    std::uint8_t scale = 0xFF;
    EXPECT_FALSE(derive_scale_from_decimal_string("", scale));
}

TEST(DeriveScaleFromDecimalString, NegativeIsRejected) {
    std::uint8_t scale = 0xFF;
    EXPECT_FALSE(derive_scale_from_decimal_string("-0.0001", scale));
}

TEST(DeriveScaleFromDecimalString, LeadingDotWithNoIntegerDigitIsRejected) {
    std::uint8_t scale = 0xFF;
    EXPECT_FALSE(derive_scale_from_decimal_string(".5", scale));
}

TEST(DeriveScaleFromDecimalString, TrailingDotWithNoFractionalDigitIsRejected) {
    std::uint8_t scale = 0xFF;
    EXPECT_FALSE(derive_scale_from_decimal_string("5.", scale));
}

TEST(DeriveScaleFromDecimalString, NonDigitCharacterIsRejected) {
    std::uint8_t scale = 0xFF;
    EXPECT_FALSE(derive_scale_from_decimal_string("1.2a", scale));
    EXPECT_FALSE(derive_scale_from_decimal_string("1a.2", scale));
}

TEST(DeriveScaleFromDecimalString, ExcessivelyLongFractionalPartIsRejected) {
    std::uint8_t scale = 0xFF;
    // 19 fractional digits -- past the [0,18] domain this function's own callers require.
    EXPECT_FALSE(derive_scale_from_decimal_string("1.1000000000000000001", scale));
}

// --- parse_decimal_to_ticks_with_scale() ---

TEST(ParseDecimalToTicksWithScale, ScaleZeroBinancePaddedIntegerParsesSuccessfully) {
    // L4 §5.1's core Binance-formatting-convention case: stepSize="1.00000000" derives
    // qty_scale=0, but the sibling minQty field still arrives as an 8-decimal-padded string.
    std::int64_t out = -1;
    ASSERT_TRUE(parse_decimal_to_ticks_with_scale("1.00000000", 0, out));
    EXPECT_EQ(out, 1);
}

TEST(ParseDecimalToTicksWithScale, ScaleZeroGenuineExcessPrecisionIsRejected) {
    std::int64_t out = -1;
    EXPECT_FALSE(parse_decimal_to_ticks_with_scale("1.00000001", 0, out));
}

TEST(ParseDecimalToTicksWithScale, ScaleFourBinancePaddedValueParsesSuccessfully) {
    std::int64_t out = -1;
    ASSERT_TRUE(parse_decimal_to_ticks_with_scale("1.23450000", 4, out));
    EXPECT_EQ(out, 12345);
}

TEST(ParseDecimalToTicksWithScale, ScaleFourGenuineExcessPrecisionIsRejected) {
    std::int64_t out = -1;
    EXPECT_FALSE(parse_decimal_to_ticks_with_scale("1.23450001", 4, out));
}

TEST(ParseDecimalToTicksWithScale, FewerDigitsThanScalePadsWithZeros) {
    std::int64_t out = -1;
    ASSERT_TRUE(parse_decimal_to_ticks_with_scale("1.5", 4, out));
    EXPECT_EQ(out, 15000);
}

TEST(ParseDecimalToTicksWithScale, BareIntegerAtNonzeroScaleScalesUp) {
    std::int64_t out = -1;
    ASSERT_TRUE(parse_decimal_to_ticks_with_scale("7", 4, out));
    EXPECT_EQ(out, 70000);
}

TEST(ParseDecimalToTicksWithScale, NegativeIsRejected) {
    std::int64_t out = -1;
    EXPECT_FALSE(parse_decimal_to_ticks_with_scale("-1.5", 4, out));
}

TEST(ParseDecimalToTicksWithScale, EmptyStringIsRejected) {
    std::int64_t out = -1;
    EXPECT_FALSE(parse_decimal_to_ticks_with_scale("", 4, out));
}

TEST(ParseDecimalToTicksWithScale, TrailingDotWithNoFractionalDigitIsRejected) {
    std::int64_t out = -1;
    EXPECT_FALSE(parse_decimal_to_ticks_with_scale("5.", 4, out));
}

TEST(ParseDecimalToTicksWithScale, NonDigitCharacterIsRejected) {
    std::int64_t out = -1;
    EXPECT_FALSE(parse_decimal_to_ticks_with_scale("1.2a", 4, out));
}

TEST(ParseDecimalToTicksWithScale, ScaleAboveEighteenIsRejected) {
    std::int64_t out = -1;
    EXPECT_FALSE(parse_decimal_to_ticks_with_scale("1", 19, out));
}

TEST(ParseDecimalToTicksWithScale, OverflowIsRejected) {
    std::int64_t out = -1;
    // integer_part * 10^18 overflows int64 for any integer_part > 9.
    EXPECT_FALSE(parse_decimal_to_ticks_with_scale("99999999999999999999", 18, out));
}

TEST(ParseDecimalToTicksWithScale, MaxValidScaleEighteenAtZeroWorks) {
    std::int64_t out = -1;
    ASSERT_TRUE(parse_decimal_to_ticks_with_scale("0.000000000000000001", 18, out));
    EXPECT_EQ(out, 1);
}
