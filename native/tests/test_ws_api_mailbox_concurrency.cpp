// Private-stream migration M2: real multi-thread tests of the one boundary the WebSocket API session adds between threads —
// WsApiSubscribeMailbox, the one-shot handoff of a pre-signed subscribe frame from the credential-owning thread to the I/O thread.
//
// A green run proves the LOGICAL contract across the boundary (every frame arrives whole, exactly one post and one take win under
// contention). On x86-TSO those checks pass even if the release store of Ready and the acquire compare-exchange were downgraded to relaxed,
// because the hardware supplies the ordering for free; ThreadSanitizer (-DHY_SANITIZER=thread, tools/wsl_verify.sh thread) is what proves
// no happens-before edge is missing, and tests/tsan_control_relaxed_mailbox.cpp is the check that this TSan setup would actually see such a
// downgrade (same discipline as tsan_control_relaxed_ring.cpp). Mirrors test_user_data_ws_concurrency.cpp: Boost-free, GTest only.
//
// Governance: L1, no network/token/order.

#include <gtest/gtest.h>
#include <hengyuan/ws_api_subscribe_mailbox.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using hy::WsApiSubscribeFrame;
using hy::WsApiSubscribeMailbox;

namespace {

// Self-checking payload, same philosophy as test_spsc_concurrency.cpp: every byte and field is derived from the sequence number, so a torn or
// partially visible slot is detectable rather than merely unlikely.
WsApiSubscribeFrame make_frame(std::uint64_t seq) {
    WsApiSubscribeFrame f{};
    const std::uint32_t len = 64U + static_cast<std::uint32_t>(seq % 900U);
    for (std::uint32_t i = 0; i < len; ++i) f.bytes[i] = static_cast<char>('a' + (seq + i) % 26);
    f.length = len;
    f.request_id = seq;
    f.signed_at_steady_ms = static_cast<std::int64_t>(seq * 7U + 5U);
    return f;
}

bool intact(const WsApiSubscribeFrame& f) {
    if (f.length < 64U || f.length > f.bytes.size()) return false;
    if (f.signed_at_steady_ms != static_cast<std::int64_t>(f.request_id * 7U + 5U)) return false;
    for (std::uint32_t i = 0; i < f.length; ++i) {
        if (f.bytes[i] != static_cast<char>('a' + (f.request_id + i) % 26)) return false;
    }
    return true;
}

void spin_pause() { std::this_thread::yield(); }

}  // namespace

TEST(WsApiMailboxConcurrency, EveryFrameCrossesWholeWhenTheProducerPostsAndTheConsumerPolls) {
    // One mailbox per round; the producer posts them in order, with an occasional pause so the consumer sometimes polls an empty box and
    // sometimes finds it already full — the order independence the session relies on (it may poll before the hot thread has posted).
    constexpr std::size_t kRounds = 3000;
    std::unique_ptr<WsApiSubscribeMailbox[]> boxes(new WsApiSubscribeMailbox[kRounds]);
    std::atomic<std::size_t> posted_ok{0};
    std::atomic<std::size_t> taken_ok{0};
    std::atomic<std::size_t> torn{0};
    std::atomic<std::size_t> second_take_succeeded{0};
    std::atomic<std::size_t> second_post_succeeded{0};

    std::thread producer([&] {
        for (std::size_t i = 0; i < kRounds; ++i) {
            if (i % 5 == 0) spin_pause();
            if (boxes[i].post(make_frame(i))) posted_ok.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread consumer([&] {
        for (std::size_t i = 0; i < kRounds; ++i) {
            WsApiSubscribeFrame out{};
            while (!boxes[i].take(out)) spin_pause();
            taken_ok.fetch_add(1, std::memory_order_relaxed);
            if (out.request_id != i || !intact(out)) torn.fetch_add(1, std::memory_order_relaxed);
            if (boxes[i].take(out)) second_take_succeeded.fetch_add(1, std::memory_order_relaxed);
            if (boxes[i].post(make_frame(i + 100000))) second_post_succeeded.fetch_add(1, std::memory_order_relaxed);
        }
    });
    producer.join();
    consumer.join();

    EXPECT_EQ(posted_ok.load(), kRounds);
    EXPECT_EQ(taken_ok.load(), kRounds);
    EXPECT_EQ(torn.load(), 0U) << "a consumer saw a frame that was not the one the producer wrote, or only part of it";
    EXPECT_EQ(second_take_succeeded.load(), 0U) << "the same signed bytes were handed out twice";
    EXPECT_EQ(second_post_succeeded.load(), 0U) << "a mailbox accepted a second frame";
}

TEST(WsApiMailboxConcurrency, OnlyOneOfSeveralRacingProducersWinsAndTheConsumerSeesThatFrameWhole) {
    constexpr std::size_t kRounds = 400;
    constexpr std::uint64_t kProducers = 4;
    std::size_t bad_rounds = 0;
    for (std::size_t r = 0; r < kRounds; ++r) {
        WsApiSubscribeMailbox box;
        std::atomic<bool> go{false};
        std::atomic<int> wins{0};
        std::vector<std::thread> producers;
        for (std::uint64_t k = 0; k < kProducers; ++k) {
            producers.emplace_back([&, k] {
                const WsApiSubscribeFrame f = make_frame(r * kProducers + k);
                while (!go.load(std::memory_order_acquire)) spin_pause();
                if (box.post(f)) wins.fetch_add(1, std::memory_order_relaxed);
            });
        }
        go.store(true, std::memory_order_release);
        for (std::thread& t : producers) t.join();
        WsApiSubscribeFrame out{};
        const bool took = box.take(out);
        const bool one_winner = wins.load() == 1;
        const bool whole = took && intact(out) && out.request_id >= r * kProducers && out.request_id < (r + 1) * kProducers;
        if (!one_winner || !whole) ++bad_rounds;
    }
    EXPECT_EQ(bad_rounds, 0U);
}

TEST(WsApiMailboxConcurrency, OnlyOneOfSeveralRacingConsumersGetsTheFrame) {
    constexpr std::size_t kRounds = 400;
    constexpr int kConsumers = 4;
    std::size_t bad_rounds = 0;
    for (std::size_t r = 0; r < kRounds; ++r) {
        WsApiSubscribeMailbox box;
        ASSERT_TRUE(box.post(make_frame(r)));
        std::atomic<bool> go{false};
        std::atomic<int> got{0};
        std::atomic<int> torn{0};
        std::vector<std::thread> consumers;
        for (int k = 0; k < kConsumers; ++k) {
            consumers.emplace_back([&] {
                WsApiSubscribeFrame out{};
                while (!go.load(std::memory_order_acquire)) spin_pause();
                if (box.take(out)) {
                    got.fetch_add(1, std::memory_order_relaxed);
                    if (out.request_id != r || !intact(out)) torn.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (std::thread& t : consumers) t.join();
        if (got.load() != 1 || torn.load() != 0) ++bad_rounds;
    }
    EXPECT_EQ(bad_rounds, 0U) << "a frame went to two consumers (a replayed signature) or to none";
}
