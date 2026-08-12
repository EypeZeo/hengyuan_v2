// Real-filesystem tests for compaction_lease.hpp's CandidateLease --
// directory rename/recreate identity fencing, non-owner release(), lease
// mutual exclusion (same-process and cross-process), and IntentStore's
// genesis write/read via the friend relationship. Governance: L2 (real
// file I/O).
//
// Also covers the six Round E breadcrumb L2 loader read methods added to
// CandidateLease this round (docs/SPEC_INVARIANTS.md's "Seal-journal Round E
// breadcrumb L2 loaders" entry), driven through
// hy::test_only::CandidateLeaseSealBreadcrumbTestAccess -- see that class's
// own comment for why a test-only friend exists (Modules 1-4's real loader
// classes are built independently and don't exist at the point this lease
// extension lands).
#include <gtest/gtest.h>
#include <hengyuan/compaction_lease.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

using namespace hy;

namespace {

std::filesystem::path make_temp_candidate_dir(std::string_view tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("hy_round_d_lease_" + std::string(tag) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

// Minimal, test-only cross-process launch/wait -- not production code, so it
// stays here rather than in a header. Deliberately narrow: this is only ever
// used to launch compaction_lease_holder with exactly three positional args.
struct ChildProcess {
#ifdef _WIN32
    PROCESS_INFORMATION pi{};
#else
    pid_t pid{-1};
#endif
};

bool spawn_child(const std::string& exe, const std::vector<std::string>& args, ChildProcess& out) {
#ifdef _WIN32
    std::string cmdline = "\"" + exe + "\"";
    for (const auto& a : args) cmdline += " \"" + a + "\"";
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    out.pi = pi;
    return true;
#else
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ);
    if (rc != 0) return false;
    out.pid = pid;
    return true;
#endif
}

// Blocks until the child exits; returns its exit code (-1 on a wait failure
// or an abnormal, non-exit termination).
int wait_child(ChildProcess& child) {
#ifdef _WIN32
    ::WaitForSingleObject(child.pi.hProcess, INFINITE);
    DWORD code = 1;
    ::GetExitCodeProcess(child.pi.hProcess, &code);
    ::CloseHandle(child.pi.hProcess);
    ::CloseHandle(child.pi.hThread);
    return static_cast<int>(code);
#else
    int status = 0;
    if (::waitpid(child.pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

}  // namespace

namespace hy::test_only {

// Exposes CandidateLease's six Round E breadcrumb read methods as public
// static wrappers, purely for this test file -- see the friend declaration
// and its comment inside compaction_lease.hpp. NOT production code; no
// production header includes this.
class CandidateLeaseSealBreadcrumbTestAccess {
public:
    static LeaseReadResult read_seal_id_watermark(CandidateLease& lease,
                                                     std::span<std::byte, kSealIdWatermarkWireBytes> out) noexcept {
        return lease.read_seal_id_watermark(out);
    }
    static LeaseReadResult read_seal_export_started(
        CandidateLease& lease, bool legacy_or_greenfield,
        std::span<std::byte, kSealExportStartedWireBytes> out) noexcept {
        return lease.read_seal_export_started(legacy_or_greenfield, out);
    }
    static LeaseReadResult read_seal_export_started_migration(
        CandidateLease& lease, std::span<std::byte, kSealExportStartedMigrationWireBytes> out) noexcept {
        return lease.read_seal_export_started_migration(out);
    }
    static LeaseReadResult read_seal_started_cleanup_tombstone(
        CandidateLease& lease, std::span<std::byte, kSealStartedCleanupWireBytes> out) noexcept {
        return lease.read_seal_started_cleanup_tombstone(out);
    }
    static LeaseReadResult read_seal_started_abandon(
        CandidateLease& lease, std::span<std::byte, kSealStartedAbandonWireBytes> out) noexcept {
        return lease.read_seal_started_abandon(out);
    }
};

}  // namespace hy::test_only

namespace {

using hy::test_only::CandidateLeaseSealBreadcrumbTestAccess;

// Writes `bytes.size()` bytes of raw content to `dir / filename` -- these
// tests exercise the lease's own I/O plumbing (fixed filename resolution,
// exact-size read, fencing), never a real codec, so the content itself is
// an arbitrary recognizable pattern, not a valid encoded wire record.
void write_raw_file(const std::filesystem::path& dir, std::string_view filename, std::span<const std::byte> bytes) {
    std::ofstream out(dir / std::string(filename), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

template <std::size_t N>
std::array<std::byte, N> pattern_bytes(std::uint8_t seed) {
    std::array<std::byte, N> buf{};
    for (std::size_t i = 0; i < N; ++i) buf[i] = static_cast<std::byte>(seed + i);
    return buf;
}

}  // namespace

TEST(CandidateLease, AcquireSucceedsOnFreshDirectory) {
    auto dir = make_temp_candidate_dir("acquire_ok");
    CandidateLease lease(dir);
    EXPECT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    EXPECT_TRUE(lease.held());
    EXPECT_FALSE(lease.fenced());
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);  // must release before removing the
                                                            // directory -- Windows keeps a
                                                            // directory in use for as long as any
                                                            // handle to it (or a file inside it)
                                                            // remains open
    std::filesystem::remove_all(dir);
}

TEST(CandidateLease, AcquireFailsOnMissingDirectory) {
    auto dir = make_temp_candidate_dir("missing") / "does_not_exist";
    CandidateLease lease(dir);
    EXPECT_EQ(lease.acquire(), LeaseAcquireStatus::DirectoryMissing);
}

TEST(CandidateLease, SecondInstanceCannotAcquireSameDirectory) {
    auto dir = make_temp_candidate_dir("mutex");
    CandidateLease lease1(dir);
    ASSERT_EQ(lease1.acquire(), LeaseAcquireStatus::Acquired);

    CandidateLease lease2(dir);
    EXPECT_EQ(lease2.acquire(), LeaseAcquireStatus::HeldElsewhere);

    ASSERT_EQ(lease1.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(CandidateLease, ReacquireAfterReleaseSucceeds) {
    auto dir = make_temp_candidate_dir("reacquire");
    CandidateLease lease1(dir);
    ASSERT_EQ(lease1.acquire(), LeaseAcquireStatus::Acquired);
    ASSERT_EQ(lease1.release(), ReleaseStatus::Released);

    CandidateLease lease2(dir);
    EXPECT_EQ(lease2.acquire(), LeaseAcquireStatus::Acquired);
    ASSERT_EQ(lease2.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(CandidateLease, NonOwnerReleaseReturnsWrongOwnerAndDoesNotClose) {
    auto dir = make_temp_candidate_dir("nonowner_release");
    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);

    ReleaseStatus non_owner_result{};
    std::thread other([&] { non_owner_result = lease.release(); });
    other.join();

    EXPECT_EQ(non_owner_result, ReleaseStatus::WrongOwner);
    EXPECT_TRUE(lease.held()) << "non-owner release() must not have actually released the lease";

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);  // owner thread can still release
    std::filesystem::remove_all(dir);
}

TEST(CandidateLease, ReleaseWithoutAcquireIsNotHeld) {
    auto dir = make_temp_candidate_dir("not_held");
    CandidateLease lease(dir);
    EXPECT_EQ(lease.release(), ReleaseStatus::NotHeld);
}

TEST(CandidateLease, MoveTransfersHeldStateAndOwnerThread) {
    auto dir = make_temp_candidate_dir("move");
    CandidateLease lease1(dir);
    ASSERT_EQ(lease1.acquire(), LeaseAcquireStatus::Acquired);

    CandidateLease lease2(std::move(lease1));
    EXPECT_TRUE(lease2.held());
    EXPECT_FALSE(lease1.held());  // moved-from

    ASSERT_EQ(lease2.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

// CandidateLease's write/read methods (create_intent_genesis_no_replace/
// read_intent_genesis/read_x1_frame) are private with IntentStore as the
// ONLY friend -- by design, nothing in this file can call them directly.
// Their coverage (round-trip, uncertain-provenance memory, directory
// identity fencing across a real write attempt, etc.) lives in
// test_compaction_intent_store.cpp, driven through the real IntentStore.
// This file's job is CandidateLease's own lifecycle surface: acquire/
// release/held/fenced/move, which are all public.

// Real cross-process mutual exclusion: acquire()'s HeldElsewhere/Acquired
// outcomes are backed by an OS-level exclusive lock (NT native share-mode
// on Windows, flock(LOCK_EX|LOCK_NB) on POSIX) -- not an in-process mutex --
// so the only faithful test is a genuine second process, spawned via
// compaction_lease_holder (tests/helpers/compaction_lease_holder.cpp, built
// as its own executable, path injected by CMake as
// HY_COMPACTION_LEASE_HOLDER_EXE).
TEST(CandidateLease, CrossProcessMutualExclusion) {
    auto dir = make_temp_candidate_dir("cross_process");
    auto status_file = std::filesystem::temp_directory_path() /
                        ("hy_round_d_lease_holder_status_" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    std::filesystem::remove(status_file);

    ChildProcess child;
    ASSERT_TRUE(spawn_child(HY_COMPACTION_LEASE_HOLDER_EXE, {dir.string(), status_file.string(), "1500"}, child))
        << "failed to launch " << HY_COMPACTION_LEASE_HOLDER_EXE;

    // Poll the status file for the helper's own acquire() outcome -- bounded
    // wait, not a fixed sleep, since process start + first syscalls has no
    // guaranteed upper bound under load.
    std::string status_content;
    bool observed = false;
    for (int i = 0; i < 100 && !observed; ++i) {
        std::ifstream in(status_file);
        if (in && std::getline(in, status_content) && !status_content.empty()) {
            observed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(observed) << "helper never wrote a status file within 5s";
    ASSERT_EQ(status_content, "ACQUIRED") << "helper reported: " << status_content;

    // The helper still holds the lease (it sleeps 1500ms before releasing)
    // -- a second acquire() from THIS process must see HeldElsewhere.
    CandidateLease lease(dir);
    EXPECT_EQ(lease.acquire(), LeaseAcquireStatus::HeldElsewhere);

    const int exit_code = wait_child(child);
    EXPECT_EQ(exit_code, 0) << "helper process did not cleanly acquire+release";

    // Now that the helper has exited (and released), the same CandidateLease
    // instance's next acquire() attempt must succeed.
    EXPECT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    EXPECT_EQ(lease.release(), ReleaseStatus::Released);

    std::filesystem::remove(status_file);
    std::filesystem::remove_all(dir);
}

// ===========================================================================
// Round E breadcrumb L2 loaders: CandidateLease's six new read methods,
// driven through CandidateLeaseSealBreadcrumbTestAccess (see its own
// comment). Confirms the shared-directory design decision (extend
// CandidateLease rather than mint a second lease class) actually works:
// these new methods go through the exact same check_can_operate() gate as
// the original read_intent_genesis/read_x1_frame, so fencing/ownership
// apply uniformly.
// ===========================================================================

TEST(SealBreadcrumbLoaders, RoundTripsSealIdWatermark) {
    auto dir = make_temp_candidate_dir("seal_id_watermark_rt");
    const auto content = pattern_bytes<kSealIdWatermarkWireBytes>(0x10);
    write_raw_file(dir, "seal-id-watermark", content);

    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);

    std::array<std::byte, kSealIdWatermarkWireBytes> out{};
    const auto result = CandidateLeaseSealBreadcrumbTestAccess::read_seal_id_watermark(lease, out);
    EXPECT_EQ(result.outcome, LeaseIoOutcome::Ok);
    EXPECT_EQ(result.read.status, compaction_detail::ReadFixedStatus::Ok);
    EXPECT_EQ(0, std::memcmp(out.data(), content.data(), content.size()));

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealBreadcrumbLoaders, RoundTripsSealExportStartedLegacyAndMigrationCompanionIndependently) {
    auto dir = make_temp_candidate_dir("seal_export_started_rt");
    const auto legacy_content = pattern_bytes<kSealExportStartedWireBytes>(0x20);
    const auto companion_content = pattern_bytes<kSealExportStartedWireBytes>(0x40);
    write_raw_file(dir, "seal-export-started", legacy_content);
    write_raw_file(dir, "seal-export-started.v2", companion_content);

    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);

    std::array<std::byte, kSealExportStartedWireBytes> legacy_out{};
    const auto legacy_result =
        CandidateLeaseSealBreadcrumbTestAccess::read_seal_export_started(lease, /*legacy_or_greenfield=*/true, legacy_out);
    EXPECT_EQ(legacy_result.outcome, LeaseIoOutcome::Ok);
    EXPECT_EQ(legacy_result.read.status, compaction_detail::ReadFixedStatus::Ok);
    EXPECT_EQ(0, std::memcmp(legacy_out.data(), legacy_content.data(), legacy_content.size()));

    std::array<std::byte, kSealExportStartedWireBytes> companion_out{};
    const auto companion_result = CandidateLeaseSealBreadcrumbTestAccess::read_seal_export_started(
        lease, /*legacy_or_greenfield=*/false, companion_out);
    EXPECT_EQ(companion_result.outcome, LeaseIoOutcome::Ok);
    EXPECT_EQ(companion_result.read.status, compaction_detail::ReadFixedStatus::Ok);
    EXPECT_EQ(0, std::memcmp(companion_out.data(), companion_content.data(), companion_content.size()));

    // The two reads must not have influenced each other.
    EXPECT_NE(0, std::memcmp(legacy_out.data(), companion_out.data(), companion_out.size()));

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealBreadcrumbLoaders, RoundTripsSealExportStartedMigration) {
    auto dir = make_temp_candidate_dir("seal_export_started_migration_rt");
    const auto content = pattern_bytes<kSealExportStartedMigrationWireBytes>(0x60);
    write_raw_file(dir, "seal-export-started.mig", content);

    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);

    std::array<std::byte, kSealExportStartedMigrationWireBytes> out{};
    const auto result = CandidateLeaseSealBreadcrumbTestAccess::read_seal_export_started_migration(lease, out);
    EXPECT_EQ(result.outcome, LeaseIoOutcome::Ok);
    EXPECT_EQ(result.read.status, compaction_detail::ReadFixedStatus::Ok);
    EXPECT_EQ(0, std::memcmp(out.data(), content.data(), content.size()));

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealBreadcrumbLoaders, RoundTripsSealStartedCleanupTombstone) {
    auto dir = make_temp_candidate_dir("seal_started_cleanup_rt");
    const auto content = pattern_bytes<kSealStartedCleanupWireBytes>(0x80);
    write_raw_file(dir, "seal-export-started.clr", content);

    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);

    std::array<std::byte, kSealStartedCleanupWireBytes> out{};
    const auto result = CandidateLeaseSealBreadcrumbTestAccess::read_seal_started_cleanup_tombstone(lease, out);
    EXPECT_EQ(result.outcome, LeaseIoOutcome::Ok);
    EXPECT_EQ(result.read.status, compaction_detail::ReadFixedStatus::Ok);
    EXPECT_EQ(0, std::memcmp(out.data(), content.data(), content.size()));

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealBreadcrumbLoaders, RoundTripsSealStartedAbandon) {
    auto dir = make_temp_candidate_dir("seal_started_abandon_rt");
    const auto content = pattern_bytes<kSealStartedAbandonWireBytes>(0xA0);
    write_raw_file(dir, "seal-export-started.abd", content);

    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);

    std::array<std::byte, kSealStartedAbandonWireBytes> out{};
    const auto result = CandidateLeaseSealBreadcrumbTestAccess::read_seal_started_abandon(lease, out);
    EXPECT_EQ(result.outcome, LeaseIoOutcome::Ok);
    EXPECT_EQ(result.read.status, compaction_detail::ReadFixedStatus::Ok);
    EXPECT_EQ(0, std::memcmp(out.data(), content.data(), content.size()));

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealBreadcrumbLoaders, AllSixReturnNotFoundWhenFileMissing) {
    auto dir = make_temp_candidate_dir("seal_breadcrumb_notfound");
    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);

    std::array<std::byte, kSealIdWatermarkWireBytes> a{};
    EXPECT_EQ(CandidateLeaseSealBreadcrumbTestAccess::read_seal_id_watermark(lease, a).read.status,
              compaction_detail::ReadFixedStatus::NotFound);
    std::array<std::byte, kSealExportStartedWireBytes> b{};
    EXPECT_EQ(CandidateLeaseSealBreadcrumbTestAccess::read_seal_export_started(lease, true, b).read.status,
              compaction_detail::ReadFixedStatus::NotFound);
    EXPECT_EQ(CandidateLeaseSealBreadcrumbTestAccess::read_seal_export_started(lease, false, b).read.status,
              compaction_detail::ReadFixedStatus::NotFound);
    std::array<std::byte, kSealExportStartedMigrationWireBytes> c{};
    EXPECT_EQ(CandidateLeaseSealBreadcrumbTestAccess::read_seal_export_started_migration(lease, c).read.status,
              compaction_detail::ReadFixedStatus::NotFound);
    std::array<std::byte, kSealStartedCleanupWireBytes> d{};
    EXPECT_EQ(CandidateLeaseSealBreadcrumbTestAccess::read_seal_started_cleanup_tombstone(lease, d).read.status,
              compaction_detail::ReadFixedStatus::NotFound);
    std::array<std::byte, kSealStartedAbandonWireBytes> e{};
    EXPECT_EQ(CandidateLeaseSealBreadcrumbTestAccess::read_seal_started_abandon(lease, e).read.status,
              compaction_detail::ReadFixedStatus::NotFound);

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealBreadcrumbLoaders, ReturnsWrongSizeWhenFileTruncated) {
    auto dir = make_temp_candidate_dir("seal_breadcrumb_wrongsize");
    // One byte short of kSealIdWatermarkWireBytes.
    std::array<std::byte, kSealIdWatermarkWireBytes - 1> truncated{};
    write_raw_file(dir, "seal-id-watermark", truncated);

    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);

    std::array<std::byte, kSealIdWatermarkWireBytes> out{};
    EXPECT_EQ(CandidateLeaseSealBreadcrumbTestAccess::read_seal_id_watermark(lease, out).read.status,
              compaction_detail::ReadFixedStatus::WrongSize);

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealBreadcrumbLoaders, ReturnsWrongSizeWhenFileLongerThanExpected) {
    auto dir = make_temp_candidate_dir("seal_breadcrumb_wrongsize_long");
    // One byte longer than kSealIdWatermarkWireBytes.
    std::array<std::byte, kSealIdWatermarkWireBytes + 1> too_long{};
    write_raw_file(dir, "seal-id-watermark", too_long);

    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);

    std::array<std::byte, kSealIdWatermarkWireBytes> out{};
    EXPECT_EQ(CandidateLeaseSealBreadcrumbTestAccess::read_seal_id_watermark(lease, out).read.status,
              compaction_detail::ReadFixedStatus::WrongSize);

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealBreadcrumbLoaders, NotHeldWhenLeaseNeverAcquired) {
    auto dir = make_temp_candidate_dir("seal_breadcrumb_notheld");
    CandidateLease lease(dir);  // never acquire()'d

    std::array<std::byte, kSealIdWatermarkWireBytes> out{};
    EXPECT_EQ(CandidateLeaseSealBreadcrumbTestAccess::read_seal_id_watermark(lease, out).outcome,
              LeaseIoOutcome::NotHeld);
}

TEST(SealBreadcrumbLoaders, WrongOwnerWhenCalledFromNonOwnerThread) {
    auto dir = make_temp_candidate_dir("seal_breadcrumb_wrongowner");
    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);

    LeaseIoOutcome non_owner_outcome{};
    std::thread other([&] {
        std::array<std::byte, kSealIdWatermarkWireBytes> out{};
        non_owner_outcome = CandidateLeaseSealBreadcrumbTestAccess::read_seal_id_watermark(lease, out).outcome;
    });
    other.join();

    EXPECT_EQ(non_owner_outcome, LeaseIoOutcome::WrongOwner);

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

// The key test proving the "extend CandidateLease rather than mint a second
// lease class" decision actually holds: the sticky fence set by directory
// rename/recreate applies to these NEW methods exactly the same way it
// already applies to read_intent_genesis (see
// IntentStoreDirectoryFencing.RenameAwayThenRestoreProvesStickyFence in
// test_compaction_intent_store.cpp) -- one lock, one fence, shared by both
// Round D's and Round E's read methods over the same directory.
//
// POSIX-only -- same already-documented platform finding as
// IntentStoreDirectoryFencing's own POSIX-only guard in
// test_compaction_intent_store.cpp (see that file's comment above its own
// RenameAwayThenRestoreProvesStickyFence test): on Windows, any handle held
// without FILE_SHARE_DELETE blocks rename/delete of the entire directory
// subtree, up to and including the top-level parent, from any process --
// the exact attack this fencing exists for is structurally unreachable on
// Windows given CandidateLease's current open flags, so there is no
// same-process way to exercise it there.
#ifndef _WIN32
TEST(SealBreadcrumbLoadersDirectoryFencing, RenameAwayThenRestoreProvesStickyFenceAppliesToNewMethods) {
    auto parent = make_temp_candidate_dir("seal_breadcrumb_fence_parent");
    write_raw_file(parent, "seal-id-watermark", pattern_bytes<kSealIdWatermarkWireBytes>(0x01));

    CandidateLease lease(parent);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    ASSERT_FALSE(lease.fenced());

    auto renamed_parent = parent;
    renamed_parent += "_renamed_away";
    std::filesystem::rename(parent, renamed_parent);

    std::array<std::byte, kSealIdWatermarkWireBytes> out{};
    const auto first = CandidateLeaseSealBreadcrumbTestAccess::read_seal_id_watermark(lease, out);
    EXPECT_EQ(first.outcome, LeaseIoOutcome::DirectoryIdentityChanged);
    EXPECT_TRUE(lease.fenced());
    ASSERT_TRUE(lease.last_identity_diagnostic().has_value());
    EXPECT_TRUE(lease.last_identity_diagnostic()->observed_path_missing);

    // Sticky: even after the directory is restored, this instance stays
    // fenced and refuses to re-touch the filesystem.
    std::filesystem::rename(renamed_parent, parent);
    const auto second = CandidateLeaseSealBreadcrumbTestAccess::read_seal_id_watermark(lease, out);
    EXPECT_EQ(second.outcome, LeaseIoOutcome::CandidateFenced);

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(parent);
}
#endif  // !_WIN32
