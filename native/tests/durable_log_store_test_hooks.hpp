// SPDX-License-Identifier: proprietary
// durable_log_store_test_hooks.hpp — test-only access to DurableLogStore's truncate fault seam.
//
// Only included by test .cpp files. Must live in namespace hy: the friend declaration in durable_log_store.hpp
// names hy::DurableLogStoreTestHooks (see binance_environment_test_hooks.hpp for the same pattern).

#pragma once

#include <hengyuan/durable_log_store.hpp>

#include <cstdint>

namespace hy {

class DurableLogStoreTestHooks {
public:
    // The next `n` DurableLogStore::truncate_log() calls -- on any instance -- fail, the way a truncate the OS refuses
    // would. The count is process-wide; always reset it (n = 0) when a test is done, whatever the outcome.
    static void fail_next_truncates(std::uint32_t n) noexcept {
        DurableLogStore::test_fail_truncates_.store(n, std::memory_order_relaxed);
    }

    // How many of the injected failures have not been consumed yet.
    static std::uint32_t pending_truncate_failures() noexcept {
        return DurableLogStore::test_fail_truncates_.load(std::memory_order_relaxed);
    }
};

// Resets the seam when it goes out of scope, so a failed assertion cannot leave it armed for the next test.
class TruncateFailureSeamGuard {
public:
    explicit TruncateFailureSeamGuard(std::uint32_t n) noexcept { DurableLogStoreTestHooks::fail_next_truncates(n); }
    ~TruncateFailureSeamGuard() { DurableLogStoreTestHooks::fail_next_truncates(0); }
    TruncateFailureSeamGuard(const TruncateFailureSeamGuard&) = delete;
    TruncateFailureSeamGuard& operator=(const TruncateFailureSeamGuard&) = delete;
};

}  // namespace hy
