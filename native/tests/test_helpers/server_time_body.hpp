// Test-only: the body a local "exchange" answers GET /api/v3/time with.
//
// sync_clock() refuses to publish an offset larger than the bound environment's recv window (SAFE-01 slice 4:
// check_clock_skew() against TransportPolicy::recv_window_ms), so a fixture that still answers with a fixed 2023
// timestamp would make every success-path test fail with ClockSkewTooLarge. The body carries the time of the
// call, which is when the fixture is built -- a few milliseconds before the client asks.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace hy::test_helpers {

// {"serverTime": <now + delta_ms>} -- delta_ms lets a test place the exchange's clock ahead of or behind the local one.
inline std::string server_time_body_offset_by(std::int64_t delta_ms) {
    const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
    return "{\"serverTime\":" + std::to_string(now_ms + delta_ms) + "}";
}

inline std::string current_server_time_body() { return server_time_body_offset_by(0); }

}  // namespace hy::test_helpers
