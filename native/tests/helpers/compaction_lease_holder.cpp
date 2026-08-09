// Standalone subprocess for CandidateLease cross-process mutual-exclusion
// testing (test_compaction_lease.cpp's CrossProcessMutualExclusion test).
// Not a GTest binary -- argv-driven, spawned as a child process by that
// test. Deliberately dependency-free (just compaction_lease.hpp + iostream)
// since it is process-launched, not linked in.
//
// Usage: compaction_lease_holder <candidate_dir> <status_file> <hold_ms>
//   1. CandidateLease::acquire() on <candidate_dir>.
//   2. Writes "ACQUIRED" (on success) or "FAILED:<status>" (on any other
//      LeaseAcquireStatus) to <status_file> and closes it immediately --
//      the parent test process polls this file rather than using a pipe,
//      keeping this helper's shape identical on both platforms instead of
//      needing separate CreateProcess-with-pipes / posix_spawn-with-pipes
//      plumbing just to synchronize "has it acquired yet."
//   3. On success: sleeps for <hold_ms> milliseconds while STILL holding
//      the lease, then release()s and exits 0.
//   4. On failure: exits 1 immediately (nothing left to hold or release).
#include <hengyuan/compaction_lease.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <candidate_dir> <status_file> <hold_ms>\n", argv[0]);
        return 2;
    }
    const std::string candidate_dir = argv[1];
    const std::string status_file = argv[2];
    const int hold_ms = std::atoi(argv[3]);

    hy::CandidateLease lease(candidate_dir);
    const hy::LeaseAcquireStatus status = lease.acquire();

    {
        std::ofstream out(status_file, std::ios::trunc);
        if (status == hy::LeaseAcquireStatus::Acquired) {
            out << "ACQUIRED";
        } else {
            out << "FAILED:" << static_cast<int>(status);
        }
    }

    if (status != hy::LeaseAcquireStatus::Acquired) {
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    const hy::ReleaseStatus release_status = lease.release();
    return release_status == hy::ReleaseStatus::Released ? 0 : 3;
}
