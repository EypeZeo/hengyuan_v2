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

}  // namespace hy
