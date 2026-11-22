#pragma once

#include <chrono>
#include <cstdint>
#include <hengyuan/order_tracker.hpp>

namespace hy::test {

[[nodiscard]] inline ReconcileTime reconcile_at_ms(std::int64_t since_epoch_ms) noexcept {
    return ReconcileTime{std::chrono::milliseconds{since_epoch_ms}};
}

}  // namespace hy::test
