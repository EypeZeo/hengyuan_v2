// SPDX-License-Identifier: proprietary
// binance_private_rest_test_hooks.hpp -- test-only access to BinancePrivateRestClient internals.
//
// Only included by test .cpp files. Must live in namespace hy -- see
// binance_signer_test_hooks.hpp's header comment for why.

#pragma once

#include <hengyuan/binance_private_rest.hpp>

namespace hy {

class BinancePrivateRestClientTestHooks {
public:
    // Publishes an offset-0 snapshot that is fresh right now (is_snapshot_fresh() holds), so a test
    // can reach the code behind the clock gate -- the signing and the network section -- without
    // first running sync_clock() against a fixture. The caller must be on the thread that owns the
    // client, like every other call into it.
    static void publish_fresh_clock(BinancePrivateRestClient& c) noexcept {
        const ClockPairSample now = fetch_clock_pair();
        ClockOffsetSnapshot s{};
        s.offset_ms = 0;
        s.error_bound_ms = 100;
        s.system_at_fetch_ms = now.system_ms();
        s.steady_at_fetch_ms = now.steady_ms();
        (void)c.clock_pub_.publish(s);
    }
};

}  // namespace hy
