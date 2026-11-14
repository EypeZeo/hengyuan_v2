// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/spsc_ring.hpp>
#include <atomic>
#include <cstdint>
#include <limits>

namespace hy {

// Sources publish their invalidating state before revoke(). A claim and a
// revocation compete on this one word. A successful claim is consumed exactly
// once; later revocations cannot retract a request that may already be in flight.
class alignas(kCacheLine) SubmitAuthority {
public:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::uint64_t epoch() const noexcept { return epoch_.load(std::memory_order_acquire); }

    bool claim(std::uint64_t expected) noexcept {
        if (expected == exhausted()) return false;
        return epoch_.compare_exchange_strong(expected, expected + 1,
                                              std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void revoke() noexcept {
        auto value = epoch_.load(std::memory_order_acquire);
        while (value != exhausted() &&
               !epoch_.compare_exchange_weak(value, value + 1,
                                             std::memory_order_acq_rel, std::memory_order_acquire)) {}
    }

    static constexpr std::uint64_t exhausted() noexcept { return std::numeric_limits<std::uint64_t>::max(); }
private:
    std::atomic<std::uint64_t> epoch_{1};
};

}  // namespace hy
