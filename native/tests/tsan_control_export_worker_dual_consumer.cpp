// NEGATIVE CONTROL for the ThreadSanitizer gate. THIS PROGRAM IS DELIBERATELY
// BROKEN AND TSan IS EXPECTED TO REPORT A DATA RACE IN IT.
//
// WHY
// ---
// export_worker.hpp's run_export_worker_once() documents ExportOutboxRing as
// requiring a single dedicated export-worker thread (durable_control_plane.hpp:
// "Consumer = export worker thread ONLY", the same SPSC contract
// ExportOutboxRing's producer side already relies on). Nothing enforces that
// contract at compile time or runtime -- it is a documented rule, and per this
// codebase's own established discipline (see tsan_control_relaxed_ring.cpp),
// a documented rule nobody checks is not evidence the rule matters. This
// binary violates the rule on purpose: two threads both drive
// ExportOutboxRing's consumer-side API (peek_oldest()/pop_after_remote_ack(),
// the exact pair run_export_worker_once() itself calls) concurrently, against
// a single producer thread pushing a large stream of tuples. A working TSan
// setup MUST flag this.
//
// Deliberately does NOT route through run_export_worker_once()/
// LastRemoteAckedTipStore/ExternalAnchorClient: those add real per-tuple file
// I/O (fsync, atomic rename), which at the iteration counts needed to give a
// timing-dependent race a reliable chance to manifest would make this binary
// take minutes to run. ExportOutboxRing's own peek_oldest()/
// pop_after_remote_ack() are the shared primitive whose single-consumer
// contract run_export_worker_once() depends on -- exercising them directly,
// in-memory only, mirrors test_spsc_concurrency.cpp's own "large iteration
// count, pure memory ops" approach for the same reliability reason.
//
// If this program exits 0 under TSan, the TSan gate is broken and every
// "no data races found" result from the real concurrency tests is
// unsubstantiated. CI (or a manual run) inverts the exit code AND greps for
// the specific diagnostic, same discipline as tsan_control_relaxed_ring.cpp.
//
// This is a standalone main(), not a GTest target: it must be runnable and
// judged on its own exit code, and it must never be swept into the normal
// ctest run where a deliberate race would look like a failure.
//
// Governance: L1, no network/token/order.

#include <hengyuan/durable_control_plane.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

constexpr std::uint64_t kItems = 200'000;

}  // namespace

int main() {
    static hy::ExportOutboxRing ring;
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kItems; ++i) {
            hy::ExportTuple t{};
            t.sequence = i;
            while (!ring.try_push(t)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    // TWO consumer threads deliberately violate ExportOutboxRing's documented
    // single-consumer contract by both calling its consumer-side API
    // concurrently -- the same pair run_export_worker_once() itself calls.
    auto consumer_fn = [&] {
        for (;;) {
            hy::ExportTuple t{};
            if (ring.peek_oldest(t)) {
                ring.pop_after_remote_ack();
                continue;
            }
            // Only stop once production has genuinely finished AND the ring
            // reports empty -- checked AFTER a failed peek, so a momentarily
            // empty-looking ring mid-stream doesn't cause an early exit.
            if (producer_done.load(std::memory_order_acquire) && ring.empty()) break;
            std::this_thread::yield();
        }
    };
    std::thread consumer_a(consumer_fn);
    std::thread consumer_b(consumer_fn);

    producer.join();
    consumer_a.join();
    consumer_b.join();

    // Reaching here means TSan did NOT abort the process. Under a working
    // TSan setup with halt_on_error=1 this line is unreachable.
    std::printf(
        "tsan_control_export_worker_dual_consumer: completed %llu items WITHOUT a "
        "ThreadSanitizer report.\n"
        "If this was built with -DHY_SANITIZER=thread, the TSan gate is NOT working and every\n"
        "\"no data races found\" result from the real concurrency tests is unsubstantiated.\n",
        static_cast<unsigned long long>(kItems));
    return 0;
}
