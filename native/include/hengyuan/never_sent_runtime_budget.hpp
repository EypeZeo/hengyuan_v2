// SPDX-License-Identifier: proprietary
// never_sent_runtime_budget.hpp -- SAFE-07a: hot-thread-only Intent-orphan telemetry and admission budget.
//
// A successfully acknowledged Intent followed by a class-A refusal is an Intent orphan only when its NeverSent closure was
// not acknowledged. Failed is counted conservatively: its frame may have landed, and the next recovery scan is the authority
// that replaces this process's live estimate with the recovered total. Intent/Prepared append failures and class-B refusals
// never pass through observe_class_a(). Recovered orphans carry no reliable refusal reason and use RecoveredUnknown.
//
// The object has no synchronization on purpose: the submit hot thread owns it and every OrchestratorContext copy only carries
// a pointer to it. Alerts are a monotone in-memory bitmask, never durable records (an alert must not grow the log whose budget
// is being approached). At 90% no new Intent may be written; existing logs are never deleted or compacted by this object.

#pragma once

#include <hengyuan/durable_order_audit_port.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace hy::never_sent {

enum class IntentOrphanReason : std::uint8_t {
    ConfirmationMissing = 0,
    ConfirmationMismatch = 1,
    SubmitPortInvalid = 2,
    InFlightRegistryUnavailable = 3,
    RateLimitExhausted = 4,
    DuplicateInFlight = 5,
    InFlightCapacityExceeded = 6,
    RecoveredUnknown = 7,
    Unknown = 8,
};

inline constexpr std::size_t kIntentOrphanReasonCount = 9;

inline constexpr std::uint8_t kIntentOrphanAlert25 = 1U << 0U;
inline constexpr std::uint8_t kIntentOrphanAlert50 = 1U << 1U;
inline constexpr std::uint8_t kIntentOrphanAlert75 = 1U << 2U;
inline constexpr std::uint8_t kIntentOrphanAlert90 = 1U << 3U;
inline constexpr std::uint8_t kIntentOrphanAlertAll =
    kIntentOrphanAlert25 | kIntentOrphanAlert50 | kIntentOrphanAlert75 | kIntentOrphanAlert90;
inline constexpr std::array<std::uint8_t, 4> kIntentOrphanAlertBits{
    kIntentOrphanAlert25, kIntentOrphanAlert50, kIntentOrphanAlert75, kIntentOrphanAlert90};
inline constexpr std::array<std::uint8_t, 4> kIntentOrphanAlertPercents{25, 50, 75, 90};

class IntentOrphanBudget {
public:
    IntentOrphanBudget(std::size_t recovered, std::size_t maximum) noexcept
        : recovered_(recovered), total_(recovered), maximum_(maximum) {
        reason_counts_[static_cast<std::size_t>(IntentOrphanReason::RecoveredUnknown)] = recovered;
        refresh_alerts();
    }

    IntentOrphanBudget(const IntentOrphanBudget&) = delete;
    IntentOrphanBudget& operator=(const IntentOrphanBudget&) = delete;
    IntentOrphanBudget(IntentOrphanBudget&&) = delete;
    IntentOrphanBudget& operator=(IntentOrphanBudget&&) = delete;

    // Called after an acknowledged Intent and a class-A refusal. Only an acknowledged closure clears the attempt.
    void observe_class_a(Reason reason, ClosureAppendStatus status) noexcept {
        if (status == ClosureAppendStatus::Appended) return;
        increment(live_);
        increment(total_);
        increment(reason_counts_[reason_index(reason)]);
        refresh_alerts();
    }

    [[nodiscard]] bool allows_new_intent() const noexcept { return (alert_mask_ & kIntentOrphanAlert90) == 0; }
    [[nodiscard]] bool stopped() const noexcept { return !allows_new_intent(); }
    [[nodiscard]] std::size_t recovered() const noexcept { return recovered_; }
    [[nodiscard]] std::size_t live() const noexcept { return live_; }
    [[nodiscard]] std::size_t total() const noexcept { return total_; }
    [[nodiscard]] std::size_t maximum() const noexcept { return maximum_; }
    [[nodiscard]] std::uint8_t alert_mask() const noexcept { return alert_mask_; }

    [[nodiscard]] std::size_t count(IntentOrphanReason reason) const noexcept {
        return reason_counts_[static_cast<std::size_t>(reason)];
    }

private:
    static void increment(std::size_t& value) noexcept {
        if (value != std::numeric_limits<std::size_t>::max()) ++value;
    }

    static std::size_t reason_index(Reason reason) noexcept {
        switch (reason) {
            case Reason::ConfirmationMissing: return static_cast<std::size_t>(IntentOrphanReason::ConfirmationMissing);
            case Reason::ConfirmationMismatch: return static_cast<std::size_t>(IntentOrphanReason::ConfirmationMismatch);
            case Reason::SubmitPortInvalid: return static_cast<std::size_t>(IntentOrphanReason::SubmitPortInvalid);
            case Reason::InFlightRegistryUnavailable:
                return static_cast<std::size_t>(IntentOrphanReason::InFlightRegistryUnavailable);
            case Reason::RateLimitExhausted: return static_cast<std::size_t>(IntentOrphanReason::RateLimitExhausted);
            case Reason::DuplicateInFlight: return static_cast<std::size_t>(IntentOrphanReason::DuplicateInFlight);
            case Reason::InFlightCapacityExceeded:
                return static_cast<std::size_t>(IntentOrphanReason::InFlightCapacityExceeded);
            default: return static_cast<std::size_t>(IntentOrphanReason::Unknown);
        }
    }

    [[nodiscard]] bool threshold_reached(std::uint8_t percent) const noexcept {
        if (maximum_ == 0) return true;
        // ceil(maximum * percent / 100), without multiplying the full size_t budget or using floating point.
        const std::size_t p = percent;
        const std::size_t threshold = (maximum_ / 100) * p + ((maximum_ % 100) * p + 99) / 100;
        return total_ >= threshold;
    }

    void refresh_alerts() noexcept {
        for (std::size_t i = 0; i < kIntentOrphanAlertBits.size(); ++i) {
            if (threshold_reached(kIntentOrphanAlertPercents[i])) alert_mask_ |= kIntentOrphanAlertBits[i];
        }
    }

    std::size_t recovered_{0};
    std::size_t live_{0};
    std::size_t total_{0};
    std::size_t maximum_{0};
    std::array<std::size_t, kIntentOrphanReasonCount> reason_counts_{};
    std::uint8_t alert_mask_{0};
};

}  // namespace hy::never_sent
