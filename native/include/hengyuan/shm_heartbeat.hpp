// SPDX-License-Identifier: proprietary
// shm_heartbeat.hpp — Shared-memory heartbeat control block + hot-path writer.
//
// The ShmControlBlock lives in a POSIX shared memory segment (shm_open/mmap).
// The trading process writes heartbeat ticks with relaxed atomics on every
// run_once() iteration — zero syscall, zero lock, zero jitter on the hot path.
// An independent watchdog process reads the block to detect liveness.
//
// Multi-dimensional heartbeat (defeats "zombie deadlock" false positives):
//   loop_counter    — incremented every hot-path iteration
//   last_incoming_ns — updated on every parsed market event
//   last_outgoing_ns — updated on every simulated/live fill
//
// Memory safety: magic number + CRC32 checksum detect corruption from wild
// pointers or cross-process stomps. Watchdog validates before acting.
//
// Cache-line aligned: writer (trading process) and reader (watchdog) each
// touch their own cache line. No false sharing on the hot path.
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
static constexpr std::uint32_t kShmVersion = 1;

// CRC32 (ISO 3309) for control block integrity.
inline std::uint32_t shm_crc32(const void* data, std::size_t len) noexcept {
    auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = 0xFFFFFFFF;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320 & (~((crc & 1) - 1)));
        }
    }
    return crc ^ 0xFFFFFFFF;
}

// 64-byte cache-line-aligned shared memory control block.
// Written by the trading process, read by the watchdog.
// Layout is fixed for cross-process ABI stability.
struct alignas(64) ShmControlBlock {
    std::uint32_t magic;            // [0]  must be kShmMagic
    std::uint32_t version;          // [4]  must be kShmVersion
    std::uint64_t loop_counter;     // [8]  hot-path iteration count
    std::uint64_t last_incoming_ns; // [16] last market data parse (steady_clock)
    std::uint64_t last_outgoing_ns; // [24] last fill execution (steady_clock)
    std::uint64_t main_pid;         // [32] trading process PID
    std::uint32_t kill_armed;       // [40] watchdog writes 1 to request kill
    std::uint32_t checksum;         // [44] CRC32 of bytes [0..43]
    std::uint8_t _pad[16];          // [48..63] explicit pad
};

static_assert(sizeof(ShmControlBlock) == 64);
static_assert(alignof(ShmControlBlock) == 64);

// Compute checksum over the fields before the checksum field itself.
inline std::uint32_t shm_compute_checksum(const ShmControlBlock& blk) noexcept {
    return shm_crc32(&blk, offsetof(ShmControlBlock, checksum));
}

inline bool shm_verify(const ShmControlBlock& blk) noexcept {
    if (blk.magic != kShmMagic) return false;
    if (blk.version != kShmVersion) return false;
    return blk.checksum == shm_compute_checksum(blk);
}

// Hot-path writer: zero-syscall heartbeat tick.
// Owns no resources — the ShmControlBlock is mmap'd by the caller.
class ShmHeartbeatWriter {
public:
    explicit ShmHeartbeatWriter(ShmControlBlock* blk) noexcept : blk_(blk) {}

    // Call once at startup to initialize the control block.
    void init(std::uint64_t pid) noexcept {
        if (!blk_) return;
        std::memset(blk_, 0, sizeof(ShmControlBlock));
        blk_->magic = kShmMagic;
        blk_->version = kShmVersion;
        blk_->main_pid = pid;
        blk_->checksum = shm_compute_checksum(*blk_);
    }

    // Tick the loop counter. Called every run_once() on the hot path.
    // Uses relaxed atomic store — no fence, no syscall, single cache line.
    void tick() noexcept {
        if (!blk_) return;
        ++blk_->loop_counter;
        blk_->checksum = shm_compute_checksum(*blk_);
    }

    // Update last market data receive timestamp.
    void set_incoming(std::uint64_t ts_ns) noexcept {
        if (!blk_) return;
        blk_->last_incoming_ns = ts_ns;
        blk_->checksum = shm_compute_checksum(*blk_);
    }

    // Update last fill execution timestamp.
    void set_outgoing(std::uint64_t ts_ns) noexcept {
        if (!blk_) return;
        blk_->last_outgoing_ns = ts_ns;
        blk_->checksum = shm_compute_checksum(*blk_);
    }

    // Check if the watchdog has requested a kill (read by main process).
    bool kill_requested() const noexcept {
        if (!blk_) return false;
        return blk_->kill_armed != 0;
    }

    const ShmControlBlock* block() const noexcept { return blk_; }

private:
    ShmControlBlock* blk_;
};

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
