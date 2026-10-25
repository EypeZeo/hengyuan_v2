// SPDX-License-Identifier: proprietary
// test_operator_resolution_evidence.cpp -- D5 slice R2: the decisions behind the evidence for an OrderOperatorResolved frame
// (operator_resolution_evidence.hpp, docs/OPERATOR_RESOLUTION_DESIGN.md 5 and 6). Pure logic, so everything is a table: each
// refusal has a case that flips exactly that one input, and each boundary is tested at the boundary and one millisecond either
// side. The response parser has its own test (test_operator_resolution_response.cpp); this one links no JSON library, which is
// the point of the split.

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <hengyuan/operator_resolution_evidence.hpp>
#include <hengyuan/operator_resolution_frame.hpp>
#include <hengyuan/order_lifecycle.hpp>

namespace {

// Not `using namespace hy;`: hy::detail and hy::operator_resolution::detail would then both be visible as `detail`.
using namespace hy::operator_resolution;
using hy::ClientOrderId;
using hy::kClientOrderIdLen;
using hy::make_client_order_id;

constexpr std::int64_t kNow = 1'790'000'000'000;  // 2026-09-21T06:13:20Z, inside the plausible range
constexpr ExchangeMs at(std::int64_t delta_ms) { return ExchangeMs{kNow + delta_ms}; }
constexpr std::int64_t kHour = 3'600'000;
constexpr std::int64_t kDay = 24 * kHour;

// constexpr is part of the contract (CLAUDE.md, mandate 3): these lines fail to COMPILE if a function stops being constant-evaluable.
static_assert(check_evidence_read(at(0), at(270'000), FenceParams{}) == FenceVerdict::Satisfied);
static_assert(check_evidence_read(at(0), at(269'999), FenceParams{}) == FenceVerdict::TooEarly);
static_assert(plan_all_orders_windows(at(0), at(kHour)).count == 1);
static_assert(client_order_id_timestamp("HY-1790000000000-1-0042")->count() == kNow);
static_assert(parse_absent_detail_code(0) == std::nullopt);

// --- the fence ------------------------------------------------------------------------------------------------------------

TEST(OperatorResolutionFence, TheDefaultsAreTheDesignNumbers) {
    const FenceParams p{};
    EXPECT_EQ(p.recv_window_ms, 60'000);
    EXPECT_EQ(p.clock_skew_ms, 60'000);
    EXPECT_EQ(p.margin_ms, 30'000);
    EXPECT_EQ(p.read_lag_ms, 120'000);
    EXPECT_EQ(kDefaultEvidenceWaitMs, 270'000) << "4.5 minutes, design 5.2";
    EXPECT_EQ(validate_fence_params(p), FenceParamsStatus::Ok);
    // The floors are the defaults: a parameter may be raised, never lowered.
    EXPECT_EQ(kRecvWindowFloorMs, 60'000);
    EXPECT_EQ(kClockSkewFloorMs, 60'000);
    EXPECT_EQ(kFenceMarginFloorMs, 30'000);
    EXPECT_EQ(kReadLagFloorMs, 120'000);
    EXPECT_EQ(kFenceParamCeilingMs, 3'600'000);
}

TEST(OperatorResolutionFence, TheDeadlineAndTheEarliestReadAreSumsOfTheFourTerms) {
    const auto deadline = fence_deadline(at(0), FenceParams{});
    const auto earliest = earliest_evidence_read(at(0), FenceParams{});
    ASSERT_TRUE(deadline.has_value());
    ASSERT_TRUE(earliest.has_value());
    EXPECT_EQ(deadline->count(), kNow + 60'000 + 60'000 + 30'000);
    EXPECT_EQ(earliest->count(), kNow + 60'000 + 60'000 + 30'000 + 120'000);
}

TEST(OperatorResolutionFence, EveryTermMovesTheBoundaryByExactlyItsOwnAmount) {
    // Raise each parameter by one second: the deadline and the earliest read move by exactly that, L only the latter. A
    // version that drops a term, or counts one twice, fails one of these lines.
    const FenceParams base{};
    const std::int64_t base_deadline = fence_deadline(at(0), base)->count();
    const std::int64_t base_earliest = earliest_evidence_read(at(0), base)->count();

    FenceParams w = base;
    w.recv_window_ms += 1000;
    EXPECT_EQ(fence_deadline(at(0), w)->count(), base_deadline + 1000);
    EXPECT_EQ(earliest_evidence_read(at(0), w)->count(), base_earliest + 1000);

    FenceParams b = base;
    b.clock_skew_ms += 1000;
    EXPECT_EQ(fence_deadline(at(0), b)->count(), base_deadline + 1000);
    EXPECT_EQ(earliest_evidence_read(at(0), b)->count(), base_earliest + 1000);

    FenceParams m = base;
    m.margin_ms += 1000;
    EXPECT_EQ(fence_deadline(at(0), m)->count(), base_deadline + 1000);
    EXPECT_EQ(earliest_evidence_read(at(0), m)->count(), base_earliest + 1000);

    FenceParams l = base;
    l.read_lag_ms += 1000;
    EXPECT_EQ(fence_deadline(at(0), l)->count(), base_deadline) << "L is not part of the fence deadline";
    EXPECT_EQ(earliest_evidence_read(at(0), l)->count(), base_earliest + 1000);

    // ...and the old boundary read is no longer enough once any term is raised.
    for (const FenceParams& raised : {w, b, m, l}) {
        EXPECT_EQ(check_evidence_read(at(0), at(270'000), raised), FenceVerdict::TooEarly);
        EXPECT_EQ(check_evidence_read(at(0), at(271'000), raised), FenceVerdict::Satisfied);
    }
}

TEST(OperatorResolutionFence, TheBoundaryIsExactToTheMillisecond) {
    EXPECT_EQ(check_evidence_read(at(0), at(269'999), FenceParams{}), FenceVerdict::TooEarly);
    EXPECT_EQ(check_evidence_read(at(0), at(270'000), FenceParams{}), FenceVerdict::Satisfied);
    EXPECT_EQ(check_evidence_read(at(0), at(270'001), FenceParams{}), FenceVerdict::Satisfied);
    EXPECT_EQ(check_evidence_read(at(0), at(kDay), FenceParams{}), FenceVerdict::Satisfied) << "waiting longer only helps";
    // the fence counts from the lock reference, wherever that is
    EXPECT_EQ(check_evidence_read(at(5'000), at(5'000 + 269'999), FenceParams{}), FenceVerdict::TooEarly);
    EXPECT_EQ(check_evidence_read(at(5'000), at(5'000 + 270'000), FenceParams{}), FenceVerdict::Satisfied);
}

TEST(OperatorResolutionFence, ReadingAtTheLockOrBeforeIsRefused) {
    EXPECT_EQ(check_evidence_read(at(0), at(0), FenceParams{}), FenceVerdict::TooEarly);
    EXPECT_EQ(check_evidence_read(at(0), at(-1), FenceParams{}), FenceVerdict::ReadBeforeLock);
    EXPECT_EQ(check_evidence_read(at(0), at(-kDay), FenceParams{}), FenceVerdict::ReadBeforeLock);
}

TEST(OperatorResolutionFence, ParametersBelowTheirFloorAreRefused) {
    FenceParams p{};
    p.recv_window_ms = kRecvWindowFloorMs - 1;
    EXPECT_EQ(validate_fence_params(p), FenceParamsStatus::RecvWindowOutOfRange);
    EXPECT_EQ(check_evidence_read(at(0), at(kDay), p), FenceVerdict::InvalidParams);
    EXPECT_FALSE(fence_deadline(at(0), p).has_value());
    EXPECT_FALSE(earliest_evidence_read(at(0), p).has_value());

    p = FenceParams{};
    p.clock_skew_ms = kClockSkewFloorMs - 1;
    EXPECT_EQ(validate_fence_params(p), FenceParamsStatus::ClockSkewOutOfRange);
    EXPECT_EQ(check_evidence_read(at(0), at(kDay), p), FenceVerdict::InvalidParams);

    p = FenceParams{};
    p.margin_ms = kFenceMarginFloorMs - 1;
    EXPECT_EQ(validate_fence_params(p), FenceParamsStatus::MarginOutOfRange);
    EXPECT_EQ(check_evidence_read(at(0), at(kDay), p), FenceVerdict::InvalidParams);

    p = FenceParams{};
    p.read_lag_ms = kReadLagFloorMs - 1;
    EXPECT_EQ(validate_fence_params(p), FenceParamsStatus::ReadLagOutOfRange);
    EXPECT_EQ(check_evidence_read(at(0), at(kDay), p), FenceVerdict::InvalidParams);

    // zero, negative and the extremes are below the floors too
    for (const std::int64_t bad : {std::int64_t{0}, std::int64_t{-1}, std::numeric_limits<std::int64_t>::min()}) {
        p = FenceParams{};
        p.read_lag_ms = bad;
        EXPECT_EQ(validate_fence_params(p), FenceParamsStatus::ReadLagOutOfRange) << bad;
    }
}

TEST(OperatorResolutionFence, ParametersAboveTheCeilingAreRefused) {
    for (int which = 0; which < 4; ++which) {
        FenceParams p{};
        std::int64_t* field = which == 0 ? &p.recv_window_ms : which == 1 ? &p.clock_skew_ms : which == 2 ? &p.margin_ms : &p.read_lag_ms;
        *field = kFenceParamCeilingMs + 1;
        EXPECT_NE(validate_fence_params(p), FenceParamsStatus::Ok) << which;
        EXPECT_EQ(check_evidence_read(at(0), at(kDay), p), FenceVerdict::InvalidParams) << which;
        *field = std::numeric_limits<std::int64_t>::max();
        EXPECT_NE(validate_fence_params(p), FenceParamsStatus::Ok) << which;
    }
}

TEST(OperatorResolutionFence, ParametersAtTheirFloorAndAtTheCeilingAreAccepted) {
    FenceParams lo{};
    EXPECT_EQ(validate_fence_params(lo), FenceParamsStatus::Ok);
    FenceParams hi{kFenceParamCeilingMs, kFenceParamCeilingMs, kFenceParamCeilingMs, kFenceParamCeilingMs};
    EXPECT_EQ(validate_fence_params(hi), FenceParamsStatus::Ok);
    EXPECT_EQ(earliest_evidence_read(at(0), hi)->count(), kNow + 4 * kFenceParamCeilingMs);
}

TEST(OperatorResolutionFence, GarbageTimesNeverSatisfyAFence) {
    // The dangerous case: a serverTime that failed to parse and defaulted to 0 as the LOCK reference would make "read - lock" a
    // huge number and pass. Every implausible reading, on either side, is refused as such.
    const std::int64_t garbage[] = {std::numeric_limits<std::int64_t>::min(), -1, 0, 1, 1000,
                                    kMinPlausibleExchangeMs - 1, kMaxPlausibleExchangeMs + 1,
                                    std::numeric_limits<std::int64_t>::max()};
    for (const std::int64_t g : garbage) {
        EXPECT_EQ(check_evidence_read(ExchangeMs{g}, at(kDay), FenceParams{}), FenceVerdict::ImplausibleTime) << "lock " << g;
        EXPECT_EQ(check_evidence_read(at(0), ExchangeMs{g}, FenceParams{}), FenceVerdict::ImplausibleTime) << "read " << g;
        EXPECT_FALSE(fence_deadline(ExchangeMs{g}, FenceParams{}).has_value()) << g;
        EXPECT_FALSE(earliest_evidence_read(ExchangeMs{g}, FenceParams{}).has_value()) << g;
        EXPECT_EQ(waited_seconds(ExchangeMs{g}, at(kDay)), 0) << g;
        EXPECT_EQ(waited_seconds(at(0), ExchangeMs{g}), 0) << g;
    }
    // the limits themselves are plausible
    EXPECT_TRUE(is_plausible(ExchangeMs{kMinPlausibleExchangeMs}));
    EXPECT_TRUE(is_plausible(ExchangeMs{kMaxPlausibleExchangeMs}));
    EXPECT_EQ(kMinPlausibleExchangeMs, 1'577'836'800'000) << "2020-01-01T00:00:00Z";
    EXPECT_EQ(kMaxPlausibleExchangeMs, 4'102'444'800'000) << "2100-01-01T00:00:00Z";
}

TEST(OperatorResolutionFence, TheTopOfThePlausibleRangeDoesNotOverflow) {
    const FenceParams hi{kFenceParamCeilingMs, kFenceParamCeilingMs, kFenceParamCeilingMs, kFenceParamCeilingMs};
    const ExchangeMs top{kMaxPlausibleExchangeMs};
    EXPECT_TRUE(earliest_evidence_read(top, hi).has_value());
    EXPECT_EQ(check_evidence_read(top, top, hi), FenceVerdict::TooEarly);
}

TEST(OperatorResolutionFence, CheckedArithmeticRefusesToWrap) {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    EXPECT_EQ(detail::checked_add(5, 7), std::optional<std::int64_t>(12));
    EXPECT_EQ(detail::checked_add(kMax, 0), std::optional<std::int64_t>(kMax));
    EXPECT_EQ(detail::checked_add(kMax - 1, 1), std::optional<std::int64_t>(kMax));
    EXPECT_FALSE(detail::checked_add(kMax, 1).has_value());
    EXPECT_FALSE(detail::checked_add(1, kMax).has_value());
    EXPECT_FALSE(detail::checked_add(kMin, -1).has_value());
    EXPECT_EQ(detail::checked_add(kMin + 1, -1), std::optional<std::int64_t>(kMin));
    EXPECT_EQ(detail::checked_sub(12, 7), std::optional<std::int64_t>(5));
    EXPECT_FALSE(detail::checked_sub(kMin, 1).has_value());
    EXPECT_FALSE(detail::checked_sub(kMax, -1).has_value());
    EXPECT_FALSE(detail::checked_sub(0, kMin).has_value());
    EXPECT_EQ(detail::checked_sub(kMin + 1, 1), std::optional<std::int64_t>(kMin));
    EXPECT_EQ(detail::checked_sub(kMax - 1, -1), std::optional<std::int64_t>(kMax));
}

TEST(OperatorResolutionFence, WaitedSecondsFloorsAndSaturates) {
    EXPECT_EQ(waited_seconds(at(0), at(0)), 0);
    EXPECT_EQ(waited_seconds(at(0), at(999)), 0);
    EXPECT_EQ(waited_seconds(at(0), at(1000)), 1);
    EXPECT_EQ(waited_seconds(at(0), at(1999)), 1);
    EXPECT_EQ(waited_seconds(at(0), at(270'000)), 270);
    EXPECT_EQ(waited_seconds(at(0), at(65'535 * 1000LL)), 65535);
    EXPECT_EQ(waited_seconds(at(0), at(65'536 * 1000LL)), 65535) << "saturates";
    EXPECT_EQ(waited_seconds(at(0), at(kDay * 30)), 65535);
    EXPECT_EQ(waited_seconds(at(0), at(-1)), 0) << "the clock went backwards";
    EXPECT_EQ(waited_seconds(at(0), at(-5'000)), 0) << "...by more than a second: not a wrapped-around 65531";
    EXPECT_EQ(waited_seconds(at(0), at(-kDay)), 0);
}

// --- the frame's machine field ------------------------------------------------------------------------------------------------

TEST(OperatorResolutionDetailCode, RoundTripsAndIsAlwaysWhatTheReaderAccepts) {
    for (const std::uint16_t waited : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{270}, std::uint16_t{65535}}) {
        for (const std::uint16_t version : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0x1234}, std::uint16_t{65535}}) {
            const AbsentDetail d{waited, version};
            const std::int64_t code = detail::make_absent_detail_code(d);
            EXPECT_TRUE(is_valid_absent_detail_code(code)) << waited << "/" << version;
            ASSERT_TRUE(parse_absent_detail_code(code).has_value());
            EXPECT_EQ(*parse_absent_detail_code(code), d);
            EXPECT_GE(code, 0) << "the sign bit is reserved";
        }
    }
}

TEST(OperatorResolutionDetailCode, TheBuilderSetsExactlyTheSixClaimsAndTheKind) {
    const std::int64_t code = detail::make_absent_detail_code(AbsentDetail{0, 0});
    EXPECT_EQ(static_cast<std::uint64_t>(code), kKindAbsent | kAbsentRequiredBits);
    EXPECT_EQ(static_cast<std::uint64_t>(code), 0x3F1ULL) << "bits 0, 4..9";
    const std::int64_t with = detail::make_absent_detail_code(AbsentDetail{0xABCD, 0x1234});
    EXPECT_EQ(static_cast<std::uint64_t>(with), 0x1234ABCD03F1ULL);
}

TEST(OperatorResolutionDetailCode, ParseRefusesEveryCodeTheReaderRefuses) {
    const std::int64_t good = detail::make_absent_detail_code(AbsentDetail{7, 3});
    ASSERT_TRUE(parse_absent_detail_code(good).has_value());
    // each of the six claims cleared, each reserved bit set, the kind changed, the sign bit
    for (const std::uint64_t claim : {kFenceSatisfied, kPointQueryUnknownOrder, kAllOrdersAbsent, kOpenOrdersAbsent,
                                      kOperatorConfirmed, kLockHeld}) {
        EXPECT_FALSE(parse_absent_detail_code(static_cast<std::int64_t>(static_cast<std::uint64_t>(good) & ~claim)).has_value());
    }
    for (int bit = 0; bit < 64; ++bit) {
        const std::uint64_t mask = std::uint64_t{1} << bit;
        if ((mask & kReservedMask) == 0) continue;
        EXPECT_FALSE(parse_absent_detail_code(static_cast<std::int64_t>(static_cast<std::uint64_t>(good) | mask)).has_value()) << bit;
    }
    for (std::uint64_t kind = 0; kind < 16; ++kind) {
        const std::uint64_t code = (static_cast<std::uint64_t>(good) & ~kKindMask) | kind;
        EXPECT_EQ(parse_absent_detail_code(static_cast<std::int64_t>(code)).has_value(), kind == kKindAbsent) << kind;
    }
    EXPECT_FALSE(parse_absent_detail_code(-1).has_value());
    EXPECT_FALSE(parse_absent_detail_code(0).has_value());
    EXPECT_FALSE(parse_absent_detail_code(std::numeric_limits<std::int64_t>::min()).has_value());
}

TEST(OperatorResolutionDetailCode, ParseAgreesWithTheReaderPredicateOnRandomAndNearValidWords) {
    std::mt19937_64 rng(0x0D5'0002'5EEDULL);
    for (int i = 0; i < 200'000; ++i) {
        const auto word = static_cast<std::int64_t>(rng());
        ASSERT_EQ(parse_absent_detail_code(word).has_value(), is_valid_absent_detail_code(word)) << word;
    }
    // near-valid words: a valid code with one to three random bits flipped (the random words above are almost never valid)
    int valid_seen = 0;
    for (int i = 0; i < 200'000; ++i) {
        auto bits = static_cast<std::uint64_t>(detail::make_absent_detail_code(
            AbsentDetail{static_cast<std::uint16_t>(rng()), static_cast<std::uint16_t>(rng())}));
        const int flips = static_cast<int>(rng() % 4);
        for (int f = 0; f < flips; ++f) bits ^= std::uint64_t{1} << (rng() % 64);
        const auto word = static_cast<std::int64_t>(bits);
        const auto parsed = parse_absent_detail_code(word);
        ASSERT_EQ(parsed.has_value(), is_valid_absent_detail_code(word)) << bits;
        if (parsed) {
            ++valid_seen;
            ASSERT_EQ(detail::make_absent_detail_code(*parsed), word) << "a valid code is rebuilt bit for bit";
        }
    }
    EXPECT_GT(valid_seen, 1000) << "the property must see valid words too";
}

// --- the allOrders windows -----------------------------------------------------------------------------------------------------

TEST(OperatorResolutionWindows, TheLimitsAreTheDocumentedOnes) {
    EXPECT_EQ(kAllOrdersMaxSpanMs, 86'399'000) << "one second under the documented 24 hours";
    EXPECT_LT(kAllOrdersMaxSpanMs, kDay);
    EXPECT_EQ(kAllOrdersMaxPages, 14u);
    EXPECT_EQ(kAllOrdersPageLimit, 1000u) << "the exchange's maximum for allOrders";
}

TEST(OperatorResolutionWindows, ARangeWithinOneSpanIsOneWindow) {
    const auto plan = plan_all_orders_windows(at(0), at(kHour));
    ASSERT_EQ(plan.status, PlanStatus::Ok);
    ASSERT_EQ(plan.count, 1u);
    EXPECT_EQ(plan.windows[0].start, at(0));
    EXPECT_EQ(plan.windows[0].end, at(kHour));
}

TEST(OperatorResolutionWindows, AZeroLengthRangeIsOneWindow) {
    const auto plan = plan_all_orders_windows(at(0), at(0));
    ASSERT_EQ(plan.status, PlanStatus::Ok);
    ASSERT_EQ(plan.count, 1u);
    EXPECT_EQ(plan.windows[0].start, at(0));
    EXPECT_EQ(plan.windows[0].end, at(0));
}

TEST(OperatorResolutionWindows, TheSplitHappensExactlyOneMillisecondPastTheSpan) {
    auto plan = plan_all_orders_windows(at(0), at(kAllOrdersMaxSpanMs));
    ASSERT_EQ(plan.status, PlanStatus::Ok);
    EXPECT_EQ(plan.count, 1u);
    plan = plan_all_orders_windows(at(0), at(kAllOrdersMaxSpanMs + 1));
    ASSERT_EQ(plan.status, PlanStatus::Ok);
    ASSERT_EQ(plan.count, 2u);
    EXPECT_EQ(plan.windows[0].start, at(0));
    EXPECT_EQ(plan.windows[0].end, at(kAllOrdersMaxSpanMs));
    EXPECT_EQ(plan.windows[1].start, at(kAllOrdersMaxSpanMs)) << "the next window starts where the previous one ended";
    EXPECT_EQ(plan.windows[1].end, at(kAllOrdersMaxSpanMs + 1));
}

TEST(OperatorResolutionWindows, FourteenPagesAreAllowedAndFifteenAreRefused) {
    auto plan = plan_all_orders_windows(at(0), at(14 * kAllOrdersMaxSpanMs));
    ASSERT_EQ(plan.status, PlanStatus::Ok);
    ASSERT_EQ(plan.count, 14u);
    for (std::size_t i = 0; i < 14; ++i) {
        EXPECT_EQ(plan.windows[i].start, at(static_cast<std::int64_t>(i) * kAllOrdersMaxSpanMs)) << i;
        EXPECT_EQ(plan.windows[i].end, at(static_cast<std::int64_t>(i + 1) * kAllOrdersMaxSpanMs)) << i;
    }
    plan = plan_all_orders_windows(at(0), at(14 * kAllOrdersMaxSpanMs + 1));
    EXPECT_EQ(plan.status, PlanStatus::TooManyPages);
    EXPECT_EQ(plan.count, 0u) << "a refused plan carries no windows to be mistaken for coverage";
    plan = plan_all_orders_windows(at(0), at(100 * kDay));
    EXPECT_EQ(plan.status, PlanStatus::TooManyPages);
}

TEST(OperatorResolutionWindows, TheyRefuseAnEmptyOrImplausibleRange) {
    EXPECT_EQ(plan_all_orders_windows(at(1), at(0)).status, PlanStatus::EmptyRange);
    EXPECT_EQ(plan_all_orders_windows(at(0), at(0)).status, PlanStatus::Ok);
    for (const std::int64_t g : {std::int64_t{0}, std::int64_t{-5}, kMinPlausibleExchangeMs - 1, kMaxPlausibleExchangeMs + 1,
                                 std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min()}) {
        EXPECT_EQ(plan_all_orders_windows(ExchangeMs{g}, at(0)).status, PlanStatus::ImplausibleTime) << g;
        EXPECT_EQ(plan_all_orders_windows(at(0), ExchangeMs{g}).status, PlanStatus::ImplausibleTime) << g;
    }
    EXPECT_EQ(plan_all_orders_windows(ExchangeMs{}, ExchangeMs{}).status, PlanStatus::ImplausibleTime)
        << "an unset range is not a range";
    EXPECT_EQ(plan_all_orders_windows(ExchangeMs{kMaxPlausibleExchangeMs}, ExchangeMs{kMaxPlausibleExchangeMs}).status, PlanStatus::Ok);
}

TEST(OperatorResolutionWindows, TheyCoverTheWholeRangeWithoutAGap) {
    std::mt19937_64 rng(0x0D5'0003'5EEDULL);
    int multi = 0;
    for (int i = 0; i < 20'000; ++i) {
        const std::int64_t start = kMinPlausibleExchangeMs + static_cast<std::int64_t>(rng() % 2'000'000'000'000ULL);
        const std::int64_t len = static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(15 * kAllOrdersMaxSpanMs));
        const auto plan = plan_all_orders_windows(ExchangeMs{start}, ExchangeMs{start + len});
        if (plan.status == PlanStatus::TooManyPages) {
            ASSERT_GT(len, 14 * kAllOrdersMaxSpanMs) << start << "+" << len;
            continue;
        }
        ASSERT_EQ(plan.status, PlanStatus::Ok) << start << "+" << len;
        ASSERT_GE(plan.count, 1u);
        ASSERT_LE(plan.count, kAllOrdersMaxPages);
        if (plan.count > 1) ++multi;
        ASSERT_EQ(plan.windows[0].start.count(), start);
        ASSERT_EQ(plan.windows[plan.count - 1].end.count(), start + len);
        for (std::size_t w = 0; w < plan.count; ++w) {
            ASSERT_LE(plan.windows[w].end.count() - plan.windows[w].start.count(), kAllOrdersMaxSpanMs);
            ASSERT_LE(plan.windows[w].start, plan.windows[w].end);
            if (w + 1 < plan.count) {
                ASSERT_EQ(plan.windows[w + 1].start, plan.windows[w].end);
            }
        }
        // the page count is the least that covers the range
        const std::int64_t need = len == 0 ? 1 : (len + kAllOrdersMaxSpanMs - 1) / kAllOrdersMaxSpanMs;
        ASSERT_EQ(static_cast<std::int64_t>(plan.count), need) << start << "+" << len;
    }
    EXPECT_GT(multi, 1000) << "the property must see multi-window plans";
}

// --- the client order id's timestamp -------------------------------------------------------------------------------------------

TEST(OperatorResolutionCoid, ParsesTheIdsTheGeneratorMakes) {
    std::mt19937_64 rng(0x0D5'0004'5EEDULL);
    for (int i = 0; i < 20'000; ++i) {
        const std::int64_t ts = kMinPlausibleExchangeMs + static_cast<std::int64_t>(rng() % 2'000'000'000'000ULL);
        const auto seq = static_cast<std::uint32_t>(rng());
        const auto symbol = static_cast<std::uint32_t>(rng());
        const ClientOrderId coid = make_client_order_id(ts, seq, symbol);
        const auto parsed = client_order_id_timestamp(coid.view());
        ASSERT_TRUE(parsed.has_value()) << coid.view();
        ASSERT_EQ(parsed->count(), ts) << coid.view();
    }
    // the corners of the grammar
    EXPECT_EQ(client_order_id_timestamp("HY-1790000000000-0-0000")->count(), kNow);
    EXPECT_EQ(client_order_id_timestamp("HY-1790000000000-4294967295-FFFF")->count(), kNow);
    EXPECT_EQ(client_order_id_timestamp(make_client_order_id(kNow, 1, 0x42).view())->count(), kNow);
}

TEST(OperatorResolutionCoid, RefusesEverythingThatIsNotOurGrammar) {
    const char* const bad[] = {
        "",
        "HY-",
        "HY",
        "hy-1790000000000-1-0042",           // prefix is case-sensitive
        "XY-1790000000000-1-0042",
        "HZ-1790000000000-1-0042",
        "HY1790000000000-1-0042",            // separators are required
        "HY-1790000000000x1-0042",
        "HY-1790000000000-1x0042",
        "HY-1790000000000-1-0042x",
        "HY-1790000000000--0042",             // an empty sequence
        "HY--1-0042",                         // an empty timestamp
        "HY---",
        "HY-1790000000000-1-004",            // three hex digits
        "HY-1790000000000-1-00420",          // five hex digits
        "HY-1790000000000-1-004a",           // lowercase hex
        "HY-1790000000000-1-004G",           // not hex
        "HY-1790000000000-1-",
        "HY-1790000000000-1",
        "HY-1790000000000-",
        "HY-1790000000000",
        "HY--1790000000000-1-0042",          // sign
        "HY-+1790000000000-1-0042",
        "HY-1790000000000--1-0042",
        "HY-01790000000000-1-0042",          // leading zero
        "HY-1790000000000-01-0042",
        "HY-1790000000000-1-0042-",          // trailing text
        "HY-1790000000000-1-0042 ",
        " HY-1790000000000-1-0042",
        "HY-1790000000000-1-0042\n",
        "HY-1790000000000-4294967296-0042",  // sequence does not fit a uint32
        "HY-1790000000000-99999999999-0042",
        "HY-9223372036854775808-1-0042",     // timestamp does not fit an int64
        "HY-99999999999999999999-1-0042",
        "HY-1790000000000x-1-0042",
        "HY-17900000000.0-1-0042",
        "HY-1790000000000-1.5-0042",
        "HY-1790000000000-1-0042-0042",
        "HY-1000-1-0042",                    // a timestamp that cannot be a wall clock (1970)
        "HY-0-1-0042",
        "HY-1577836799999-1-0042",           // one millisecond before the plausible range
        "HY-4102444800001-1-0042",           // one millisecond after
    };
    for (const char* const s : bad) {
        EXPECT_FALSE(client_order_id_timestamp(s).has_value()) << "[" << s << "]";
    }
    EXPECT_TRUE(client_order_id_timestamp("HY-1577836800000-1-0042").has_value());
    EXPECT_TRUE(client_order_id_timestamp("HY-4102444800000-1-0042").has_value());

    // an embedded NUL where the fourth hex digit belongs: the 22 visible characters plus the explicit \0 (23 bytes)
    const std::string with_nul("HY-1790000000000-1-004\0", 23);
    ASSERT_EQ(with_nul.size(), 23u);
    EXPECT_FALSE(client_order_id_timestamp(with_nul).has_value());
    // longer than the field a client order id lives in
    const std::string too_long = "HY-" + std::string(30, '1') + "-1-0042";
    ASSERT_GT(too_long.size(), kClientOrderIdLen);
    EXPECT_FALSE(client_order_id_timestamp(too_long).has_value());
    // exactly the length of the longest id the generator can make (32) is parsed, and one more digit makes it implausible
    EXPECT_TRUE(client_order_id_timestamp("HY-1790000000000-4294967295-FFFF").has_value());
    EXPECT_FALSE(client_order_id_timestamp("HY-17900000000000-4294967295-FFFF").has_value());
}

// --- the interval allOrders must cover ----------------------------------------------------------------------------------------

constexpr std::int64_t kTwoW = 120'000;

TEST(OperatorResolutionRange, StartsTwoRecvWindowsBeforeTheEarlierOfTheTwoAnchors) {
    const std::string coid_later(make_client_order_id(kNow - 10'000, 1, 0x42).view());
    const std::string coid_earlier(make_client_order_id(kNow - 50'000, 1, 0x42).view());
    // the frame anchor is the earlier one
    auto r = all_orders_range(at(-50'000), coid_later, at(kHour), FenceParams{});
    ASSERT_EQ(r.status, RangeStatus::Ok);
    EXPECT_EQ(r.start, at(-50'000 - kTwoW));
    EXPECT_EQ(r.end, at(kHour));
    // the id is the earlier one: the minimum, not the preferred frame time, is used
    r = all_orders_range(at(-10'000), coid_earlier, at(kHour), FenceParams{});
    ASSERT_EQ(r.status, RangeStatus::Ok);
    EXPECT_EQ(r.start, at(-50'000 - kTwoW));
    // both equal
    r = all_orders_range(at(-50'000), coid_earlier, at(kHour), FenceParams{});
    ASSERT_EQ(r.status, RangeStatus::Ok);
    EXPECT_EQ(r.start, at(-50'000 - kTwoW));
}

TEST(OperatorResolutionRange, EitherAnchorAloneIsEnough) {
    const std::string coid(make_client_order_id(kNow - 50'000, 1, 0x42).view());
    auto r = all_orders_range(std::nullopt, coid, at(kHour), FenceParams{});
    ASSERT_EQ(r.status, RangeStatus::Ok);
    EXPECT_EQ(r.start, at(-50'000 - kTwoW));
    r = all_orders_range(at(-30'000), "not one of our ids", at(kHour), FenceParams{});
    ASSERT_EQ(r.status, RangeStatus::Ok);
    EXPECT_EQ(r.start, at(-30'000 - kTwoW));
}

TEST(OperatorResolutionRange, WithoutAnyAnchorThereIsNoRange) {
    EXPECT_EQ(all_orders_range(std::nullopt, "", at(kHour), FenceParams{}).status, RangeStatus::NoUsableAnchor);
    EXPECT_EQ(all_orders_range(std::nullopt, "garbage", at(kHour), FenceParams{}).status, RangeStatus::NoUsableAnchor);
    EXPECT_EQ(all_orders_range(std::nullopt, "HY-1000-1-0042", at(kHour), FenceParams{}).status, RangeStatus::NoUsableAnchor)
        << "an id whose timestamp is not a plausible wall clock is no anchor";
    EXPECT_EQ(all_orders_range(ExchangeMs{}, "", at(kHour), FenceParams{}).status, RangeStatus::ImplausibleTime)
        << "a frame anchor that is present but garbage is refused, not ignored";
}

TEST(OperatorResolutionRange, AnAnchorAfterTheReadContradictsTheInputs) {
    EXPECT_EQ(all_orders_range(at(kHour + 1), "", at(kHour), FenceParams{}).status, RangeStatus::AnchorAfterRead);
    EXPECT_EQ(all_orders_range(at(kHour), "", at(kHour), FenceParams{}).status, RangeStatus::Ok) << "equal is fine";
    // the id is earlier than the frame anchor here, so the (late) frame anchor does not matter
    const std::string coid(make_client_order_id(kNow - 50'000, 1, 0x42).view());
    EXPECT_EQ(all_orders_range(at(kHour + 1), coid, at(kHour), FenceParams{}).status, RangeStatus::Ok);
    // the id alone, after the read
    const std::string future(make_client_order_id(kNow + 2 * kHour, 1, 0x42).view());
    EXPECT_EQ(all_orders_range(std::nullopt, future, at(kHour), FenceParams{}).status, RangeStatus::AnchorAfterRead);
}

TEST(OperatorResolutionRange, TheStartMovesWithTheRecvWindow) {
    FenceParams p{};
    p.recv_window_ms = 90'000;  // raised
    const auto r = all_orders_range(at(-50'000), "", at(kHour), p);
    ASSERT_EQ(r.status, RangeStatus::Ok);
    EXPECT_EQ(r.start, at(-50'000 - 180'000)) << "2W with the raised W";
    EXPECT_EQ(r.end, at(kHour));
}

TEST(OperatorResolutionRange, RefusesInvalidParametersAndImplausibleTimes) {
    FenceParams bad{};
    bad.recv_window_ms = 1;
    EXPECT_EQ(all_orders_range(at(-50'000), "", at(kHour), bad).status, RangeStatus::InvalidParams);
    // a garbage frame anchor is named as garbage, whichever side of the plausible range it falls on (not "after the read")
    for (const std::int64_t g : {std::int64_t{0}, std::int64_t{-1}, kMinPlausibleExchangeMs - 1, kMaxPlausibleExchangeMs + 1,
                                 std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min()}) {
        EXPECT_EQ(all_orders_range(ExchangeMs{g}, "", at(kHour), FenceParams{}).status, RangeStatus::ImplausibleTime) << g;
    }
    EXPECT_EQ(all_orders_range(at(-50'000), "", ExchangeMs{}, FenceParams{}).status, RangeStatus::ImplausibleTime);
    EXPECT_EQ(all_orders_range(at(-50'000), "", ExchangeMs{kMaxPlausibleExchangeMs + 1}, FenceParams{}).status,
              RangeStatus::ImplausibleTime);
    // an anchor so close to the floor that 2W before it leaves the plausible range
    EXPECT_EQ(all_orders_range(ExchangeMs{kMinPlausibleExchangeMs + 1000}, "", at(kHour), FenceParams{}).status,
              RangeStatus::ImplausibleTime);
    EXPECT_EQ(all_orders_range(ExchangeMs{kMinPlausibleExchangeMs + kTwoW}, "", at(kHour), FenceParams{}).status,
              RangeStatus::Ok) << "exactly at the floor is allowed";
}

// --- the conclusion -----------------------------------------------------------------------------------------------------------

struct Fixture {
    std::string coid;
    std::vector<PageVerdict> pages;
    AbsentConclusionInput in{};

    // 5 hours of lock-to-read idle time would be silly; the lock is at kNow, the read 300 s later, the order's anchor 50 h
    // before the read, so the plan has three windows.
    Fixture() {
        coid = std::string(make_client_order_id(kNow + 300'000 - 50 * kHour - 5'000, 1, 0x42).view());
        in.client_order_id = coid;
        in.frame_anchor = at(300'000 - 50 * kHour);
        in.lock_ref = at(0);
        in.read_ref = at(300'000);
        in.point_query = PointQueryVerdict::NoSuchOrder;
        resize_pages_to_plan();
        in.open_orders = PageVerdict::Clear;
        in.lock_held = true;
        in.operator_confirmed = true;
        in.tool_version = 0x0102;
    }

    std::size_t planned() const {
        const auto range = all_orders_range(in.frame_anchor, in.client_order_id, in.read_ref, in.fence);
        return plan_all_orders_windows(range.start, range.end).count;
    }
    void resize_pages_to_plan() {
        pages.assign(planned(), PageVerdict::Clear);
        in.all_orders_pages = std::span<const PageVerdict>(pages);
    }
    AbsentConclusion conclude() {
        in.all_orders_pages = std::span<const PageVerdict>(pages);
        return conclude_absent(in);
    }
};

TEST(OperatorResolutionConclusion, EverythingInAndCleanConcludesAbsent) {
    Fixture f;
    ASSERT_EQ(f.pages.size(), 3u) << "the fixture is meant to need three windows";
    const AbsentConclusion c = f.conclude();
    EXPECT_EQ(c.status, AbsentConclusionStatus::Absent);
    EXPECT_EQ(c.fence, FenceVerdict::Satisfied);
    EXPECT_TRUE(is_valid_absent_detail_code(c.detail_code)) << "what the reader of R1 accepts";
    const auto parsed = parse_absent_detail_code(c.detail_code);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->waited_seconds, 300);
    EXPECT_EQ(parsed->tool_version, 0x0102);
    EXPECT_EQ(static_cast<std::uint64_t>(c.detail_code), 0x0102'012C'03F1ULL);
}

TEST(OperatorResolutionConclusion, ANothingInputNeverConcludes) {
    const AbsentConclusion c = conclude_absent(AbsentConclusionInput{});
    EXPECT_NE(c.status, AbsentConclusionStatus::Absent);
    EXPECT_EQ(c.status, AbsentConclusionStatus::LockNotHeld);
    EXPECT_EQ(c.detail_code, 0);
    EXPECT_EQ(AbsentConclusion{}.status, AbsentConclusionStatus::NotEvaluated);
    EXPECT_EQ(AbsentConclusion{}.fence, FenceVerdict::Unset);
    // the zero value of every evidence enum is "no evidence"
    EXPECT_EQ(PointQueryVerdict{}, PointQueryVerdict::Unusable);
    EXPECT_EQ(PageVerdict{}, PageVerdict::Malformed);
    EXPECT_EQ(AbsentConclusionInput{}.point_query, PointQueryVerdict::Unusable);
    EXPECT_EQ(AbsentConclusionInput{}.open_orders, PageVerdict::Malformed);
}

struct Fault {
    const char* name;
    std::function<void(Fixture&)> apply;
    AbsentConclusionStatus expected;
};

TEST(OperatorResolutionConclusion, EachMissingPieceOfEvidenceIsItsOwnRefusal) {
    const std::vector<Fault> faults = {
        {"lock not held", [](Fixture& f) { f.in.lock_held = false; }, AbsentConclusionStatus::LockNotHeld},
        {"read one millisecond early", [](Fixture& f) { f.in.read_ref = at(269'999); },
         AbsentConclusionStatus::FenceNotSatisfied},
        {"read before the lock", [](Fixture& f) { f.in.read_ref = at(-1); }, AbsentConclusionStatus::FenceNotSatisfied},
        {"unset lock reference", [](Fixture& f) { f.in.lock_ref = ExchangeMs{}; }, AbsentConclusionStatus::FenceNotSatisfied},
        {"unset read reference", [](Fixture& f) { f.in.read_ref = ExchangeMs{}; }, AbsentConclusionStatus::FenceNotSatisfied},
        {"read lag below its floor", [](Fixture& f) { f.in.fence.read_lag_ms = kReadLagFloorMs - 1; },
         AbsentConclusionStatus::InvalidFenceParams},
        {"recv window below its floor", [](Fixture& f) { f.in.fence.recv_window_ms = 0; },
         AbsentConclusionStatus::InvalidFenceParams},
        {"point query unusable", [](Fixture& f) { f.in.point_query = PointQueryVerdict::Unusable; },
         AbsentConclusionStatus::PointQueryNotAbsent},
        {"order exists", [](Fixture& f) { f.in.point_query = PointQueryVerdict::OrderExists; },
         AbsentConclusionStatus::PointQueryNotAbsent},
        {"order archived", [](Fixture& f) { f.in.point_query = PointQueryVerdict::Archived; },
         AbsentConclusionStatus::PointQueryNotAbsent},
        {"other error", [](Fixture& f) { f.in.point_query = PointQueryVerdict::OtherError; },
         AbsentConclusionStatus::PointQueryNotAbsent},
        {"no anchor at all", [](Fixture& f) { f.in.frame_anchor.reset(); f.in.client_order_id = "garbage"; },
         AbsentConclusionStatus::NoUsableRange},
        {"anchor after the read", [](Fixture& f) { f.in.frame_anchor = at(300'001); f.in.client_order_id = "garbage"; },
         AbsentConclusionStatus::NoUsableRange},
        {"more than fourteen windows", [](Fixture& f) { f.in.frame_anchor = at(300'000 - 15 * kDay); f.in.client_order_id = "garbage"; },
         AbsentConclusionStatus::RangeNotPlannable},
        {"no pages at all", [](Fixture& f) { f.pages.clear(); }, AbsentConclusionStatus::AllOrdersIncomplete},
        {"one page too few", [](Fixture& f) { f.pages.pop_back(); }, AbsentConclusionStatus::AllOrdersIncomplete},
        {"one page too many", [](Fixture& f) { f.pages.push_back(PageVerdict::Clear); }, AbsentConclusionStatus::AllOrdersIncomplete},
        {"open orders listed", [](Fixture& f) { f.in.open_orders = PageVerdict::Listed; }, AbsentConclusionStatus::OpenOrdersNotClear},
        {"open orders malformed", [](Fixture& f) { f.in.open_orders = PageVerdict::Malformed; },
         AbsentConclusionStatus::OpenOrdersNotClear},
        {"open orders truncated", [](Fixture& f) { f.in.open_orders = PageVerdict::PossiblyTruncated; },
         AbsentConclusionStatus::OpenOrdersNotClear},
        {"operator did not confirm", [](Fixture& f) { f.in.operator_confirmed = false; }, AbsentConclusionStatus::NotConfirmed},
    };
    for (const Fault& fault : faults) {
        SCOPED_TRACE(fault.name);
        Fixture f;
        fault.apply(f);
        const AbsentConclusion c = f.conclude();
        EXPECT_EQ(c.status, fault.expected);
        EXPECT_EQ(c.detail_code, 0) << "a refusal carries no machine field";
    }
}

TEST(OperatorResolutionConclusion, EveryAllOrdersPageMustBeClear) {
    for (std::size_t bad_page = 0; bad_page < 3; ++bad_page) {
        for (const PageVerdict verdict : {PageVerdict::Malformed, PageVerdict::Listed, PageVerdict::PossiblyTruncated}) {
            SCOPED_TRACE(std::to_string(bad_page) + "/" + std::to_string(static_cast<int>(verdict)));
            Fixture f;
            f.pages[bad_page] = verdict;
            const AbsentConclusion c = f.conclude();
            EXPECT_EQ(c.status, AbsentConclusionStatus::AllOrdersNotClear);
            EXPECT_EQ(c.detail_code, 0);
        }
    }
}

TEST(OperatorResolutionConclusion, TheZeroValueOfEveryVerdictIsNeverAPass) {
    // A verdict, status or plan nobody filled in is the 0 value, and 0 must never be the value that lets the conclusion through.
    EXPECT_NE(FenceVerdict{}, FenceVerdict::Satisfied);
    EXPECT_NE(PointQueryVerdict{}, PointQueryVerdict::NoSuchOrder);
    EXPECT_NE(PageVerdict{}, PageVerdict::Clear);
    EXPECT_NE(PlanStatus{}, PlanStatus::Ok);
    EXPECT_NE(RangeStatus{}, RangeStatus::Ok);
    EXPECT_NE(AbsentConclusionStatus{}, AbsentConclusionStatus::Absent);
    EXPECT_EQ(AllOrdersPlan{}.status, PlanStatus::Unset);
    EXPECT_EQ(AllOrdersPlan{}.count, 0u);
    EXPECT_EQ(AllOrdersRange{}.status, RangeStatus::Unset);
    EXPECT_NE(AbsentConclusion{}.status, AbsentConclusionStatus::Absent);
    // ...and no two members of the evidence enums share the pass value
    EXPECT_NE(static_cast<int>(PageVerdict::Clear), static_cast<int>(PageVerdict::Malformed));
    EXPECT_NE(static_cast<int>(PointQueryVerdict::NoSuchOrder), static_cast<int>(PointQueryVerdict::Unusable));
}

TEST(OperatorResolutionConclusion, TheIntervalEndsAtTheEvidenceReadNotAtTheLock) {
    // The anchor is placed so that [anchor - 2W, read] needs three windows while [anchor - 2W, lock] (300 s shorter) needs two.
    Fixture f;
    const std::int64_t length_to_read = 2 * kAllOrdersMaxSpanMs + 10'000;
    f.in.client_order_id = "garbage";
    f.in.frame_anchor = ExchangeMs{f.in.read_ref.count() - (length_to_read - kTwoW)};
    ASSERT_EQ(f.planned(), 3u);
    f.pages.assign(2, PageVerdict::Clear);
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::AllOrdersIncomplete) << "two pages cover only the lock";
    f.pages.assign(3, PageVerdict::Clear);
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::Absent);
    // and the start is two recv windows before the anchor: one second less than that would need only two windows
    f.in.frame_anchor = ExchangeMs{f.in.read_ref.count() - (length_to_read - kTwoW) + 11'000};
    EXPECT_EQ(f.planned(), 2u);
}

TEST(OperatorResolutionConclusion, TheClientOrderIdIsAnAnchorOfTheInterval) {
    // The frame anchor alone needs two windows; the id is 100 s earlier and needs three. A conclusion that ignored the id would
    // expect two pages and call three "incomplete".
    Fixture f;
    f.in.frame_anchor = ExchangeMs{f.in.read_ref.count() - (2 * kAllOrdersMaxSpanMs - 200'000)};
    f.in.client_order_id = "garbage";
    ASSERT_EQ(f.planned(), 2u);
    f.coid = std::string(make_client_order_id(f.in.frame_anchor->count() - 100'000, 1, 0x42).view());
    f.in.client_order_id = f.coid;
    ASSERT_EQ(f.planned(), 3u);
    f.pages.assign(2, PageVerdict::Clear);
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::AllOrdersIncomplete);
    f.pages.assign(3, PageVerdict::Clear);
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::Absent);
}

TEST(OperatorResolutionConclusion, TheWindowCountIsRecomputedNotTrusted) {
    // A tool that planned a shorter interval than the order needs would hand in fewer pages; the count the conclusion expects is
    // the one of the real interval, so that is "incomplete", never "absent".
    Fixture f;
    f.in.frame_anchor = at(300'000 - 3 * kHour);
    f.in.client_order_id = "garbage";
    EXPECT_EQ(f.planned(), 1u);
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::AllOrdersIncomplete) << "three pages handed in for a one-window plan";
    f.resize_pages_to_plan();
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::Absent);
}

TEST(OperatorResolutionConclusion, TheFirstFailureInProtocolOrderIsTheOneReported) {
    Fixture f;
    f.in.lock_held = false;
    f.in.read_ref = at(0);
    f.in.point_query = PointQueryVerdict::OrderExists;
    f.in.open_orders = PageVerdict::Listed;
    f.in.operator_confirmed = false;
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::LockNotHeld);
    f.in.lock_held = true;
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::FenceNotSatisfied);
    f.in.read_ref = at(300'000);
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::PointQueryNotAbsent);
    f.in.point_query = PointQueryVerdict::NoSuchOrder;
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::OpenOrdersNotClear);
    f.in.open_orders = PageVerdict::Clear;
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::NotConfirmed);
    f.in.operator_confirmed = true;
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::Absent);
}

TEST(OperatorResolutionConclusion, TheWaitedSecondsAreTheRealGapBetweenTheTwoReadings) {
    Fixture f;
    f.in.read_ref = at(270'000 + 1'500);
    const AbsentConclusion c = f.conclude();
    ASSERT_EQ(c.status, AbsentConclusionStatus::Absent);
    EXPECT_EQ(parse_absent_detail_code(c.detail_code)->waited_seconds, 271) << "floor of 271.5 s";
}

TEST(OperatorResolutionConclusion, ARaisedParameterRaisesTheWait) {
    Fixture f;
    f.in.fence.read_lag_ms = 180'000;  // 60 + 60 + 30 + 180 = 330 s from the lock
    f.in.read_ref = at(300'000);
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::FenceNotSatisfied) << "300 s is not enough any more";
    EXPECT_EQ(f.conclude().fence, FenceVerdict::TooEarly);
    f.in.read_ref = at(329'999);
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::FenceNotSatisfied);
    f.in.read_ref = at(330'000);
    EXPECT_EQ(f.conclude().status, AbsentConclusionStatus::Absent);
}

}  // namespace
