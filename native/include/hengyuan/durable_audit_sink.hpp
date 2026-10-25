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
//     record types, no group-commit batching.
//   - Reads order-event frames at format version 5 and 6 (D5 slice R1, docs/OPERATOR_RESOLUTION_DESIGN.md): v6 may
//     carry AuditEventType::OrderOperatorResolved, the offline operator tool's closure of an order nothing else
//     can close. The store's version never goes back; this class writes the version its own tip has (5 today).
//     See durable_frame_codec.hpp's FORMAT v5 -> v6 note and AuditSinkOpenMode below.
//   - Real KeyRing-backed key_id selection (Phase 2, docs/SPEC_INVARIANTS.md) --
//     every append does a live KeyRing::active_key(active_key_id_, ...) lookup
//     (never cached), and recovery resolves each frame's OWN key_id via
//     peek_frame_key_id() + a KeyRing lookup.
//   - Real runtime key rotation (Phase 4, docs/SPEC_INVARIANTS.md) --
//     rotate_active_key() safely switches active_key_id_ without a process
//     restart. Durable evidence of the rotation itself
//     (DurableRecordType::KeyRotated) goes to a SEPARATE sidecar log
//     (path+".keyrotations"), not this class's main OrderEvent log -- see
//     rotate_active_key()'s own doc comment for why.
//
// THREAD OWNERSHIP: like InFlightRegistry/AuditRingSink, this class is meant
// for single-writer use from one owner thread. No internal synchronization --
// rotate_active_key() is no exception, it must only ever be called from the
// same owning thread as append_durable()/run_recovery_scan().

#pragma once

#include <hengyuan/control_plane_frame_codec.hpp>
#include <hengyuan/durable_control_plane.hpp>
#include <hengyuan/durable_frame_codec.hpp>
#include <hengyuan/durable_log_store.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/operator_resolution_frame.hpp>
#include <hengyuan/order_lifecycle.hpp>
#include <hengyuan/position_truth.hpp>
#include <hengyuan/secure_wipe.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

// Platform-native, non-throwing CSPRNG for store-identity generation (see
// "Store identity" section below) -- deliberately NOT std::random_device,
// whose constructor may throw inside this file's noexcept methods.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <cerrno>
#include <sys/random.h>
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

// Reads ONLY the key_id field (offset 1+8+kMacLen = 41, 4 bytes LE -- past
// format_version+sequence_number+mac, before anchor_mac), without verifying
// anchor_mac or format_version. Same philosophy as durable_frame_codec.hpp's
// peek_frame_key_id() (Phase 2, docs/SPEC_INVARIANTS.md): callers use this to
// pick the right KeyRing key BEFORE calling decode_tip_anchor, which is what
// actually verifies the anchor. Returns false (out_key_id untouched) only
// because `in` is too short to contain the field -- a tip-anchor is always
// atomically replaced whole, so there is no legal "partial write survives on
// disk" case to be lenient about here (unlike a log frame's torn tail).
inline bool peek_tip_anchor_key_id(std::span<const std::byte> in, std::uint32_t& out_key_id) noexcept {
    constexpr std::size_t kKeyIdOffset = 1 + 8 + kMacLen;  // format_version, sequence_number, mac
    constexpr std::size_t kMinBytes = kKeyIdOffset + 4;
    if (in.size() < kMinBytes) return false;
    const std::byte* p = in.data() + kKeyIdOffset;
    out_key_id = read_u32_le(p);
    return true;
}

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
    // Constant-time (audit SEC-MACCMP-010), see crypto::constant_time_equal.
    if (!crypto::constant_time_equal(expected,
                                      std::span<const std::byte>(in.data() + content_len, kMacLen))) {
        return false;
    }
    out.sequence_number = seq;
    out.mac = mac;
    out.key_id = key_id;
    return true;
}

// The scan status once recovery has dealt with the bytes after the last complete frame (SAFE-03 crash-window slice,
// blueprint L-36). Clean = the log now ends on a frame boundary (nothing to cut, or the torn tail was cut and the new
// size is durable); IoError = the truncate failed, so the torn tail is still on disk and the next append would land
// behind it -- the caller fences; Corrupt = at least one whole frame of unexplained bytes, which no crash in the
// middle of ONE append can leave (they are kept, not cut). `dropped` receives the number of bytes cut.
inline RecoveryScanStatus cut_torn_tail(DurableLogStore& store, std::uint64_t valid_end, std::uint64_t file_size,
                                        std::uint64_t max_tail_bytes, std::uint64_t& dropped) noexcept {
    switch (discard_torn_tail(store, valid_end, file_size, max_tail_bytes, dropped)) {
        case TornTail::None:
        case TornTail::Dropped: return RecoveryScanStatus::Clean;
        case TornTail::TooLong: return RecoveryScanStatus::Corrupt;
        case TornTail::IoError: return RecoveryScanStatus::IoError;
    }
    return RecoveryScanStatus::IoError;
}

}  // namespace detail

// --- Store identity (implementation-internal, NOT spec-pinned) ---
//
// Phase 5 (docs/SPEC_INVARIANTS.md): a persistent, randomly-generated 128-bit
// label for "this specific store instance" -- established once on first
// construction, signed under key_ring_/active_key_id_ like every other
// structure in this class (NOT a raw KEK -- this class's constructor has
// never taken a KekLoader&, and adding one just for this would force a
// signature change onto an already-stable, three-phases-old constructor).
// Persisted via a third DurableLogStore sidecar, same pattern as the
// KeyRotated sidecar.
//
// Exists so ExportTuple.store_uuid_lo/hi (previously always 0 -- a real,
// pre-existing semantic gap this round closes) actually distinguishes one
// physical store from another to an external anchor. Deliberately
// independent of `generation` (ExportTuple.generation stays hardcoded 0 --
// that belongs to the compaction/generation-switch subsystem, a different,
// still out-of-scope problem).
//
//   [format_version: u8][store_uuid_lo: u64 LE][store_uuid_hi: u64 LE]
//   [key_id: u32 LE][mac: 32 bytes = HMAC(key_ring key for key_id, above)]
inline constexpr std::uint8_t kStoreIdentityFormatVersion = 1;
inline constexpr std::size_t kStoreIdentitySize = 1 + 8 + 8 + 4 + kMacLen;  // 53

namespace detail {

inline std::size_t encode_store_identity(std::span<std::byte, kStoreIdentitySize> out,
                                          std::uint64_t store_uuid_lo, std::uint64_t store_uuid_hi,
                                          std::uint32_t key_id, std::span<const std::byte> hmac_key) noexcept {
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    write_u8(p, kStoreIdentityFormatVersion);
    write_u64_le(p, store_uuid_lo);
    write_u64_le(p, store_uuid_hi);
    write_u32_le(p, key_id);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    write_bytes(p, mac.bytes.data(), kMacLen);
    return kStoreIdentitySize;
}

struct DecodedStoreIdentity {
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint32_t key_id{0};
};

// Same "peek the key_id before deciding which KeyRing key to verify with"
// philosophy as peek_tip_anchor_key_id()/peek_frame_key_id() -- a store
// identity file may have been signed under an earlier active_key_id_ than
// the one this process currently has active.
inline bool peek_store_identity_key_id(std::span<const std::byte> in, std::uint32_t& out_key_id) noexcept {
    constexpr std::size_t kKeyIdOffset = 1 + 8 + 8;  // format_version, store_uuid_lo, store_uuid_hi
    constexpr std::size_t kMinBytes = kKeyIdOffset + 4;
    if (in.size() < kMinBytes) return false;
    const std::byte* p = in.data() + kKeyIdOffset;
    out_key_id = read_u32_le(p);
    return true;
}

inline bool decode_store_identity(std::span<const std::byte> in, std::span<const std::byte> hmac_key,
                                   DecodedStoreIdentity& out) noexcept {
    if (in.size() != kStoreIdentitySize) return false;
    const std::byte* p = in.data();
    const std::byte* const content_start = p;
    if (read_u8(p) != kStoreIdentityFormatVersion) return false;
    DecodedStoreIdentity v{};
    v.store_uuid_lo = read_u64_le(p);
    v.store_uuid_hi = read_u64_le(p);
    v.key_id = read_u32_le(p);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    std::array<std::byte, kMacLen> mac{};
    read_bytes(p, mac.data(), kMacLen);
    if (!crypto::constant_time_equal(expected_mac, mac)) return false;  // audit SEC-MACCMP-010
    out = v;
    return true;
}

// Fills `out` with cryptographically-strong random bytes via the platform's
// native, non-throwing CSPRNG -- deliberately NOT std::random_device, whose
// constructor may throw std::exception on some platforms while every method
// in this class (and this free function itself) is noexcept; an exception
// escaping here would terminate the process. Same "raw platform API, never a
// throwing C++ standard-library facility" discipline kek_loader.hpp already
// established for KEK loading. Returns false (out left untouched) on any
// failure -- callers must treat that as "could not establish an identity"
// and degrade (see DurableAuditSink's store_identity_degraded_), never fall
// back to a weak/predictable/zero value.
inline bool fill_random_bytes(std::span<std::byte> out) noexcept {
#if defined(_WIN32)
    const NTSTATUS status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()),
                                             static_cast<ULONG>(out.size()),
                                             BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status == 0;  // STATUS_SUCCESS == 0
#elif defined(__linux__)
    std::size_t total = 0;
    while (total < out.size()) {
        const ssize_t n = ::getrandom(out.data() + total, out.size() - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        total += static_cast<std::size_t>(n);
    }
    return true;
#else
    return false;  // unsupported platform, fail-closed
#endif
}

}  // namespace detail

// TODO 1A.3 follow-up: WHY this sink fenced, without changing fenced()'s
// existing meaning or the unconditional-permanent-fence policy itself --
// still fences on any failure, including DiskFull; this only makes the
// reason diagnosable. See durable_log_store.hpp's IoWriteStatus for the same
// framing at the platform-I/O layer this is sourced from.
enum class FenceReason : std::uint8_t {
    None = 0,       // not fenced
    IoError = 1,    // encode failure, non-disk-full I/O failure, or a non-clean recovery scan
    DiskFull = 2,   // ENOSPC/EDQUOT (POSIX) or ERROR_DISK_FULL/ERROR_HANDLE_DISK_FULL (Windows)
};

// D5 / R1 (docs/OPERATOR_RESOLUTION_DESIGN.md 7.2 and 10): how the recovery scan treats the capacity bounds.
//
//   Trading        every start of the live process. The bounds are hard: more unresolved orders than
//                  kMaxInFlight, more Intent-only orphans than the budget, or more operator-resolved orders than
//                  kMaxOperatorResolved is RecoveryScanStatus::CapacityExceeded and the sink fences (the "L5 does
//                  not start until an operator resolves this" rule).
//   OperatorOpen   the OFFLINE operator tool only (slice R3). The same scan, but the three bounds COUNT instead of
//                  failing: the sink stays usable, so the tool can append the one frame that resolves an order even
//                  when more than kMaxInFlight orders are stuck (otherwise the store that most needs an operator
//                  could not be written to), and recovery_overflow() says what the lists do not hold. Corrupt and
//                  IoError fence exactly as before.
//
// The trading process must NEVER open a store this way: test_operator_resolution_guard.cpp pins that no source
// outside this header names OperatorOpen.
enum class AuditSinkOpenMode : std::uint8_t {
    Trading = 0,
    OperatorOpen = 1,
};

// What an OperatorOpen scan counted past the capacity of the lists above (all zero for a Trading open, which fails
// instead, and for an OperatorOpen scan that fit).
struct RecoveryOverflow {
    std::size_t checkpoints{0};        // unresolved orders beyond kMaxInFlight (their checkpoints are not populated)
    std::size_t intent_orphans{0};     // Intent-only COIDs beyond the orphan budget
    std::size_t operator_resolved{0};  // operator-resolved orders beyond kMaxOperatorResolved
    bool any() const noexcept { return checkpoints != 0 || intent_orphans != 0 || operator_resolved != 0; }
};

class DurableAuditSink {
public:
    // path: base path for the log (e.g. "state/audit.log"); the lock sidecar
    // and tip-anchor files are derived from it (path+".lock", path+".tip").
    // key_ring: NOT owned, must outlive this sink. active_key_id: fixed for
    // this instance's lifetime -- see class header's KeyRing note (Phase 2).
    // max_intent_orphans (SAFE-03 slice 2): how many Intent-only COIDs recovery tolerates before it reports
    // CapacityExceeded (durable_control_plane.hpp's kMaxIntentOrphans). It can only TIGHTEN the budget -- a larger
    // value is clamped to kMaxIntentOrphans -- so a test can reach the boundary without writing 4096 frames and
    // no caller can lift the bound. Production passes nothing.
    // open_mode (D5 / R1): see AuditSinkOpenMode. Production passes nothing (Trading).
    DurableAuditSink(const std::string& path, KeyRing& key_ring, std::uint32_t active_key_id,
                     std::size_t max_intent_orphans = kMaxIntentOrphans,
                     AuditSinkOpenMode open_mode = AuditSinkOpenMode::Trading) noexcept
        : log_store_(path, path + ".lock", path + ".tip"),
          rotation_log_store_(path + ".keyrotations", path + ".keyrotations.lock", path + ".keyrotations.tip"),
          store_identity_store_(path + ".storeid", path + ".storeid.lock", path + ".storeid.tip"),
          key_ring_(key_ring),
          active_key_id_(active_key_id),
          max_intent_orphans_(max_intent_orphans < kMaxIntentOrphans ? max_intent_orphans : kMaxIntentOrphans),
          open_mode_(open_mode) {
        if (!acquire_lock()) {
            recovery_status_ = RecoveryScanStatus::IoError;
            return;
        }
        if (!open_log()) {
            recovery_status_ = RecoveryScanStatus::IoError;
            release_lock();
            return;
        }

        // Phase 4 (docs/SPEC_INVARIANTS.md): the KeyRotated sidecar is a
        // SEPARATE durability domain from the main order-audit log -- a problem
        // opening/verifying it must never fence or otherwise affect
        // append_durable()'s ability to do this class's primary job. It only
        // gates rotate_active_key() (see that method's own doc comment).
        //
        // ORDERING (audit KEY-ROTATE-008): this block now runs BEFORE
        // run_recovery_scan(), not after. rotate_active_key() is a two-step durable
        // protocol -- sidecar record first, main-log tip re-anchor second -- and a
        // crash between the two leaves the main tip anchor signed under the OLD key
        // while the sidecar durably says a rotation to the new one happened. The
        // main scan's anchor.key_id consistency check has to be able to consult that
        // sidecar evidence to tell "interrupted rotation, nothing lost" apart from
        // "unauthorized identity swap", and it can only do that if the sidecar has
        // already been scanned. Scanning the sidecar first is safe because it does
        // not depend on the main log in any way.
        if (rotation_log_store_.acquire_lock()) {
            if (rotation_log_store_.open_log()) {
                rotation_log_open_ = true;
                if (!run_rotation_recovery_scan()) {
                    rotation_fenced_ = true;
                }
            } else {
                rotation_log_store_.release_lock();
            }
        }

        recovery_status_ = run_recovery_scan();
        if (recovery_status_ != RecoveryScanStatus::Clean &&
            recovery_status_ != RecoveryScanStatus::Recovered) {
            // Fail closed: do not allow appends against a log we couldn't
            // cleanly account for. Keep the file handles open only so
            // is_open()/recovery_status() can report on them; append_durable()
            // itself checks fenced_.
            fenced_ = true;
            // Not disk-full-specific: this is a recovery-time READ/scan
            // failure (Corrupt/CapacityExceeded/IoError), never a write
            // running out of space. recovery_status() already carries the
            // precise reason; fence_reason() just needs to say "not none".
            fence_reason_ = FenceReason::IoError;
        }

        // Phase 5 (docs/SPEC_INVARIANTS.md): store identity, same "own
        // independent durability domain, never fences the main log" treatment
        // as the rotation sidecar above -- establish_store_identity() only
        // ever sets store_identity_degraded_, never fenced_.
        establish_store_identity(path + ".storeid.tip");
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
    // None whenever !fenced(). Sticky once set (unlike DurableLogStore::
    // last_write_status(), which reflects only the latest call) -- fencing
    // itself is permanent, so the reason for it should stay readable for the
    // rest of this object's lifetime, not just until the next failed call.
    FenceReason fence_reason() const noexcept { return fence_reason_; }
    RecoveryScanStatus recovery_status() const noexcept { return recovery_status_; }

    // Every non-exchange-final order recovery_scan() found, in scan order --
    // EXCEPT the Intent-only ones (nothing was ever sent for those; see
    // recovered_intent_orphans()).
    // Fixed capacity matching InFlightRegistry's own kMaxInFlight -- if more
    // than that were found, recovery_status() is CapacityExceeded and this
    // array's first kMaxInFlight entries are populated (best-effort, but the
    // caller must treat CapacityExceeded as fail-closed regardless per the
    // same "L5 does not start until an operator resolves this" rule as every
    // other RecoveryScanStatus failure).
    std::span<const OrderRecoveryCheckpoint> recovered_checkpoints() const noexcept {
        return std::span<const OrderRecoveryCheckpoint>(checkpoints_.data(), checkpoint_count_);
    }

    // SAFE-03 slice 2 (SUBMITPORT spec round 9, "Intent-only orphans"): how many COIDs the scan found whose LAST
    // durable frame is Intent. They are counted, not recovered -- not in recovered_checkpoints(), so
    // apply_recovery() never registers them in flight and StartupRecovery never waits for them -- and they stay in
    // the log for audit. max_intent_orphans() is their budget (kMaxIntentOrphans unless a test tightened it);
    // one more than that is CapacityExceeded.
    std::size_t recovered_intent_orphans() const noexcept { return intent_orphan_count_; }
    std::size_t max_intent_orphans() const noexcept { return max_intent_orphans_; }

    // D5 / R1 (docs/OPERATOR_RESOLUTION_DESIGN.md 7.3): the orders an OrderOperatorResolved frame closed, in log
    // order. They are FINAL -- not in recovered_checkpoints(), not counted as orphans, nothing for
    // apply_recovery()/StartupRecovery to wait for -- and this list exists only so the late-fill alarm (slice R4)
    // can recognise an execution report for one of them. Complete or the open failed: see kMaxOperatorResolved
    // (durable_control_plane.hpp); an OperatorOpen scan that overflowed it holds the first kMaxOperatorResolved and
    // counts the rest in recovery_overflow().
    std::span<const OperatorResolvedOrder> recovered_operator_resolved() const noexcept {
        return std::span<const OperatorResolvedOrder>(operator_resolved_.data(), operator_resolved_count_);
    }
    // Everything the scan found, listed or not (equals the span's size unless an OperatorOpen scan overflowed).
    std::size_t recovered_operator_resolved_total() const noexcept {
        return operator_resolved_count_ + recovery_overflow_.operator_resolved;
    }
    // What an OperatorOpen scan counted past the capacity of the three recovery lists (all zero otherwise).
    const RecoveryOverflow& recovery_overflow() const noexcept { return recovery_overflow_; }
    AuditSinkOpenMode open_mode() const noexcept { return open_mode_; }

    // The format version append_durable() writes: always the store's own tip version (D5 / R1, see the FORMAT
    // v5 -> v6 note in durable_frame_codec.hpp). 5 for every store that exists today -- an empty one included.
    std::uint8_t write_format_version() const noexcept { return write_version_; }

    // SAFE-03 crash-window slice (blueprint L-36): how many bytes of a torn tail -- the partial frame a crash in the
    // middle of an append left behind -- recovery cut off the end of the main log (and, for the second accessor, off
    // the key-rotation sidecar) before the first new append. append_durable() appends at the PHYSICAL end of the
    // file, so a tail that was only skipped when scanning would sit between the last valid frame and the next one
    // and make the NEXT start Corrupt. 0 = the file ended on a frame boundary. The cut bytes are discarded, not
    // kept (owner decision, 2026-10-14): they are an incomplete record whose append was never acknowledged.
    std::uint64_t recovered_torn_tail_bytes() const noexcept { return torn_tail_bytes_; }
    std::uint64_t recovered_rotation_torn_tail_bytes() const noexcept { return rotation_torn_tail_bytes_; }

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

        // Live per-call lookup (never cached) -- Phase 2, docs/SPEC_INVARIANTS.md.
        // Unknown/retired active_key_id is a correctable configuration
        // mistake, not corruption: Failed, but do NOT fence.
        std::array<std::byte, kKeyBlockSize> key_block{};
        if (!key_ring_.active_key(active_key_id_, key_block)) {
            result.status = AuditAppendResult::Status::Failed;
            return result;
        }
        std::span<const std::byte> hmac_key(key_block.data(), key_block.size());

        std::array<std::byte, kOrderEventFrameSize> buf{};
        const auto n = encode_order_event_frame(buf, active_key_id_, next_sequence_,
                                                 FrameTimeKind::ServerCorrectedUtc,
                                                 now_ms, rec, tip_mac_, hmac_key, write_version_);
        if (n != kOrderEventFrameSize) {
            // Pure encoding check, never touches log_store_ -- not a disk-full scenario by construction. D5 / R1:
            // it now also refuses a record whose event type is not legal for write_version_ (OrderOperatorResolved
            // on a v5 store): that is a caller bug, and fencing here is what keeps the log from ever holding a
            // frame its own next start would call Corrupt.
            fenced_ = true;
            fence_reason_ = FenceReason::IoError;
            result.status = AuditAppendResult::Status::Failed;
            return result;
        }

        if (!append_bytes_to_log(buf.data(), buf.size())) {
            fenced_ = true;
            fence_reason_ = (log_store_.last_write_status() == IoWriteStatus::DiskFull)
                                 ? FenceReason::DiskFull
                                 : FenceReason::IoError;
            result.status = AuditAppendResult::Status::Failed;
            return result;
        }

        std::array<std::byte, kMacLen> this_mac{};
        std::memcpy(this_mac.data(), buf.data() + kOrderEventFrameSize - kMacLen, kMacLen);

        if (!write_tip_anchor(next_sequence_, this_mac, active_key_id_, hmac_key)) {
            fenced_ = true;
            fence_reason_ = (log_store_.last_write_status() == IoWriteStatus::DiskFull)
                                 ? FenceReason::DiskFull
                                 : FenceReason::IoError;
            result.status = AuditAppendResult::Status::Failed;
            return result;
        }

        result.status = AuditAppendResult::Status::Acked;
        result.sequence = next_sequence_;
        tip_mac_ = this_mac;
        ++next_sequence_;

        // Phase 5 (docs/SPEC_INVARIANTS.md): store_identity_degraded_ suppresses
        // this entire block -- when the store's identity could not be
        // established/verified, pushing a tuple with a zero or unpersisted-
        // and-therefore-unstable-across-restart store_uuid would be worse
        // than not exporting at all (a real, previously-shipped self-
        // contradiction this round's second external review round caught and
        // fixed: a "degraded" state must behave like "no export_outbox_
        // wired", never like "export_outbox_ wired but lying about identity").
        if (export_outbox_ && !store_identity_degraded_) {
            // Best-effort: a full ring (no consumer exists yet this round)
            // must never fail or fence the local append -- local durability
            // is the correctness-critical part; see class header + this
            // round's SPEC_INVARIANTS.md entry.
            ExportTuple t{};
            t.store_uuid_lo = store_uuid_lo_;
            t.store_uuid_hi = store_uuid_hi_;
            t.generation = 0;  // compaction/generation-switch subsystem, still out of scope
            t.sequence = result.sequence;
            std::memcpy(t.tip_mac.data(), this_mac.data(), t.tip_mac.size());
            t.key_id = active_key_id_;
            t.enqueued_utc_ms = now_ms;
            t.time_kind = static_cast<std::uint8_t>(FrameTimeKind::ServerCorrectedUtc);
            (void)export_outbox_->try_push(t);
        }

        return result;
    }

    // Optional: wire an ExportOutboxRing to receive a tip tuple on every
    // successful Acked append (see class header's ExportOutboxRing note).
    // Nullable; unset by default. Not owned.
    //
    // Lifetime contract (Phase 5, docs/SPEC_INVARIANTS.md -- implicit since
    // this method was first written, made explicit here): call this once,
    // before the export-worker/submit thread starts draining the ring.
    // Never reset() it or let the pointed-to ExportOutboxRing be destroyed
    // while a worker may still be reading it -- this class holds a raw,
    // non-owning pointer and does no lifetime tracking of its own.
    void set_export_outbox(ExportOutboxRing* ring) noexcept { export_outbox_ = ring; }

    // Phase 4 (docs/SPEC_INVARIANTS.md): safely switches active_key_id_ to
    // new_key_id without a process restart. Caller must have already loaded
    // new_key_id into key_ring (the "prepare" step) -- this call only ever
    // switches to a key already proven resolvable.
    //
    // Must be called from the same owning thread as append_durable()/
    // run_recovery_scan() -- this class has no internal synchronization and
    // rotate_active_key() is no exception (see class header's THREAD
    // OWNERSHIP note).
    //
    // Sequencing (both steps durable BEFORE active_key_id_ itself flips --
    // this codebase's "durable-before-flip, never flip-then-rollback"
    // discipline, same as live_submit_orchestrator.hpp's Phase 3 gates):
    //   1. Append a KeyRotated record to the SIDECAR log (not this log),
    //      signed under new_key_id, old_key_id carried as authenticated
    //      payload data. This is durable evidence a rotation was attempted,
    //      independent of whether step 2 below completes.
    //   2. Re-anchor THIS log's existing tip under new_key_id (skipped if
    //      the log is still empty -- nothing to re-anchor yet). Without this
    //      step, a crash between "rotated" and the first subsequent
    //      append_durable() call would leave the tip-anchor signed under the
    //      OLD key while a restart configured with the NEW key_id would
    //      misjudge that as Corrupt via the existing anchor.key_id ==
    //      active_key_id_ consistency check (Phase 2) -- even though nothing
    //      was actually lost. This step is what closes that crash window.
    // Only after both steps durably succeed does active_key_id_ actually
    // flip. Any failure along the way leaves active_key_id_ untouched.
    //
    // A sidecar problem (never opened, or fenced by a prior failed rotation)
    // fails this closed WITHOUT touching the main log at all -- you cannot
    // rotate without being able to durably evidence it, but append_durable()
    // keeps working normally regardless.
    bool rotate_active_key(std::uint32_t new_key_id, std::int64_t now_ms) noexcept {
        if (fenced_ || !log_open_) return false;
        if (rotation_fenced_ || !rotation_log_open_) return false;

        std::array<std::byte, kKeyBlockSize> new_key_block{};
        if (!key_ring_.active_key(new_key_id, new_key_block)) return false;  // "prepare" not done yet
        std::span<const std::byte> new_hmac_key(new_key_block.data(), new_key_block.size());

        const std::uint32_t old_key_id = active_key_id_;
        const std::uint64_t seq_at_rotation = (next_sequence_ == 0) ? kNoPriorTipSequence : next_sequence_ - 1;

        // Step 1: durable evidence in the sidecar, signed under new_key_id.
        KeyRotationPayload payload{old_key_id, new_key_id, seq_at_rotation};
        std::array<std::byte, kKeyRotatedFrameSize> rot_buf{};
        const auto rn = encode_key_rotated_frame(rot_buf, new_key_id, rotation_next_sequence_,
                                                  FrameTimeKind::ServerCorrectedUtc, now_ms, payload,
                                                  rotation_tip_mac_, new_hmac_key);
        if (rn != kKeyRotatedFrameSize || !rotation_log_store_.append_and_fsync(rot_buf)) {
            rotation_fenced_ = true;
            secure_wipe(new_key_block.data(), new_key_block.size());
            return false;
        }
        std::array<std::byte, kMacLen> rot_mac{};
        std::memcpy(rot_mac.data(), rot_buf.data() + kKeyRotatedFrameSize - kMacLen, kMacLen);
        std::array<std::byte, kTipAnchorSize> rot_anchor{};
        detail::encode_tip_anchor(rot_anchor, rotation_next_sequence_, rot_mac, new_key_id, new_hmac_key);
        if (!rotation_log_store_.write_tip_anchor(rot_anchor)) {
            rotation_fenced_ = true;
            secure_wipe(new_key_block.data(), new_key_block.size());
            return false;
        }
        rotation_tip_mac_ = rot_mac;
        ++rotation_next_sequence_;

        // Step 2: re-anchor the main log's tip under new_key_id (skipped on
        // an empty log -- nothing to re-anchor; the first append_durable()
        // call will write the first-ever frame+anchor under new_key_id once
        // active_key_id_ has flipped below).
        if (next_sequence_ != 0) {
            if (!write_tip_anchor(next_sequence_ - 1, tip_mac_, new_key_id, new_hmac_key)) {
                fenced_ = true;  // genuine I/O failure -- same self-fencing discipline as append_durable()
                fence_reason_ = (log_store_.last_write_status() == IoWriteStatus::DiskFull)
                                     ? FenceReason::DiskFull
                                     : FenceReason::IoError;
                secure_wipe(new_key_block.data(), new_key_block.size());
                return false;
            }
        }

        secure_wipe(new_key_block.data(), new_key_block.size());
        active_key_id_ = new_key_id;
        return true;
    }

    std::uint32_t active_key_id() const noexcept { return active_key_id_; }
    bool rotation_log_open() const noexcept { return rotation_log_open_; }
    bool rotation_fenced() const noexcept { return rotation_fenced_; }

    // Phase 5 (docs/SPEC_INVARIANTS.md): 0/0 whenever store_identity_degraded()
    // is true -- callers must not treat 0/0 here as a real identity, only as
    // "not currently established".
    std::uint64_t store_uuid_lo() const noexcept { return store_uuid_lo_; }
    std::uint64_t store_uuid_hi() const noexcept { return store_uuid_hi_; }
    // True iff the store-identity sidecar could not be established/verified
    // (corrupt file, or a first-time generation whose durable write failed).
    // Never affects fenced()/append_durable()'s core correctness -- it only
    // suppresses the export_outbox_ push in append_durable() (see that
    // method's own comment), so a caller cannot mistake a degraded process
    // for one that has simply never had export_outbox_ wired up at all.
    bool store_identity_degraded() const noexcept { return store_identity_degraded_; }

    // AUDIT KEY-ROTATE-008: the most recent rotation this sink's sidecar records, or
    // nullopt if it has never rotated. Exists so a caller/operator can tell an
    // interrupted rotation apart from an unauthorized identity swap -- the sidecar
    // always held that information, nothing ever read it back out.
    const std::optional<KeyRotationPayload>& last_rotation() const noexcept { return last_rotation_; }

    // True iff this construction found the main tip anchor still signed under
    // last_rotation()->old_key_id and completed the interrupted rotation's step 2
    // itself. Purely informational (the sink is fully usable either way), but worth
    // surfacing: it means the process previously died inside rotate_active_key().
    bool completed_interrupted_rotation() const noexcept { return completed_interrupted_rotation_; }

    // AUDIT KEY-RETIRE-009: every distinct key_id that appears in this log, gathered
    // during recovery.
    //
    // KeyRing::retire()'s documented precondition is "the caller has already
    // confirmed no retained frame still needs this key_id". With no compaction (see
    // this class's scope note) the log keeps every frame forever and run_recovery_scan()
    // re-resolves each frame's OWN key_id from offset 0 on every start -- so retiring
    // a key that ever signed anything turns the whole log Corrupt and fences the sink
    // permanently. That made the precondition unsatisfiable in principle AND
    // uncheckable in practice, because nothing exposed which key_ids were in use.
    // This does. It does not remove the underlying limit (kMaxLiveKeys bounds the
    // number of rotations this store can ever accumulate until compaction exists);
    // it makes the limit visible and the precondition testable instead of a trap.
    std::span<const std::uint32_t> observed_key_ids() const noexcept {
        return std::span<const std::uint32_t>(observed_key_ids_.data(), observed_key_id_count_);
    }

private:
    static constexpr std::size_t kKeyBlockSize = hy::kKeyBlockSize;

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
        // TODO 1A.3 follow-up (PositionTruth): captured once on first appearance,
        // same treatment as symbol_id -- direction is fixed at order-intent time
        // and never changes across an order's lifetime.
        OrderSide side{OrderSide::Buy};
        // D5 / R1: the mode of the Intent frame, so an OrderOperatorResolved frame can be held to "the same order".
        ExecutionMode mode{ExecutionMode::DryRun};
        // D5 / R1: an OrderOperatorResolved frame was accepted for this COID. It must be that COID's last frame.
        bool operator_resolved{false};
        // R-10 / L-30: the header time of the FIRST frame that put this order in Submitting and in
        // Ambiguous, kept only when that frame's time is a ServerCorrectedUtc reading (anything
        // else is no wall time). The `*_seen` flags say "first frame already seen", so a later
        // frame in the same state -- or a frame whose time is 0 -- can never move the anchor.
        bool submitting_seen{false};
        std::int64_t submitting_utc_ms{0};
        bool ambiguous_seen{false};
        std::int64_t ambiguous_utc_ms{0};
    };

    // D5 / R1 (docs/OPERATOR_RESOLUTION_DESIGN.md 7.2): may THIS OrderOperatorResolved frame close the order whose
    // replay state is `rs`? Every "no" is Corrupt for the whole log (the caller returns it). Two rules are enforced
    // elsewhere: "v6 only" by decode_order_event_frame() (event 22 in a v5 frame never decodes) and "the last frame
    // of its COID" by the caller (rs.operator_resolved).
    static bool accepts_operator_resolution(const ReplayState& rs, const AuditRecord& rec) noexcept {
        // 1. The order is in a state nothing automatic can finish: Submitting (after Prepared), Ambiguous, or
        //    EscalatedToOperator. The first two already reach Rejected through the shared state machine; the third
        //    only through the operator edge, which validate_transition() deliberately does not have.
        switch (rs.state) {
            case OrderState::Submitting:
            case OrderState::Ambiguous:
                if (validate_transition(rs.state, OrderState::Rejected) != TransitionResult::Ok) return false;
                break;
            case OrderState::EscalatedToOperator:
                if (validate_operator_resolution(rs.state, OrderState::Rejected) != TransitionResult::Ok) return false;
                break;
            default:
                return false;
        }
        // The order must never have shown a sign of existing on the exchange: no exchange id, no fill (the average
        // price is only ever recorded together with a fill, so it needs no check of its own).
        if (rs.exchange_order_id != 0 || rs.filled_qty_ticks != 0) return false;
        // 2. What the frame says: Rejected, nothing filled, no exchange id, and the same order the Intent frame
        //    described.
        if (rec.resulting_state != OrderState::Rejected) return false;
        if (rec.exchange_order_id != 0 || rec.filled_qty_ticks != 0 || rec.avg_fill_price_ticks != 0) return false;
        if (rec.symbol_id != rs.symbol_id || rec.side != rs.side || rec.mode != rs.mode) return false;
        if (rec.price_ticks != rs.intended_price_ticks || rec.qty_ticks != rs.intended_qty_ticks) return false;
        // 3. The machine field has the shape of an ABSENT conclusion (operator_resolution_frame.hpp).
        return operator_resolution::is_valid_absent_detail_code(rec.detail_code);
    }

    // AUDIT REC-NOEXCEPT-006: the scan below allocates in several places (the whole
    // log buffer, the per-COID map, the per-COID std::string keys, the mac vector),
    // and it is called from a noexcept constructor. An escaping std::bad_alloc was
    // therefore std::terminate(), silently bypassing the entire fail-closed design
    // this class is built around -- IoError/Corrupt/CapacityExceeded all exist so a
    // problem leaves a diagnosable process behind, and abort() leaves none.
    // Allocation failure now takes the same fail-closed exit as an I/O failure.
    RecoveryScanStatus run_recovery_scan() noexcept {
        try {
            return run_recovery_scan_impl();
        } catch (...) {
            return RecoveryScanStatus::IoError;
        }
    }

    RecoveryScanStatus run_recovery_scan_impl() {
        std::vector<std::byte> content;
        if (!read_entire_log(content)) return RecoveryScanStatus::IoError;

        if (content.empty()) {
            return finalize_scan_with_anchor_check(/*log_tip_valid=*/false, 0, {}, {});
        }

        std::unordered_map<std::string, ReplayState> per_coid;
        // AUDIT REC-DETERM-007: recovered_checkpoints() is documented as being "in
        // scan order", and CapacityExceeded's contract says "this array's first
        // kMaxInFlight entries are populated". Iterating per_coid directly delivered
        // neither -- std::unordered_map's order is unspecified and varies with hash
        // seed, insertion history and standard-library version, so two processes
        // recovering the SAME log could produce different checkpoint orders and, at
        // capacity, different SUBSETS. Recording first-appearance order alongside the
        // map makes the documented contract true by construction.
        std::vector<std::string> coid_scan_order;
        std::vector<std::array<std::byte, kMacLen>> macs_by_sequence;

        std::size_t offset = 0;
        std::uint64_t expected_seq = 0;
        std::array<std::byte, kMacLen> running_prev_mac{};  // zero at seq 0
        // D5 / R1: the highest frame format version seen so far in this log (0 = none yet).
        std::uint8_t max_version_seen = 0;

        while (offset < content.size()) {
            std::span<const std::byte> remaining(content.data() + offset, content.size() - offset);

            // Resolve THIS frame's own key_id (Phase 2, docs/SPEC_INVARIANTS.md)
            // -- peek_frame_key_id() failing (fewer than 6 bytes left) does
            // NOT mean Corrupt; it means "not even enough for the key_id
            // field", which is a strict subset of decode_order_event_frame's
            // own 27-byte Truncated threshold. Leave key_block as an all-zero
            // dummy and let decode_order_event_frame's own length check
            // classify it as Truncated (the correct torn-tail outcome) --
            // never short-circuit to Corrupt on a peek failure alone. Only a
            // SUCCESSFUL peek followed by a failed KeyRing lookup is a real
            // "signing key unresolvable in this environment" Corrupt.
            std::uint32_t frame_key_id = 0;
            std::array<std::byte, kKeyBlockSize> frame_key_block{};
            if (peek_frame_key_id(remaining, frame_key_id)) {
                if (!key_ring_.active_key(frame_key_id, frame_key_block)) return RecoveryScanStatus::Corrupt;
                note_observed_key_id(frame_key_id);
            }

            DecodedOrderFrame frame{};
            std::size_t frame_size = 0;
            const auto status = decode_order_event_frame(
                remaining, std::span<const std::byte>(frame_key_block.data(), frame_key_block.size()), frame,
                frame_size);

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
            // D5 / R1: the format version of a store never goes back. A v5 frame after a v6 one means the log was
            // spliced, or written by a build that did not know the store had moved on; either way it is not a log
            // this reader will vouch for. (The first v6 frame has no such predecessor requirement: a store may
            // start at v6.)
            if (frame.format_version < max_version_seen) return RecoveryScanStatus::Corrupt;
            max_version_seen = frame.format_version;

            std::string coid(frame.record.client_order_id);
            auto [it, inserted] = per_coid.try_emplace(coid);
            if (inserted) coid_scan_order.push_back(coid);
            auto& rs = it->second;
            // D5 / R1 (design 7.2 rule 4): an OrderOperatorResolved frame is the LAST frame of its COID. Anything
            // after it -- another resolution, a stray Rejected, a second Intent -- is Corrupt, the same-state
            // repeat of a final order included (the generic branch below would let it through).
            if (rs.operator_resolved) return RecoveryScanStatus::Corrupt;
            const bool operator_frame = frame.record.event_type == AuditEventType::OrderOperatorResolved;
            if (!rs.established) {
                // A COID starts with its Intent frame. The operator event is never one: refused even when it
                // claims resulting_state Intent.
                if (operator_frame) return RecoveryScanStatus::Corrupt;
                if (frame.record.resulting_state != OrderState::Intent) return RecoveryScanStatus::Corrupt;
                rs.established = true;
                rs.state = OrderState::Intent;
                rs.intended_price_ticks = frame.record.price_ticks;
                rs.intended_qty_ticks = frame.record.qty_ticks;
                rs.symbol_id = frame.record.symbol_id;
                rs.side = frame.record.side;
                rs.mode = frame.record.mode;
            } else if (operator_frame) {
                // The only way an order leaves EscalatedToOperator (or Submitting / Ambiguous) without the
                // exchange having said anything: design 7.2 rules 1 to 3.
                if (!accepts_operator_resolution(rs, frame.record)) return RecoveryScanStatus::Corrupt;
                rs.state = OrderState::Rejected;
                rs.operator_resolved = true;
                if (operator_resolved_count_ < operator_resolved_.size()) {
                    OperatorResolvedOrder& entry = operator_resolved_[operator_resolved_count_++];
                    entry = OperatorResolvedOrder{};
                    std::memcpy(entry.client_order_id.id, frame.record.client_order_id, kClientOrderIdLen);
                    entry.symbol_id = frame.record.symbol_id;
                    entry.side = frame.record.side;
                    entry.qty_ticks = frame.record.qty_ticks;
                    entry.frame_sequence = frame.sequence_number;
                } else {
                    ++recovery_overflow_.operator_resolved;
                }
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

            // R-10 / L-30: remember when the order first entered Submitting and Ambiguous -- the
            // MAC-verified header time, and only if it is a wall time (frame_wall_time_ms(): any
            // kind but ServerCorrectedUtc is not, and must not seed the wall clock).
            if (rs.state == OrderState::Submitting && !rs.submitting_seen) {
                rs.submitting_seen = true;
                rs.submitting_utc_ms = frame_wall_time_ms(frame.time_kind, frame.recorded_utc_ms);
            }
            if (rs.state == OrderState::Ambiguous && !rs.ambiguous_seen) {
                rs.ambiguous_seen = true;
                rs.ambiguous_utc_ms = frame_wall_time_ms(frame.time_kind, frame.recorded_utc_ms);
            }

            macs_by_sequence.push_back(frame.mac);
            running_prev_mac = frame.mac;
            ++expected_seq;
            offset += frame_size;
        }

        if (expected_seq == 0) {
            // Every frame present was a torn tail at sequence 0 -- same as
            // an empty log for anchor-checking purposes.
            const auto first = finalize_scan_with_anchor_check(/*log_tip_valid=*/false, 0, {}, {});
            if (first != RecoveryScanStatus::Clean && first != RecoveryScanStatus::Recovered) return first;
            // A first frame that never completed leaves no anchor and nothing to lose: cut it, so that the first
            // append starts the file instead of following debris.
            const auto first_cut = detail::cut_torn_tail(log_store_, offset, content.size(), kOrderEventFrameSize,
                                                         torn_tail_bytes_);
            return first_cut == RecoveryScanStatus::Clean ? first : first_cut;
        }

        auto status = finalize_scan_with_anchor_check(/*log_tip_valid=*/true, expected_seq - 1,
                                                        macs_by_sequence.back(), macs_by_sequence);
        if (status != RecoveryScanStatus::Clean && status != RecoveryScanStatus::Recovered) return status;

        // SAFE-03 crash-window slice (blueprint L-36): the scan stopped at `offset`, the end of the last complete
        // frame; anything after it is the partial frame a crash left behind (or nothing). The anchor check above
        // has passed, so the log up to `offset` is proven -- cut the rest. A log that failed the check, or holds a
        // complete-but-wrong frame, never gets here and is left byte-for-byte as it was.
        const auto cut = detail::cut_torn_tail(log_store_, offset, content.size(), kOrderEventFrameSize,
                                               torn_tail_bytes_);
        if (cut != RecoveryScanStatus::Clean) return cut;

        next_sequence_ = expected_seq;
        tip_mac_ = macs_by_sequence.back();
        // D5 / R1: from here on this sink writes the version its store's tip already has -- 5 for every store that
        // exists today, 6 only once a v6 frame is in the log. That is what keeps the "versions never go back" rule
        // above consistent with this class's own appends: the first v6 frame (the operator tool's) followed by the
        // trading process's v5 appends would be Corrupt at the next start.
        write_version_ = (max_version_seen >= kFrameFormatVersionV6) ? kFrameFormatVersionV6 : kFrameFormatVersion;

        // AUDIT KEY-ROTATE-008: finish the interrupted rotation's step 2 now that the
        // true tip is known. Doing it here (rather than lazily on the next append)
        // means the on-disk anchor is consistent with active_key_id_ before this
        // constructor returns, so a second crash immediately afterwards recovers
        // cleanly through the ordinary path instead of hitting this branch again.
        if (interrupted_rotation_) {
            std::array<std::byte, kKeyBlockSize> key_block{};
            if (!key_ring_.active_key(active_key_id_, key_block)) return RecoveryScanStatus::Corrupt;
            const bool ok = write_tip_anchor(next_sequence_ - 1, tip_mac_, active_key_id_,
                                              std::span<const std::byte>(key_block.data(), key_block.size()));
            secure_wipe(key_block.data(), key_block.size());
            if (!ok) return RecoveryScanStatus::IoError;
            completed_interrupted_rotation_ = true;
        }

        // D5 / R1: the operator-resolved list must be COMPLETE to be of any use (a written-off order that is live
        // after all can fill weeks later), so a Trading open with more entries than it holds does not start. An
        // OperatorOpen scan keeps the first kMaxOperatorResolved and counted the rest while scanning.
        // (Every bound is written as "not OperatorOpen is strict", so that only the one explicit mode relaxes
        // anything and an out-of-range value of the enum can never become tolerant.)
        if (recovery_overflow_.operator_resolved != 0 && open_mode_ != AuditSinkOpenMode::OperatorOpen) {
            return RecoveryScanStatus::CapacityExceeded;
        }

        // Iterate coid_scan_order, NOT per_coid: first-appearance order in the log,
        // so this is reproducible across runs, platforms and standard-library
        // versions (audit REC-DETERM-007).
        for (const auto& coid : coid_scan_order) {
            const ReplayState& rs = per_coid.at(coid);
            if (is_exchange_final(rs.state)) continue;  // nothing to recover (an operator-resolved order is Rejected)
            if (rs.state == OrderState::Intent) {
                // SUBMITPORT spec round 9 ("Intent-only orphans"): the last durable frame is Intent, so
                // OrderSubmitPrepared never landed and nothing was ever sent. Not a checkpoint -- it would be
                // registered in flight and never resolved (nothing reconciles an Intent) -- only a count, against
                // its own budget.
                if (intent_orphan_count_ >= max_intent_orphans_) {
                    if (open_mode_ != AuditSinkOpenMode::OperatorOpen) return RecoveryScanStatus::CapacityExceeded;
                    ++recovery_overflow_.intent_orphans;  // OperatorOpen: counted, not stored
                    continue;
                }
                ++intent_orphan_count_;
                continue;
            }
            if (checkpoint_count_ >= checkpoints_.size()) {
                if (open_mode_ != AuditSinkOpenMode::OperatorOpen) return RecoveryScanStatus::CapacityExceeded;
                ++recovery_overflow_.checkpoints;  // OperatorOpen: counted, not populated
                continue;
            }

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
            cp.side = rs.side;
            if (cp.resulting_state == OrderState::Ambiguous) {
                // R-10 / L-30: the durable "first became uncertain" time. A genuine Ambiguous uses
                // the frame that made it so; the Submitting -> Ambiguous remap above uses the
                // Submitting frame -- the earliest moment its fate could have become unknown, which
                // errs toward an earlier quarantine, the safe direction.
                cp.unknown_since_utc_ms =
                    (rs.state == OrderState::Submitting) ? rs.submitting_utc_ms : rs.ambiguous_utc_ms;
            }
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

        // Peek the anchor's OWN key_id (Phase 2, docs/SPEC_INVARIANTS.md) --
        // do not assume it must be active_key_id_. A tip-anchor is always
        // atomically replaced whole, so a peek failure here is unconditionally
        // Corrupt (unlike a log frame's torn tail, there is no legal partial-
        // write state to be lenient about).
        std::uint32_t anchor_key_id = 0;
        if (!detail::peek_tip_anchor_key_id(anchor_bytes, anchor_key_id)) return RecoveryScanStatus::Corrupt;

        std::array<std::byte, kKeyBlockSize> anchor_key_block{};
        if (!key_ring_.active_key(anchor_key_id, anchor_key_block)) return RecoveryScanStatus::Corrupt;

        detail::DecodedTipAnchor anchor{};
        if (!detail::decode_tip_anchor(
                anchor_bytes, std::span<const std::byte>(anchor_key_block.data(), anchor_key_block.size()),
                anchor)) {
            return RecoveryScanStatus::Corrupt;
        }

        // Consistency check: the anchor's own (now MAC-verified) key_id must
        // match what this instance was configured with. A mismatch means the
        // key actually used to sign the last durable state differs from the
        // one this restart was told to use -- without a real rotation
        // protocol (Phase 4), that can only be a configuration mistake or an
        // unauthorized identity swap, never silently accepted.
        if (anchor.key_id != active_key_id_) {
            // AUDIT KEY-ROTATE-008: a crash between rotate_active_key()'s step 1
            // (durable sidecar record) and step 2 (re-anchor the main tip under the
            // new key) lands exactly here -- the anchor is still signed under the OLD
            // key while this process was configured with the NEW one. Nothing was
            // lost, but the check used to call it Corrupt and fence the sink
            // permanently, with no API anywhere that could tell an operator the
            // difference between that and a genuine unauthorized identity swap
            // (run_rotation_recovery_scan() decoded the KeyRotated payload and threw
            // it away).
            //
            // The sidecar is durable evidence, and it is now consulted: accept ONLY
            // the one transition it actually proves -- old_key_id matches what signed
            // the anchor, new_key_id matches what we were configured with. Everything
            // else stays Corrupt. mark_interrupted_rotation() then has step 2
            // completed by the caller once the true tip is known.
            if (!last_rotation_ || anchor.key_id != last_rotation_->old_key_id ||
                active_key_id_ != last_rotation_->new_key_id) {
                return RecoveryScanStatus::Corrupt;
            }
            interrupted_rotation_ = true;
        }

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

    // Phase 4: sidecar-scoped recovery, deliberately much simpler than
    // run_recovery_scan() above -- the sidecar holds only KeyRotated frames
    // (no multi-type dispatch needed), and every record is a permanent
    // historical fact rather than "unresolved state to fold" (no per-COID
    // replay, no OrderRecoveryCheckpoint-equivalent). This only needs to (a)
    // verify the hash-chain + tip-anchor MAC integrity and (b) establish
    // rotation_next_sequence_/rotation_tip_mac_ so future rotate_active_key()
    // calls continue the chain correctly. Returns false on ANY problem
    // (I/O, corruption, or a dangling anchor with no matching frame) -- the
    // caller (constructor) treats false as rotation_fenced_ = true, which
    // gates ONLY rotate_active_key(), never append_durable().
    // Same noexcept-allocation treatment as run_recovery_scan() above (audit
    // REC-NOEXCEPT-006). false here only sets rotation_fenced_, which gates
    // rotate_active_key() and nothing else -- strictly better than terminate().
    bool run_rotation_recovery_scan() noexcept {
        try {
            return run_rotation_recovery_scan_impl();
        } catch (...) {
            return false;
        }
    }

    bool run_rotation_recovery_scan_impl() {
        std::vector<std::byte> content;
        if (!rotation_log_store_.read_whole_log(content)) return false;

        if (content.empty()) {
            std::vector<std::byte> anchor_bytes;
            // An anchor with no frames behind it is corruption, not a corner
            // case -- the sidecar's own anchor is only ever written together
            // with (immediately after) the frame it points at.
            return !rotation_log_store_.read_tip_anchor(anchor_bytes);
        }

        std::size_t offset = 0;
        std::uint64_t expected_seq = 0;
        std::array<std::byte, kMacLen> running_prev_mac{};
        std::array<std::byte, kMacLen> last_mac{};
        std::array<std::byte, kMacLen> prev_of_last_mac{};  // the MAC of the frame before the last one

        while (offset < content.size()) {
            std::span<const std::byte> remaining(content.data() + offset, content.size() - offset);

            // KeyRotatedRecord is NOT the shared [format_version][record_type]
            // [key_id]... envelope -- its key_id sits at offset 1, not offset
            // 2 (see control_plane_frame_codec.hpp's KeyRotatedRecord header
            // comment). peek_frame_key_id() (durable_frame_codec.hpp) would
            // read the wrong bytes here; peek_key_rotated_record_key_id() is
            // the correctly-offset counterpart.
            std::uint32_t frame_key_id = 0;
            std::array<std::byte, kKeyBlockSize> frame_key_block{};
            if (peek_key_rotated_record_key_id(remaining, frame_key_id)) {
                if (!key_ring_.active_key(frame_key_id, frame_key_block)) return false;
            }

            DecodedKeyRotatedFrame frame{};
            std::size_t frame_size = 0;
            const auto status = decode_key_rotated_frame(
                remaining, std::span<const std::byte>(frame_key_block.data(), frame_key_block.size()), frame,
                frame_size);

            if (status == FrameDecodeStatus::Truncated) break;  // torn tail, discard
            if (status != FrameDecodeStatus::Ok) return false;
            if (frame.sequence_number != expected_seq) return false;
            if (frame.prev_mac != running_prev_mac) return false;

            // AUDIT KEY-ROTATE-008: the payload used to be decoded and dropped on the
            // floor here, which is why an interrupted rotation was indistinguishable
            // from corruption. Keeping the LAST one is enough: it describes the most
            // recent rotation, and that is the only one whose step 2 could still be
            // outstanding.
            last_rotation_ = frame.payload;

            prev_of_last_mac = last_mac;
            last_mac = frame.mac;
            running_prev_mac = frame.mac;
            ++expected_seq;
            offset += frame_size;
        }

        if (expected_seq == 0) {
            std::vector<std::byte> anchor_bytes;
            if (rotation_log_store_.read_tip_anchor(anchor_bytes)) return false;
            // No complete frame and no anchor: a first KeyRotated frame that never completed is cut (same reasoning
            // as the main log's first frame above).
            return detail::cut_torn_tail(rotation_log_store_, offset, content.size(), kKeyRotatedFrameSize,
                                         rotation_torn_tail_bytes_) == RecoveryScanStatus::Clean;
        }

        std::vector<std::byte> anchor_bytes;
        if (!rotation_log_store_.read_tip_anchor(anchor_bytes)) return false;

        std::uint32_t anchor_key_id = 0;
        if (!detail::peek_tip_anchor_key_id(anchor_bytes, anchor_key_id)) return false;
        std::array<std::byte, kKeyBlockSize> anchor_key_block{};
        if (!key_ring_.active_key(anchor_key_id, anchor_key_block)) return false;

        detail::DecodedTipAnchor anchor{};
        if (!detail::decode_tip_anchor(
                anchor_bytes, std::span<const std::byte>(anchor_key_block.data(), anchor_key_block.size()),
                anchor)) {
            return false;
        }
        // rotate_active_key() fsyncs the sidecar frame and THEN writes the sidecar anchor, so a crash between the
        // two leaves the anchor exactly one frame behind a perfectly good frame (the late-fsync window, which the
        // main log's own anchor check has always accepted). Accept that -- and nothing else: the MAC at the
        // anchor's sequence must match what the anchor claims.
        const bool at_tip = anchor.sequence_number == expected_seq - 1 && anchor.mac == last_mac;
        const bool one_behind = expected_seq >= 2 && anchor.sequence_number == expected_seq - 2 &&
                                anchor.mac == prev_of_last_mac;
        if (!at_tip && !one_behind) return false;

        // The anchor check passed: cut a torn tail (blueprint L-36), exactly as for the main log.
        if (detail::cut_torn_tail(rotation_log_store_, offset, content.size(), kKeyRotatedFrameSize,
                                  rotation_torn_tail_bytes_) != RecoveryScanStatus::Clean) {
            return false;
        }

        rotation_next_sequence_ = expected_seq;
        rotation_tip_mac_ = last_mac;
        return true;
    }

    // Phase 5 (docs/SPEC_INVARIANTS.md): establishes (or verifies) this
    // sink's persistent store_uuid_lo_/hi_. Called once from the
    // constructor, after the rotation sidecar block. A problem here NEVER
    // fences the main log or the rotation sidecar -- it only sets
    // store_identity_degraded_, which append_durable() checks before pushing
    // to export_outbox_ (see that method's own comment: degraded must behave
    // like "no export_outbox_ wired", never like "wired but lying").
    // Same noexcept-allocation treatment as the two scans above (audit
    // REC-NOEXCEPT-006): std::vector<std::byte> raw and the std::string path both
    // allocate, and degrading is always available here, so there is never a reason
    // to terminate.
    void establish_store_identity(const std::string& tip_path) noexcept {
        try {
            establish_store_identity_impl(tip_path);
        } catch (...) {
            store_identity_degraded_ = true;
        }
    }

    void establish_store_identity_impl(const std::string& tip_path) {
        if (!store_identity_store_.acquire_lock()) {
            store_identity_degraded_ = true;
            return;
        }

        std::vector<std::byte> raw;
        if (store_identity_store_.read_tip_anchor(raw)) {
            std::uint32_t file_key_id = 0;
            std::array<std::byte, kKeyBlockSize> file_key_block{};
            if (!detail::peek_store_identity_key_id(raw, file_key_id) ||
                !key_ring_.active_key(file_key_id, file_key_block)) {
                // Too short to even contain a key_id, or the key_id it names
                // is unknown/retired -- treated the same as a decode failure
                // below: degrade, never regenerate.
                store_identity_degraded_ = true;
                return;
            }
            detail::DecodedStoreIdentity decoded{};
            if (!detail::decode_store_identity(
                    raw, std::span<const std::byte>(file_key_block.data(), file_key_block.size()), decoded)) {
                // File exists but does not decode to a self-consistent value
                // -- corruption or tampering. MUST NOT regenerate a fresh
                // identity here: that would silently mask the problem behind
                // a brand-new, equally-plausible-looking UUID. Degrade
                // instead (store_uuid_lo_/hi_ stay 0).
                store_identity_degraded_ = true;
                return;
            }
            store_uuid_lo_ = decoded.store_uuid_lo;
            store_uuid_hi_ = decoded.store_uuid_hi;
            return;
        }

        // read_tip_anchor() collapses "file absent" and "I/O error" into one
        // false -- distinguish them via the non-throwing error_code overload
        // (the throwing overload could raise inside this noexcept method).
        std::error_code ec;
        const bool exists = std::filesystem::exists(tip_path, ec);
        if (ec || exists) {
            // Either the probe itself failed, or the file is there but
            // read_tip_anchor() still failed for a real I/O reason (not
            // "absent") -- do not attempt first-time generation over what
            // might be a real, unreadable file.
            store_identity_degraded_ = true;
            return;
        }

        // Genuinely absent: first run. Generate a new identity.
        std::array<std::byte, 16> random_bytes{};
        if (!detail::fill_random_bytes(random_bytes)) {
            store_identity_degraded_ = true;
            return;
        }
        std::uint64_t lo = 0;
        std::uint64_t hi = 0;
        std::memcpy(&lo, random_bytes.data(), 8);
        std::memcpy(&hi, random_bytes.data() + 8, 8);

        std::array<std::byte, kKeyBlockSize> key_block{};
        if (!key_ring_.active_key(active_key_id_, key_block)) {
            store_identity_degraded_ = true;
            return;
        }
        std::array<std::byte, kStoreIdentitySize> buf{};
        detail::encode_store_identity(buf, lo, hi, active_key_id_,
                                       std::span<const std::byte>(key_block.data(), key_block.size()));
        if (!store_identity_store_.write_tip_anchor(buf)) {
            // Generated but NOT durable -- using it anyway would give this
            // process an identity that vanishes on restart (a future
            // restart would silently generate a DIFFERENT identity for the
            // same physical store), which is worse than simply not
            // exporting: it would look stable for this process's lifetime
            // while silently not being so. Degrade instead of using the
            // in-memory-only value.
            store_identity_degraded_ = true;
            return;
        }
        store_uuid_lo_ = lo;
        store_uuid_hi_ = hi;
    }

    // --- Platform I/O: thin delegates to DurableLogStore (docs/SPEC_INVARIANTS.md's
    // "Phase 1" entry -- extracted this round so this class and
    // ControlPlaneLogSink share one crash-consistency implementation instead
    // of maintaining two independently-drifting copies). This is a pure
    // refactor: every method below has the exact same signature/return
    // semantics it always did; test_durable_audit_sink.cpp is unmodified and
    // passing unchanged is the regression evidence. Only the tip-anchor
    // MAC/key_id encoding stays here (DurableLogStore has no notion of
    // frame formats, MACs, or keys -- it only moves bytes).
    bool acquire_lock() noexcept { return log_store_.acquire_lock(); }
    void release_lock() noexcept { log_store_.release_lock(); }
    bool open_log() noexcept {
        log_open_ = log_store_.open_log();
        return log_open_;
    }
    void close_log() noexcept {
        log_store_.close_log();
        log_open_ = false;
    }
    bool read_entire_log(std::vector<std::byte>& out) noexcept { return log_store_.read_whole_log(out); }
    bool append_bytes_to_log(const std::byte* data, std::size_t n) noexcept {
        return log_store_.append_and_fsync(std::span<const std::byte>(data, n));
    }
    // key_id is explicit (not read from the active_key_id_ member) so
    // rotate_active_key() (Phase 4) can re-anchor the tip under new_key_id
    // BEFORE active_key_id_ itself is flipped -- see that method's own doc
    // comment for why this ordering matters. append_durable() passes
    // active_key_id_ explicitly; this is a zero-behavior-change signature
    // extension for that existing call site.
    bool write_tip_anchor(std::uint64_t sequence_number, std::array<std::byte, kMacLen> mac, std::uint32_t key_id,
                          std::span<const std::byte> hmac_key) noexcept {
        std::array<std::byte, kTipAnchorSize> buf{};
        detail::encode_tip_anchor(buf, sequence_number, mac, key_id, hmac_key);
        return log_store_.write_tip_anchor(buf);
    }
    bool read_tip_anchor(std::vector<std::byte>& out) noexcept { return log_store_.read_tip_anchor(out); }

    DurableLogStore log_store_;
    // Phase 4: separate DurableLogStore instance for the KeyRotated sidecar
    // (path+".keyrotations"/.lock/.tip) -- see rotate_active_key()'s doc
    // comment for why this isn't mixed into log_store_ above.
    DurableLogStore rotation_log_store_;
    // Phase 5: separate DurableLogStore instance for the store-identity
    // sidecar (path+".storeid"/.lock/.tip) -- same "own independent
    // durability domain" treatment as rotation_log_store_ above; see
    // establish_store_identity()'s doc comment.
    DurableLogStore store_identity_store_;
    KeyRing& key_ring_;
    // Phase 4: no longer const -- rotate_active_key() is the one and only
    // place this is ever reassigned, always after durably confirming the
    // rotation (see that method's doc comment for the exact ordering).
    std::uint32_t active_key_id_;

    bool log_open_{false};
    bool fenced_{false};
    FenceReason fence_reason_{FenceReason::None};
    RecoveryScanStatus recovery_status_{RecoveryScanStatus::IoError};

    std::uint64_t next_sequence_{0};
    std::array<std::byte, kMacLen> tip_mac_{};

    std::array<OrderRecoveryCheckpoint, kMaxInFlight> checkpoints_{};
    std::size_t checkpoint_count_{0};
    std::size_t max_intent_orphans_{kMaxIntentOrphans};
    // D5 / R1. open_mode_ is fixed at construction. write_version_ starts at 5 and is set ONCE, by the recovery scan,
    // to the highest version the store's frames carry -- nothing else assigns it (test_operator_resolution_guard.cpp
    // pins that), so no code path can write the first v6 frame of a store; the operator tool's (R3) will have to
    // be a deliberate, reviewed addition.
    AuditSinkOpenMode open_mode_{AuditSinkOpenMode::Trading};
    std::uint8_t write_version_{kFrameFormatVersion};
    std::array<OperatorResolvedOrder, kMaxOperatorResolved> operator_resolved_{};
    std::size_t operator_resolved_count_{0};
    RecoveryOverflow recovery_overflow_{};
    std::size_t intent_orphan_count_{0};
    std::uint64_t torn_tail_bytes_{0};
    std::uint64_t rotation_torn_tail_bytes_{0};

    ExportOutboxRing* export_outbox_{nullptr};

    // Phase 4: KeyRotated sidecar state. Independent of log_open_/fenced_
    // above -- a sidecar problem gates ONLY rotate_active_key(), never
    // append_durable()/run_recovery_scan(). See rotate_active_key()'s doc
    // comment; there is no bounded in-memory rotation-history array (every
    // rotation is a permanent historical fact once written, not "unresolved
    // state needing recovery" the way an open order is -- offline auditing
    // reads the raw sidecar file directly).
    bool rotation_log_open_{false};
    bool rotation_fenced_{false};
    std::uint64_t rotation_next_sequence_{0};
    std::array<std::byte, kMacLen> rotation_tip_mac_{};
    // Audit KEY-ROTATE-008: the sidecar's last KeyRotated payload, retained instead
    // of discarded, plus the two flags recording what the main scan did with it.
    std::optional<KeyRotationPayload> last_rotation_{};
    bool interrupted_rotation_{false};
    bool completed_interrupted_rotation_{false};

    // Audit KEY-RETIRE-009: distinct key_ids seen while scanning the main log.
    // Bounded by kMaxLiveKeys because any key_id NOT in the ring already fails the
    // scan as Corrupt before reaching here, so more than kMaxLiveKeys distinct ones
    // is unreachable.
    std::array<std::uint32_t, kMaxLiveKeys> observed_key_ids_{};
    std::size_t observed_key_id_count_{0};

    void note_observed_key_id(std::uint32_t key_id) noexcept {
        for (std::size_t i = 0; i < observed_key_id_count_; ++i) {
            if (observed_key_ids_[i] == key_id) return;
        }
        if (observed_key_id_count_ < observed_key_ids_.size()) {
            observed_key_ids_[observed_key_id_count_++] = key_id;
        }
    }

    // Phase 5: store identity. Both stay 0 whenever store_identity_degraded_
    // is true -- see establish_store_identity()'s doc comment.
    std::uint64_t store_uuid_lo_{0};
    std::uint64_t store_uuid_hi_{0};
    bool store_identity_degraded_{false};
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
//
// R-10 / L-30: a checkpoint recovered as Ambiguous carries the durable time it first became
// uncertain (unknown_since_utc_ms). It seeds the record's WALL anchor -- so the unresolved time
// keeps accumulating across a restart or a crash loop -- unless recovered_wall_anchor() rejects it
// (not a plausible wall time, or ahead of `now_wall_ms`; pass OrderRecord::kClockUnset when the
// caller has no wall clock, and only the lower bound is checked). The monotonic anchor cannot be
// restored, that clock starts over; the tracker stamps it when it first sees the record.
inline OrderRecord checkpoint_to_order_record(const OrderRecoveryCheckpoint& cp,
                                              std::int64_t now_wall_ms = OrderRecord::kClockUnset) noexcept {
    OrderRecord rec{};
    rec.client_order_id = cp.client_order_id;
    rec.exchange_order_id = cp.exchange_order_id;
    rec.symbol_id = cp.symbol_id;
    rec.state = cp.resulting_state;
    rec.intended_price_ticks = cp.intended_price_ticks;
    rec.intended_qty_ticks = cp.intended_qty_ticks;
    rec.filled_qty_ticks = cp.filled_qty_ticks;
    rec.avg_fill_price_ticks = cp.avg_fill_price_ticks;
    // TODO 1A.3 follow-up (PositionTruth): this used to be missing, which meant
    // every recovered OrderRecord silently defaulted to OrderSide::Buy
    // regardless of the real side -- a minimal PositionTruth folding recovered
    // orders (seed_position_truth(), below) needs the real direction.
    rec.side = cp.side;
    if (cp.resulting_state == OrderState::Ambiguous) {
        rec.unknown_since_wall_ms = recovered_wall_anchor(cp.unknown_since_utc_ms, now_wall_ms);
        rec.wall_anchor_recovered = (rec.unknown_since_wall_ms != OrderRecord::kClockUnset);
    }
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

// TODO 1A.3 follow-up (PositionTruth): seeds a freshly-constructed (all-zero)
// PositionTruth from recovered checkpoints at startup, the same role
// repopulate_in_flight_registry() plays for InFlightRegistry above. Same
// scope caveat as that function's own header comment: this does NOT wire
// into any real process-startup call path -- orchestrate_submit() has no
// real construction site outside test/demo harnesses today (see this
// header's own "Startup recovery integration" note above), so there is no
// real "process boot" code to fold this into yet. Exercised directly from
// tests, exactly like checkpoint_to_order_record() already is.
//
// Each checkpoint's filled_qty_ticks is a CUMULATIVE per-order total, not a
// delta -- but because `truth` starts at zero for every symbol here, adding
// each checkpoint's full filled_qty_ticks once is the correct one-time seed
// (this is establishing bootstrap state from a set of DISTINCT orders, not a
// live incremental update against an already-partially-seeded truth; unlike
// PositionTruth's live-path callers, which must pass a delta because their
// target may already be non-zero).
inline void seed_position_truth(PositionTruth& truth, std::span<const OrderRecoveryCheckpoint> checkpoints) noexcept {
    for (const auto& cp : checkpoints) {
        if (cp.filled_qty_ticks <= 0) continue;
        truth.apply_fill(cp.symbol_id, cp.side, cp.filled_qty_ticks);
    }
}

}  // namespace hy
