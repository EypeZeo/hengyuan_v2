// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/store_binding.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace hy {

struct OperatorResetScope {
    StoreEnvironment environment{static_cast<StoreEnvironment>(0)};
    std::uint64_t account_uid{0};
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};

    [[nodiscard]] bool valid() const noexcept {
        return is_valid_store_environment(environment) && account_uid != 0 &&
               (store_uuid_lo != 0 || store_uuid_hi != 0);
    }

    bool operator==(const OperatorResetScope&) const noexcept = default;
};

struct OperatorResetTarget {
    OperatorResetScope scope;
    std::uint64_t revision{0};
};

struct OperatorResetApproval {
    OperatorResetTarget target;
    std::int64_t not_after_ms{0};
};

// Provisioned by the process owner. The callback verifies the signature for
// the current process challenge. Its context outlives the pipeline.
struct OperatorResetApprovalPort {
    using Verify = std::optional<OperatorResetApproval> (*)(void*, std::span<const unsigned char>) noexcept;
    Verify verify{nullptr};
    void* context{nullptr};
};

enum class OperatorResetResult {
    Applied,
    Unavailable,
    InvalidSignature,
    WrongStore,
    NotReady,
    ClockUnavailable,
    Expired,
    StateChanged,
    WriteFailed,
};

inline constexpr const char* operator_reset_result_name(OperatorResetResult result) noexcept {
    switch (result) {
        case OperatorResetResult::Applied: return "Applied";
        case OperatorResetResult::Unavailable: return "Unavailable";
        case OperatorResetResult::InvalidSignature: return "InvalidSignature";
        case OperatorResetResult::WrongStore: return "WrongStore";
        case OperatorResetResult::NotReady: return "NotReady";
        case OperatorResetResult::ClockUnavailable: return "ClockUnavailable";
        case OperatorResetResult::Expired: return "Expired";
        case OperatorResetResult::StateChanged: return "StateChanged";
        case OperatorResetResult::WriteFailed: return "WriteFailed";
    }
    return "Unknown";
}

}  // namespace hy
