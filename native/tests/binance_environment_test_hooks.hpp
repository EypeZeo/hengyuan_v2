// SPDX-License-Identifier: proprietary
// binance_environment_test_hooks.hpp — test-only access to
// BoundHmacCredentials internals.
//
// Only included by test .cpp files. Must live in namespace hy — see
// binance_signer_test_hooks.hpp's header comment for why.

#pragma once

#include <hengyuan/binance_environment.hpp>

namespace hy {

class BoundHmacCredentialsTestHooks {
public:
    static void wipe_api_key(BoundHmacCredentials& c) noexcept { c.wipe_api_key(); }

    static std::span<const char> sign(BoundHmacCredentials& c,
                                       std::string_view payload) noexcept {
        return c.sign(payload);
    }

    static std::pair<QuerySigningError, std::unique_ptr<BoundHmacCredentials>>
    load_and_bind_credentials_with_lock_behavior(
        const EnvironmentBinding& binding, SecureEnvLoader& loader,
        BinanceSigner::MemoryLockBehavior b) noexcept {
        return BoundHmacCredentials::load_and_bind_credentials_impl(binding, loader, b);
    }
};

// Test-only access to EnvironmentBinding's private policy (see the friend declaration there).
class EnvironmentBindingTestHooks {
public:
    // A copy of `b` whose allowlist is exactly `allowlist`. The factories never produce a binding
    // whose allowlist excludes its own host, and this is the one state in which a client's
    // endpoint check can be proven to fire before any network attempt.
    static EnvironmentBinding with_allowlist(const EnvironmentBinding& b,
                                              const EndpointAllowlist& allowlist) noexcept {
        EnvironmentBinding out = b;
        out.transport_policy_.endpoint_allowlist = allowlist;
        return out;
    }

    // A copy of `b` bound to `host` instead, whose allowlist is exactly that host. The two factories only know
    // the two real exchange hosts; the public fetchers' tests need a binding for a loopback fixture
    // ("127.0.0.1"), for the name on a fixture certificate ("wrong-san.test.invalid") and for hosts that are no
    // hostname at all. `host` must outlive the binding (a string literal in every caller).
    static EnvironmentBinding with_host(const EnvironmentBinding& b, std::string_view host) noexcept {
        EnvironmentBinding out = b;
        out.base_host_ = host;
        out.transport_policy_.endpoint_allowlist = EndpointAllowlist{};
        out.transport_policy_.endpoint_allowlist.hosts[0] = host;
        out.transport_policy_.endpoint_allowlist.count = 1;
        return out;
    }
};

}  // namespace hy
