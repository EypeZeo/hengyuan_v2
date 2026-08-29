// SPDX-License-Identifier: proprietary
// binance_query_signing_test_hooks.hpp — test-only access to
// CanonicalUnsignedQuery internals.
//
// Only included by test .cpp files. Must live in namespace hy — see
// binance_signer_test_hooks.hpp's header comment for why.

#pragma once

#include <hengyuan/binance_query_signing.hpp>

namespace hy {

class CanonicalUnsignedQueryTestHooks {
public:
    // build_canonical_query() rejects empty param spans, so the production
    // path can never produce a bytes().empty() instance — this constructs
    // one anyway, solely so build_signed_query()'s defensive empty-input
    // branch has something real to test. Never used by production code.
    static CanonicalUnsignedQuery make_empty_for_test() noexcept {
        return CanonicalUnsignedQuery{};
    }
};

}  // namespace hy
