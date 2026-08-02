// SPDX-License-Identifier: proprietary
// durable_audit_sink.hpp — real, file-backed durable audit log.
//
// Governance: L2 (real file I/O). Closes the gap order_tracker.hpp's own file
// header names explicitly: InFlightRegistry/OrderTracker are pure in-memory
// state, so a process crash loses all in-flight order tracking. This file is
// the minimal closed-loop slice that fixes that -- see docs/SPEC_INVARIANTS.md's
// "durable 审计日志" entry for the full scope decision (what's in this round,
// what's deliberately deferred, and why).
//
// SCOPE (repeating the ledger's summary at the point of use, since this is
// the file those decisions constrain):
//   - Real append_durable() + fsync, spec-defined frame format (durable_frame_codec.hpp).
//   - Local tip-anchor file, atomically replaced -- NOT the external anchor
//     network service (ExternalAnchorClient, out of scope, mock-only seam via
//     ExportOutboxRing).
//   - recovery_scan() rebuilds OrderRecoveryCheckpoint entries for every
//     non-exchange-final order; caller repopulates InFlightRegistry from that.
//   - Single writer per process, enforced by an OS-level exclusive lock on a
//     sidecar file; any Failed append permanently fences this instance for
//     its remaining lifetime (formal/durable_log_recovery.tla's
//     FencedWriterMakesNoNewWrites, now given a real code analog).
//   - Only DurableRecordType::OrderEvent frames. No compaction, no L4-owned
//     record types, no group-commit batching, no key rotation (key_id fixed
//     to 0 -- construction takes a pre-derived raw key).
//
// THREAD OWNERSHIP: like InFlightRegistry/AuditRingSink, this class is meant
// for single-writer use from one owner thread. No internal synchronization.

#pragma once

#include <hengyuan/durable_control_plane.hpp>
#include <hengyuan/durable_frame_codec.hpp>
#include <hengyuan/order_lifecycle.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>  // flock() -- the BSD advisory-lock function, distinct
                        // from <fcntl.h>'s POSIX `struct flock` record-lock
                        // type of the same name. Omitting this header makes
                        // GCC resolve `flock(...)` as a struct-flock
                        // aggregate-init instead of the function call (a real,
                        // GCC-only compile failure -- MSVC has no such
                        // ambiguity since this whole branch is POSIX-only).
#include <unistd.h>
#endif

namespace hy {

// --- Local tip-anchor wire format (implementation-internal, NOT spec-pinned
// byte-for-byte -- the spec describes the anchor's ROLE, not its exact local
// on-disk layout; see durable_frame_codec.hpp's own header comment for the
// same kind of flagged, deliberate width choice on payload_length) ---
//
//   [format_version: u8][sequence_number: u64 LE][mac: 32 bytes]
//   [key_id: u32 LE][anchor_mac: 32 bytes =
//       HMAC-SHA256(format_version||sequence_number||mac||key_id)]
//
// kTipAnchorFormatVersion is INDEPENDENT of durable_frame_codec.hpp's
// kFrameFormatVersion (Phase 0, 轨道 key-rotation substrate) -- it used to
// silently reuse that same constant as its own version byte, which meant a
// future frame-format-only change would have forced an unrelated tip-anchor
// rewrite too. The tip-anchor's own byte layout has not changed shape here
// (kTipAnchorSize is unchanged, key_id's slot already existed) -- only
// encode/decode's HANDLING of key_id changes, from hardcoded-0 to real.
inline constexpr std::uint8_t kTipAnchorFormatVersion = 1;
inline constexpr std::size_t kTipAnchorSize = 1 + 8 + kMacLen + 4 + kMacLen;  // 77 bytes

namespace detail {

inline std::size_t encode_tip_anchor(std::span<std::byte, kTipAnchorSize> out,
                                      std::uint64_t sequence_number,
                                      std::span<const std::byte, kMacLen> mac,
                                      std::uint32_t key_id,
                                      std::span<const std::byte> hmac_key) noexcept {
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    write_u8(p, kTipAnchorFormatVersion);
    write_u64_le(p, sequence_number);
    write_bytes(p, mac.data(), kMacLen);
    write_u32_le(p, key_id);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto anchor_mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    write_bytes(p, anchor_mac.bytes.data(), kMacLen);
    return kTipAnchorSize;
}

struct DecodedTipAnchor {
    std::uint64_t sequence_number{0};
    std::array<std::byte, kMacLen> mac{};
    std::uint32_t key_id{0};
};

// false = anchor is absent-shaped/malformed/tampered -- caller decides what
// that means (IoError vs Corrupt depends on whether the log is empty).
inline bool decode_tip_anchor(std::span<const std::byte> in, std::span<const std::byte> hmac_key,
                               DecodedTipAnchor& out) noexcept {
    if (in.size() != kTipAnchorSize) return false;
    const std::byte* p = in.data();
    const std::byte* const content_start = p;
    if (read_u8(p) != kTipAnchorFormatVersion) return false;
    const std::uint64_t seq = read_u64_le(p);
    std::array<std::byte, kMacLen> mac{};
    read_bytes(p, mac.data(), kMacLen);
    const std::uint32_t key_id = read_u32_le(p);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    if (std::memcmp(expected.bytes.data(), in.data() + content_len, kMacLen) != 0) return false;
    out.sequence_number = seq;
    out.mac = mac;
    out.key_id = key_id;
    return true;
}

}  // namespace detail

class DurableAuditSink {
public:
    // path: base path for the log (e.g. "state/audit.log"); the lock sidecar
    // and tip-anchor files are derived from it (path+".lock", path+".tip").
    // hmac_key: pre-derived raw key material, copied and retained for the
    // lifetime of this instance (no rotation this round -- see class header).
    DurableAuditSink(const std::string& path, std::span<const std::byte> hmac_key) noexcept
        : log_path_(path), lock_path_(path + ".lock"), tip_path_(path + ".tip") {
        const std::size_t key_len = hmac_key.size() < kKeyBlockSize ? hmac_key.size() : kKeyBlockSize;
        // Pre-derive the block-sized key once (HMAC's own "hash if > block
        // size, else zero-pad" rule) so every append_durable() call doesn't
        // redo that work for a long key -- HmacSha256's constructor applied
        // to an already-block-sized key just zero-pads trivially, so passing
        // key_block_ into hmac_sha256() on every call is equivalent to
        // passing the raw key every time, just cheaper for a long key.
        if (hmac_key.size() > crypto::HmacSha256::kBlockSize) {
            const auto hashed = crypto::sha256(hmac_key);
            std::memcpy(key_block_.data(), hashed.bytes.data(), hashed.bytes.size());
        } else {
            std::memcpy(key_block_.data(), hmac_key.data(), key_len);
        }

        if (!acquire_lock()) {
            recovery_status_ = RecoveryScanStatus::IoError;
            return;
        }
        if (!open_log()) {
            recovery_status_ = RecoveryScanStatus::IoError;
            release_lock();
            return;
        }

        recovery_status_ = run_recovery_scan();
        if (recovery_status_ != RecoveryScanStatus::Clean &&
            recovery_status_ != RecoveryScanStatus::Recovered) {
            // Fail closed: do not allow appends against a log we couldn't
            // cleanly account for. Keep the file handles open only so
            // is_open()/recovery_status() can report on them; append_durable()
            // itself checks fenced_.
            fenced_ = true;
        }
    }

    ~DurableAuditSink() {
        close_log();
        release_lock();
    }

    DurableAuditSink(const DurableAuditSink&) = delete;
    DurableAuditSink& operator=(const DurableAuditSink&) = delete;
    DurableAuditSink(DurableAuditSink&&) = delete;
    DurableAuditSink& operator=(DurableAuditSink&&) = delete;

    // True iff the lock+log were successfully acquired/opened -- independent
    // of whether recovery_status() came back Clean/Recovered. A caller must
    // check BOTH is_open() and recovery_status() before trusting this sink;
    // append_durable() itself refuses once fenced() regardless.
    bool is_open() const noexcept { return log_open_; }
    bool fenced() const noexcept { return fenced_; }
    RecoveryScanStatus recovery_status() const noexcept { return recovery_status_; }

    // Every non-exchange-final order recovery_scan() found, in scan order.
    // Fixed capacity matching InFlightRegistry's own kMaxInFlight -- if more
    // than that were found, recovery_status() is CapacityExceeded and this
    // array's first kMaxInFlight entries are populated (best-effort, but the
    // caller must treat CapacityExceeded as fail-closed regardless per the
    // same "L5 does not start until an operator resolves this" rule as every
    // other RecoveryScanStatus failure).
    std::span<const OrderRecoveryCheckpoint> recovered_checkpoints() const noexcept {
        return std::span<const OrderRecoveryCheckpoint>(checkpoints_.data(), checkpoint_count_);
    }

    // Appends one OrderEvent frame: encode -> write -> fsync log -> write tip
    // anchor -> fsync anchor -> Acked. Any failure at any step permanently
    // fences this instance (formal/durable_log_recovery.tla's
    // FencedWriterMakesNoNewWrites) -- never partially succeeds, never
    // auto-retries. now_ms is caller-supplied (this codebase's established
    // clock-injection style, not read from a global clock here).
    AuditAppendResult append_durable(const AuditRecord& rec, std::int64_t now_ms) noexcept {
        AuditAppendResult result{};
        if (fenced_ || !log_open_) {
            result.status = AuditAppendResult::Status::Failed;
            return result;
        }

        std::array<std::byte, kOrderEventFrameSize> buf{};
        // key_id stays the fixed 0 this class has always used (class header's
        // own "no key rotation this round" scope note) -- durable_frame_codec.hpp
        // gained a real key_id field in its v4 bump (Phase 0, 轨道 key-rotation
        // substrate), but wiring THIS class to a KeyRing for real per-frame
        // key selection is Phase 2's job, not this one. This literal 0 is a
        // compile-compatibility placeholder, not a functional change.
        const auto n = encode_order_event_frame(buf, /*key_id=*/0u, next_sequence_,
                                                 FrameTimeKind::ServerCorrectedUtc,
                                                 now_ms, rec, tip_mac_, key_span());
        if (n != kOrderEventFrameSize) {
            fenced_ = true;
            result.status = AuditAppendResult::Status::Failed;
            return result;
        }

        if (!append_bytes_to_log(buf.data(), buf.size())) {
            fenced_ = true;
            result.status = AuditAppendResult::Status::Failed;
            return result;
        }

        std::array<std::byte, kMacLen> this_mac{};
        std::memcpy(this_mac.data(), buf.data() + kOrderEventFrameSize - kMacLen, kMacLen);

        if (!write_tip_anchor(next_sequence_, this_mac)) {
            fenced_ = true;
            result.status = AuditAppendResult::Status::Failed;
            return result;
        }

        result.status = AuditAppendResult::Status::Acked;
        result.sequence = next_sequence_;
        tip_mac_ = this_mac;
        ++next_sequence_;

        if (export_outbox_) {
            // Best-effort: a full ring (no consumer exists yet this round)
            // must never fail or fence the local append -- local durability
            // is the correctness-critical part; see class header + this
            // round's SPEC_INVARIANTS.md entry.
            ExportTuple t{};
            t.generation = 0;
            t.sequence = result.sequence;
            std::memcpy(t.tip_mac.data(), this_mac.data(), t.tip_mac.size());
            t.key_id = 0;
            t.enqueued_utc_ms = now_ms;
            t.time_kind = static_cast<std::uint8_t>(FrameTimeKind::ServerCorrectedUtc);
            (void)export_outbox_->try_push(t);
        }

        return result;
    }

    // Optional: wire an ExportOutboxRing to receive a tip tuple on every
    // successful Acked append (see class header's ExportOutboxRing note).
    // Nullable; unset by default. Not owned.
    void set_export_outbox(ExportOutboxRing* ring) noexcept { export_outbox_ = ring; }

private:
    static constexpr std::size_t kKeyBlockSize = crypto::HmacSha256::kBlockSize;

    std::span<const std::byte> key_span() const noexcept {
        return std::span<const std::byte>(key_block_.data(), key_block_.size());
    }

    // --- Per-COID replay state, scan-local only (never persisted as
    // instance state beyond the final checkpoints_ output) ---
    struct ReplayState {
        bool established{false};
        OrderState state{OrderState::Intent};
        std::int64_t intended_price_ticks{0};
        std::int64_t intended_qty_ticks{0};
        std::int64_t filled_qty_ticks{0};
        std::int64_t avg_fill_price_ticks{0};
        std::uint32_t symbol_id{0};
        std::int64_t exchange_order_id{0};
    };

    RecoveryScanStatus run_recovery_scan() noexcept {
        std::vector<std::byte> content;
        if (!read_entire_log(content)) return RecoveryScanStatus::IoError;

        if (content.empty()) {
            return finalize_scan_with_anchor_check(/*log_tip_valid=*/false, 0, {}, {});
        }

        std::unordered_map<std::string, ReplayState> per_coid;
        std::vector<std::array<std::byte, kMacLen>> macs_by_sequence;

        std::size_t offset = 0;
        std::uint64_t expected_seq = 0;
        std::array<std::byte, kMacLen> running_prev_mac{};  // zero at seq 0

        while (offset < content.size()) {
            std::span<const std::byte> remaining(content.data() + offset, content.size() - offset);
            DecodedOrderFrame frame{};
            std::size_t frame_size = 0;
            const auto status = decode_order_event_frame(remaining, key_span(), frame, frame_size);

            if (status == FrameDecodeStatus::Truncated) {
                // Only legal at the physical end -- discard just this tail
                // and stop scanning (torn write).
                break;
            }
            if (status != FrameDecodeStatus::Ok) {
                return RecoveryScanStatus::Corrupt;
            }
            if (frame.sequence_number != expected_seq) return RecoveryScanStatus::Corrupt;
            if (frame.prev_mac != running_prev_mac) return RecoveryScanStatus::Corrupt;
            if (frame.record_type != DurableRecordType::OrderEvent) {
                // Nothing this build ever writes any other record type this
                // round -- seeing one means a future-format log or tampering.
                return RecoveryScanStatus::Corrupt;
            }

            std::string coid(frame.record.client_order_id);
            auto& rs = per_coid[coid];
            if (!rs.established) {
                if (frame.record.resulting_state != OrderState::Intent) return RecoveryScanStatus::Corrupt;
                rs.established = true;
                rs.state = OrderState::Intent;
                rs.intended_price_ticks = frame.record.price_ticks;
                rs.intended_qty_ticks = frame.record.qty_ticks;
                rs.symbol_id = frame.record.symbol_id;
            } else if (frame.record.resulting_state == rs.state) {
                if (frame.record.filled_qty_ticks < rs.filled_qty_ticks) return RecoveryScanStatus::Corrupt;
                if (frame.record.filled_qty_ticks > rs.intended_qty_ticks) return RecoveryScanStatus::Corrupt;
            } else {
                if (validate_transition(rs.state, frame.record.resulting_state) != TransitionResult::Ok) {
                    return RecoveryScanStatus::Corrupt;
                }
                rs.state = frame.record.resulting_state;
            }
            if (frame.record.filled_qty_ticks > rs.filled_qty_ticks) {
                rs.filled_qty_ticks = frame.record.filled_qty_ticks;
                rs.avg_fill_price_ticks = frame.record.avg_fill_price_ticks;
            }
            if (frame.record.exchange_order_id != 0) rs.exchange_order_id = frame.record.exchange_order_id;

            macs_by_sequence.push_back(frame.mac);
            running_prev_mac = frame.mac;
            ++expected_seq;
            offset += frame_size;
        }

        if (expected_seq == 0) {
            // Every frame present was a torn tail at sequence 0 -- same as
            // an empty log for anchor-checking purposes.
            return finalize_scan_with_anchor_check(/*log_tip_valid=*/false, 0, {}, {});
        }

        auto status = finalize_scan_with_anchor_check(/*log_tip_valid=*/true, expected_seq - 1,
                                                        macs_by_sequence.back(), macs_by_sequence);
        if (status != RecoveryScanStatus::Clean && status != RecoveryScanStatus::Recovered) return status;

        next_sequence_ = expected_seq;
        tip_mac_ = macs_by_sequence.back();

        for (const auto& [coid, rs] : per_coid) {
            if (is_exchange_final(rs.state)) continue;  // nothing to recover
            if (checkpoint_count_ >= checkpoints_.size()) return RecoveryScanStatus::CapacityExceeded;

            OrderRecoveryCheckpoint cp{};
            std::strncpy(cp.client_order_id.id, coid.c_str(), kClientOrderIdLen);
            // Submitting -> Ambiguous remap (SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md
            // 1096-1109): a recovered "last durable event was Submitting, no
            // outcome ever landed" order means genuinely uncertain, not safely
            // still-in-progress.
            cp.resulting_state = (rs.state == OrderState::Submitting) ? OrderState::Ambiguous : rs.state;
            cp.intended_price_ticks = rs.intended_price_ticks;
            cp.intended_qty_ticks = rs.intended_qty_ticks;
            cp.filled_qty_ticks = rs.filled_qty_ticks;
            cp.avg_fill_price_ticks = rs.avg_fill_price_ticks;
            cp.symbol_id = rs.symbol_id;
            cp.exchange_order_id = rs.exchange_order_id;
            checkpoints_[checkpoint_count_++] = cp;
        }

        return checkpoint_count_ > 0 ? RecoveryScanStatus::Recovered : RecoveryScanStatus::Clean;
    }

    // Tip-anchor cross-check (docs/SPEC_INVARIANTS.md / plan §5.2). Returns
    // Clean/Recovered to mean "anchor check passed, caller may proceed to
    // finish assembling checkpoints" -- the actual Clean-vs-Recovered
    // distinction for a non-empty log is decided by the caller based on
    // whether any checkpoints end up non-empty; this function only ever
    // returns Clean (for the "anchor OK, nothing more to say here" case) or
    // an error status.
    RecoveryScanStatus finalize_scan_with_anchor_check(
        bool log_tip_valid, std::uint64_t log_tip_sequence, std::array<std::byte, kMacLen> log_tip_mac,
        const std::vector<std::array<std::byte, kMacLen>>& macs_by_sequence) noexcept {
        std::vector<std::byte> anchor_bytes;
        const bool anchor_present = read_tip_anchor(anchor_bytes);

        if (!anchor_present) {
            return log_tip_valid ? RecoveryScanStatus::IoError : RecoveryScanStatus::Clean;
        }
        if (!log_tip_valid) {
            return RecoveryScanStatus::Corrupt;  // anchor claims a tip; log has none
        }

        detail::DecodedTipAnchor anchor{};
        if (!detail::decode_tip_anchor(anchor_bytes, key_span(), anchor)) return RecoveryScanStatus::Corrupt;

        if (anchor.sequence_number > log_tip_sequence) return RecoveryScanStatus::Corrupt;  // tail deletion
        if (anchor.sequence_number == log_tip_sequence) {
            return anchor.mac == log_tip_mac ? RecoveryScanStatus::Clean : RecoveryScanStatus::Corrupt;
        }
        // anchor.sequence_number < log_tip_sequence: the late-fsync window
        // (crashed between "fsync log" and "fsync anchor"). Acceptable only
        // if the log's own frame at that sequence number -- already proven
        // continuous from index 0 through the true tip by the chain check in
        // the main scan loop -- has a mac matching what the anchor claims.
        if (anchor.sequence_number >= macs_by_sequence.size()) return RecoveryScanStatus::Corrupt;
        return macs_by_sequence[anchor.sequence_number] == anchor.mac ? RecoveryScanStatus::Clean
                                                                       : RecoveryScanStatus::Corrupt;
    }

    // --- Platform I/O ---
#if defined(_WIN32)
    bool acquire_lock() noexcept {
        // No-sharing CreateFileA is itself an OS-enforced exclusive lock: a
        // second handle opened anywhere for this path fails with a sharing
        // violation. Same idiom env_loader.hpp already uses for anti-symlink
        // opens, reused here for mutual exclusion instead.
        lock_handle_ = CreateFileA(lock_path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return lock_handle_ != INVALID_HANDLE_VALUE;
    }
    void release_lock() noexcept {
        if (lock_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(lock_handle_);
            lock_handle_ = INVALID_HANDLE_VALUE;
        }
    }
    bool open_log() noexcept {
        log_handle_ = CreateFileA(log_path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        log_open_ = log_handle_ != INVALID_HANDLE_VALUE;
        return log_open_;
    }
    void close_log() noexcept {
        if (log_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(log_handle_);
            log_handle_ = INVALID_HANDLE_VALUE;
        }
        log_open_ = false;
    }
    bool read_entire_log(std::vector<std::byte>& out) noexcept {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(log_handle_, &size)) return false;
        out.resize(static_cast<std::size_t>(size.QuadPart));
        if (out.empty()) return true;
        if (SetFilePointer(log_handle_, 0, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) return false;
        DWORD read_bytes = 0;
        BOOL ok = ReadFile(log_handle_, out.data(), static_cast<DWORD>(out.size()), &read_bytes, nullptr);
        return ok && static_cast<std::size_t>(read_bytes) == out.size();
    }
    bool append_bytes_to_log(const std::byte* data, std::size_t n) noexcept {
        if (SetFilePointer(log_handle_, 0, nullptr, FILE_END) == INVALID_SET_FILE_POINTER) return false;
        DWORD written = 0;
        if (!WriteFile(log_handle_, data, static_cast<DWORD>(n), &written, nullptr)) return false;
        if (static_cast<std::size_t>(written) != n) return false;
        return FlushFileBuffers(log_handle_) != 0;
    }
    bool write_tip_anchor(std::uint64_t sequence_number, std::array<std::byte, kMacLen> mac) noexcept {
        std::array<std::byte, kTipAnchorSize> buf{};
        // key_id stays 0 here for the same reason append_durable()'s frame
        // write does -- this tip-anchor's key_id must match whatever key_id
        // actually signed the corresponding log frame, and that's hardcoded
        // to 0 until Phase 2 wires this class to a real KeyRing.
        detail::encode_tip_anchor(buf, sequence_number, mac, /*key_id=*/0u, key_span());

        const std::string tmp_path = tip_path_ + ".tmp";
        HANDLE h = CreateFileA(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        BOOL ok = WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr);
        ok = ok && (static_cast<std::size_t>(written) == buf.size());
        ok = ok && FlushFileBuffers(h);
        CloseHandle(h);
        if (!ok) return false;

        // MOVEFILE_WRITE_THROUGH: NTFS + FlushFileBuffers on the file handle
        // above is the honest contract here -- this is NOT POSIX directory-
        // fsync equivalence (no parent-directory metadata flush on Windows in
        // this implementation). Crash-consistency under this exact procedure
        // is what's claimed, not power-loss equivalence to the POSIX path.
        return MoveFileExA(tmp_path.c_str(), tip_path_.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    bool read_tip_anchor(std::vector<std::byte>& out) noexcept {
        HANDLE h = CreateFileA(tip_path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(h, &size)) {
            CloseHandle(h);
            return false;
        }
        out.resize(static_cast<std::size_t>(size.QuadPart));
        bool ok = true;
        if (!out.empty()) {
            DWORD read_bytes = 0;
            ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read_bytes, nullptr) &&
                 static_cast<std::size_t>(read_bytes) == out.size();
        }
        CloseHandle(h);
        return ok;
    }

    HANDLE lock_handle_{INVALID_HANDLE_VALUE};
    HANDLE log_handle_{INVALID_HANDLE_VALUE};
#else
    bool acquire_lock() noexcept {
        lock_fd_ = ::open(lock_path_.c_str(), O_CREAT | O_RDWR, 0600);
        if (lock_fd_ < 0) return false;
        if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
            ::close(lock_fd_);
            lock_fd_ = -1;
            return false;
        }
        return true;
    }
    void release_lock() noexcept {
        if (lock_fd_ >= 0) {
            ::flock(lock_fd_, LOCK_UN);
            ::close(lock_fd_);
            lock_fd_ = -1;
        }
    }
    bool open_log() noexcept {
        log_fd_ = ::open(log_path_.c_str(), O_CREAT | O_RDWR, 0600);
        log_open_ = log_fd_ >= 0;
        return log_open_;
    }
    void close_log() noexcept {
        if (log_fd_ >= 0) {
            ::close(log_fd_);
            log_fd_ = -1;
        }
        log_open_ = false;
    }
    bool read_entire_log(std::vector<std::byte>& out) noexcept {
        const off_t size = ::lseek(log_fd_, 0, SEEK_END);
        if (size < 0) return false;
        out.resize(static_cast<std::size_t>(size));
        if (out.empty()) return true;
        if (::lseek(log_fd_, 0, SEEK_SET) < 0) return false;
        std::size_t total = 0;
        while (total < out.size()) {
            const ssize_t n = ::read(log_fd_, out.data() + total, out.size() - total);
            if (n <= 0) return false;
            total += static_cast<std::size_t>(n);
        }
        return true;
    }
    bool append_bytes_to_log(const std::byte* data, std::size_t n) noexcept {
        if (::lseek(log_fd_, 0, SEEK_END) < 0) return false;
        std::size_t total = 0;
        while (total < n) {
            const ssize_t written = ::write(log_fd_, data + total, n - total);
            if (written <= 0) return false;
            total += static_cast<std::size_t>(written);
        }
        return ::fsync(log_fd_) == 0;
    }
    bool write_tip_anchor(std::uint64_t sequence_number, std::array<std::byte, kMacLen> mac) noexcept {
        std::array<std::byte, kTipAnchorSize> buf{};
        // key_id stays 0 here for the same reason append_durable()'s frame
        // write does -- this tip-anchor's key_id must match whatever key_id
        // actually signed the corresponding log frame, and that's hardcoded
        // to 0 until Phase 2 wires this class to a real KeyRing.
        detail::encode_tip_anchor(buf, sequence_number, mac, /*key_id=*/0u, key_span());

        const std::string tmp_path = tip_path_ + ".tmp";
        int fd = ::open(tmp_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd < 0) return false;
        std::size_t total = 0;
        bool ok = true;
        while (ok && total < buf.size()) {
            const ssize_t written = ::write(fd, buf.data() + total, buf.size() - total);
            if (written <= 0) {
                ok = false;
                break;
            }
            total += static_cast<std::size_t>(written);
        }
        ok = ok && (::fsync(fd) == 0);
        ::close(fd);
        if (!ok) return false;

        if (::rename(tmp_path.c_str(), tip_path_.c_str()) != 0) return false;

        // Directory-entry fsync: the rename itself needs the containing
        // directory's metadata flushed for crash-durability, not just the
        // file's own contents (SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md's own
        // atomic-replace contract, L4 §10.3).
        const auto slash = tip_path_.find_last_of('/');
        const std::string dir = (slash == std::string::npos) ? "." : tip_path_.substr(0, slash);
        int dir_fd = ::open(dir.c_str(), O_RDONLY);
        if (dir_fd < 0) return false;
        const bool dir_ok = (::fsync(dir_fd) == 0);
        ::close(dir_fd);
        return dir_ok;
    }
    bool read_tip_anchor(std::vector<std::byte>& out) noexcept {
        int fd = ::open(tip_path_.c_str(), O_RDONLY);
        if (fd < 0) return false;
        const off_t size = ::lseek(fd, 0, SEEK_END);
        if (size < 0) {
            ::close(fd);
            return false;
        }
        out.resize(static_cast<std::size_t>(size));
        bool ok = true;
        if (!out.empty()) {
            if (::lseek(fd, 0, SEEK_SET) < 0) {
                ok = false;
            } else {
                std::size_t total = 0;
                while (ok && total < out.size()) {
                    const ssize_t n = ::read(fd, out.data() + total, out.size() - total);
                    if (n <= 0) {
                        ok = false;
                        break;
                    }
                    total += static_cast<std::size_t>(n);
                }
            }
        }
        ::close(fd);
        return ok;
    }

    int lock_fd_{-1};
    int log_fd_{-1};
#endif

    std::string log_path_;
    std::string lock_path_;
    std::string tip_path_;
    std::array<std::byte, kKeyBlockSize> key_block_{};

    bool log_open_{false};
    bool fenced_{false};
    RecoveryScanStatus recovery_status_{RecoveryScanStatus::IoError};

    std::uint64_t next_sequence_{0};
    std::array<std::byte, kMacLen> tip_mac_{};

    std::array<OrderRecoveryCheckpoint, kMaxInFlight> checkpoints_{};
    std::size_t checkpoint_count_{0};

    ExportOutboxRing* export_outbox_{nullptr};
};

// --- Startup recovery integration ---
//
// Scope-limited per docs/SPEC_INVARIANTS.md's durable-audit-log entry: this
// reconstructs InFlightRegistry/OrderRecord state from a recovered
// DurableAuditSink at process startup. It does NOT wire into
// live_submit_orchestrator.hpp's orchestrate_submit() (Gates 6/8/9) -- that's
// a separate, larger, not-yet-scoped change to the hot submit path itself.

// Reconstructs the OrderRecord a recovered checkpoint represents. Directly
// assigns `state` rather than going through transition_to()/
// validate_transition() -- this is establishing bootstrap state after a
// restart, not a live transition from a running state machine, so the
// transition-legality gate doesn't apply here (the checkpoint's state was
// already proven reachable by recovery_scan()'s own per-COID replay, which
// DID run every intermediate step through validate_transition()).
inline OrderRecord checkpoint_to_order_record(const OrderRecoveryCheckpoint& cp) noexcept {
    OrderRecord rec{};
    rec.client_order_id = cp.client_order_id;
    rec.exchange_order_id = cp.exchange_order_id;
    rec.symbol_id = cp.symbol_id;
    rec.state = cp.resulting_state;
    rec.intended_price_ticks = cp.intended_price_ticks;
    rec.intended_qty_ticks = cp.intended_qty_ticks;
    rec.filled_qty_ticks = cp.filled_qty_ticks;
    rec.avg_fill_price_ticks = cp.avg_fill_price_ticks;
    return rec;
}

// Registers every recovered checkpoint's coid into `registry` (fresh at
// startup, so no duplicate/capacity conflicts are expected under correct
// wiring -- recovery_scan() itself already fails closed with
// CapacityExceeded before this would ever be called with more entries than
// kMaxInFlight). Returns the count actually registered; a return value less
// than checkpoints.size() means register_submit() refused an entry (e.g. a
// malformed/empty coid slipping through would be a recovery_scan() bug, not
// expected in practice) -- the caller should treat that as fail-closed
// (refuse to proceed) rather than silently continuing with partial recovery.
inline std::size_t repopulate_in_flight_registry(InFlightRegistry& registry,
                                                   std::span<const OrderRecoveryCheckpoint> checkpoints) noexcept {
    std::size_t registered = 0;
    for (const auto& cp : checkpoints) {
        if (registry.register_submit(std::string_view(cp.client_order_id.id))) ++registered;
    }
    return registered;
}

}  // namespace hy
