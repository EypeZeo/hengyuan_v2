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
    // Value-initialized (not left indeterminate): GCC's -Wmaybe-uninitialized
    // flags try_pop()'s `buf_[t & kMask]` read under -O2 combined with ASan
    // instrumentation (RelWithDebInfo + -DHY_SANITIZER=address) — it cannot
    // prove, across the producer/consumer happens-before relationship, that a
    // slot is never read before its first try_push. Neither plain Release GCC
    // nor MSVC /W4 flags this; it is GCC-under-ASan specific. This zero-init
    // happens once at construction (not on the hot push/pop path), so it does
    // not conflict with CLAUDE.md's hot-path zero-allocation mandate — it is
    // the same one-time-cost pattern already used for head_/tail_ above.
    alignas(kCacheLine) T buf_[N]{};

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
        // TODO 1A.4 batch 2 (AUDIT BUILD-STRINGOP-UDS-EVENT-021): GCC 14, -O3 Release (the
        // "none" tier this repo mirrors from ci-native.yml), false-positives
        // -Werror=stringop-overflow= here once T grows large enough (first observed with
        // UserDataWsEvent, ~200 bytes with six embedded char[24] fields) AND this call gets
        // deeply inlined through several layers of caller (a test helper calling
        // drain_user_data_events()-adjacent code, or BinanceUserDataWsSession::on_read()) --
        // the diagnostic's own reported "destination object" is a totally unrelated
        // std::atomic<size_t>'s 8-byte _M_i member from an unrelated header pulled in
        // transitively (e.g. gtest's own <memory> include), not this array -- a clear
        // cross-attribution error in GCC's -O3 points-to analysis after inlining, not a real
        // bug: buf_[h & kMask] is bounded by kMask by construction (same array-index masking
        // idiom every other ring/table in this codebase uses), and this exact line has been
        // exercised without incident by every SMALLER T this template has ever been
        // instantiated with (BinanceMarketEvent, ReconcileEvent, ...) plus MSVC's own /W4 and
        // this same file's ASan-tier build, none of which flag anything here. Scoped to GNU
        // only (Clang has no such warning), and to this one assignment -- same "diagnostic-only,
        // changes no code generation" reasoning CompilerWarnings.cmake's own
        // hengyuan_allow_third_party_tsan_fences() documents for a different GCC false positive.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
        buf_[h & kMask] = item;  // (1) write the slot fully
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
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
    //
    // LOAD ORDER IS LOAD-BEARING: tail_ must be read FIRST. The producer only
    // ever advances head_ and the consumer only ever advances tail_, so
    // head_ >= tail_ always holds for a consistent snapshot — but these are two
    // independent loads with no atomicity between them. Reading head_ first
    // allowed the consumer to advance tail_ PAST the observed head_ in the gap,
    // making the unsigned subtraction underflow: head=5, tail=3 at the first
    // load; consumer pops twice; second load sees tail=6; 5 - 6 wraps to
    // ~SIZE_MAX. A telemetry gauge that intermittently reports
    // 18446744073709551615 reads as memory corruption to whoever sees it.
    //
    // Reading tail_ first inverts the skew: the head_ observed afterwards can
    // only have grown, so h >= t is guaranteed (see the acquire-load ordering
    // note below) and the subtraction can never underflow.
    //
    // THE OVER-REPORT IS **NOT** BOUNDED BY RING CAPACITY — an earlier version
    // of this comment claimed it was; that reasoning was wrong. `t` is a
    // snapshot from before `h` is read; if the consumer keeps advancing tail_
    // during the gap between the two loads (e.g. this thread is preempted),
    // `h - t` grows by however much tail_ moved in that gap, which has no
    // upper bound — `SpscRingConcurrency.SizeApproxStaysInRangeUnderConcurrentPolling`
    // (test_spsc_concurrency.cpp) reproduces values above capacity() under real
    // producer/consumer contention, not just in theory. Explicitly clamped
    // below so the documented contract ("approximate occupancy") is actually
    // true by construction instead of merely usually true.
    //
    // Ordering proof (why tail-then-head prevents underflow): each load here
    // is memory_order_acquire, which forbids any later operation in this
    // thread's program order — including the second load — from being
    // reordered before it. So the head_ load is guaranteed to observe a value
    // from no earlier than the tail_ load's real time, and since head_ only
    // increases, head_at_or_after(tail_read_time) >= head_at(tail_read_time)
    // >= tail_at(tail_read_time) = t.
    std::size_t size_approx() const noexcept {
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t raw = h - t;  // never underflows, see above
        return raw > N ? N : raw;       // but IS only bounded once clamped
    }

    bool empty_approx() const noexcept { return size_approx() == 0; }

    // AUDIT TEST-GAP-SPSC-034: head_/tail_ are monotonic and never masked, so
    // the occupancy arithmetic's correctness across the 2^64 counter wrap is a
    // real property that no amount of pushing can reach (2^64 pushes). This
    // hook seeds the counters so the wrap point is reachable in a test. It is
    // the only way to set them to anything other than zero-then-increment;
    // production code has no such path. Unconditional declaration, literal-
    // identical across every TU, no ODR risk.
    friend class SpscRingTestHooks;
};

}  // namespace hy
