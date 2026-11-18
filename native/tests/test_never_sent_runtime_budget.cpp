// SAFE-07a: runtime Intent-orphan accounting, thresholds and typed in-flight registration failures.
#include <gtest/gtest.h>

#include <hengyuan/never_sent_runtime_budget.hpp>
#include <hengyuan/order_lifecycle.hpp>

#include <array>
#include <cstddef>
#include <cstdio>

namespace hy {
namespace {

using never_sent::IntentOrphanBudget;
using never_sent::IntentOrphanReason;
using never_sent::Reason;

TEST(IntentOrphanBudget, ZeroBudgetFailsClosedWithEveryAlert) {
    IntentOrphanBudget budget{0, 0};
    EXPECT_TRUE(budget.stopped());
    EXPECT_FALSE(budget.allows_new_intent());
    EXPECT_EQ(budget.alert_mask(), never_sent::kIntentOrphanAlertAll);
    EXPECT_EQ(budget.total(), 0u);
    EXPECT_EQ(budget.count(IntentOrphanReason::RecoveredUnknown), 0u);
}

TEST(IntentOrphanBudget, RecoveredBaselineUsesOnlyTheUnknownReasonAndNeverRepeatsAlerts) {
    IntentOrphanBudget budget{3, 4};
    EXPECT_EQ(budget.recovered(), 3u);
    EXPECT_EQ(budget.live(), 0u);
    EXPECT_EQ(budget.total(), 3u);
    EXPECT_EQ(budget.count(IntentOrphanReason::RecoveredUnknown), 3u);
    EXPECT_EQ(budget.alert_mask(), never_sent::kIntentOrphanAlert25 | never_sent::kIntentOrphanAlert50 |
                                      never_sent::kIntentOrphanAlert75);
    EXPECT_TRUE(budget.allows_new_intent());

    budget.observe_class_a(Reason::ConfirmationMissing, ClosureAppendStatus::Appended);
    EXPECT_EQ(budget.total(), 3u);
    EXPECT_EQ(budget.live(), 0u);

    budget.observe_class_a(Reason::ConfirmationMissing, ClosureAppendStatus::NotAttempted);
    EXPECT_EQ(budget.total(), 4u);
    EXPECT_EQ(budget.live(), 1u);
    EXPECT_EQ(budget.count(IntentOrphanReason::ConfirmationMissing), 1u);
    EXPECT_EQ(budget.alert_mask(), never_sent::kIntentOrphanAlertAll);
    EXPECT_TRUE(budget.stopped());

    budget.observe_class_a(Reason::ConfirmationMissing, ClosureAppendStatus::NotEnabled);
    EXPECT_EQ(budget.total(), 5u);
    EXPECT_EQ(budget.live(), 2u);
    EXPECT_EQ(budget.count(IntentOrphanReason::ConfirmationMissing), 2u);
    EXPECT_EQ(budget.count(IntentOrphanReason::RecoveredUnknown), 3u);
    EXPECT_EQ(budget.alert_mask(), never_sent::kIntentOrphanAlertAll);
    EXPECT_TRUE(budget.stopped());
}

TEST(IntentOrphanBudget, ThresholdsAreMonotoneAndNinetyPercentStopsTheNextIntent) {
    IntentOrphanBudget budget{0, 100};
    for (std::size_t i = 1; i <= 90; ++i) {
        budget.observe_class_a(Reason::RateLimitExhausted, ClosureAppendStatus::NotEnabled);
        EXPECT_EQ(budget.total(), i);
        if (i < 25) {
            EXPECT_EQ(budget.alert_mask(), 0u);
        } else if (i < 50) {
            EXPECT_EQ(budget.alert_mask(), never_sent::kIntentOrphanAlert25);
        } else if (i < 75) {
            EXPECT_EQ(budget.alert_mask(), never_sent::kIntentOrphanAlert25 | never_sent::kIntentOrphanAlert50);
        } else if (i < 90) {
            EXPECT_EQ(budget.alert_mask(), never_sent::kIntentOrphanAlert25 | never_sent::kIntentOrphanAlert50 |
                                              never_sent::kIntentOrphanAlert75);
        } else {
            EXPECT_EQ(budget.alert_mask(), never_sent::kIntentOrphanAlertAll);
        }
        EXPECT_EQ(budget.allows_new_intent(), i < 90);
    }
    const auto mask = budget.alert_mask();
    budget.observe_class_a(Reason::RateLimitExhausted, ClosureAppendStatus::NotEnabled);
    EXPECT_EQ(budget.alert_mask(), mask) << "repeated observations never raise a new level or clear an old one";
}

TEST(IntentOrphanBudget, SmallBudgetsRoundThresholdsUpAndStopAtNinetyPercent) {
    IntentOrphanBudget one{0, 1};
    EXPECT_TRUE(one.allows_new_intent());
    one.observe_class_a(Reason::SubmitPortInvalid, ClosureAppendStatus::NotWired);
    EXPECT_EQ(one.alert_mask(), never_sent::kIntentOrphanAlertAll);
    EXPECT_TRUE(one.stopped());

    IntentOrphanBudget two{1, 2};
    EXPECT_EQ(two.alert_mask(), never_sent::kIntentOrphanAlert25 | never_sent::kIntentOrphanAlert50);
    EXPECT_TRUE(two.allows_new_intent());
    two.observe_class_a(Reason::DuplicateInFlight, ClosureAppendStatus::Refused);
    EXPECT_EQ(two.alert_mask(), never_sent::kIntentOrphanAlertAll);
    EXPECT_TRUE(two.stopped());

    IntentOrphanBudget four{0, 4};
    constexpr std::array<std::uint8_t, 4> masks{
        never_sent::kIntentOrphanAlert25,
        static_cast<std::uint8_t>(never_sent::kIntentOrphanAlert25 | never_sent::kIntentOrphanAlert50),
        static_cast<std::uint8_t>(never_sent::kIntentOrphanAlert25 | never_sent::kIntentOrphanAlert50 |
                                  never_sent::kIntentOrphanAlert75),
        never_sent::kIntentOrphanAlertAll,
    };
    for (std::size_t i = 0; i < masks.size(); ++i) {
        four.observe_class_a(Reason::ConfirmationMismatch, ClosureAppendStatus::Failed);
        EXPECT_EQ(four.alert_mask(), masks[i]);
        EXPECT_EQ(four.allows_new_intent(), i + 1 < masks.size());
    }
}

TEST(IntentOrphanBudget, EveryClassARefusalHasAnAccurateReasonBucketForEveryUnclosedStatus) {
    constexpr std::array<Reason, 7> reasons{
        Reason::ConfirmationMissing, Reason::ConfirmationMismatch, Reason::SubmitPortInvalid,
        Reason::InFlightRegistryUnavailable, Reason::RateLimitExhausted, Reason::DuplicateInFlight,
        Reason::InFlightCapacityExceeded,
    };
    constexpr std::array<IntentOrphanReason, 7> buckets{
        IntentOrphanReason::ConfirmationMissing, IntentOrphanReason::ConfirmationMismatch,
        IntentOrphanReason::SubmitPortInvalid, IntentOrphanReason::InFlightRegistryUnavailable,
        IntentOrphanReason::RateLimitExhausted, IntentOrphanReason::DuplicateInFlight,
        IntentOrphanReason::InFlightCapacityExceeded,
    };
    constexpr std::array<ClosureAppendStatus, 5> statuses{
        ClosureAppendStatus::NotAttempted, ClosureAppendStatus::NotEnabled, ClosureAppendStatus::NotWired,
        ClosureAppendStatus::Refused, ClosureAppendStatus::Failed,
    };
    for (std::size_t i = 0; i < reasons.size(); ++i) {
        for (const ClosureAppendStatus status : statuses) {
            IntentOrphanBudget budget{0, 100};
            budget.observe_class_a(reasons[i], status);
            EXPECT_EQ(budget.total(), 1u);
            EXPECT_EQ(budget.live(), 1u);
            EXPECT_EQ(budget.count(buckets[i]), 1u) << static_cast<int>(reasons[i]) << " / " << static_cast<int>(status);
            for (std::size_t other = 0; other < never_sent::kIntentOrphanReasonCount; ++other) {
                if (other != static_cast<std::size_t>(buckets[i])) {
                    EXPECT_EQ(budget.count(static_cast<IntentOrphanReason>(other)), 0u);
                }
            }
        }
    }
}

TEST(IntentOrphanBudget, AnUnexpectedReasonIsCountedConservativelyNotDropped) {
    IntentOrphanBudget budget{0, 100};
    budget.observe_class_a(Reason::ReadinessRevoked, ClosureAppendStatus::Failed);
    EXPECT_EQ(budget.total(), 1u);
    EXPECT_EQ(budget.count(IntentOrphanReason::Unknown), 1u);
}

TEST(InFlightRegistration, DuplicateAndCapacityAreDistinctEvenWhenTheTableIsFull) {
    InFlightRegistry registry;
    const auto empty = registry.register_submit_detailed("");
    EXPECT_EQ(empty.status, InFlightRegistrationStatus::EmptyClientOrderId);
    EXPECT_FALSE(empty.registered());
    EXPECT_FALSE(empty.handle.valid());

    const auto first = registry.register_submit_detailed("id00");
    ASSERT_TRUE(first.registered());
    ASSERT_TRUE(first.handle.valid());
    EXPECT_EQ(first.status, InFlightRegistrationStatus::Success);
    EXPECT_EQ(registry.count(), 1u);

    const auto duplicate = registry.register_submit_detailed("id00");
    EXPECT_EQ(duplicate.status, InFlightRegistrationStatus::Duplicate);
    EXPECT_FALSE(duplicate.registered());
    EXPECT_EQ(registry.count(), 1u);

    for (std::size_t i = 1; i < kMaxInFlight; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "id%02u", static_cast<unsigned>(i));
        ASSERT_TRUE(registry.register_submit_detailed(id).registered()) << i;
    }
    ASSERT_EQ(registry.count(), kMaxInFlight);

    const auto full_duplicate = registry.register_submit_detailed("id00");
    EXPECT_EQ(full_duplicate.status, InFlightRegistrationStatus::Duplicate) << "a full table must not relabel a duplicate as capacity";
    const auto overflow = registry.register_submit_detailed("different-id");
    EXPECT_EQ(overflow.status, InFlightRegistrationStatus::CapacityExhausted);
    EXPECT_FALSE(overflow.handle.valid());
    EXPECT_EQ(registry.count(), kMaxInFlight);
    EXPECT_FALSE(registry.register_submit("legacy-overflow"));
    EXPECT_FALSE(registry.register_submit_handle("legacy-overflow").valid()) << "the legacy API retains its invalid-handle contract";
}

TEST(InFlightRegistration, TypedAndLegacySuccessPathsShareTheSlotGeneration) {
    InFlightRegistry registry;
    const auto first = registry.register_submit_detailed("same-id");
    ASSERT_TRUE(first.registered());
    const auto generation = first.handle.generation;
    ASSERT_TRUE(registry.mark_resolved_handle(first.handle, "same-id"));
    const auto replacement = registry.register_submit_detailed("replacement");
    ASSERT_TRUE(replacement.registered());
    EXPECT_EQ(replacement.handle.slot_index, first.handle.slot_index);
    EXPECT_NE(replacement.handle.generation, generation);
}

}  // namespace
}  // namespace hy
