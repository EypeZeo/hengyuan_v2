// SPDX-License-Identifier: proprietary
// intent_channel.hpp — SPSC channel for strategy → executor intent delivery.
//
// Strategies (Python, C++ research, or future auto-strategy) push
// ExecutionIntents into this channel from the cold path. The hot path
// (HotThread) drains and executes them against the current book via
// SimExecutor. This decouples strategy timing from market-data latency.
//
// Governance: L2, no network/token/order. The channel carries SIMULATED
// intents only. Live intents would require L5 authorization + HMAC signing.

#pragma once

#include <hengyuan/execution_types.hpp>
#include <hengyuan/spsc_ring.hpp>
#include <cstdint>

namespace hy {

// Intent with attribution: which strategy submitted it, and when.
struct TimestampedIntent {
    ExecutionIntent intent{};
    std::uint32_t strategy_id{0};
    std::uint64_t submit_ts_ns{0};  // steady_clock when pushed
};

static_assert(std::is_trivially_copyable_v<TimestampedIntent>,
              "TimestampedIntent must be trivially copyable for SPSC ring");

// Default channel size: 1024 slots. Strategy submission rate is orders of
// magnitude slower than market data, so this is generous.
template <std::size_t N = 1024>
using IntentChannel = SpscRing<TimestampedIntent, N>;

}  // namespace hy
