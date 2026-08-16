// SPDX-License-Identifier: proprietary
// seal_journal_store_lease.hpp — SealJournalStoreLease: the ONLY public
// surface for touching a seal-journal store directory's files (Round E
// breadcrumb L2 loaders, docs/SPEC_INVARIANTS.md's "Seal-journal Round E
// breadcrumb L2 loaders" entry).
//
// Governance: L2 (real file I/O). This class follows CandidateLease's
// (compaction_lease.hpp) already-six-times-reviewed shape verbatim -- read
// that file's own top-of-file comment for the five historical design
// mistakes this shape exists to prevent (forgeable evidence / TOCTOU /
// handle escape via public accessor / path traversal via free-form
// filenames / release() with no owner-thread check). This is a NEW class
// rather than an extension of CandidateLease because the directory is
// genuinely different: `seal-journal/<store_uuid_lo_hex16>
// <store_uuid_hi_hex16>/` (durable_control_plane.hpp:750/871,
// BINANCE_PRIVATE_REST_L4_SPEC.md:4392), never listed alongside
// compaction-candidate-intent/seal-export-started's breadcrumb directory in
// any filename enumeration in this repo's specs.
//
// SCOPE: was read-only through the breadcrumb-loaders round. The
// "Seal-journal Round E SealJournalCommitWatermark CREATE_NEW + two-lock
// coordination" entry adds this class's first write method
// (create_seal_journal_commit_watermark_no_replace, CREATE_NEW only, no
// REPLACE/delete of any kind) plus its first friend outside the two
// loaders/test-accessor: `IntentPhaseAdvancer`. Two friend-only typed read
// methods (SealJournalCommitWatermark's `.jhw`, SealJournalTombstoneWire's
// `.jts`) remain unchanged. Unlike CandidateLease, this directory holds
// MANY candidates' files side by side (`<candidate_id_hex16>.jhw`/
// `<candidate_id_hex16>-<journal_seq_hex16>.jts`, selected by filename, not
// one lease instance per candidate) -- do not read the "Store" in the class
// name as "one instance per store forever"; a fresh instance is constructed
// per acquire/release cycle the same way CandidateLease is, it is simply
// not scoped to a single candidate_id the way CandidateLease's directory is
// scoped to a single compaction candidate.
//
// See windows_native_io.hpp / seal_journal_store_io.hpp for the
// platform-specific mechanics this class is built on.
//
// LOCK ORDER (first time this codebase has ever needed one -- read this
// before acquiring both this class and CandidateLease in the same scope):
// CandidateLease MUST be acquired before SealJournalStoreLease, and
// released after it (SealJournalStoreLease released first). Intent is the
// single source of truth CandidateLease guards; SealJournalStoreLease
// guards satellite state outside it. This order is documentation-enforced
// only -- there is no compiler mechanism preventing a future caller from
// acquiring them in the opposite order and deadlocking against a
// concurrent caller that follows this rule (same class of honest
// limitation as this codebase's friend-isolation comments elsewhere).
// IntentPhaseAdvancer (intent_phase_advancer.hpp) is the one caller today;
// it does not acquire either lease itself, it only uses two already-held
// references, so this rule binds whoever constructs an IntentPhaseAdvancer,
// not IntentPhaseAdvancer's own code.

#pragma once

#include <hengyuan/durable_control_plane.hpp>  // kSealJournalCommitWatermarkWireBytes and friends
#include <hengyuan/seal_journal_store_io.hpp>

#ifdef _WIN32
#include <hengyuan/windows_native_io.hpp>
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>  // flock()
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hy {

enum class SealJournalLeaseAcquireStatus : std::uint8_t {
    Acquired,
    HeldElsewhere,
    DirectoryMissing,
    RejectedSymlinkOrReparse,
    Failed,
};

enum class SealJournalLeaseReleaseStatus : std::uint8_t {
    Released,
    WrongOwner,  // a non-owner thread called release() -- handle untouched
    NotHeld,
};

// Fixed-size, no heap-allocated path strings -- same shape as
// compaction_lease.hpp's CandidateIdentityDiagnostic, duplicated (not
// reused) because that struct is a member of a different, independently-
// reviewed trust boundary.
struct SealJournalStoreIdentityDiagnostic {
    std::uint64_t expected_dev_or_volume_serial{0};
    std::uint64_t expected_inode_or_file_index{0};
    std::uint64_t observed_dev_or_volume_serial{0};  // 0 if observed_path_missing
    std::uint64_t observed_inode_or_file_index{0};
    bool observed_path_missing{false};
};

enum class SealJournalLeaseIoOutcome : std::uint8_t {
    Ok,                        // admitted -- see the accompanying ReadFixedResult
    WrongOwner,
    NotHeld,
    StoreDirFenced,            // sticky fence already set; filesystem NOT re-touched
    DirectoryIdentityChanged,  // fence set BY this call (first detection)
};

struct SealJournalLeaseReadResult {
    SealJournalLeaseIoOutcome outcome{SealJournalLeaseIoOutcome::WrongOwner};
    seal_journal_store_detail::ReadFixedResult read{};  // meaningful only when outcome == Ok
};

struct SealJournalLeaseWriteResult {
    SealJournalLeaseIoOutcome outcome{SealJournalLeaseIoOutcome::WrongOwner};
    seal_journal_store_detail::PublishResult publish{};  // meaningful only when outcome == Ok
};

namespace seal_journal_store_detail_impl {

inline std::size_t hash_this_thread() noexcept {
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

}  // namespace seal_journal_store_detail_impl

// Forward-declared friends -- Round E's two per-store loader classes
// (seal_journal_commit_tombstone_loader.hpp).
class SealJournalCommitWatermarkLoader;
class SealJournalTombstoneLoader;

// Forward-declared friend -- intent_phase_advancer.hpp's write path. See
// that class's own file for the full design rationale and its SCOPE
// comment for why this is test-only, not wired into any production call
// path.
class IntentPhaseAdvancer;

namespace test_only {
// Pre-integration test accessor -- see the friend declaration inside
// SealJournalStoreLease below. Deliberately not reachable from any
// production include; only test_seal_journal_store_lease.cpp defines/uses
// this type.
class SealJournalStoreLeaseTestAccess;
}  // namespace test_only

class SealJournalStoreLease {
public:
    // The path is used only to acquire the directory handle and to perform
    // periodic identity re-checks; artifact reads are always handle-relative.
    // Windows caches the NT-prefixed representation at construction so the
    // re-check does not allocate, but never uses it to reconstruct a file
    // read path.
    explicit SealJournalStoreLease(std::filesystem::path store_dir) : store_dir_(std::move(store_dir)) {
#ifdef _WIN32
        // Cache the NT-prefixed path outside acquire()/load(). The periodic
        // identity check can then reopen this directory without constructing
        // a std::wstring on the zero-allocation loader path.
        const auto& native_path = store_dir_.native();
        store_dir_nt_path_.reserve(4 + native_path.size());
        store_dir_nt_path_.append(L"\\??\\");
        store_dir_nt_path_.append(native_path);
#endif
    }

    ~SealJournalStoreLease() {
#ifndef NDEBUG
        if (held_.load(std::memory_order_relaxed)) {
            assert(seal_journal_store_detail_impl::hash_this_thread() ==
                   owner_thread_hash_.load(std::memory_order_relaxed));
        }
#endif
        release_handles();
    }

    SealJournalStoreLease(const SealJournalStoreLease&) = delete;
    SealJournalStoreLease& operator=(const SealJournalStoreLease&) = delete;

    SealJournalStoreLease(SealJournalStoreLease&& other) noexcept
        : owner_thread_hash_(other.owner_thread_hash_.load(std::memory_order_relaxed)),
          held_(other.held_.load(std::memory_order_relaxed)),
          fenced_(other.fenced_.load(std::memory_order_relaxed)),
          last_diagnostic_(other.last_diagnostic_),
#ifdef _WIN32
          store_dir_(std::move(other.store_dir_)),
          store_dir_nt_path_(std::move(other.store_dir_nt_path_))
#else
          store_dir_(std::move(other.store_dir_))
#endif
    {
        move_handles_from(other);
        other.held_.store(false, std::memory_order_relaxed);
        other.fenced_.store(false, std::memory_order_relaxed);
    }
    SealJournalStoreLease& operator=(SealJournalStoreLease&& other) noexcept {
        if (this == &other) return *this;
        release_handles();
        owner_thread_hash_.store(other.owner_thread_hash_.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
        held_.store(other.held_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        fenced_.store(other.fenced_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        last_diagnostic_ = other.last_diagnostic_;
        store_dir_ = std::move(other.store_dir_);
#ifdef _WIN32
        store_dir_nt_path_ = std::move(other.store_dir_nt_path_);
#endif
        move_handles_from(other);
        other.held_.store(false, std::memory_order_relaxed);
        other.fenced_.store(false, std::memory_order_relaxed);
        return *this;
    }

    SealJournalLeaseAcquireStatus acquire() noexcept;
    SealJournalLeaseReleaseStatus release() noexcept;
    bool held() const noexcept { return held_.load(std::memory_order_acquire); }
    bool fenced() const noexcept { return fenced_.load(std::memory_order_acquire); }
    std::optional<SealJournalStoreIdentityDiagnostic> last_identity_diagnostic() const noexcept {
        return last_diagnostic_;
    }

private:
    friend class hy::SealJournalCommitWatermarkLoader;
    friend class hy::SealJournalTombstoneLoader;
    // Coordinator's own pre-integration test accessor -- Modules 1-4's real
    // loader classes above don't exist yet at the point this class lands,
    // so this round's own tests need a friend to drive the two methods
    // through. Harmless to keep permanently once the real loaders land.
    friend class hy::test_only::SealJournalStoreLeaseTestAccess;
    // See this file's header comment's LOCK ORDER section and
    // intent_phase_advancer.hpp's own friend-isolation comment (identical
    // C++ friendship-is-class-scoped-not-method-scoped limitation applies
    // here too).
    friend class hy::IntentPhaseAdvancer;

    SealJournalLeaseReadResult read_seal_journal_commit_watermark(
        std::uint64_t candidate_id, std::span<std::byte, kSealJournalCommitWatermarkWireBytes> out) noexcept;
    // CREATE_NEW only -- this class has no REPLACE/delete primitive.
    // candidate_id selects the filename (SealJournalArtifactName::
    // for_commit_watermark), same division of responsibility as
    // CandidateLease's write methods: this method performs no semantic
    // validation of encoded_watermark itself, the caller is responsible for
    // having already encoded a value this candidate_id is legally allowed
    // to bind (IntentPhaseAdvancer only ever calls this once per
    // build_nonce, immediately after deriving that exact candidate_id from
    // the SealIdWatermark advance).
    SealJournalLeaseWriteResult create_seal_journal_commit_watermark_no_replace(
        std::uint64_t candidate_id,
        std::span<const std::byte, kSealJournalCommitWatermarkWireBytes> encoded_watermark) noexcept;
    SealJournalLeaseReadResult read_seal_journal_tombstone(
        std::uint64_t candidate_id, std::uint64_t journal_seq,
        std::span<std::byte, kSealJournalTombstoneBytes> out) noexcept;

    SealJournalLeaseIoOutcome check_can_operate() noexcept;
    bool identity_still_matches_path() noexcept;
    void release_handles() noexcept;
    void move_handles_from(SealJournalStoreLease& other) noexcept;

    std::atomic<std::size_t> owner_thread_hash_{0};
    std::atomic<bool> held_{false};
    std::atomic<bool> fenced_{false};
    std::optional<SealJournalStoreIdentityDiagnostic> last_diagnostic_{};
    std::filesystem::path store_dir_;

#ifdef _WIN32
    std::wstring store_dir_nt_path_;
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
// Windows-only: read_validated_exact_win, the direct analogue of
// compaction_lease.hpp's own copy -- duplicated (not reused) for the same
// trust-boundary reason seal_journal_store_io.hpp states at its top.
// ===========================================================================
#ifdef _WIN32

namespace seal_journal_store_detail_impl {

inline std::wstring_view ascii_to_wide(
    std::string_view s,
    std::array<wchar_t, seal_journal_store_detail::kMaxArtifactNameLen>& out) noexcept {
    assert(s.size() <= out.size());
    if (s.size() > out.size()) return {};
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<wchar_t>(static_cast<unsigned char>(s[i]));
    }
    return std::wstring_view(out.data(), s.size());
}

inline seal_journal_store_detail::ReadFixedResult read_validated_exact_win(
    const win_native::RawHandle& dir, const seal_journal_store_detail::SealJournalArtifactName& name,
    std::span<std::byte> out) noexcept {
    seal_journal_store_detail::ReadFixedResult result{};
    std::array<wchar_t, seal_journal_store_detail::kMaxArtifactNameLen> final_name_storage{};
    const std::wstring_view final_name = ascii_to_wide(name.relative_name(), final_name_storage);

    win_native::RawHandle h;
    const auto open_result = win_native::open_existing_relative(dir, final_name, h);
    if (open_result == win_native::RelativeOpenResult::NotFound) {
        result.status = seal_journal_store_detail::ReadFixedStatus::NotFound;
        return result;
    }
    if (open_result == win_native::RelativeOpenResult::RejectedReparsePoint) {
        result.status = seal_journal_store_detail::ReadFixedStatus::NotRegularFile;
        return result;
    }
    if (open_result != win_native::RelativeOpenResult::Opened) {
        result.status = seal_journal_store_detail::ReadFixedStatus::IoError;
        return result;
    }

    DWORD read_n = 0;
    if (!::ReadFile(h.get(), out.data(), static_cast<DWORD>(out.size()), &read_n, nullptr)) {
        result.status = seal_journal_store_detail::ReadFixedStatus::IoError;
        return result;
    }
    if (read_n != out.size()) {
        result.status = seal_journal_store_detail::ReadFixedStatus::WrongSize;
        return result;
    }
    std::array<std::byte, 1> probe{};
    DWORD probe_n = 0;
    ::ReadFile(h.get(), probe.data(), 1, &probe_n, nullptr);
    if (probe_n > 0) {
        result.status = seal_journal_store_detail::ReadFixedStatus::WrongSize;  // file longer than expected
        return result;
    }
    result.status = seal_journal_store_detail::ReadFixedStatus::Ok;
    return result;
}

inline std::wstring make_unique_tmp_name_win(std::string_view final_name) noexcept {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    std::array<wchar_t, seal_journal_store_detail::kMaxArtifactNameLen> final_name_storage{};
    std::wstring out(ascii_to_wide(final_name, final_name_storage));
    out += L".tmp-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(n);
    return out;
}

// Direct analogue of compaction_lease.hpp's write_validated_no_replace_win
// -- duplicated (not reused) for the same trust-boundary reason
// seal_journal_store_io.hpp states at its top. CREATE_NEW only, same as
// this file's POSIX write_validated_no_replace().
inline seal_journal_store_detail::PublishResult write_validated_no_replace_win(
    const win_native::RawHandle& dir, const seal_journal_store_detail::SealJournalArtifactName& name,
    std::span<const std::byte> bytes) noexcept {
    seal_journal_store_detail::PublishResult result{};
    std::array<wchar_t, seal_journal_store_detail::kMaxArtifactNameLen> final_name_storage{};
    const std::wstring_view final_name = ascii_to_wide(name.relative_name(), final_name_storage);
    const std::wstring tmp_name = make_unique_tmp_name_win(name.relative_name());

    win_native::RawHandle tmp;
    if (win_native::create_new_relative(dir, tmp_name, tmp) != win_native::RelativeCreateResult::Created) {
        result.state = seal_journal_store_detail::PublishCommitState::NotPublished;
        return result;
    }

    DWORD written = 0;
    BOOL ok = ::WriteFile(tmp.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    ok = ok && written == static_cast<DWORD>(bytes.size());
    ok = ok && ::FlushFileBuffers(tmp.get());
    if (!ok) {
        result.state = seal_journal_store_detail::PublishCommitState::NotPublished;
        return result;
    }

    NTSTATUS rename_status = 0;
    const auto rename_result =
        win_native::rename_no_replace(tmp, dir, std::wstring(final_name), &rename_status);
    if (rename_result == win_native::RelativeRenameResult::AlreadyExists) {
        tmp.reset();
        win_native::RawHandle existing;
        if (win_native::open_existing_relative(dir, final_name, existing) !=
            win_native::RelativeOpenResult::Opened) {
            result.state = seal_journal_store_detail::PublishCommitState::NotPublished;
            return result;
        }
        std::vector<std::byte> existing_bytes(bytes.size() + 1);
        DWORD read_n = 0;
        if (!::ReadFile(existing.get(), existing_bytes.data(), static_cast<DWORD>(existing_bytes.size()),
                         &read_n, nullptr)) {
            result.state = seal_journal_store_detail::PublishCommitState::NotPublished;
            return result;
        }
        const bool byte_equal =
            read_n == bytes.size() && std::memcmp(existing_bytes.data(), bytes.data(), bytes.size()) == 0;
        if (byte_equal) {
            result.state = seal_journal_store_detail::PublishCommitState::DurablyPublished;
            result.provenance = seal_journal_store_detail::PublishProvenance::FoundPreExisting;
            return result;
        }
        result.state = seal_journal_store_detail::PublishCommitState::NotPublished;
        return result;
    }
    if (rename_result != win_native::RelativeRenameResult::Renamed) {
        result.state = seal_journal_store_detail::PublishCommitState::NotPublished;
        return result;
    }
    tmp.reset();

    if (::FlushFileBuffers(dir.get())) {
        result.state = seal_journal_store_detail::PublishCommitState::DurablyPublished;
        result.provenance = seal_journal_store_detail::PublishProvenance::CreatedThisCallDurable;
    } else {
        result.state = seal_journal_store_detail::PublishCommitState::PublishedNamespaceUncertain;
        result.provenance = seal_journal_store_detail::PublishProvenance::CreatedThisCallUncertain;
    }
    return result;
}

}  // namespace seal_journal_store_detail_impl

#endif  // _WIN32

// ===========================================================================
// SealJournalStoreLease method definitions
// ===========================================================================

inline void SealJournalStoreLease::release_handles() noexcept {
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

inline void SealJournalStoreLease::move_handles_from(SealJournalStoreLease& other) noexcept {
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

inline SealJournalLeaseAcquireStatus SealJournalStoreLease::acquire() noexcept {
    owner_thread_hash_.store(seal_journal_store_detail_impl::hash_this_thread(), std::memory_order_release);

#ifdef _WIN32
    const auto dir_result = win_native::open_directory_nt_path(store_dir_nt_path_, dir_handle_);
    if (dir_result == win_native::DirOpenResult::NotFound) return SealJournalLeaseAcquireStatus::DirectoryMissing;
    if (dir_result == win_native::DirOpenResult::RejectedReparsePoint) {
        return SealJournalLeaseAcquireStatus::RejectedSymlinkOrReparse;
    }
    if (dir_result != win_native::DirOpenResult::Opened) return SealJournalLeaseAcquireStatus::Failed;

    if (!win_native::query_file_identity(dir_handle_, expected_identity_)) {
        dir_handle_.reset();
        return SealJournalLeaseAcquireStatus::Failed;
    }

    const auto lock_result =
        win_native::open_or_create_exclusive_relative(dir_handle_, L"seal-journal-store.lock", lock_handle_);
    if (lock_result == win_native::ExclusiveLockResult::HeldElsewhere) {
        dir_handle_.reset();
        return SealJournalLeaseAcquireStatus::HeldElsewhere;
    }
    if (lock_result != win_native::ExclusiveLockResult::Acquired) {
        dir_handle_.reset();
        return SealJournalLeaseAcquireStatus::Failed;
    }

    held_.store(true, std::memory_order_release);
    return SealJournalLeaseAcquireStatus::Acquired;
#else
    dir_fd_ = ::open(store_dir_.c_str(), O_DIRECTORY | O_NOFOLLOW);
    if (dir_fd_ < 0) {
        if (errno == ENOENT) return SealJournalLeaseAcquireStatus::DirectoryMissing;
        if (errno == ELOOP || errno == ENOTDIR) return SealJournalLeaseAcquireStatus::RejectedSymlinkOrReparse;
        return SealJournalLeaseAcquireStatus::Failed;
    }
    struct stat st {};
    if (::fstat(dir_fd_, &st) != 0) {
        ::close(dir_fd_);
        dir_fd_ = -1;
        return SealJournalLeaseAcquireStatus::Failed;
    }
    expected_dev_ = static_cast<std::uint64_t>(st.st_dev);
    expected_ino_ = static_cast<std::uint64_t>(st.st_ino);

    lock_fd_ = ::openat(dir_fd_, "seal-journal-store.lock", O_CREAT | O_RDWR, 0600);
    if (lock_fd_ < 0) {
        ::close(dir_fd_);
        dir_fd_ = -1;
        return SealJournalLeaseAcquireStatus::Failed;
    }
    if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
        const bool held_elsewhere = (errno == EWOULDBLOCK);
        ::close(lock_fd_);
        lock_fd_ = -1;
        ::close(dir_fd_);
        dir_fd_ = -1;
        return held_elsewhere ? SealJournalLeaseAcquireStatus::HeldElsewhere : SealJournalLeaseAcquireStatus::Failed;
    }

    held_.store(true, std::memory_order_release);
    return SealJournalLeaseAcquireStatus::Acquired;
#endif
}

inline SealJournalLeaseReleaseStatus SealJournalStoreLease::release() noexcept {
    // held_ checked before owner-thread -- same reordering fix as
    // CandidateLease::release(), for the same reason (a never-acquired
    // lease must report NotHeld, not WrongOwner).
    if (!held_.load(std::memory_order_acquire)) {
        return SealJournalLeaseReleaseStatus::NotHeld;
    }
    if (seal_journal_store_detail_impl::hash_this_thread() != owner_thread_hash_.load(std::memory_order_acquire)) {
        return SealJournalLeaseReleaseStatus::WrongOwner;  // handle untouched
    }
    release_handles();
    held_.store(false, std::memory_order_release);
    return SealJournalLeaseReleaseStatus::Released;
}

inline bool SealJournalStoreLease::identity_still_matches_path() noexcept {
#ifdef _WIN32
    win_native::RawHandle probe;
    if (win_native::open_directory_nt_path(store_dir_nt_path_, probe) != win_native::DirOpenResult::Opened) {
        return false;
    }
    win_native::FileIdentity observed{};
    if (!win_native::query_file_identity(probe, observed)) return false;
    return observed.volume_serial == expected_identity_.volume_serial &&
           observed.file_index == expected_identity_.file_index;
#else
    struct stat st {};
    if (::stat(store_dir_.c_str(), &st) != 0) return false;
    return static_cast<std::uint64_t>(st.st_dev) == expected_dev_ &&
           static_cast<std::uint64_t>(st.st_ino) == expected_ino_;
#endif
}

inline SealJournalLeaseIoOutcome SealJournalStoreLease::check_can_operate() noexcept {
    if (!held_.load(std::memory_order_acquire)) {
        return SealJournalLeaseIoOutcome::NotHeld;
    }
    if (seal_journal_store_detail_impl::hash_this_thread() != owner_thread_hash_.load(std::memory_order_acquire)) {
        return SealJournalLeaseIoOutcome::WrongOwner;
    }
    if (fenced_.load(std::memory_order_acquire)) {
        return SealJournalLeaseIoOutcome::StoreDirFenced;
    }
    if (identity_still_matches_path()) {
        return SealJournalLeaseIoOutcome::Ok;
    }

    // First detection -- populate the fixed-size diagnostic and set the
    // sticky fence. Every subsequent call short-circuits at the fenced_
    // check above, without reaching this block again.
    SealJournalStoreIdentityDiagnostic diag{};
#ifdef _WIN32
    diag.expected_dev_or_volume_serial = expected_identity_.volume_serial;
    diag.expected_inode_or_file_index = expected_identity_.file_index;
    win_native::RawHandle probe;
    win_native::FileIdentity observed{};
    if (win_native::open_directory_nt_path(store_dir_nt_path_, probe) == win_native::DirOpenResult::Opened &&
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
    if (::stat(store_dir_.c_str(), &st) == 0) {
        diag.observed_dev_or_volume_serial = static_cast<std::uint64_t>(st.st_dev);
        diag.observed_inode_or_file_index = static_cast<std::uint64_t>(st.st_ino);
        diag.observed_path_missing = false;
    } else {
        diag.observed_path_missing = true;
    }
#endif
    last_diagnostic_ = diag;
    fenced_.store(true, std::memory_order_release);
    return SealJournalLeaseIoOutcome::DirectoryIdentityChanged;
}

inline SealJournalLeaseReadResult SealJournalStoreLease::read_seal_journal_commit_watermark(
    std::uint64_t candidate_id, std::span<std::byte, kSealJournalCommitWatermarkWireBytes> out) noexcept {
    SealJournalLeaseReadResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != SealJournalLeaseIoOutcome::Ok) return result;

    const auto name = seal_journal_store_detail::SealJournalArtifactName::for_commit_watermark(candidate_id);
#ifdef _WIN32
    result.read = seal_journal_store_detail_impl::read_validated_exact_win(dir_handle_, name, out);
#else
    result.read = seal_journal_store_detail::read_validated_exact(dir_fd_, name, out);
#endif
    return result;
}

inline SealJournalLeaseWriteResult SealJournalStoreLease::create_seal_journal_commit_watermark_no_replace(
    std::uint64_t candidate_id,
    std::span<const std::byte, kSealJournalCommitWatermarkWireBytes> encoded_watermark) noexcept {
    SealJournalLeaseWriteResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != SealJournalLeaseIoOutcome::Ok) return result;

    const auto name = seal_journal_store_detail::SealJournalArtifactName::for_commit_watermark(candidate_id);
#ifdef _WIN32
    result.publish = seal_journal_store_detail_impl::write_validated_no_replace_win(dir_handle_, name,
                                                                                      encoded_watermark);
#else
    result.publish = seal_journal_store_detail::write_validated_no_replace(dir_fd_, name, encoded_watermark);
#endif
    return result;
}

inline SealJournalLeaseReadResult SealJournalStoreLease::read_seal_journal_tombstone(
    std::uint64_t candidate_id, std::uint64_t journal_seq,
    std::span<std::byte, kSealJournalTombstoneBytes> out) noexcept {
    SealJournalLeaseReadResult result{};
    result.outcome = check_can_operate();
    if (result.outcome != SealJournalLeaseIoOutcome::Ok) return result;

    const auto name = seal_journal_store_detail::SealJournalArtifactName::for_tombstone(candidate_id, journal_seq);
#ifdef _WIN32
    result.read = seal_journal_store_detail_impl::read_validated_exact_win(dir_handle_, name, out);
#else
    result.read = seal_journal_store_detail::read_validated_exact(dir_fd_, name, out);
#endif
    return result;
}

}  // namespace hy
