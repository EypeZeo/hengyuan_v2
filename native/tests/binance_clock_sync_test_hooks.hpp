// SPDX-License-Identifier: proprietary
// binance_clock_sync_test_hooks.hpp — test-only synthetic construction of
// ClockPairSample.
//
// Only included by test .cpp files. Must live in namespace hy — see
// binance_signer_test_hooks.hpp's header comment for why.
//
// This is the ONLY test-side way to build a ClockPairSample from two
// arbitrary int64 values (production code can only get one from
// fetch_clock_pair()) — see binance_clock_sync.hpp's AUDIT
// L4-CLOCKPAIR-API-002 comment for why that restriction exists.

#pragma once

#include <hengyuan/binance_clock_sync.hpp>

#include <cstdint>

namespace hy {

class ClockPairSampleTestHooks {
public:
    static ClockPairSample make(std::int64_t system_ms, std::int64_t steady_ms) noexcept {
        return ClockPairSample(system_ms, steady_ms);
    }
};

}  // namespace hy
