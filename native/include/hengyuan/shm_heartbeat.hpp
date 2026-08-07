// SPDX-License-Identifier: proprietary
// shm_heartbeat.hpp — Shared-memory heartbeat control block + hot-path writer.
//
// The ShmControlBlock lives in a POSIX shared memory segment (shm_open/mmap).
// The trading process writes heartbeat ticks on every run_once() iteration —
// zero syscall, zero lock, minimal jitter on the hot path. An independent
// watchdog PROCESS reads the block to detect liveness.
//
// Multi-dimensional heartbeat (defeats "zombie deadlock" false positives):
//   loop_counter     — incremented every hot-path iteration
//   last_incoming_ns — updated on every parsed market event
//   last_outgoing_ns — updated on every simulated/live fill
//
// ---------------------------------------------------------------------------
// AUDIT PERF-SHM-004: WHAT THIS FILE USED TO CLAIM, AND WHAT IT ACTUALLY DID
// ---------------------------------------------------------------------------
// This header used to say the writer used "relaxed atomics". It did not: every
// field was a plain integer, <atomic> was included but never used, and the
// writer's updates raced the watchdog's 64-byte memcpy of the same bytes. Two
// separate problems came out of that:
//
//   1. CORRECTNESS. Plain concurrent access to the same object from two
//      processes is a data race in the C++ memory model, and the kill path was
//      the worst of it: the watchdog's `kill_armed = 1` and the trading
//      process's `kill_armed != 0` had no happens-before edge whatsoever, so
//      nothing guaranteed the kill request was ever observed. TSan cannot see
//      this because the two sides are different processes — the race lived in
//      exactly the blind spot of this repo's verification strategy.
//   2. COST. Integrity was a CRC32 over 44 bytes recomputed after EVERY field
//      write, with a bit-serial (8 shifts/byte) inner loop. hot_thread.hpp
//      calls tick() and set_incoming() for every accepted market event, so that
//      was two full CRC32 passes per event: measured at 700–725 ns/event on
//      -O2 -march=native, against a stated sub-microsecond end-to-end budget.
//
// The fix addresses both at once with a SEQLOCK over atomic fields:
//
//   * Every field is std::atomic and accessed with memory_order_relaxed, so
//      there is no data race by construction (and TSan can now be pointed at
//      an in-process two-thread test of the same protocol).
//   * `seq` is even when the block is stable and odd while a write is in
//      progress. A reader that observes the same even `seq` before and after
//      its loads knows every field it read came from one consistent generation.
//      That is a strictly stronger guarantee than the CRC provided (the CRC was
//      itself written non-atomically after the field it covered, so a reader
//      could see a new counter with a stale checksum and call it corruption).
//   * tick() is now two relaxed atomic stores plus one relaxed increment. The
//      CRC is gone from the hot path entirely.
//
// Corruption from a wild pointer or a cross-process stomp is still detected, by
// magic/version plus "the reader could not obtain a stable generation" — see
// shm_read_snapshot(). What is deliberately NOT claimed any more is a
// cryptographic or checksum-grade integrity guarantee; the previous CRC was
// never that either (it was unkeyed and adjacent to the data it covered).
//
// Cache-line aligned: the whole block is one line, so a reader never straddles
// two lines mid-snapshot.
//
// Governance: L2 (shared memory infrastructure, no network/token/order).
// Linux-only (shm_open, mmap). Compiles but is a no-op on non-Linux.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace hy {

static constexpr std::uint32_t kShmMagic = 0x31575948;  // "HYW1" little-endian
// Bumped 1 -> 2 with the seqlock layout change (audit PERF-SHM-004). The block is
// a cross-process ABI; an old watchdog attached to a new writer would otherwise
// misread it as valid. A version mismatch is fail-closed on both sides.
static constexpr std::uint32_t kShmVersion = 2;

// 64-byte cache-line-aligned shared memory control block.
// Written by the trading process, read by the watchdog. Layout is fixed for
// cross-process ABI stability.
//
// Every field is atomic because BOTH processes touch this memory concurrently.
// The WRITER stores every payload field with memory_order_relaxed -- ordering is
// established by the seqlock's release stores on `seq`, so paying for per-field
// ordering on the hot path would be pure cost, and on x86-64/ARM64 a relaxed
// store of a lock-free atomic compiles to a plain store. The READER uses acquire
// loads; see shm_read_snapshot() for why that side is not symmetric.
struct alignas(64) ShmControlBlock {
    std::atomic<std::uint32_t> seq;              // [ 0] even = stable, odd = write in progress
    std::atomic<std::uint32_t> magic;            // [ 4] must be kShmMagic
    std::atomic<std::uint32_t> version;          // [ 8] must be kShmVersion
    std::atomic<std::uint32_t> kill_armed;       // [12] watchdog writes 1 to request kill
    std::atomic<std::uint64_t> loop_counter;     // [16] hot-path iteration count
    std::atomic<std::uint64_t> last_incoming_ns; // [24] last market data parse (steady_clock)
    std::atomic<std::uint64_t> last_outgoing_ns; // [32] last fill execution (steady_clock)
    std::atomic<std::uint64_t> main_pid;         // [40] trading process PID
    std::uint8_t _pad[16];                       // [48..63] explicit pad to a full cache line
};

static_assert(sizeof(ShmControlBlock) == 64);
static_assert(alignof(ShmControlBlock) == 64);
// A shared-memory atomic that is not lock-free would be backed by a process-local
// lock table, which is worse than useless across a process boundary: each process
// would take a different lock and the "atomic" would silently not be.
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "ShmControlBlock's atomics live in shared memory and MUST be lock-free");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "ShmControlBlock's atomics live in shared memory and MUST be lock-free");
static_assert(sizeof(std::atomic<std::uint32_t>) == 4 && sizeof(std::atomic<std::uint64_t>) == 8,
              "atomic must not add storage — the block is a fixed cross-process ABI");

// Plain, non-atomic value snapshot handed back to readers. This is what the
// watchdog reasons about; it never dereferences the shared block directly.
struct ShmHeartbeatSnapshot {
    std::uint32_t magic{0};
    std::uint32_t version{0};
    std::uint32_t kill_armed{0};
    std::uint64_t loop_counter{0};
    std::uint64_t last_incoming_ns{0};
    std::uint64_t last_outgoing_ns{0};
    std::uint64_t main_pid{0};
};

// How many times shm_read_snapshot() retries before giving up. A writer only
// holds an odd `seq` for a handful of instructions, so more than a couple of
// retries means the writer is stuck mid-update (or the memory is being stomped)
// — which is itself the signal the watchdog wants, not something to spin on.
inline constexpr int kShmReadMaxAttempts = 8;

// Seqlock read. Returns false if a stable generation could not be obtained
// within kShmReadMaxAttempts — the caller must treat that as "cannot vouch for
// this block", exactly as it used to treat a checksum mismatch.
// WHY THE PAYLOAD LOADS ARE ACQUIRE AND NOT RELAXED-PLUS-A-FENCE
// -------------------------------------------------------------
// The textbook seqlock reader is "relaxed payload loads, then
// atomic_thread_fence(acquire), then re-read seq". That does not compile here:
// GCC rejects std::atomic_thread_fence under -fsanitize=thread outright
// ("'atomic_thread_fence' is not supported with '-fsanitize=thread'",
// -Werror=tsan), because TSan's happens-before model cannot represent a
// standalone fence. Found by actually building this file under the TSan job, not
// predicted -- and worth stating, because "just put the fence back" is the
// natural-looking cleanup that would silently break the TSan build again.
//
// Making each payload load acquire achieves the same ordering by a different
// route: an acquire load forbids any LATER operation in program order -- here the
// second seq load -- from being reordered before it, so the payload reads are
// pinned between the two seq observations, which is exactly the bracketing the
// seqlock validity argument needs. It is strictly stronger per-load than the
// fence version, and the cost lands entirely on the READER (the watchdog, polling
// at ~2 Hz), never on the writer's hot path, which keeps its relaxed stores.
inline bool shm_read_snapshot(const ShmControlBlock& blk, ShmHeartbeatSnapshot& out) noexcept {
    for (int attempt = 0; attempt < kShmReadMaxAttempts; ++attempt) {
        const std::uint32_t s1 = blk.seq.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0u) continue;  // writer mid-update

        ShmHeartbeatSnapshot v{};
        v.magic = blk.magic.load(std::memory_order_acquire);
        v.version = blk.version.load(std::memory_order_acquire);
        v.kill_armed = blk.kill_armed.load(std::memory_order_acquire);
        v.loop_counter = blk.loop_counter.load(std::memory_order_acquire);
        v.last_incoming_ns = blk.last_incoming_ns.load(std::memory_order_acquire);
        v.last_outgoing_ns = blk.last_outgoing_ns.load(std::memory_order_acquire);
        v.main_pid = blk.main_pid.load(std::memory_order_acquire);

        // Ordered after every acquire load above, so observing the same even
        // generation proves all of them came from it.
        if (blk.seq.load(std::memory_order_relaxed) == s1) {
            out = v;
            return true;
        }
    }
    return false;
}

// Structural validity of an already-obtained snapshot. Split from
// shm_read_snapshot() so a caller can distinguish "could not read a stable
// generation" from "read it fine, but it isn't our block".
inline bool shm_snapshot_valid(const ShmHeartbeatSnapshot& s) noexcept {
    return s.magic == kShmMagic && s.version == kShmVersion;
}

// Convenience: read + structural check in one call.
inline bool shm_verify(const ShmControlBlock& blk) noexcept {
    ShmHeartbeatSnapshot s{};
    return shm_read_snapshot(blk, s) && shm_snapshot_valid(s);
}

// Hot-path writer: zero-syscall heartbeat tick.
// Owns no resources — the ShmControlBlock is mmap'd by the caller.
//
// SINGLE WRITER. The seqlock below is a single-producer protocol: two concurrent
// writers would interleave their odd/even transitions and hand readers a
// generation that never existed. The trading process's hot thread is the only
// writer; the watchdog only ever writes kill_armed, which is deliberately OUTSIDE
// the seqlock (see arm_kill()).
class ShmHeartbeatWriter {
public:
    explicit ShmHeartbeatWriter(ShmControlBlock* blk) noexcept : blk_(blk) {}

    // Call once at startup to initialize the control block.
    void init(std::uint64_t pid) noexcept {
        if (!blk_) return;
        begin_write();
        blk_->magic.store(kShmMagic, std::memory_order_relaxed);
        blk_->version.store(kShmVersion, std::memory_order_relaxed);
        blk_->kill_armed.store(0, std::memory_order_relaxed);
        blk_->loop_counter.store(0, std::memory_order_relaxed);
        blk_->last_incoming_ns.store(0, std::memory_order_relaxed);
        blk_->last_outgoing_ns.store(0, std::memory_order_relaxed);
        blk_->main_pid.store(pid, std::memory_order_relaxed);
        std::memset(blk_->_pad, 0, sizeof(blk_->_pad));
        end_write();
    }

    // Tick the loop counter. Called every run_once() on the hot path.
    // Three relaxed atomic operations, no CRC, no fence on x86/ARM's usual
    // lowering of relaxed — see this file's PERF-SHM-004 note.
    void tick() noexcept {
        if (!blk_) return;
        begin_write();
        blk_->loop_counter.store(blk_->loop_counter.load(std::memory_order_relaxed) + 1,
                                  std::memory_order_relaxed);
        end_write();
    }

    // Update last market data receive timestamp.
    void set_incoming(std::uint64_t ts_ns) noexcept {
        if (!blk_) return;
        begin_write();
        blk_->last_incoming_ns.store(ts_ns, std::memory_order_relaxed);
        end_write();
    }

    // Update last fill execution timestamp.
    void set_outgoing(std::uint64_t ts_ns) noexcept {
        if (!blk_) return;
        begin_write();
        blk_->last_outgoing_ns.store(ts_ns, std::memory_order_relaxed);
        end_write();
    }

    // Check if the watchdog has requested a kill (read by main process).
    //
    // ACQUIRE, not relaxed, and deliberately outside the seqlock: this is the
    // fail-closed kill path, the one place where a missed observation is a safety
    // failure rather than a stale gauge. It pairs with arm_kill()'s release store
    // from the watchdog process, giving the edge that plain `kill_armed != 0`
    // never had. Reading it outside the seqlock also means a stuck writer (odd
    // seq) can never mask a pending kill request.
    bool kill_requested() const noexcept {
        if (!blk_) return false;
        return blk_->kill_armed.load(std::memory_order_acquire) != 0;
    }

    const ShmControlBlock* block() const noexcept { return blk_; }

private:
    // seq goes even -> odd (write in progress) -> even+2 (stable again).
    // Release on both ends so a reader that sees the new even value also sees
    // every payload store made between them.
    void begin_write() noexcept {
        blk_->seq.store(blk_->seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
    void end_write() noexcept {
        blk_->seq.store(blk_->seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    ShmControlBlock* blk_;
};

// Watchdog-side kill request. Outside the seqlock on purpose (see
// kill_requested()): release-store pairs with the trading process's acquire-load.
inline void shm_arm_kill(ShmControlBlock& blk) noexcept {
    blk.kill_armed.store(1, std::memory_order_release);
}

// ---- POSIX shared memory helpers (Linux only) ----

#ifdef __linux__

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// RAII wrapper for POSIX shared memory segment.
class ShmSegment {
public:
    ShmSegment() = default;
    ~ShmSegment() { close(); }
    ShmSegment(const ShmSegment&) = delete;
    ShmSegment& operator=(const ShmSegment&) = delete;

    // Create or open a shared memory segment. Returns the mapped pointer.
    // owner=true: creates + truncates. owner=false: opens existing.
    ShmControlBlock* open(const char* name, bool owner) noexcept {
        close();
        name_ = name;
        owner_ = owner;

        int flags = owner ? (O_CREAT | O_RDWR) : O_RDWR;
        fd_ = shm_open(name, flags, 0600);
        if (fd_ < 0) return nullptr;

        if (owner) {
            if (ftruncate(fd_, static_cast<off_t>(sizeof(ShmControlBlock))) != 0) {
                ::close(fd_);
                fd_ = -1;
                shm_unlink(name);
                return nullptr;
            }
        }

        void* ptr = mmap(nullptr, sizeof(ShmControlBlock),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (ptr == MAP_FAILED) {
            ::close(fd_);
            fd_ = -1;
            if (owner) shm_unlink(name);
            return nullptr;
        }

        blk_ = static_cast<ShmControlBlock*>(ptr);
        return blk_;
    }

    void close() noexcept {
        if (blk_) {
            munmap(blk_, sizeof(ShmControlBlock));
            blk_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (owner_ && !name_.empty()) {
            shm_unlink(name_.c_str());
            name_.clear();
        }
    }

    ShmControlBlock* block() noexcept { return blk_; }
    bool is_open() const noexcept { return blk_ != nullptr; }

private:
    int fd_{-1};
    ShmControlBlock* blk_{nullptr};
    std::string name_;
    bool owner_{false};
};

#endif  // __linux__

}  // namespace hy
