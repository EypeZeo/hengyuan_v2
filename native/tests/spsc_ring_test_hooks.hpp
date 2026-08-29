// SPDX-License-Identifier: proprietary
// spsc_ring_test_hooks.hpp — test-only seeding of SpscRing's monotonic indices.
//
// Only included by test .cpp files. Must live in namespace hy — see
// binance_signer_test_hooks.hpp's header comment for why.
//
// AUDIT TEST-GAP-SPSC-034: SpscRing's head_/tail_ are monotonic std::size_t
// counters that are never masked, so their correctness across the 2^64
// wrap-around is a real property of the occupancy arithmetic
// (`h - cached_tail_ >= N`). It could not be tested by pushing: reaching the
// wrap needs 2^64 pushes. The existing TEST(SpscRing, WrapAround) covers the
// SLOT index wrapping (h & kMask) after 40 pushes, which is a different
// property despite the name.
//
// Seeding the counters directly is the only way to exercise the real wrap.
// This hook is the sole way to do it -- production code has no path that sets
// these to anything but 0-then-increment.

#pragma once

#include <hengyuan/spsc_ring.hpp>

#include <cstddef>

namespace hy {

class SpscRingTestHooks {
public:
    // Places both counters at `seed` -- an empty ring whose next push will
    // occur at index `seed`. Choosing seed = SIZE_MAX - k puts the wrap point
    // k pushes away.
    template <typename T, std::size_t N>
    static void seed_indices(SpscRing<T, N>& ring, std::size_t seed) noexcept {
        ring.head_.store(seed, std::memory_order_relaxed);
        ring.tail_.store(seed, std::memory_order_relaxed);
        ring.cached_tail_ = seed;
        ring.cached_head_ = seed;
    }

    template <typename T, std::size_t N>
    static std::size_t head(const SpscRing<T, N>& ring) noexcept {
        return ring.head_.load(std::memory_order_relaxed);
    }

    template <typename T, std::size_t N>
    static std::size_t tail(const SpscRing<T, N>& ring) noexcept {
        return ring.tail_.load(std::memory_order_relaxed);
    }
};

}  // namespace hy
