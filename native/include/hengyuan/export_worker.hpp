// SPDX-License-Identifier: proprietary
// export_worker.hpp — best-effort telemetry consumer for ExportOutboxRing.
//
// Governance: L2 (real file I/O for LastRemoteAckedTip persistence; the
// remote leg is a caller-supplied ExternalAnchorClient& -- this file performs
// no network I/O itself, same as every other L2 file in this codebase that
// takes a port/client by reference).
//
// SCOPE (docs/SPEC_INVARIANTS.md's "Phase 5" entry has the full decision
// record, including two rounds of external review triage -- repeating only
// the load-bearing summary here):
//   - run_export_worker_once() drains AT MOST one tuple per call from an
//     ExportOutboxRing, waits for a genuine remote Ack, durably persists the
//     resulting LastRemoteAckedTip baseline, and only THEN pops the ring.
//     Any step failing leaves the tuple at the head for a future retry --
//     nothing here silently advances past an unconfirmed export.
//   - This is a BEST-EFFORT TELEMETRY consumer, not a safety mechanism. It
//     does not implement startup reconstruction (a lost-on-restart tuple
//     stays lost until that future round exists) or an append-path
//     backpressure fence (a full ExportOutboxRing keeps silently dropping
//     new pushes, per ExportOutboxRing::try_push()'s own existing contract).
//     Nothing in this file's output may be treated as an input to rollback
//     detection or hard-lag admission until both of those exist.
//   - ExternalAnchorClient stays permanently mock-only (same precedent as
//     SubmitPort) -- this file only ever holds it by reference.
//   - No production thread driver here, matching order_tracker.hpp's
//     poll_once() precedent: run_export_worker_once() is a plain noexcept
//     function, the caller decides when to call it again.
//
// THREAD OWNERSHIP: run_export_worker_once() must only ever be called from a
// single dedicated export-worker thread -- ExportOutboxRing's own contract is
// SPSC (one producer, one consumer). tsan_control_export_worker_dual_consumer.cpp
// is a manual, non-ctest negative control proving a second concurrent caller
// is a real, TSan-detectable data race, not just a documented rule nobody
// checks.

#pragma once

#include <hengyuan/durable_control_plane.hpp>
#include <hengyuan/durable_frame_codec.hpp>
#include <hengyuan/durable_log_store.hpp>
#include <hengyuan/kek_loader.hpp>
#include <hengyuan/secure_wipe.hpp>
#include <hengyuan/sha256.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

namespace hy {

// --- LastRemoteAckedTip wire encoding ---
//
// Implementation-internal, NOT spec-pinned byte-for-byte -- same flagged,
// deliberate-width-choice status as durable_audit_sink.hpp's own local
// tip-anchor wire format. Signed under the raw KEK (NOT a KeyRing-derived
// per-key_id key) -- LastRemoteAckedTip's own struct comment
// (durable_control_plane.hpp) already documents this as "HMAC under KEK over
// the above", a deliberately different, independent trust boundary from
// every other MAC in this codebase.
//
//   [format_version: u8][store_uuid_lo: u64 LE][store_uuid_hi: u64 LE]
//   [generation: u32 LE][sequence: u64 LE][tip_mac: 32 bytes][key_id: u32 LE]
//   [mac: 32 bytes = HMAC-SHA256(KEK, everything above)]
inline constexpr std::uint8_t kLastRemoteAckedTipFormatVersion = 1;
inline constexpr std::size_t kLastRemoteAckedTipContentSize = 1 + 8 + 8 + 4 + 8 + 32 + 4;  // 65
inline constexpr std::size_t kLastRemoteAckedTipWireSize = kLastRemoteAckedTipContentSize + kMacLen;  // 97

inline std::size_t encode_last_remote_acked_tip(std::span<std::byte, kLastRemoteAckedTipWireSize> out,
                                                  const LastRemoteAckedTip& v,
                                                  std::span<const std::byte, kKekSize> kek) noexcept {
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    detail::write_u8(p, kLastRemoteAckedTipFormatVersion);
    detail::write_u64_le(p, v.store_uuid_lo);
    detail::write_u64_le(p, v.store_uuid_hi);
    detail::write_u32_le(p, v.generation);
    detail::write_u64_le(p, v.sequence);
    detail::write_bytes(p, v.tip_mac.data(), v.tip_mac.size());
    detail::write_u32_le(p, v.key_id);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(kek, std::span<const std::byte>(content_start, content_len));
    detail::write_bytes(p, mac.bytes.data(), kMacLen);
    return kLastRemoteAckedTipWireSize;
}

// `in` must be EXACTLY kLastRemoteAckedTipWireSize bytes -- unlike a durable
// log frame, a tip-style file is always atomically replaced whole, so there
// is no legal "torn tail" case to be lenient about here (same reasoning as
// durable_audit_sink.hpp's peek_tip_anchor_key_id()).
inline bool decode_last_remote_acked_tip(std::span<const std::byte> in,
                                          std::span<const std::byte, kKekSize> kek,
                                          LastRemoteAckedTip& out) noexcept {
    if (in.size() != kLastRemoteAckedTipWireSize) return false;
    const std::byte* p = in.data();
    const std::byte* const content_start = p;

    const std::uint8_t version = detail::read_u8(p);
    if (version != kLastRemoteAckedTipFormatVersion) return false;

    LastRemoteAckedTip v{};
    v.store_uuid_lo = detail::read_u64_le(p);
    v.store_uuid_hi = detail::read_u64_le(p);
    v.generation = detail::read_u32_le(p);
    v.sequence = detail::read_u64_le(p);
    detail::read_bytes(p, v.tip_mac.data(), v.tip_mac.size());
    v.key_id = detail::read_u32_le(p);

    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac =
        crypto::hmac_sha256(kek, std::span<const std::byte>(content_start, content_len));

    std::array<std::byte, kMacLen> mac{};
    detail::read_bytes(p, mac.data(), kMacLen);

    if (!crypto::constant_time_equal(expected_mac, mac)) return false;  // audit SEC-MACCMP-010
    out = v;
    return true;
}

// --- LastRemoteAckedTipStore ---
//
// Absent: no breadcrumb file exists yet -- a legal "never exported before".
// Corrupt: the file exists but failed size/MAC verification -- must NEVER be
//   treated the same as Absent (doing so would silently regenerate a fresh
//   baseline over real corruption/tampering, masking it).
// IoError: the underlying read genuinely failed for a reason other than
//   "file does not exist" (permission, disk error, etc.), OR the store does
//   not currently hold its lock.
enum class LastRemoteAckedTipReadStatus : std::uint8_t {
    Absent = 0,
    Valid = 1,
    Corrupt = 2,
    IoError = 3,
};

// Ok: either a genuine forward advance, a first-ever write (Absent baseline),
//   or an idempotent replay of the exact same value already on disk.
// Regressed: new (generation, sequence) is behind what's already on disk --
//   refused, disk value unchanged.
// Conflicting: same store_uuid + (generation, sequence) position as what's on
//   disk, but tip_mac/key_id differ -- OR store_uuid itself differs. This is
//   a distinct failure mode from Corrupt: Corrupt means "the file on disk
//   doesn't decode to a self-consistent value at all"; Conflicting means "it
//   decodes fine, but the new value being written is logically inconsistent
//   with it" -- the two are never merged.
// IoError: the prior read() came back Corrupt/IoError (never blindly
//   overwrite a baseline that couldn't be verified), the store does not hold
//   its lock, or the underlying write itself failed.
enum class LastRemoteAckedTipWriteStatus : std::uint8_t {
    Ok = 0,
    Regressed = 1,
    Conflicting = 2,
    IoError = 3,
};

class LastRemoteAckedTipStore {
public:
    // kek: copied ONCE here (kek_copy_, wiped on destruction) -- same
    // established precedent as KeyRing::KeyRing() (key_ring.hpp) -- NOT a
    // live KekLoader& re-queried per call, which would leave a TOCTOU/
    // cross-thread-wipe surface this codebase's KeyRing precedent already
    // avoids for the exact same reason.
    LastRemoteAckedTipStore(const std::string& path, std::span<const std::byte, kKekSize> kek) noexcept
        : log_store_(path + ".unused_log", path + ".lock", path + ".tip"), tip_path_(path + ".tip") {
        std::memcpy(kek_copy_.data(), kek.data(), kek.size());
    }

    ~LastRemoteAckedTipStore() { secure_wipe(kek_copy_.data(), kek_copy_.size()); }

    LastRemoteAckedTipStore(const LastRemoteAckedTipStore&) = delete;
    LastRemoteAckedTipStore& operator=(const LastRemoteAckedTipStore&) = delete;
    LastRemoteAckedTipStore(LastRemoteAckedTipStore&&) = delete;
    LastRemoteAckedTipStore& operator=(LastRemoteAckedTipStore&&) = delete;

    // Must be called (and succeed) before write()/read() will do anything --
    // both check lock_held_ internally rather than trusting the caller to
    // have checked this return value, so a second instance that loses the
    // race for the same path cannot silently write around the winner.
    bool open() noexcept {
        lock_held_ = log_store_.acquire_lock();
        return lock_held_;
    }

    LastRemoteAckedTipReadStatus read(LastRemoteAckedTip& out) noexcept {
        if (!lock_held_) return LastRemoteAckedTipReadStatus::IoError;

        std::vector<std::byte> raw;
        if (!log_store_.read_tip_anchor(raw)) {
            // read_tip_anchor() collapses "file absent" and "I/O error" into
            // one false -- distinguish them here via the non-throwing
            // error_code overload (the throwing overload could raise inside
            // this noexcept function and terminate the process, a real risk
            // flagged by external review, not a hypothetical one).
            std::error_code ec;
            const bool exists = std::filesystem::exists(tip_path_, ec);
            if (ec) return LastRemoteAckedTipReadStatus::IoError;
            return exists ? LastRemoteAckedTipReadStatus::IoError : LastRemoteAckedTipReadStatus::Absent;
        }

        if (raw.size() != kLastRemoteAckedTipWireSize) return LastRemoteAckedTipReadStatus::Corrupt;

        LastRemoteAckedTip decoded{};
        if (!decode_last_remote_acked_tip(raw, std::span<const std::byte, kKekSize>(kek_copy_), decoded)) {
            return LastRemoteAckedTipReadStatus::Corrupt;
        }
        out = decoded;
        return LastRemoteAckedTipReadStatus::Valid;
    }

    LastRemoteAckedTipWriteStatus write(const LastRemoteAckedTip& tip) noexcept {
        if (!lock_held_) return LastRemoteAckedTipWriteStatus::IoError;

        LastRemoteAckedTip existing{};
        const auto read_status = read(existing);
        if (read_status == LastRemoteAckedTipReadStatus::Valid) {
            if (existing.store_uuid_lo != tip.store_uuid_lo || existing.store_uuid_hi != tip.store_uuid_hi) {
                return LastRemoteAckedTipWriteStatus::Conflicting;
            }
            const bool same_position =
                existing.generation == tip.generation && existing.sequence == tip.sequence;
            if (same_position) {
                if (existing.key_id != tip.key_id || existing.tip_mac != tip.tip_mac) {
                    return LastRemoteAckedTipWriteStatus::Conflicting;
                }
                // Identical replay of what's already on disk -- fall through
                // and write anyway (harmless, idempotent).
            } else if (tip.generation < existing.generation ||
                       (tip.generation == existing.generation && tip.sequence < existing.sequence)) {
                return LastRemoteAckedTipWriteStatus::Regressed;
            }
        } else if (read_status != LastRemoteAckedTipReadStatus::Absent) {
            // Corrupt or IoError -- refuse to write over a baseline whose
            // current state could not be verified.
            return LastRemoteAckedTipWriteStatus::IoError;
        }

        std::array<std::byte, kLastRemoteAckedTipWireSize> buf{};
        encode_last_remote_acked_tip(buf, tip, std::span<const std::byte, kKekSize>(kek_copy_));
        return log_store_.write_tip_anchor(buf) ? LastRemoteAckedTipWriteStatus::Ok
                                                 : LastRemoteAckedTipWriteStatus::IoError;
    }

private:
    DurableLogStore log_store_;  // only acquire_lock/release_lock/write_tip_anchor/read_tip_anchor used
    std::string tip_path_;       // duplicated from log_store_'s own (private) tip path, only to drive
                                  // the Absent-vs-IoError std::filesystem::exists() probe above
    std::array<std::byte, kKekSize> kek_copy_{};
    bool lock_held_{false};
};

// --- ExportWorkerPolicy + backoff formula ---
//
// A caller-supplied policy, not spec -- same disclosure as order_tracker.hpp's
// ReconcilePollPolicy, whose reconcile_backoff_delay_ms() this mirrors
// exactly. This is only the numeric formula: consecutive_failures is tracked
// and passed in by the caller (no internal state here), and there is no
// production thread driver in this file that actually sleeps/wakes on this
// schedule -- same "mechanism exists, driver doesn't yet" boundary
// order_tracker.hpp's own poll_once() has had since it was written. A real
// driver would want a hybrid wake ("wake once on 0->1 enqueue, then bounded
// timer backoff", avoiding a kernel wake per append) rather than a busy
// poll -- that shape is noted here as a starting point for that future,
// independent round, not implemented in this one.
struct ExportWorkerPolicy {
    std::int64_t base_retry_interval_ms{500};
    std::uint32_t backoff_multiplier{4};
    std::int64_t max_retry_interval_ms{10000};
};

inline std::int64_t export_worker_backoff_delay_ms(const ExportWorkerPolicy& policy,
                                                     std::uint32_t consecutive_failures) noexcept {
    if (policy.base_retry_interval_ms <= 0 || policy.max_retry_interval_ms <= 0) {
        return policy.max_retry_interval_ms > 0 ? policy.max_retry_interval_ms : 0;
    }
    std::int64_t interval = policy.base_retry_interval_ms;
    if (interval > policy.max_retry_interval_ms) return policy.max_retry_interval_ms;
    const std::int64_t multiplier =
        policy.backoff_multiplier == 0 ? 1 : static_cast<std::int64_t>(policy.backoff_multiplier);
    for (std::uint32_t i = 0; i < consecutive_failures; ++i) {
        if (interval > policy.max_retry_interval_ms / multiplier) {
            return policy.max_retry_interval_ms;
        }
        interval *= multiplier;
    }
    return interval > policy.max_retry_interval_ms ? policy.max_retry_interval_ms : interval;
}

// --- run_export_worker_once() ---

enum class ExportRunStatus : std::uint8_t {
    Empty = 0,                  // nothing to drain
    Exported = 1,                // remote Acked, baseline durably written, ring popped
    RemoteRejected = 2,          // remote rejected or transport failed (spec does not
                                  // distinguish these at this layer); tuple stays at head
    BaselineWriteFailed = 3,     // LastRemoteAckedTipWriteStatus::IoError; tuple stays at head
    BaselineConflict = 4,        // Regressed/Conflicting -- the ring's tuple contradicts the
                                  // already-durable baseline; needs investigation, not a
                                  // blind retry; tuple stays at head
    InternalInconsistency = 5,   // defensive re-peek before pop found a different head than
                                  // what was exported -- cannot happen under the documented
                                  // single-consumer contract; surfaces a contract violation
                                  // (e.g. a second concurrent caller) rather than popping blind
};

namespace detail {

inline bool export_tuples_equal(const ExportTuple& a, const ExportTuple& b) noexcept {
    return a.store_uuid_lo == b.store_uuid_lo && a.store_uuid_hi == b.store_uuid_hi &&
           a.generation == b.generation && a.sequence == b.sequence && a.tip_mac == b.tip_mac &&
           a.key_id == b.key_id && a.enqueued_utc_ms == b.enqueued_utc_ms &&
           a.time_kind == b.time_kind;
}

}  // namespace detail

// Must only be called from a single dedicated export-worker thread -- see
// this file's top-of-file THREAD OWNERSHIP note. outbox/anchor/baseline_store
// are references, not owned: the caller must ensure all three outlive every
// call to this function; a reference does not itself guard against dangling,
// that responsibility stays with the caller.
//
// ExternalAnchorClient's pure virtual methods are already declared noexcept
// in the existing ABI (durable_control_plane.hpp, an earlier round, not
// changed here). Per C++ semantics, an implementation that throws terminates
// the process during its own stack unwind, before control ever returns here
// -- worker-side code cannot and does not need to try to catch that; this is
// the same contract every other noexcept virtual interface in this codebase
// (e.g. SubmitPort) already carries, not a new risk introduced by this file.
inline ExportRunStatus run_export_worker_once(ExportOutboxRing& outbox, ExternalAnchorClient& anchor,
                                               LastRemoteAckedTipStore& baseline_store) noexcept {
    ExportTuple t{};
    if (!outbox.peek_oldest(t)) return ExportRunStatus::Empty;

    const auto result = anchor.export_tip_and_wait_bounded(
        t.store_uuid_lo, t.store_uuid_hi, t.generation, t.sequence,
        std::span<const std::uint8_t, 32>(t.tip_mac), t.key_id);
    if (!result.acked()) return ExportRunStatus::RemoteRejected;

    // Baseline must be durably written BEFORE the ring advances: if the
    // process crashes between "remote Acked" and "ring popped", the baseline
    // having already landed means a restart re-exports the same tip at worst
    // (harmless, wasteful) rather than the reverse -- a baseline claiming an
    // export that the ring never actually confirmed.
    LastRemoteAckedTip baseline{};
    baseline.store_uuid_lo = t.store_uuid_lo;
    baseline.store_uuid_hi = t.store_uuid_hi;
    baseline.generation = t.generation;
    baseline.sequence = t.sequence;
    baseline.tip_mac = t.tip_mac;
    baseline.key_id = t.key_id;

    const auto write_status = baseline_store.write(baseline);
    if (write_status == LastRemoteAckedTipWriteStatus::IoError) return ExportRunStatus::BaselineWriteFailed;
    if (write_status != LastRemoteAckedTipWriteStatus::Ok) return ExportRunStatus::BaselineConflict;

    ExportTuple head_check{};
    if (!outbox.peek_oldest(head_check) || !detail::export_tuples_equal(head_check, t)) {
        return ExportRunStatus::InternalInconsistency;
    }

    outbox.pop_after_remote_ack();
    return ExportRunStatus::Exported;
}

}  // namespace hy
