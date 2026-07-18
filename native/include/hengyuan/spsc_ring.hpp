// SPDX-License-Identifier: proprietary
//
// spsc_ring.hpp — HengYuan native core, lock-free single-producer/single-
// consumer ring buffer. The decoupling boundary between the Boost.Asio I/O
// thread (producer; future P2-CORE-IO-01) and the pinned hot thread
// (consumer) — so multi-connection async I/O never pollutes the deterministic
// hot path (ADR-018 D4).
//
// Governance (P2-CORE-01-IMPL, ADR-016 L1):
//   - Header-only data structure. NO network, NO token, NO order placement.
//
// Design properties:
//   - Zero heap allocation in steady state: the slot buffer is allocated once
//     at construction; try_push is a single ~sizeof(T) byte copy, no malloc.
//   - No false sharing: head_ and tail_ each occupy their own cache line, so
//     producer and consumer never ping-pong a shared line via MESI.
//   - No torn reads / no reordering: try_push publishes the slot write with a
//     release store to head_; try_pop reads head_ with an acquire load. The
//     release/acquire pair establishes happens-before, so the consumer can
//     never observe a half-written slot. On x86 (TSO) these are near-free; the
//     explicit ordering is what keeps it correct on ARM (see P2-CORE-05).

#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace hy {

inline constexpr std::size_t kCacheLine = 64;

// Lock-free SPSC ring. Exactly one producer thread may call try_push; exactly
// one consumer thread may call try_pop. N must be a power of two.
//
// T must be trivially copyable (POD-like) so that slot assignment is a plain
// byte copy with no allocation, no exceptions, and no destructor ordering.
template <typename T, std::size_t N>
class SpscRing {
    static_assert(N >= 2, "SpscRing capacity must be >= 2");
    static_assert((N & (N - 1)) == 0, "SpscRing capacity N must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRing<T> requires trivially-copyable T for zero-alloc memcpy semantics");

    static constexpr std::size_t kMask = N - 1;

    // head_ is written only by the producer, read by the consumer.
    // tail_ is written only by the consumer, read by the producer.
    // Each sits on its own cache line to avoid false sharing.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};

    // Per-side cached copy of the opposite index. Lets each side avoid touching
    // the other's atomic on every call — only refreshed when the local view
    // says the ring is (apparently) full/empty. Amortizes cross-core traffic.
    alignas(kCacheLine) std::size_t cached_tail_{0};  // producer-private
    alignas(kCacheLine) std::size_t cached_head_{0};  // consumer-private

    // Slot storage, allocated once with the object. No per-push allocation.
    alignas(kCacheLine) T buf_[N];

public:
    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;

    static constexpr std::size_t capacity() noexcept { return N; }

    // Producer-only. Returns false when the ring is full (backpressure signal:
    // caller decides — conflatable streams drop+count; non-conflatable depth
    // deltas must trigger a REST resync rather than silently dropping).
    bool try_push(const T& item) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);  // own index: relaxed
        // Occupancy = h - tail. Apparent-full check against the cached tail
        // first; only pay the cross-core acquire load if it looks full.
        if (h - cached_tail_ >= N) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (h - cached_tail_ >= N) {
                return false;  // genuinely full
            }
        }
        buf_[h & kMask] = item;  // (1) write the slot fully
        // (2) release: the slot write above is guaranteed to be visible to a
        // consumer that observes this new head value. Blocks both compiler and
        // CPU reordering -> no torn read.
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Consumer-only. Returns false when the ring is empty.
    bool try_pop(T& out) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);  // own index: relaxed
        if (cached_head_ == t) {
            cached_head_ = head_.load(std::memory_order_acquire);  // pairs with push's release
            if (cached_head_ == t) {
                return false;  // empty
            }
        }
        out = buf_[t & kMask];  // safe: acquire established happens-before the producer's slot write
        tail_.store(t + 1, std::memory_order_release);  // publish consumption progress
        return true;
    }

    // Approximate occupancy for telemetry only. Not synchronized; do not use
    // for control flow (use try_push/try_pop return values instead).
    std::size_t size_approx() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return h - t;
    }

    bool empty_approx() const noexcept { return size_approx() == 0; }
};

}  // namespace hy
