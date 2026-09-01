// Real two-thread stress tests for TODO 1A.4's new cross-thread boundaries:
// UserDataWsEventRing (WS I/O thread -> hot thread) and ListenKeyPublisher (hot thread ->
// WS I/O thread). Mirrors test_spsc_concurrency.cpp's own SpscRingConcurrency/
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
#include <hengyuan/binance_listen_key_publisher.hpp>
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

TEST(ListenKeyPublisherConcurrency, LoadNeverObservesATornSnapshot) {
    ListenKeyPublisher pub;
    constexpr std::uint64_t kPublishes = 100'000;

    std::atomic<bool> producer_done{false};
    std::uint64_t torn_count = 0;
    std::uint64_t observed_count = 0;
    std::uint64_t max_seq_seen = 0;

    std::thread producer([&pub, &producer_done] {
        for (std::uint64_t i = 1; i <= kPublishes; ++i) {
            const std::string key = "K" + std::to_string(i);
            // issued_at_ms/expires_at_ms are DERIVED from the same i the key encodes -- a
            // torn/inconsistent snapshot is one where these three fields don't agree, not
            // merely unlikely to occur by chance.
            EXPECT_TRUE(pub.publish(key, static_cast<std::int64_t>(i),
                                     static_cast<std::int64_t>(i) * 2));
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread reader([&pub, &producer_done, &torn_count, &observed_count, &max_seq_seen] {
        // Loops until the producer is done AND at least one load has observed the final
        // publish -- otherwise this thread could exit before ever exercising a genuinely
        // concurrent load.
        for (;;) {
            const ListenKeySnapshot snap = pub.load();
            if (snap.seq > 0) {
                ++observed_count;
                if (snap.seq > max_seq_seen) max_seq_seen = snap.seq;
                const std::string expected_key = "K" + std::to_string(snap.issued_at_ms);
                const bool consistent = (snap.view() == expected_key) &&
                                         (snap.expires_at_ms == snap.issued_at_ms * 2);
                if (!consistent) ++torn_count;
            }
            if (producer_done.load(std::memory_order_acquire) && max_seq_seen >= kPublishes) {
                break;
            }
        }
    });

    producer.join();
    reader.join();

    EXPECT_EQ(torn_count, 0u)
        << "load() observed a snapshot whose key/issued_at_ms/expires_at_ms disagreed -- this "
           "is what a missing mutex lock looks like";
    EXPECT_GT(observed_count, 0u) << "reader thread never observed a single published snapshot";
    EXPECT_EQ(max_seq_seen, kPublishes);
}
