// NEGATIVE CONTROL for the ThreadSanitizer gate. THIS PROGRAM IS DELIBERATELY
// BROKEN AND TSan IS EXPECTED TO REPORT A DATA RACE IN IT.
//
// The real SubmitAuthority uses acquire/release ordering on its epoch. This
// control keeps the same publish-then-observe protocol but downgrades every
// epoch operation to relaxed. A plain payload write before revoke() and a plain
// read after observing the changed epoch therefore have no happens-before edge.
// A working TSan run must report the payload race.
//
// This is a standalone main(), so the deliberate race stays outside normal
// GoogleTest/CTest runs. The TSan control runner checks for the race diagnostic.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <thread>

namespace {

class RelaxedSubmitAuthority {
public:
    std::uint64_t epoch() const noexcept { return epoch_.load(std::memory_order_relaxed); }

    bool claim(std::uint64_t expected) noexcept {
        if (expected == std::numeric_limits<std::uint64_t>::max()) return false;
        return epoch_.compare_exchange_strong(expected, expected + 1U, std::memory_order_relaxed,
                                               std::memory_order_relaxed);
    }

    void revoke() noexcept {
        auto value = epoch_.load(std::memory_order_relaxed);
        while (value != std::numeric_limits<std::uint64_t>::max() &&
               !epoch_.compare_exchange_weak(value, value + 1U, std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {}
    }

private:
    std::atomic<std::uint64_t> epoch_{1};
};

struct Payload {
    std::uint64_t value{0};
};

}  // namespace

int main() {
    RelaxedSubmitAuthority authority;
    Payload payload{};
    const std::uint64_t original = authority.epoch();
    std::uint64_t observed_payload = 0;

    std::thread producer([&] {
        payload.value = 0x5a17c0deULL;
        authority.revoke();
    });
    std::thread observer([&] {
        while (authority.epoch() == original) std::this_thread::yield();
        observed_payload = payload.value;
    });

    producer.join();
    observer.join();
    std::printf("tsan_control_relaxed_submit_authority: observed payload %llu without a TSan report.\n",
                static_cast<unsigned long long>(observed_payload));
    return 0;
}
