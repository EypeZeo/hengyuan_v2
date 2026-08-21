// SPDX-License-Identifier: proprietary
// compaction_lease.hpp — CandidateLease: the ONLY public surface for
// touching the breadcrumb directory's files (Round D, docs/
// SPEC_INVARIANTS.md's "Seal-journal Round D" entry, sixth and final design
// revision). Despite the class/file name (kept for minimal diff against the
// already-shipped, already-six-times-reviewed Round D code -- see the Round
// E breadcrumb L2 loaders ledger entry for why this was extended in place
// rather than renamed or duplicated into a second lease class), this
// directory is NOT compaction-specific: BINANCE_PRIVATE_REST_L4_SPEC.md:
// 3152-3163 lists compaction-candidate-intent/.x1/.xgc (Round D) and
// seal-export-started/.v2/.mig/.clr/.abd (Round E) as siblings in the SAME
// breadcrumb directory. A second, independent lease class opening the same
// physical directory would mean two uncoordinated locks over one directory
// -- exactly the kind of cross-class coordination gap Round D's six review
// rounds exist to prevent one level down; extending this class instead
// keeps a single lock/handle/atomic-state owner for the whole directory.
//
// Governance: L2 (real file I/O). Six external Architect reviews converged
// on this shape -- read this comment before changing anything, it exists
// specifically to stop the next five mistakes from being made again:
//
// 1. CandidateLease exposes ONLY lifecycle state (acquire/release/held/
//    fenced/last_identity_diagnostic). It does NOT export any handle type,
//    generic filename-accepting I/O method, or artifact-name parameter --
//    a v4 design that returned `const CandidateDirHandle&` from a public
//    accessor let callers save the reference past the lease's lifetime and
//    write concurrently from a non-owner thread; a v5 design that kept
//    public `publish_no_replace(std::string_view, ...)` allowed path
//    traversal via "..", since a directory handle only fixes the START of
//    pathname resolution, not the final component. Both are closed here by
//    construction: the only mutating method is a single, friend-only,
//    typed genesis write (create_intent_genesis_no_replace) whose filename
//    is not a parameter at all -- it always publishes
//    ValidatedArtifactName::for_compaction_candidate_intent()
//    (compaction_breadcrumb_io.hpp), a compile-time-fixed literal.
//
// 2. Originally IntentStore (compaction_intent_store.hpp) was the ONLY
//    friend. Round E breadcrumb L2 loaders added five more friends (each a
//    single-type, read-only loader class -- SealIdWatermarkLoader/
//    SealExportStartedLoader/SealExportStartedMigrationLoader/
//    SealStartedCleanupTombstoneLoader/SealStartedAbandonLoader), same
//    "one friend per typed consumer, no generic accessor" discipline --
//    nothing else in this codebase can call the private I/O methods.
//
// 3. Thread-confined by atomics, not a mutex (Round D review's own stated
//    preference for this "small first slice"): owner_thread_hash_/held_/
//    fenced_ are std::atomic. Every I/O method AND release() check
//    owner-thread identity FIRST, via an atomic load, before touching any
//    handle -- a non-owner thread calling release() while the owner is
//    mid-I/O returns WrongOwner without ever reaching the code that would
//    close a handle the owner is using. No mutex is needed because the
//    only cross-thread interaction this class permits (a non-owner calling
//    release()) terminates at that atomic load; genuine I/O only ever runs
//    on the owner thread.
//
// 4. Identity mismatch (directory renamed/recreated after acquire()) is a
//    STICKY fence: the first detection sets fenced_ permanently and
//    records a fixed-size CandidateIdentityDiagnostic (old/new identity,
//    no heap-allocated path strings); every subsequent call on this
//    instance returns CandidateFenced immediately, WITHOUT re-touching the
//    filesystem. This is what makes "just retry" structurally incapable of
//    working around a detected identity change -- recovery must construct
//    a fresh CandidateLease against a re-audited path, not keep calling
//    this one.
//
// See windows_native_io.hpp / compaction_breadcrumb_io.hpp for the
// platform-specific mechanics (NT native handle-relative I/O on Windows,
// openat/linkat/renameat2 on POSIX) this class is built on.

#pragma once

#include <hengyuan/compaction_breadcrumb_io.hpp>
#include <hengyuan/durable_control_plane.hpp>  // kSealIdWatermarkWireBytes and friends

#ifdef _WIN32
#include <hengyuan/windows_native_io.hpp>
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>  // flock()
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hy {

enum class LeaseAcquireStatus : std::uint8_t {
    Acquired,
    HeldElsewhere,
    DirectoryMissing,
    RejectedSymlinkOrReparse,
    Failed,
};

enum class ReleaseStatus : std::uint8_t {
    Released,
    WrongOwner,  // a non-owner thread called release() -- handle untouched
    NotHeld,
};

// Fixed-size, no heap-allocated path strings (Round D review v6's P1-1
// fix) -- an identity mismatch is an operational discovery signal, not
// proof the old handle is untrustworthy on its own; recovery needs SOME
// diagnostic to act on, not just a bare "fenced" boolean.
struct CandidateIdentityDiagnostic {
    std::uint64_t expected_dev_or_volume_serial{0};
    std::uint64_t expected_inode_or_file_index{0};
    std::uint64_t observed_dev_or_volume_serial{0};  // 0 if observed_path_missing
    std::uint64_t observed_inode_or_file_index{0};
    bool observed_path_missing{false};
};

// The four outcomes every private I/O method funnels through before it is
// allowed to touch a filesystem -- see check_can_operate() below. IntentStore
// (compaction_intent_store.hpp) maps these onto its own GenesisStatus/
// LoadStatus.
enum class LeaseIoOutcome : std::uint8_t {
    Ok,                        // admitted -- see the accompanying PublishResult/ReadFixedResult
    WrongOwner,
    NotHeld,
    CandidateFenced,           // sticky fence already set; filesystem NOT re-touched
    DirectoryIdentityChanged,  // fence set BY this call (first detection)
};

struct LeaseWriteResult {
    LeaseIoOutcome outcome{LeaseIoOutcome::WrongOwner};
    compaction_detail::PublishResult publish{};  // meaningful only when outcome == Ok
};

struct LeaseReadResult {
    LeaseIoOutcome outcome{LeaseIoOutcome::WrongOwner};
    compaction_detail::ReadFixedResult read{};  // meaningful only when outcome == Ok
};

namespace compaction_detail {

inline std::size_t hash_this_thread() noexcept {
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

}  // namespace compaction_detail

class IntentStore;  // forward-declared friend

// Round E breadcrumb L2 loaders (docs/SPEC_INVARIANTS.md's "Seal-journal
// Round E breadcrumb L2 loaders" entry) -- forward-declared friends for the
// five read-only loader classes added this round, same "one friend per
// consumer, no generic accessor" discipline IntentStore already established
// above.
class SealIdWatermarkLoader;
class SealExportStartedLoader;
// These two are class TEMPLATES (parameterized on the lease type) so their
// unit tests can drive them against a duck-typed mock -- the production
// instantiation is exactly one: SealExportStartedMigrationLoader<CandidateLease>.
// CandidateLease's `friend class hy::SealExportStartedMigrationLoader;`
// declarations name the primary template and therefore admit every
// specialization, so the friendship contract is unchanged.
template <typename LeaseT>
class SealExportStartedMigrationLoader;
template <typename LeaseT>
class SealStartedCleanupTombstoneLoader;
class SealStartedAbandonLoader;

// `.xgc` (docs/SPEC_INVARIANTS.md's "Seal-journal Round E `.xgc` decode +
// loader" entry) -- read-only, same "one friend per consumer" discipline as
// the six loaders above. Not a class template: the production instantiation
// is the only one this loader ever needs (CandidateLease), same shape as
// SealIdWatermarkLoader/SealExportStartedLoader/SealStartedAbandonLoader.
class CompactionIntentGcAuthorizedLoader;

// Round E receipt/raise_intent_phase() (docs/SPEC_INVARIANTS.md's "Seal-journal
// Round E receipt + raise_intent_phase() (Building->Reserved)" entry) --
// forward-declared friend for the phase-advance write path. See that class's
// own file (intent_phase_advancer.hpp) for the full design rationale (three
// rounds of adversarial review) and its SCOPE comment for why this is
// test-only this round, not wired into any production call path.
class IntentPhaseAdvancer;

namespace test_only {
// Pre-integration test accessor -- see the friend declaration inside
// CandidateLease below. Deliberately NOT in namespace hy::compaction_detail
// or anywhere a production include could reach it by accident; only
// test_compaction_lease.cpp defines/uses this type.
class CandidateLeaseSealBreadcrumbTestAccess;
}  // namespace test_only

class CandidateLease {
public:
    // Path is used only inside acquire() to open the directory/lock handles
    // -- never retained for later I/O (all subsequent operations are
    // handle-relative). Retained in candidate_dir_ ONLY for the periodic
    // identity re-check (stat-by-path vs identity-by-handle comparison),
    // never to reconstruct a read/write path.
    explicit CandidateLease(std::filesystem::path candidate_dir) : candidate_dir_(std::move(candidate_dir)) {}

    // If still held() at destruction, debug builds assert the destroying
    // thread matches the owner -- a real misuse (owner should always
    // release() before letting this object destruct/move), but not the
    // same class of "any misuse is memory-unsafe" problem key_ring.hpp's
    // KeyRing solves with an unconditional std::terminate(): this class's
    // own destructor doesn't hand out anything another object could
    // dangling-reference the way a PinnedKeyHandle does, so there's no
    // Release-build UAF at stake here, only a documented usage contract.
    ~CandidateLease() {
#ifndef NDEBUG
        if (held_.load(std::memory_order_relaxed)) {
            assert(compaction_detail::hash_this_thread() == owner_thread_hash_.load(std::memory_order_relaxed));
        }
#endif
        release_handles();
    }

    CandidateLease(const CandidateLease&) = delete;
    CandidateLease& operator=(const CandidateLease&) = delete;

    // Move must only be called from the owner thread (same discipline as
    // every other operation) -- the moved-to object inherits owner_thread_
    // hash and held/fenced state atomically-but-not-concurrently (there is
    // no legitimate concurrent access to `other` during its own move, by
    // the same single-owner-thread contract this whole class enforces).
    CandidateLease(CandidateLease&& other) noexcept
        : owner_thread_hash_(other.owner_thread_hash_.load(std::memory_order_relaxed)),
          held_(other.held_.load(std::memory_order_relaxed)),
          fenced_(other.fenced_.load(std::memory_order_relaxed)),
          last_diagnostic_(other.last_diagnostic_),
          candidate_dir_(std::move(other.candidate_dir_)) {
        move_handles_from(other);
        other.held_.store(false, std::memory_order_relaxed);
        other.fenced_.store(false, std::memory_order_relaxed);
    }
    CandidateLease& operator=(CandidateLease&& other) noexcept {
        if (this == &other) return *this;
        release_handles();
        owner_thread_hash_.store(other.owner_thread_hash_.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
        held_.store(other.held_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        fenced_.store(other.fenced_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        last_diagnostic_ = other.last_diagnostic_;
        candidate_dir_ = std::move(other.candidate_dir_);
        move_handles_from(other);
        other.held_.store(false, std::memory_order_relaxed);
        other.fenced_.store(false, std::memory_order_relaxed);
        return *this;
    }

    LeaseAcquireStatus acquire() noexcept;
    ReleaseStatus release() noexcept;
    bool held() const noexcept { return held_.load(std::memory_order_acquire); }
    bool fenced() const noexcept { return fenced_.load(std::memory_order_acquire); }
    std::optional<CandidateIdentityDiagnostic> last_identity_diagnostic() const noexcept {
        return last_diagnostic_;
    }

private:
    friend class hy::IntentStore;

    // Round D's ONLY write operation. Filename is not a parameter -- always
    // ValidatedArtifactName::for_compaction_candidate_intent(), a
    // compile-time-fixed literal (compaction_breadcrumb_io.hpp).
    LeaseWriteResult create_intent_genesis_no_replace(std::span<const std::byte, 140> encoded_intent) noexcept;
    LeaseReadResult read_intent_genesis(std::span<std::byte, 140> out) noexcept;
    // seq is caller-validated to {1,2,3} by IntentStore before this is
    // called; this method itself just formats the fixed hex16 filename
    // (compaction_detail::ValidatedArtifactName::for_x1()) and reads --
    // Round D never WRITES a `.x1`, but inspect_x1_chain() (read-only
    // diagnostic) needs to read one if it happens to exist.
    LeaseReadResult read_x1_frame(std::uint64_t build_nonce, std::uint32_t seq,
                                   std::span<std::byte, 176> out) noexcept;

    // Round E breadcrumb L2 loaders (docs/SPEC_INVARIANTS.md's "Seal-journal
    // Round E breadcrumb L2 loaders" entry) -- six read-only methods for the
    // seal-journal precondition/tombstone/migration/cleanup/abandon files
    // confirmed to share this candidate directory with compaction-candidate-
    // intent/.x1/.xgc above. Same discipline as read_intent_genesis/
    // read_x1_frame: filename is never a parameter, always a fixed
    // ValidatedArtifactName literal; friend-only, one loader class per
    // method (no generic "read anything" accessor).
    friend class hy::SealIdWatermarkLoader;
    friend class hy::SealExportStartedLoader;
    template <typename LeaseT>
    friend class hy::SealExportStartedMigrationLoader;
    template <typename LeaseT>
    friend class hy::SealStartedCleanupTombstoneLoader;
    friend class hy::SealStartedAbandonLoader;
    friend class hy::CompactionIntentGcAuthorizedLoader;
    // Coordinator's own pre-integration test accessor -- Modules 1-4's real
    // loader classes above don't exist yet at the point this lease extension
    // lands, so this round's own tests (test_compaction_lease.cpp) need a
    // friend to drive the six new methods through. Harmless to keep
    // permanently once the real loaders land (same "one friend per narrow
    // typed consumer" discipline, just an additional one for tests).
    friend class hy::test_only::CandidateLeaseSealBreadcrumbTestAccess;

    // Round E receipt/raise_intent_phase() (docs/SPEC_INVARIANTS.md's
    // "Seal-journal Round E receipt + raise_intent_phase() (Building->
    // Reserved)" entry) -- the 2nd through 6th write methods this class has
    // ever had (the first is create_intent_genesis_no_replace above): `.x1`,
    // Intent REPLACE, (added when raise_intent_phase() was revised to
    // fold in the watermark advance itself, per the "Seal-journal Round E
    // SealIdWatermark advance" entry) the watermark's CREATE_NEW/REPLACE
    // pair, and (added by the "Seal-journal Round E Reserved->
    // StartedPublished (NativeV2Started)" entry) the seal-export-started
    // CREATE_NEW. Same discipline: filename is never a parameter (always
    // ValidatedArtifactName::for_x1()/for_compaction_candidate_intent()/
    // for_seal_id_watermark()/for_seal_export_started()), friend-only,
    // single narrow consumer. NOTE (see that class's own
    // comment): C++ friendship is class-scoped, not method-scoped --
    // IntentPhaseAdvancer being a friend also grants it compiler-level
    // access to every other private member of CandidateLease, including
    // create_intent_genesis_no_replace and all six read methods above. That
    // is a property of this codebase's friend-based capability model, not
    // something this comment or IntentPhaseAdvancer's own design closes;
    // isolation between "what IntentStore does" and "what IntentPhaseAdvancer
    // does" is a code-review-discipline boundary, not a compiler-enforced one.
    friend class hy::IntentPhaseAdvancer;

    LeaseWriteResult write_x1_frame_no_replace(std::uint64_t build_nonce, std::uint32_t seq,
                                                std::span<const std::byte, 176> encoded_transition) noexcept;
    // REPLACE, not CREATE_NEW -- an Intent already exists (this is a phase
    // raise on an existing genesis, never a second genesis). Encoded bytes
    // are the caller's responsibility to have already validated via
    // validate_intent_transition() before calling this.
    LeaseWriteResult replace_intent_phase(std::span<const std::byte, 140> encoded_intent) noexcept;

    // Round E watermark-advance (docs/SPEC_INVARIANTS.md's "Seal-journal Round
    // E SealIdWatermark advance" entry): the fourth and fifth write methods
    // this class has ever had. CREATE_NEW is the bootstrap path (this store's
    // very first candidate, no seal-id-watermark on disk yet); REPLACE is
    // every advance after that. Both are friend-only, same discipline as
    // every other write method here -- filename is never a parameter.
    LeaseWriteResult create_seal_id_watermark_no_replace(
        std::span<const std::byte, kSealIdWatermarkWireBytes> encoded_watermark) noexcept;
    LeaseWriteResult replace_seal_id_watermark(
        std::span<const std::byte, kSealIdWatermarkWireBytes> encoded_watermark) noexcept;

    LeaseReadResult read_seal_id_watermark(std::span<std::byte, kSealIdWatermarkWireBytes> out) noexcept;
    // legacy_or_greenfield selects the filename only (see
    // ValidatedArtifactName::for_seal_export_started()) -- both files hold
    // the same SealExportStartedWire wire shape.
    LeaseReadResult read_seal_export_started(bool legacy_or_greenfield,
                                              std::span<std::byte, kSealExportStartedWireBytes> out) noexcept;
    // Round E Reserved->StartedPublished (docs/SPEC_INVARIANTS.md's
    // "Seal-journal Round E Reserved->StartedPublished (NativeV2Started)"
    // entry): CREATE_NEW only, same discipline as every other write method
    // here -- no REPLACE variant exists for this file (L is written exactly
    // once, at StartedPublished; PostSeal/Abandon cleanup, which would
    // eventually unlink it, is out of scope for this round and this repo).
    // legacy_or_greenfield selects the filename only, same parameter as the
    // read method above -- this round always passes true (greenfield;
    // MigratedV2Started's legacy-companion write path is deferred).
    LeaseWriteResult create_seal_export_started_no_replace(
        bool legacy_or_greenfield, std::span<const std::byte, kSealExportStartedWireBytes> encoded_started) noexcept;
    LeaseReadResult read_seal_export_started_migration(
        std::span<std::byte, kSealExportStartedMigrationWireBytes> out) noexcept;
    LeaseReadResult read_seal_started_cleanup_tombstone(
        std::span<std::byte, kSealStartedCleanupWireBytes> out) noexcept;
    LeaseReadResult read_seal_started_abandon(std::span<std::byte, kSealStartedAbandonWireBytes> out) noexcept;

    // `.xgc` (docs/SPEC_INVARIANTS.md's "Seal-journal Round E `.xgc` decode +
    // loader" entry) -- read-only, same discipline as the six methods above,
    // with one difference: the filename carries build_nonce (
    // ValidatedArtifactName::for_xgc(build_nonce), compaction_breadcrumb_
    // io.hpp), not a fixed literal -- there is exactly one `.xgc` per
    // candidate, not one per breadcrumb-directory-worth of candidates, so
    // build_nonce (this call's only variable input) selects which file,
    // never a caller-supplied filename/path.
    LeaseReadResult read_compaction_intent_gc_authorized(
        std::uint64_t build_nonce, std::span<std::byte, kCompactionIntentGcAuthorizedWireBytes> out) noexcept;

    LeaseIoOutcome check_can_operate() noexcept;
    bool identity_still_matches_path() noexcept;
    void release_handles() noexcept;
    void move_handles_from(CandidateLease& other) noexcept;

    std::atomic<std::size_t> owner_thread_hash_{0};
    std::atomic<bool> held_{false};
    std::atomic<bool> fenced_{false};
    std::optional<CandidateIdentityDiagnostic> last_diagnostic_{};
    std::filesystem::path candidate_dir_;

#ifdef _WIN32
    win_native::RawHandle dir_handle_;
    win_native::RawHandle lock_handle_;
    win_native::FileIdentity expected_identity_{};
#else
    int dir_fd_{-1};
    int lock_fd_{-1};
    std::uint64_t expected_dev_{0};
    std::uint64_t expected_ino_{0};
#endif
};

// ===========================================================================
// Windows-only combination helpers: windows_native_io.hpp provides the raw
// NT-native primitives (open/create/rename), compaction_breadcrumb_io.hpp's
// POSIX half provides the FULL publish/read algorithm (tmp -> write ->
// fsync -> no-replace publish -> byte-compare-on-collision -> parent
// flush) already combined for openat/linkat/renameat2. This is that same
// algorithm written once for the Windows primitives -- kept here (not in
// windows_native_io.hpp, which stays a thin, mechanical primitive wrapper)
// because it is CandidateLease-specific business logic.
// ===========================================================================
#ifdef _WIN32

namespace compaction_detail {

// ValidatedArtifactName::relative_name() is guaranteed pure ASCII
// (is_traversal_safe_name() rejects anything else) -- this widen is a
// trivial zero-extension, not a real encoding conversion.
inline std::wstring ascii_to_wide(std::string_view s) noexcept {
    std::wstring out;
    out.reserve(s.size());
    for (const char c : s) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return out;
}

inline std::wstring make_unique_tmp_name_win(std::string_view final_name) noexcept {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    std::wstring out = ascii_to_wide(final_name);
    out += L".tmp-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(n);
    return out;
}

inline PublishResult write_validated_no_replace_win(const win_native::RawHandle& dir,
                                                       const ValidatedArtifactName& name,
                                                       std::span<const std::byte> bytes) noexcept {
    PublishResult result{};
    const std::wstring final_name = ascii_to_wide(name.relative_name());
    const std::wstring tmp_name = make_unique_tmp_name_win(name.relative_name());

    win_native::RawHandle tmp;
    if (win_native::create_new_relative(dir, tmp_name, tmp) != win_native::RelativeCreateResult::Created) {
        result.state = PublishCommitState::NotPublished;
        return result;
    }

    DWORD written = 0;
    BOOL ok = ::WriteFile(tmp.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    ok = ok && written == static_cast<DWORD>(bytes.size());
    ok = ok && ::FlushFileBuffers(tmp.get());
    if (!ok) {
        result.state = PublishCommitState::NotPublished;
        return result;  // tmp's RawHandle destructor closes it; the leftover
                         // uniquely-named tmp file is harmless uncollected
                         // garbage, same reasoning as the POSIX side
    }

    NTSTATUS rename_status = 0;
    const auto rename_result = win_native::rename_no_replace(tmp, dir, final_name, &rename_status);
    if (rename_result == win_native::RelativeRenameResult::AlreadyExists) {
        tmp.reset();  // done with the tmp handle; must close before reading the final name
                       // (Round D's genesis writer opens the final name with no sharing)
        win_native::RawHandle existing;
        if (win_native::open_existing_relative(dir, final_name, existing) !=
            win_native::RelativeOpenResult::Opened) {
            result.state = PublishCommitState::NotPublished;
            return result;
        }
        std::vector<std::byte> existing_bytes(bytes.size() + 1);  // +1 detects "longer than expected"
        DWORD read_n = 0;
        if (!::ReadFile(existing.get(), existing_bytes.data(), static_cast<DWORD>(existing_bytes.size()),
                         &read_n, nullptr)) {
            result.state = PublishCommitState::NotPublished;
            return result;
        }
        const bool byte_equal =
            read_n == bytes.size() && std::memcmp(existing_bytes.data(), bytes.data(), bytes.size()) == 0;
        if (byte_equal) {
            result.state = PublishCommitState::DurablyPublished;
            result.provenance = PublishProvenance::FoundPreExisting;
            return result;
        }
        result.state = PublishCommitState::NotPublished;  // real conflict, never overwritten
        return result;
    }
    if (rename_result != win_native::RelativeRenameResult::Renamed) {
        result.state = PublishCommitState::NotPublished;
        return result;
    }
    tmp.reset();  // rename doesn't invalidate the handle, but we're done with it

    // Parent-directory durability on Windows is genuinely undecidable in
    // general (unlike POSIX fsync(dir_fd), which either succeeds or is a
    // definite failure) -- FlushFileBuffers on a directory handle is not
    // universally guaranteed. Failure here is classified Uncertain, not
    // Failed, because the file itself IS confirmed durable with correct
    // content at this point.
    if (::FlushFileBuffers(dir.get())) {
        result.state = PublishCommitState::DurablyPublished;
        result.provenance = PublishProvenance::CreatedThisCallDurable;
    } else {
        result.state = PublishCommitState::PublishedNamespaceUncertain;
        result.provenance = PublishProvenance::CreatedThisCallUncertain;
    }
    return result;
}

// Round E receipt/raise_intent_phase() (docs/SPEC_INVARIANTS.md's
// "Seal-journal Round E receipt + raise_intent_phase() (Building->Reserved)"
// entry): the Windows-native combination helper for write_validated_replace
// (compaction_breadcrumb_io.hpp's POSIX side) -- same tmp-write-fsync-
// then-atomic-rename shape as write_validated_no_replace_win above, but
// using rename_with_replace() (ReplaceIfExists=TRUE) instead of
// rename_no_replace(), and therefore with NO byte-compare-on-collision
// branch: a REPLACE has no AlreadyExists outcome to react to, unlike a
// no-replace publish.
inline PublishResult write_validated_replace_win(const win_native::RawHandle& dir, const ValidatedArtifactName& name,
                                                    std::span<const std::byte> bytes) noexcept {
    PublishResult result{};
    const std::wstring final_name = ascii_to_wide(name.relative_name());
    const std::wstring tmp_name = make_unique_tmp_name_win(name.relative_name());

    win_native::RawHandle tmp;
    if (win_native::create_new_relative(dir, tmp_name, tmp) != win_native::RelativeCreateResult::Created) {
        result.state = PublishCommitState::NotPublished;
        return result;
    }

    DWORD written = 0;
    BOOL ok = ::WriteFile(tmp.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    ok = ok && written == static_cast<DWORD>(bytes.size());
    ok = ok && ::FlushFileBuffers(tmp.get());
    if (!ok) {
        result.state = PublishCommitState::NotPublished;
        return result;  // tmp's RawHandle destructor closes it; the leftover
                         // uniquely-named tmp file is harmless uncollected
                         // garbage, same reasoning as the POSIX side
    }

    NTSTATUS rename_status = 0;
    const auto rename_result = win_native::rename_with_replace(tmp, dir, final_name, &rename_status);
    if (rename_result != win_native::RelativeRenameResult::Renamed) {
        result.state = PublishCommitState::NotPublished;
        return result;
    }
    tmp.reset();  // rename doesn't invalidate the handle, but we're done with it

    // Same Windows parent-directory-durability-is-undecidable reasoning as
    // write_validated_no_replace_win above.
    if (::FlushFileBuffers(dir.get())) {
        result.state = PublishCommitState::DurablyPublished;
        result.provenance = PublishProvenance::CreatedThisCallDurable;
    } else {
        result.state = PublishCommitState::PublishedNamespaceUncertain;
        result.provenance = PublishProvenance::CreatedThisCallUncertain;
    }
    return result;
}

inline ReadFixedResult read_validated_exact_win(const win_native::RawHandle& dir,
                                                  const ValidatedArtifactName& name,
                                                  std::span<std::byte> out) noexcept {
    ReadFixedResult result{};
    const std::wstring final_name = ascii_to_wide(name.relative_name());

    win_native::RawHandle h;
    const auto open_result = win_native::open_existing_relative(dir, final_name, h);
    if (open_result == win_native::RelativeOpenResult::NotFound) {
        result.status = ReadFixedStatus::NotFound;
        return result;
    }
    if (open_result == win_native::RelativeOpenResult::RejectedReparsePoint) {
        result.status = ReadFixedStatus::NotRegularFile;
        return result;
    }
    if (open_result != win_native::RelativeOpenResult::Opened) {
        result.status = ReadFixedStatus::IoError;
        return result;
    }

    DWORD read_n = 0;
    if (!::ReadFile(h.get(), out.data(), static_cast<DWORD>(out.size()), &read_n, nullptr)) {
        result.status = ReadFixedStatus::IoError;
        return result;
    }
    if (read_n != out.size()) {
        result.status = ReadFixedStatus::WrongSize;
        return result;
    }
    std::array<std::byte, 1> probe{};
    DWORD probe_n = 0;
    ::ReadFile(h.get(), probe.data(), 1, &probe_n, nullptr);
    if (probe_n > 0) {
        result.status = ReadFixedStatus::WrongSize;  // file longer than expected
        return result;
    }
    result.status = ReadFixedStatus::Ok;
    return result;
}

}  // namespace compaction_detail

#endif  // _WIN32

// ===========================================================================
// CandidateLease method definitions
// ===========================================================================

inline void CandidateLease::release_handles() noexcept {
#ifdef _WIN32
    lock_handle_.reset();
    dir_handle_.reset();
#else
    if (lock_fd_ >= 0) {
        ::flock(lock_fd_, LOCK_UN);
        ::close(lock_fd_);
        lock_fd_ = -1;
    }
    if (dir_fd_ >= 0) {
        ::close(dir_fd_);
        dir_fd_ = -1;
    }
#endif
}

inline void CandidateLease::move_handles_from(CandidateLease& other) noexcept {
#ifdef _WIN32
    dir_handle_ = std::move(other.dir_handle_);
    lock_handle_ = std::move(other.lock_handle_);
    expected_identity_ = other.expected_identity_;
#else
    dir_fd_ = other.dir_fd_;
    lock_fd_ = other.lock_fd_;
    expected_dev_ = other.expected_dev_;
    expected_ino_ = other.expected_ino_;
    other.dir_fd_ = -1;
    other.lock_fd_ = -1;
#endif
}

inline LeaseAcquireStatus CandidateLease::acquire() noexcept {
    owner_thread_hash_.store(compaction_detail::hash_this_thread(), std::memory_order_release);

#ifdef _WIN32
    const auto dir_result = win_native::open_directory(candidate_dir_.wstring(), dir_handle_);
    if (dir_result == win_native::DirOpenResult::NotFound) return LeaseAcquireStatus::DirectoryMissing;
    if (dir_result == win_native::DirOpenResult::RejectedReparsePoint) {
        return LeaseAcquireStatus::RejectedSymlinkOrReparse;
    }
    if (dir_result != win_native::DirOpenResult::Opened) return LeaseAcquireStatus::Failed;

    if (!win_native::query_file_identity(dir_handle_, expected_identity_)) {
        dir_handle_.reset();
        return LeaseAcquireStatus::Failed;
    }

    const auto lock_result =
        win_native::open_or_create_exclusive_relative(dir_handle_, L"compaction-candidate.lock", lock_handle_);
    if (lock_result == win_native::ExclusiveLockResult::HeldElsewhere) {
        dir_handle_.reset();
        return LeaseAcquireStatus::HeldElsewhere;
    }
    if (lock_result != win_native::ExclusiveLockResult::Acquired) {
        dir_handle_.reset();
        return LeaseAcquireStatus::Failed;
    }

    held_.store(true, std::memory_order_release);
    return LeaseAcquireStatus::Acquired;
#else
    dir_fd_ = ::open(candidate_dir_.c_str(), O_DIRECTORY | O_NOFOLLOW);
    if (dir_fd_ < 0) {
        if (errno == ENOENT) return LeaseAcquireStatus::DirectoryMissing;
        if (errno == ELOOP || errno == ENOTDIR) return LeaseAcquireStatus::RejectedSymlinkOrReparse;
        return LeaseAcquireStatus::Failed;
    }
    struct stat st {};
    if (::fstat(dir_fd_, &st) != 0) {
        ::close(dir_fd_);
        dir_fd_ = -1;
        return LeaseAcquireStatus::Failed;
    }
    expected_dev_ = static_cast<std::uint64_t>(st.st_dev);
    expected_ino_ = static_cast<std::uint64_t>(st.st_ino);

    lock_fd_ = ::openat(dir_fd_, "compaction-candidate.lock", O_CREAT | O_RDWR, 0600);
    if (lock_fd_ < 0) {
        ::close(dir_fd_);
        dir_fd_ = -1;
        return LeaseAcquireStatus::Failed;
    }
    if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
        const bool held_elsewhere = (errno == EWOULDBLOCK);
        ::close(lock_fd_);
        lock_fd_ = -1;
        ::close(dir_fd_);
        dir_fd_ = -1;
        return held_elsewhere ? LeaseAcquireStatus::HeldElsewhere : LeaseAcquireStatus::Failed;
    }

    held_.store(true, std::memory_order_release);
    return LeaseAcquireStatus::Acquired;
#endif
}

inline ReleaseStatus CandidateLease::release() noexcept {
    // held_ is checked FIRST, not owner-thread: a lease that was never
    // acquire()'d has owner_thread_hash_ still at its default value (0),
    // which essentially no real thread's hash equals -- checking owner
    // first would misclassify "never acquired, any thread calls release()"
    // as WrongOwner instead of the correct NotHeld (found by actually
    // running this test, not by reasoning about it in the abstract: a
    // fresh CandidateLease's release() returned WrongOwner where NotHeld
    // was expected). This reordering does not weaken the P0-2 safety
    // property -- held_ is a plain atomic load, touches no handle, so a
    // non-owner thread still can't reach release_handles() below without
    // ALSO passing the owner-thread check that follows.
    if (!held_.load(std::memory_order_acquire)) {
        return ReleaseStatus::NotHeld;
    }
    if (compaction_detail::hash_this_thread() != owner_thread_hash_.load(std::memory_order_acquire)) {
        return ReleaseStatus::WrongOwner;  // handle untouched
    }
    release_handles();
    held_.store(false, std::memory_order_release);
    return ReleaseStatus::Released;
}

inline bool CandidateLease::identity_still_matches_path() noexcept {
#ifdef _WIN32
    win_native::RawHandle probe;
    if (win_native::open_directory(candidate_dir_.wstring(), probe) != win_native::DirOpenResult::Opened) {
        return false;
    }
    win_native::FileIdentity observed{};
    if (!win_native::query_file_identity(probe, observed)) return false;
    return observed.volume_serial == expected_identity_.volume_serial &&
           observed.file_index == expected_identity_.file_index;
#else
    struct stat st {};
    if (::stat(candidate_dir_.c_str(), &st) != 0) return false;
    return static_cast<std::uint64_t>(st.st_dev) == expected_dev_ &&
           static_cast<std::uint64_t>(st.st_ino) == expected_ino_;
#endif
}

inline LeaseIoOutcome CandidateLease::check_can_operate() noexcept {
    // Same ordering fix as release() above: held_ before owner-thread, so a
    // never-acquired lease reports NotHeld rather than a misleading
    // WrongOwner. Still safe: held_ is a plain atomic load, no handle is
    // touched until both this AND the owner-thread check pass.
    if (!held_.load(std::memory_order_acquire)) {
        return LeaseIoOutcome::NotHeld;
    }
    if (compaction_detail::hash_this_thread() != owner_thread_hash_.load(std::memory_order_acquire)) {
        return LeaseIoOutcome::WrongOwner;
    }
    if (fenced_.load(std::memory_order_acquire)) {
        return LeaseIoOutcome::CandidateFenced;
    }
    if (identity_still_matches_path()) {
        return LeaseIoOutcome::Ok;
    }

    // First detection -- populate the fixed-size diagnostic and set the
    // sticky fence. Every subsequent call short-circuits at the fenced_
    // check above, without reaching this block again.
    CandidateIdentityDiagnostic diag{};
#ifdef _WIN32
    diag.expected_dev_or_volume_serial = expected_identity_.volume_serial;
    diag.expected_inode_or_file_index = expected_identity_.file_index;
    win_native::RawHandle probe;
    win_native::FileIdentity observed{};
    if (win_native::open_directory(candidate_dir_.wstring(), probe) == win_native::DirOpenResult::Opened &&
        win_native::query_file_identity(probe, observed)) {
        diag.observed_dev_or_volume_serial = observed.volume_serial;
        diag.observed_inode_or_file_index = observed.file_index;
        diag.observed_path_missing = false;
    } else {
        diag.observed_path_missing = true;
    }
#else
    diag.expected_dev_or_volume_serial = expected_dev_;
    diag.expected_inode_or_file_index = expected_ino_;
    struct stat st {};
    if (::stat(candidate_dir_.c_str(), &st) == 0) {
        diag.observed_dev_or_volume_serial = static_cast<std::uint64_t>(st.st_dev);
        diag.observed_inode_or_file_index = static_cast<std::uint64_t>(st.st_ino);
        diag.observed_path_missing = false;
    } else {
        diag.observed_path_missing = true;
    }
#endif
    last_diagnostic_ = diag;
    fenced_.store(true, std::memory_order_release);
    return LeaseIoOutcome::DirectoryIdentityChanged;
}

inline LeaseWriteResult CandidateLease::create_intent_genesis_no_replace(
    std::span<const std::byte, 140> encoded_intent) noexcept {
    LeaseWriteResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != LeaseIoOutcome::Ok) return result;

    const auto name = compaction_detail::ValidatedArtifactName::for_compaction_candidate_intent();
#ifdef _WIN32
    result.publish = compaction_detail::write_validated_no_replace_win(dir_handle_, name, encoded_intent);
#else
    result.publish = compaction_detail::write_validated_no_replace(dir_fd_, name, encoded_intent);
#endif
    return result;
}

inline LeaseReadResult CandidateLease::read_intent_genesis(std::span<std::byte, 140> out) noexcept {
    LeaseReadResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != LeaseIoOutcome::Ok) return result;

    const auto name = compaction_detail::ValidatedArtifactName::for_compaction_candidate_intent();
#ifdef _WIN32
    result.read = compaction_detail::read_validated_exact_win(dir_handle_, name, out);
#else
    result.read = compaction_detail::read_validated_exact(dir_fd_, name, out);
#endif
    return result;
}

inline LeaseReadResult CandidateLease::read_x1_frame(std::uint64_t build_nonce, std::uint32_t seq,
                                                        std::span<std::byte, 176> out) noexcept {
    LeaseReadResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != LeaseIoOutcome::Ok) return result;

    const auto name = compaction_detail::ValidatedArtifactName::for_x1(build_nonce, seq);
#ifdef _WIN32
    result.read = compaction_detail::read_validated_exact_win(dir_handle_, name, out);
#else
    result.read = compaction_detail::read_validated_exact(dir_fd_, name, out);
#endif
    return result;
}

// Round E receipt/raise_intent_phase() (docs/SPEC_INVARIANTS.md's "Seal-
// journal Round E receipt + raise_intent_phase() (Building->Reserved)"
// entry). Mirrors create_intent_genesis_no_replace()'s exact shape:
// check_can_operate() first, then a fixed ValidatedArtifactName, then the
// appropriate publish primitive for the platform. build_nonce/seq select the
// filename only (ValidatedArtifactName::for_x1()); the caller
// (IntentPhaseAdvancer) is responsible for having already validated
// encoded_transition via validate_intent_transition() before calling this --
// this method performs no semantic validation of the payload itself, same
// division of responsibility as create_intent_genesis_no_replace above.
inline LeaseWriteResult CandidateLease::write_x1_frame_no_replace(
    std::uint64_t build_nonce, std::uint32_t seq, std::span<const std::byte, 176> encoded_transition) noexcept {
    LeaseWriteResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != LeaseIoOutcome::Ok) return result;

    const auto name = compaction_detail::ValidatedArtifactName::for_x1(build_nonce, seq);
#ifdef _WIN32
    result.publish = compaction_detail::write_validated_no_replace_win(dir_handle_, name, encoded_transition);
#else
    result.publish = compaction_detail::write_validated_no_replace(dir_fd_, name, encoded_transition);
#endif
    return result;
}

// REPLACE, not CREATE_NEW -- see the private declaration's comment for why
// (an Intent already exists; this is a phase raise on an existing genesis).
inline LeaseWriteResult CandidateLease::replace_intent_phase(std::span<const std::byte, 140> encoded_intent) noexcept {
    LeaseWriteResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != LeaseIoOutcome::Ok) return result;

    const auto name = compaction_detail::ValidatedArtifactName::for_compaction_candidate_intent();
#ifdef _WIN32
    result.publish = compaction_detail::write_validated_replace_win(dir_handle_, name, encoded_intent);
#else
    result.publish = compaction_detail::write_validated_replace(dir_fd_, name, encoded_intent);
#endif
    return result;
}

// CREATE_NEW -- bootstrap path, this store's first-ever candidate. Refuses
// (AlreadyExists) if a watermark already exists, same as
// create_intent_genesis_no_replace's genesis discipline.
inline LeaseWriteResult CandidateLease::create_seal_id_watermark_no_replace(
    std::span<const std::byte, kSealIdWatermarkWireBytes> encoded_watermark) noexcept {
    LeaseWriteResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != LeaseIoOutcome::Ok) return result;

    const auto name = compaction_detail::ValidatedArtifactName::for_seal_id_watermark();
#ifdef _WIN32
    result.publish = compaction_detail::write_validated_no_replace_win(dir_handle_, name, encoded_watermark);
#else
    result.publish = compaction_detail::write_validated_no_replace(dir_fd_, name, encoded_watermark);
#endif
    return result;
}

// REPLACE -- every advance after the bootstrap CREATE_NEW. See
// IntentPhaseAdvancer's WatermarkAdvanceProvenanceMemory for why a bare
// REPLACE (no byte-compare-on-collision, unlike the no-replace primitive)
// requires that caller-side idempotent-retry guard rather than relying on
// anything this method itself detects.
inline LeaseWriteResult CandidateLease::replace_seal_id_watermark(
    std::span<const std::byte, kSealIdWatermarkWireBytes> encoded_watermark) noexcept {
    LeaseWriteResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != LeaseIoOutcome::Ok) return result;

    const auto name = compaction_detail::ValidatedArtifactName::for_seal_id_watermark();
#ifdef _WIN32
    result.publish = compaction_detail::write_validated_replace_win(dir_handle_, name, encoded_watermark);
#else
    result.publish = compaction_detail::write_validated_replace(dir_fd_, name, encoded_watermark);
#endif
    return result;
}

inline LeaseReadResult CandidateLease::read_seal_id_watermark(
    std::span<std::byte, kSealIdWatermarkWireBytes> out) noexcept {
    LeaseReadResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != LeaseIoOutcome::Ok) return result;

    const auto name = compaction_detail::ValidatedArtifactName::for_seal_id_watermark();
#ifdef _WIN32
    result.read = compaction_detail::read_validated_exact_win(dir_handle_, name, out);
#else
    result.read = compaction_detail::read_validated_exact(dir_fd_, name, out);
#endif
    return result;
}

inline LeaseReadResult CandidateLease::read_seal_export_started(
    bool legacy_or_greenfield, std::span<std::byte, kSealExportStartedWireBytes> out) noexcept {
    LeaseReadResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != LeaseIoOutcome::Ok) return result;

    const auto name = compaction_detail::ValidatedArtifactName::for_seal_export_started(legacy_or_greenfield);
#ifdef _WIN32
    result.read = compaction_detail::read_validated_exact_win(dir_handle_, name, out);
#else
    result.read = compaction_detail::read_validated_exact(dir_fd_, name, out);
#endif
    return result;
}

inline LeaseWriteResult CandidateLease::create_seal_export_started_no_replace(
    bool legacy_or_greenfield, std::span<const std::byte, kSealExportStartedWireBytes> encoded_started) noexcept {
    LeaseWriteResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != LeaseIoOutcome::Ok) return result;

    const auto name = compaction_detail::ValidatedArtifactName::for_seal_export_started(legacy_or_greenfield);
#ifdef _WIN32
    result.publish = compaction_detail::write_validated_no_replace_win(dir_handle_, name, encoded_started);
#else
    result.publish = compaction_detail::write_validated_no_replace(dir_fd_, name, encoded_started);
#endif
    return result;
}

inline LeaseReadResult CandidateLease::read_seal_export_started_migration(
    std::span<std::byte, kSealExportStartedMigrationWireBytes> out) noexcept {
    LeaseReadResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != LeaseIoOutcome::Ok) return result;

    const auto name = compaction_detail::ValidatedArtifactName::for_seal_export_started_migration();
#ifdef _WIN32
    result.read = compaction_detail::read_validated_exact_win(dir_handle_, name, out);
#else
    result.read = compaction_detail::read_validated_exact(dir_fd_, name, out);
#endif
    return result;
}

inline LeaseReadResult CandidateLease::read_seal_started_cleanup_tombstone(
    std::span<std::byte, kSealStartedCleanupWireBytes> out) noexcept {
    LeaseReadResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != LeaseIoOutcome::Ok) return result;

    const auto name = compaction_detail::ValidatedArtifactName::for_seal_started_cleanup_tombstone();
#ifdef _WIN32
    result.read = compaction_detail::read_validated_exact_win(dir_handle_, name, out);
#else
    result.read = compaction_detail::read_validated_exact(dir_fd_, name, out);
#endif
    return result;
}

inline LeaseReadResult CandidateLease::read_seal_started_abandon(
    std::span<std::byte, kSealStartedAbandonWireBytes> out) noexcept {
    LeaseReadResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != LeaseIoOutcome::Ok) return result;

    const auto name = compaction_detail::ValidatedArtifactName::for_seal_started_abandon();
#ifdef _WIN32
    result.read = compaction_detail::read_validated_exact_win(dir_handle_, name, out);
#else
    result.read = compaction_detail::read_validated_exact(dir_fd_, name, out);
#endif
    return result;
}

inline LeaseReadResult CandidateLease::read_compaction_intent_gc_authorized(
    std::uint64_t build_nonce, std::span<std::byte, kCompactionIntentGcAuthorizedWireBytes> out) noexcept {
    LeaseReadResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != LeaseIoOutcome::Ok) return result;

    const auto name = compaction_detail::ValidatedArtifactName::for_xgc(build_nonce);
#ifdef _WIN32
    result.read = compaction_detail::read_validated_exact_win(dir_handle_, name, out);
#else
    result.read = compaction_detail::read_validated_exact(dir_fd_, name, out);
#endif
    return result;
}

}  // namespace hy
