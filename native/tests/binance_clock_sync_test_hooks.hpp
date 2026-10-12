// SPDX-License-Identifier: proprietary
// binance_clock_sync_test_hooks.hpp — test-only synthetic construction of
// ClockPairSample and ClockReadyToken.
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

// The ONLY test-side way to mint a ClockReadyToken for a timestamp of the test's choosing -- the signature
// vectors and the range checks of build_signed_query() need fixed values, and no clock was ever published
// there. Production code can only get a token from issue_clock_ready_token() (SAFE-01 slice 5); this class is
// defined nowhere under include/ or src/, and tests/test_rest_freeze_guard.cpp pins that production code
// names it only in the friend declaration of ClockReadyToken.
class ClockReadyTokenTestHooks {
public:
    static ClockReadyToken make(std::int64_t server_ms) noexcept { return ClockReadyToken(server_ms); }
};

}  // namespace hy
