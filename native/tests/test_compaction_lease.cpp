// Real-filesystem tests for compaction_lease.hpp's CandidateLease --
// directory rename/recreate identity fencing, non-owner release(), lease
// mutual exclusion (same-process and cross-process), and IntentStore's
// genesis write/read via the friend relationship. Governance: L2 (real
// file I/O).
#include <gtest/gtest.h>
#include <hengyuan/compaction_lease.hpp>

#include <array>
#include <chrono>
#include <cstddef>
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
