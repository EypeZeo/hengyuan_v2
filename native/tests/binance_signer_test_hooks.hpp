// SPDX-License-Identifier: proprietary
// binance_signer_test_hooks.hpp — test-only access to BinanceSigner internals.
//
// Only included by test .cpp files. Production code never references this
// header, so this implementation never enters the production binary.
//
// Must live in namespace hy — an unqualified `friend class
// BinanceSignerTestHooks;` inside hy::BinanceSigner injects the name into
// hy (the innermost enclosing namespace of the friend declaration), not the
// global namespace. Defining this class outside namespace hy would create
// an unrelated ::BinanceSignerTestHooks with no access to BinanceSigner's
// private members.

#pragma once

#include <hengyuan/binance_signer.hpp>

namespace hy {

class BinanceSignerTestHooks {
public:
    static bool init_with_lock_behavior(BinanceSigner& s, std::string_view secret,
                                         BinanceSigner::MemoryLockBehavior b) noexcept {
        return s.init_impl(secret, b);
    }
    static BinanceSigner::SignFailure last_failure(const BinanceSigner& s) noexcept {
        return s.last_failure_;
    }
    static bool is_locked(const BinanceSigner& s) noexcept { return s.is_locked_; }
};

}  // namespace hy
