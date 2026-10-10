// SPDX-License-Identifier: proprietary
// rest_test_seam.hpp — the only way a REST client can be pointed at a different TCP target or be
// given an extra TLS trust anchor (audit P1-001, fault case FI-032).
//
// Both abilities used to be plain public fields of PrivateRestConfig, RestSnapshotConfig and
// PublicRestConfig (`connect_host_override`, `extra_trusted_ca_pem_path`). A production configuration
// could therefore redirect a signed request to any host, or make the process trust any CA, and nothing
// in the type system said that was a test-only ability. They are no longer part of any *Config.
//
// A RestTestSeam is an opaque value:
//   * it can be default-constructed -- the empty seam, "no redirection, no extra trust anchor", which is
//     what every production caller passes (or simply omits);
//   * its contents can only be filled by RestTestSeamBuilder, a class that is declared a friend below and
//     DEFINED ONLY under native/tests/ (test_helpers/rest_test_seam_builder.hpp). The freeze-guard test
//     (tests/test_rest_freeze_guard.cpp) fails if that name appears anywhere else under include/ or src/.
//
// What a seam changes (and nothing else): where resolve/connect go, and which extra CA file is loaded in
// ADDITION to the system trust store. SNI, the Host header, hostname verification and the endpoint
// allowlist check all keep using the logical host, so a seam can point a connection at a local fixture but
// cannot weaken the verification of whatever answers there; the extra CA only ever adds a trust anchor.

#pragma once

#include <string>

namespace hy {

class RestTestSeam {
public:
    RestTestSeam() = default;

    // Extra trust anchor (PEM file), loaded in addition to the system store; empty = none.
    [[nodiscard]] const std::string& extra_trusted_ca_pem_path() const noexcept { return ca_pem_path_; }

    // TCP target for resolve + connect when it must differ from the logical host (e.g. a loopback
    // fixture); empty = connect to the logical host. Tests need it because a synthetic `.invalid`
    // hostname (RFC 2606) cannot be relied on to fail DNS resolution -- observed in this repo's own
    // WSL2 environment, where an unresolvable name resolved to a synthesized address instead of NXDOMAIN
    // -- so a test using one fake name for both "where to connect" and "what to verify" would silently
    // connect to some unrelated real host instead of the local fixture.
    [[nodiscard]] const std::string& connect_host_override() const noexcept { return connect_host_; }

private:
    friend class RestTestSeamBuilder;  // defined only in native/tests/test_helpers/

    std::string ca_pem_path_;
    std::string connect_host_;
};

}  // namespace hy
