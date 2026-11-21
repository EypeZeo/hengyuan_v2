// SPDX-License-Identifier: proprietary
// binance_private_rest_test_hooks.hpp -- test-only access to BinancePrivateRestClient internals.
//
// Only included by test .cpp files. Must live in namespace hy -- see
// binance_signer_test_hooks.hpp's header comment for why.

#pragma once

#include <hengyuan/binance_private_rest.hpp>
#include <hengyuan/rest_test_seam.hpp>

#include <string>
#include <utility>

#include "test_helpers/rest_test_seam_builder.hpp"

namespace hy {

class BinancePrivateRestClientTestHooks {
public:
    // Publishes a clock snapshot that is fresh right now (is_snapshot_fresh() holds), so a test
    // can reach the code behind the clock gate -- the signing and the network section -- without
    // first running sync_clock() against a fixture. Publication only touches the synchronized clock publisher.
    static void publish_fresh_clock(BinancePrivateRestClient& c, std::int64_t offset_ms = 0) noexcept {
        const ClockPairSample now = fetch_clock_pair();
        ClockOffsetSnapshot s{};
        s.offset_ms = offset_ms;
        s.error_bound_ms = 100;
        s.system_at_fetch_ms = now.system_ms();
        s.steady_at_fetch_ms = now.steady_ms();
        (void)c.clock_pub_.publish(s);
    }

    // Points the client at a test fixture (rest_test_seam.hpp): every network call it makes from now
    // on resolves and connects where the seam says. Same threading rule as above.
    static void set_seam(BinancePrivateRestClient& c, RestTestSeam seam) {
        c.test_seam_ = std::move(seam);
    }

    // The common case: a loopback fixture, trusting `trusted_ca_pem_path` (empty = no extra CA).
    static void use_loopback(BinancePrivateRestClient& c, std::string trusted_ca_pem_path = {}) {
        set_seam(c, RestTestSeamBuilder::loopback(std::move(trusted_ca_pem_path)));
    }
};

}  // namespace hy
