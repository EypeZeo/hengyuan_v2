// SPDX-License-Identifier: proprietary
// rest_test_seam_builder.hpp -- the one class that may fill a RestTestSeam (rest_test_seam.hpp).
//
// Defined here, under tests/, and nowhere else: RestTestSeam names it as its friend, and
// tests/test_rest_freeze_guard.cpp fails if the name shows up anywhere under include/ or src/.
// Must live in namespace hy -- see binance_signer_test_hooks.hpp's header comment for why.

#pragma once

#include <hengyuan/rest_test_seam.hpp>

#include <string>
#include <utility>

namespace hy {

class RestTestSeamBuilder {
public:
    // Resolve and connect go to the loopback fixture; SNI, the Host header and hostname verification
    // keep using the logical host. `trusted_ca_pem_path` (the fixture's leaf certificate) is trusted in
    // addition to the system store; empty = no extra trust anchor, for a fixture that never completes a
    // TLS handshake anyway.
    static RestTestSeam loopback(std::string trusted_ca_pem_path = {}) {
        return with(std::move(trusted_ca_pem_path), "127.0.0.1");
    }

    // Arbitrary values, for the cases that need a different connect target (or none).
    static RestTestSeam with(std::string trusted_ca_pem_path, std::string connect_host) {
        RestTestSeam s;
        s.ca_pem_path_ = std::move(trusted_ca_pem_path);
        s.connect_host_ = std::move(connect_host);
        return s;
    }
};

}  // namespace hy
