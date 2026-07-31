// NEGATIVE CONTROL for the ThreadSanitizer gate. THIS PROGRAM IS DELIBERATELY
// BROKEN AND TSan IS EXPECTED TO REPORT A DATA RACE IN IT.
//
// WHY
// ---
// test_spsc_concurrency.cpp checks the LOGICAL contract of hy::SpscRing (no
// loss, no duplication, FIFO, intact payloads). On x86-TSO those checks pass
// even if every memory_order_release in spsc_ring.hpp were downgraded to
// memory_order_relaxed, because the hardware supplies the ordering for free.
// ThreadSanitizer is therefore the ONLY thing in this repo that can detect such
// a downgrade on the x86 runners CI uses.
//
// That makes the TSan job's own sensitivity a load-bearing assumption. A TSan
// run that reports "no races" is evidence only if TSan on that runner, with
// those flags, would actually have reported one. This binary is the check on
// that assumption: it contains a ring with the release/acquire pair removed, so
// a working TSan setup MUST flag it.
//
// If this program exits 0 under TSan, the TSan gate is broken and every
// "no data races found" result from the real concurrency tests is unsubstantiated.
// CI inverts the exit code AND greps for the specific diagnostic, because a
// non-zero exit alone could mean the binary crashed for an unrelated reason
// (missing runtime, flag drift, build error) — which would silently disable this
// control while looking like it was working. Same discipline as the
// expected-to-fail TLA+ configs in ci-spec-verification.yml.
//
// This is a standalone main(), not a GTest target: it must be runnable and
// judged on its own exit code, and it must never be swept into the normal
// ctest run where a deliberate race would look like a failure.
//
// Governance: L1, no network/token/order.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

// A deliberately-incorrect twin of hy::SpscRing. The ONLY difference from the
// real implementation is the memory ordering: the producer publishes the slot
// with a RELAXED store and the consumer reads the index with a RELAXED load, so
// no happens-before edge is established between the slot write and the slot
// read. Everything else — index arithmetic, power-of-two mask, capacity check —
// is identical, so any race TSan reports is attributable to the ordering alone.
template <typename T, std::size_t N>
class RelaxedRing {
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two");
    static constexpr std::size_t kMask = N - 1;

    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) T buf_[N];

public:
    bool try_push(const T& item) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_relaxed);  // BUG: should be acquire
        if (h - t >= N) return false;
        buf_[h & kMask] = item;
        // BUG: should be memory_order_release. With relaxed, the slot write above
        // is not ordered before this index publication, so a consumer observing
        // the new head has no guarantee of seeing the slot contents.
        head_.store(h + 1, std::memory_order_relaxed);
        return true;
    }

    bool try_pop(T& out) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_relaxed);  // BUG: should be acquire
        if (h == t) return false;
        out = buf_[t & kMask];
        tail_.store(t + 1, std::memory_order_relaxed);  // BUG: should be release
        return true;
    }
};

struct Payload {
    std::uint64_t a{0};
    std::uint64_t b{0};
    std::uint64_t c{0};
    std::uint64_t d{0};
};

constexpr std::uint64_t kItems = 100'000;
constexpr std::size_t kSlots = 64;

}  // namespace

int main() {
    static RelaxedRing<Payload, kSlots> ring;
    std::uint64_t consumed = 0;

    std::thread producer([] {
        for (std::uint64_t i = 0; i < kItems; ++i) {
            Payload p{i, i * 2u, i * 3u, i * 4u};
            while (!ring.try_push(p)) {
            }
        }
    });

    std::thread consumer([&consumed] {
        Payload out{};
        while (consumed < kItems) {
            if (ring.try_pop(out)) {
                ++consumed;
            }
        }
    });

    producer.join();
    consumer.join();

    // Reaching here means TSan did NOT abort the process. Under a working TSan
    // setup with halt_on_error=1 this line is unreachable. Print loudly so a CI
    // log makes the failure mode obvious rather than looking like a clean pass.
    std::printf(
        "tsan_control_relaxed_ring: completed %llu items WITHOUT a ThreadSanitizer report.\n"
        "If this was built with -DHY_SANITIZER=thread, the TSan gate is NOT working and every\n"
        "\"no data races found\" result from the real concurrency tests is unsubstantiated.\n",
        static_cast<unsigned long long>(consumed));
    return 0;
}
