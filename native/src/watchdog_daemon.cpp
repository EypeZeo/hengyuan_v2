// SPDX-License-Identifier: proprietary
// watchdog_daemon.cpp — Independent watchdog process for HengYuan trading core.
//
// Monitors shared-memory heartbeat from the trading process. If the main
// process hangs (loop_counter stalls), loses market data (last_incoming_ns
// stalls), or corrupts memory (checksum mismatch), the watchdog:
//   1. Arms kill_armed flag in SHM (main process checks this)
//   2. Sends SIGKILL to main process after grace period
//
// Mutual death guarantee: prctl(PR_SET_PDEATHSIG, SIGKILL) ensures that if
// the parent (launcher) dies, the watchdog dies too. The main process also
// monitors watchdog PID liveness — if watchdog dies, main arms its own kill
// switch (fail-closed, never naked).
//
// SIMULATION INFRASTRUCTURE. No Binance Private API, no HMAC, no order.
// Governance: L4 (system-level process monitoring).
// Linux only. Usage:
//   ./watchdog_daemon --shm /hy_heartbeat --timeout 10 [--poll-ms 500]

#ifdef __linux__

#include <hengyuan/shm_heartbeat.hpp>

#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>

static std::atomic<bool> g_stop{false};
static void sig_handler(int) { g_stop.store(true); }

static std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

int main(int argc, char* argv[]) {
    std::string shm_name = "/hy_heartbeat";
    int timeout_s = 10;
    int poll_ms = 500;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--shm" && i + 1 < argc) shm_name = argv[++i];
        else if (arg == "--timeout" && i + 1 < argc) timeout_s = std::atoi(argv[++i]);
        else if (arg == "--poll-ms" && i + 1 < argc) poll_ms = std::atoi(argv[++i]);
    }
    if (timeout_s <= 0) timeout_s = 10;
    if (poll_ms <= 0) poll_ms = 500;

    // If our parent dies, we die too (fail-closed).
    // Use syscall() to avoid <sys/prctl.h> kernel-header __u64 issue on Ubuntu 24.04.
    constexpr int kPrSetPdeathsig = 1;
    syscall(SYS_prctl, kPrSetPdeathsig, SIGKILL);

    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    std::printf("=== HengYuan Watchdog Daemon ===\n");
    std::printf("SHM: %s  timeout: %ds  poll: %dms\n", shm_name.c_str(), timeout_s, poll_ms);
    std::printf("SIMULATION INFRASTRUCTURE — no orders, no money.\n\n");

    // Open existing SHM (main process creates it)
    hy::ShmSegment shm;
    std::printf("Waiting for SHM segment '%s'...\n", shm_name.c_str());
    while (!g_stop.load()) {
        if (shm.open(shm_name.c_str(), false)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!shm.is_open()) {
        std::printf("Aborted: SHM not found.\n");
        return 1;
    }

    auto* blk = shm.block();
    if (!hy::shm_verify(*blk)) {
        std::printf("ERROR: SHM control block invalid (bad magic/checksum).\n");
        return 1;
    }

    std::uint64_t main_pid = blk->main_pid;
    std::printf("Connected. Main PID=%" PRIu64 "\n", main_pid);

    // Monitoring state
    std::uint64_t prev_loop = blk->loop_counter;
    std::uint64_t prev_incoming = blk->last_incoming_ns;
    auto last_activity = std::chrono::steady_clock::now();
    int stale_checks = 0;

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));

        // Check if main process is still alive
        if (kill(static_cast<pid_t>(main_pid), 0) != 0) {
            std::printf("[watchdog] Main process PID=%" PRIu64 " is dead. Exiting.\n", main_pid);
            break;
        }

        // Read and verify control block
        hy::ShmControlBlock snapshot;
        std::memcpy(&snapshot, blk, sizeof(snapshot));

        if (!hy::shm_verify(snapshot)) {
            ++stale_checks;
            std::printf("[watchdog] CHECKSUM MISMATCH (count=%d). Memory corruption?\n", stale_checks);
            if (stale_checks >= 3) {
                std::printf("[watchdog] CRITICAL: 3 consecutive checksum failures. ARMING KILL.\n");
                blk->kill_armed = 1;
                std::this_thread::sleep_for(std::chrono::seconds(2));
                std::printf("[watchdog] Sending SIGKILL to PID %" PRIu64 "\n", main_pid);
                kill(static_cast<pid_t>(main_pid), SIGKILL);
                break;
            }
            continue;
        }
        stale_checks = 0;

        // Multi-dimensional liveness check
        bool loop_advanced = (snapshot.loop_counter > prev_loop);
        bool incoming_advanced = (snapshot.last_incoming_ns > prev_incoming);

        if (loop_advanced || incoming_advanced) {
            last_activity = std::chrono::steady_clock::now();
        }

        auto since_activity = std::chrono::steady_clock::now() - last_activity;
        auto stale_s = std::chrono::duration_cast<std::chrono::seconds>(since_activity).count();

        if (stale_s >= timeout_s) {
            std::printf("[watchdog] TIMEOUT: no activity for %ds.\n", static_cast<int>(stale_s));
            std::printf("  loop_counter: %" PRIu64 " (prev %" PRIu64 ")\n",
                        snapshot.loop_counter, prev_loop);
            std::printf("  last_incoming_ns: %" PRIu64 " (prev %" PRIu64 ")\n",
                        snapshot.last_incoming_ns, prev_incoming);
            std::printf("  last_outgoing_ns: %" PRIu64 "\n", snapshot.last_outgoing_ns);

            // Cross-check: if incoming is very recent but loop stalled,
            // the hot thread is dead but I/O thread may still run.
            std::uint64_t now = now_ns();
            bool incoming_recent = (now - snapshot.last_incoming_ns) < 5'000'000'000ULL;  // 5s
            if (incoming_recent && !loop_advanced) {
                std::printf("  DIAGNOSIS: I/O thread alive but hot thread HUNG.\n");
            }

            std::printf("[watchdog] ARMING KILL SWITCH.\n");
            blk->kill_armed = 1;

            // Grace period: let main process self-terminate cleanly
            std::this_thread::sleep_for(std::chrono::seconds(2));

            // Force kill if still alive
            if (kill(static_cast<pid_t>(main_pid), 0) == 0) {
                std::printf("[watchdog] Grace period expired. SIGKILL PID %" PRIu64 "\n", main_pid);
                kill(static_cast<pid_t>(main_pid), SIGKILL);
            }
            break;
        }

        prev_loop = snapshot.loop_counter;
        prev_incoming = snapshot.last_incoming_ns;

        // Periodic status (every 10 polls)
        static int poll_count = 0;
        if (++poll_count % 20 == 0) {
            std::printf("[watchdog] OK  loop=%" PRIu64 "  incoming_age=%.1fs  outgoing_age=%.1fs\n",
                        snapshot.loop_counter,
                        static_cast<double>(now_ns() - snapshot.last_incoming_ns) / 1e9,
                        snapshot.last_outgoing_ns > 0
                            ? static_cast<double>(now_ns() - snapshot.last_outgoing_ns) / 1e9
                            : -1.0);
        }
    }

    std::printf("[watchdog] Shutdown.\n");
    return 0;
}

#else  // non-Linux

#include <cstdio>
int main() {
    std::printf("watchdog_daemon requires Linux (shm_open, prctl).\n");
    return 1;
}

#endif
