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
// the exact pair run_export_worker_once() itself calls), against a producer
// thread. A working TSan setup MUST flag this.
//
// Deliberately does NOT route through run_export_worker_once()/
// LastRemoteAckedTipStore/ExternalAnchorClient: those add real per-tuple file
// I/O (fsync, atomic rename). ExportOutboxRing's own peek_oldest()/
// pop_after_remote_ack() are the shared primitive whose single-consumer
// contract run_export_worker_once() depends on -- exercising them directly,
// in memory only, is enough.
//
// HOW THE REPORT IS MADE INDEPENDENT OF SCHEDULING
// ------------------------------------------------
// An earlier version of this control let two consumers and a producer run free
// over 200,000 items and relied on the scheduler to interleave them finely
// enough for TSan to see one unordered pair of accesses. TSan reports two
// accesses that have no happens-before edge between them -- they do not have
// to overlap in time -- but whether the two consumers ever touch the same slot
// unordered depends on how the threads happen to interleave. In CI that
// version finished "WITHOUT a ThreadSanitizer report" on three consecutive
// executions (two attempts on one branch, one on master) after passing on the
// earlier runs of the workflow, and in a WSL2 experiment (g++ 14, the CI flags)
// it stayed silent in 5 of 30 runs when pinned to one CPU while reporting in
// all 30 runs on 16 CPUs. A gate control whose verdict depends on the
// scheduler proves nothing either way.
//
// This version fixes the order in which the three threads run, with RELAXED
// atomics only (they create no happens-before edge, so they cannot hide the
// race), and makes exactly one slot change hands the wrong way:
//
//   main      fills the ring (256 items) before any other thread exists;
//             thread creation orders those writes before everything below.
//   A         peek_oldest(): a plain READ of slot 0, then says so.
//   B         waits for A, then pop_after_remote_ack(): head 0 -> 1, release.
//   producer  waits for B, then try_push(): its acquire load of head sees B's
//             release store, so slot 0 looks free, and it overwrites slot 0
//             with a plain WRITE.
//
// A's read of slot 0 is not ordered before the producer's write: A published
// nothing that the producer (or B) acquired -- the only link from A to B to the
// producer is a relaxed flag, which is not synchronisation. With the single
// consumer the contract demands, the same read is followed by that consumer's
// own release store of head, which the producer's acquire load orders it
// before; with two consumers it is not. That lost ordering is exactly what the
// single-consumer contract exists to prevent.
//
// Do not "tidy" the flag into acquire/release: that adds the very edge this
// control exists to lack, and TSan would stay silent.
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

constexpr int kPeeked = 1;  // consumer A has read slot 0
constexpr int kPopped = 2;  // consumer B has advanced head past slot 0

// RELAXED on purpose: this only fixes the order in which the threads run; an
// acquire/release pair here would be a happens-before edge and would hide the race.
void wait_for(const std::atomic<int>& phase, int want) {
    while (phase.load(std::memory_order_relaxed) < want) std::this_thread::yield();
}

}  // namespace

int main() {
    static hy::ExportOutboxRing ring;
    for (std::uint64_t i = 0; i < hy::ExportOutboxRing::kCapacity; ++i) {
        hy::ExportTuple t{};
        t.sequence = i;
        if (!ring.try_push(t)) {
            std::printf("tsan_control_export_worker_dual_consumer: setup failed, the ring refused item %llu\n",
                        static_cast<unsigned long long>(i));
            return 2;
        }
    }

    std::atomic<int> phase{0};

    // All three are created here, so thread creation orders none of them after another.
    std::thread consumer_a([&] {
        hy::ExportTuple t{};
        (void)ring.peek_oldest(t);  // plain read of slot 0
        phase.store(kPeeked, std::memory_order_relaxed);
    });
    std::thread consumer_b([&] {
        wait_for(phase, kPeeked);
        ring.pop_after_remote_ack();  // head 0 -> 1, release
        phase.store(kPopped, std::memory_order_relaxed);
    });
    std::thread producer([&] {
        wait_for(phase, kPopped);
        hy::ExportTuple t{};
        t.sequence = hy::ExportOutboxRing::kCapacity;
        while (!ring.try_push(t)) std::this_thread::yield();  // plain write of slot 0
    });

    consumer_a.join();
    consumer_b.join();
    producer.join();

    // Reaching here means TSan did NOT abort the process. Under a working TSan
    // setup with halt_on_error=1 this line is unreachable.
    std::printf(
        "tsan_control_export_worker_dual_consumer: completed WITHOUT a ThreadSanitizer report.\n"
        "If this was built with -DHY_SANITIZER=thread, the TSan gate is NOT working and every\n"
        "\"no data races found\" result from the real concurrency tests is unsubstantiated.\n");
    return 0;
}
