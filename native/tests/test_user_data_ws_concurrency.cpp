// Real two-thread stress test for TODO 1A.4's cross-thread boundary:
// UserDataWsEventRing (WS I/O thread -> hot thread). Its sibling boundary, ListenKeyPublisher
// (hot thread -> WS I/O thread), was retired with the listenKey stack in M5b-2; the ring is shared
// with the WebSocket API session, which is why this file stays on the TSan and ARM64 lists.
// Mirrors test_spsc_concurrency.cpp's own SpscRingConcurrency/
// IntentChannelConcurrency pattern -- a green run here proves the LOGICAL contract across the
// boundary; ThreadSanitizer (-DHY_SANITIZER=thread, see native/cmake/Sanitizers.cmake and
// tools/wsl_verify.sh) is what actually proves no missing acquire/release edge, and is
// required, not optional, before trusting this file's design (same caveat
// test_spsc_concurrency.cpp's own header states).
//
// This is the first genuinely new cross-thread boundary in this codebase since
// order_tracker.hpp's own SpscRing pair (see the TODO 1A.4 plan's own reasoning for why this
// batch, unlike TODO 1A.3's follow-up, actually needs the WSL2 `thread` tier).
//
// Governance: L1, no network/token/order. GTest only.

#include <gtest/gtest.h>
#include <hengyuan/binance_user_data_event.hpp>

#include <cstring>
#include <string>
#include <thread>

using namespace hy;

namespace {

// Self-checking payload, same philosophy as test_spsc_concurrency.cpp's make_item()/
// item_is_intact(): every field is derived from the sequence number, so a torn or
// partially-visible slot is detectable rather than merely unlikely.
UserDataWsEvent make_item(std::uint64_t seq) {
    UserDataWsEvent ev{};
    ev.kind = UserDataEventKind::ExecutionReport;
    ev.event_time_ms = static_cast<std::int64_t>(seq * 11u + 3u);
    ev.exchange_order_id = static_cast<std::int64_t>(seq * 7u + 2u);
    const std::string coid = "HY-" + std::to_string(seq);
    std::memcpy(ev.coid.id, coid.data(), coid.size());
    ev.coid.id[coid.size()] = '\0';
    return ev;
}

bool item_is_intact(const UserDataWsEvent& ev, std::uint64_t seq) {
    if (ev.kind != UserDataEventKind::ExecutionReport) return false;
    if (ev.event_time_ms != static_cast<std::int64_t>(seq * 11u + 3u)) return false;
    if (ev.exchange_order_id != static_cast<std::int64_t>(seq * 7u + 2u)) return false;
    return ev.coid.view() == ("HY-" + std::to_string(seq));
}

constexpr std::uint64_t kItems = 100'000;

}  // namespace

TEST(UserDataWsEventRingConcurrency, NoLossNoDuplicationStrictFifo) {
    UserDataWsEventRing ring;

    std::uint64_t pushed = 0;
    std::uint64_t received = 0;
    std::uint64_t torn_payload_count = 0;
    std::uint64_t first_torn_seq = UINT64_MAX;

    std::thread producer([&ring, &pushed] {
        for (std::uint64_t i = 0; i < kItems; ++i) {
            const UserDataWsEvent ev = make_item(i);
            while (!ring.try_push(ev)) {
                // full -- spin, never drop
            }
            ++pushed;
        }
    });

    std::thread consumer([&ring, &received, &torn_payload_count, &first_torn_seq] {
        UserDataWsEvent out{};
        std::uint64_t expected = 0;
        while (received < kItems) {
            if (!ring.try_pop(out)) {
                continue;  // empty -- spin
            }
            // Strict FIFO by construction (SpscRing's own contract) -- the only real failure
            // mode to check for is a torn/partially-visible payload at the expected slot,
            // exactly matching test_spsc_concurrency.cpp's own SpscRingConcurrency test.
            if (!item_is_intact(out, expected)) {
                ++torn_payload_count;
                if (first_torn_seq == UINT64_MAX) first_torn_seq = expected;
            }
            ++expected;
            ++received;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(pushed, kItems);
    EXPECT_EQ(received, kItems) << "an event was lost or duplicated across the thread boundary";
    EXPECT_EQ(torn_payload_count, 0u)
        << "torn/partially-visible slot observed at seq " << first_torn_seq
        << " -- this is what a missing release/acquire pair looks like on weak-memory hardware";
}
