// NEGATIVE CONTROL for the ThreadSanitizer gate, for the WebSocket API subscribe mailbox. THIS PROGRAM IS DELIBERATELY BROKEN AND TSan IS
// EXPECTED TO REPORT A DATA RACE IN IT.
//
// WHY
// ---
// test_ws_api_mailbox_concurrency.cpp checks the LOGICAL contract of hy::WsApiSubscribeMailbox (every frame crosses whole, one post and one
// take win under contention). On x86-TSO those checks pass even if the release store that publishes the frame and the acquire compare-exchange
// that claims it were downgraded to relaxed, because the hardware supplies the ordering for free. ThreadSanitizer is therefore the only thing
// that can detect such a downgrade on the x86 runners CI uses, which makes the TSan job's own sensitivity for THIS boundary a load-bearing
// assumption — the same argument tsan_control_relaxed_ring.cpp makes for the ring. This binary contains the mailbox protocol with the
// release/acquire pair removed, so a working TSan setup MUST flag it. If it exits 0 under TSan, every "no data races found" result for the
// mailbox is unsubstantiated.
//
// CI and tools/wsl_verify.sh invert the exit code AND grep for the specific diagnostic (a non-zero exit alone could mean the binary crashed
// for an unrelated reason), exactly as for the other controls.
//
// DETERMINISTIC SCHEDULE
// ----------------------
// Exactly ONE frame crosses, in a fixed order, so the verdict does not depend on scheduling (see tsan_control_relaxed_ring.cpp for the
// history of why that matters). The producer posts (a plain write of the slot, then a RELAXED publication of Ready) and only then raises
// `phase` with a RELAXED store; the consumer waits for `phase` with RELAXED loads and only then takes (a RELAXED claim, a plain read of the
// slot). Relaxed atomics fix the ORDER of the two slot accesses in real time but create no happens-before edge in TSan's model, so TSan always
// holds a write and a later, unordered read of the same slot — and it is exactly the mailbox's relaxed publication that leaves them
// unordered. Do NOT "tidy" `phase` into release/acquire: that adds the very edge this control must not have, and TSan goes quiet.
//
// A standalone main(), not a GTest target: it must be judged on its own exit code and never be swept into the normal ctest run.
//
// Governance: L1, no network/token/order.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

struct Frame {
    std::array<char, 1536> bytes{};
    std::uint32_t length{0};
    std::uint64_t request_id{0};
};

// A deliberately-incorrect twin of hy::WsApiSubscribeMailbox. The ONLY difference from the real implementation is the memory ordering: the
// producer publishes Ready with a RELAXED store and the consumer claims it with RELAXED orders, so nothing orders the slot write before the
// slot read. The state machine itself (Empty -> Writing -> Ready -> Taking -> Taken) is identical, so any race TSan reports is attributable to
// the ordering alone.
class RelaxedMailbox {
    static constexpr std::uint32_t kEmpty = 0;
    static constexpr std::uint32_t kWriting = 1;
    static constexpr std::uint32_t kReady = 2;
    static constexpr std::uint32_t kTaking = 3;
    static constexpr std::uint32_t kTaken = 4;

    alignas(64) std::atomic<std::uint32_t> state_{kEmpty};
    alignas(64) Frame slot_{};

public:
    bool post(const Frame& f) noexcept {
        std::uint32_t expected = kEmpty;
        if (!state_.compare_exchange_strong(expected, kWriting, std::memory_order_relaxed, std::memory_order_relaxed)) return false;
        slot_ = f;
        // BUG: should be memory_order_release. With relaxed, the slot write above is not ordered before this publication, so a consumer that
        // observes Ready has no guarantee of seeing the slot contents.
        state_.store(kReady, std::memory_order_relaxed);
        return true;
    }

    bool take(Frame& out) noexcept {
        std::uint32_t expected = kReady;
        // BUG: should be memory_order_acquire.
        if (!state_.compare_exchange_strong(expected, kTaking, std::memory_order_relaxed, std::memory_order_relaxed)) return false;
        out = slot_;
        state_.store(kTaken, std::memory_order_relaxed);  // BUG: should be memory_order_release
        return true;
    }
};

}  // namespace

int main() {
    static RelaxedMailbox box;
    // RELAXED on purpose (see the header): it orders the two slot accesses in real time without synchronising them.
    std::atomic<int> phase{0};
    bool taken = false;

    std::thread producer([&phase] {
        Frame f{};
        f.length = 100;
        f.request_id = 42;
        for (std::size_t i = 0; i < f.length; ++i) f.bytes[i] = 'x';
        (void)box.post(f);
        phase.store(1, std::memory_order_relaxed);
    });

    std::thread consumer([&phase, &taken] {
        while (phase.load(std::memory_order_relaxed) != 1) {
            std::this_thread::yield();
        }
        Frame out{};
        while (!box.take(out)) {
            std::this_thread::yield();
        }
        taken = out.request_id == 42;
    });

    producer.join();
    consumer.join();

    // Reaching here means TSan did NOT abort the process. Under a working TSan setup with halt_on_error=1 this line is unreachable. Print
    // loudly so a CI log makes the failure mode obvious rather than looking like a clean pass.
    std::printf(
        "tsan_control_relaxed_mailbox: crossed %d frame WITHOUT a ThreadSanitizer report.\n"
        "If this was built with -DHY_SANITIZER=thread, the TSan gate is NOT working and every\n"
        "\"no data races found\" result for the subscribe mailbox is unsubstantiated.\n",
        taken ? 1 : 0);
    return 0;
}
