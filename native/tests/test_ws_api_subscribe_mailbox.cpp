// Private-stream migration M2: ws_api_subscribe_mailbox.hpp -- the one-shot handoff of a pre-signed subscribe frame. Single-threaded contract
// tests here; the two-thread behaviour (release/acquire publication, the compare-exchange gates under contention) is in
// test_ws_api_mailbox_concurrency.cpp and is judged by ThreadSanitizer.
//
// Governance: L1, no network/token/order. GTest only.

#include <gtest/gtest.h>
#include <hengyuan/ws_api_subscribe_mailbox.hpp>

#include <cstdint>
#include <cstring>

using hy::WsApiSubscribeFrame;
using hy::WsApiSubscribeMailbox;

namespace {

WsApiSubscribeFrame make_frame(std::uint64_t seq, std::uint32_t len) {
    WsApiSubscribeFrame f{};
    for (std::uint32_t i = 0; i < len; ++i) f.bytes[i] = static_cast<char>('a' + (seq + i) % 26);
    f.length = len;
    f.request_id = seq;
    f.signed_at_steady_ms = static_cast<std::int64_t>(seq * 7U + 5U);
    return f;
}

bool same_frame(const WsApiSubscribeFrame& a, const WsApiSubscribeFrame& b) {
    return a.length == b.length && a.request_id == b.request_id && a.signed_at_steady_ms == b.signed_at_steady_ms &&
           std::memcmp(a.bytes.data(), b.bytes.data(), a.length) == 0;
}

}  // namespace

TEST(WsApiSubscribeMailbox, StartsEmptyAndATakeFromAnEmptyMailboxChangesNothing) {
    WsApiSubscribeMailbox box;
    EXPECT_FALSE(box.posted());
    EXPECT_FALSE(box.taken());
    WsApiSubscribeFrame out = make_frame(9, 10);
    const WsApiSubscribeFrame untouched = out;
    EXPECT_FALSE(box.take(out));
    EXPECT_TRUE(same_frame(out, untouched)) << "a failed take must not write the caller's frame";
    EXPECT_FALSE(box.posted());
    EXPECT_FALSE(box.taken());
}

TEST(WsApiSubscribeMailbox, ThePostedFrameComesOutIntact) {
    WsApiSubscribeMailbox box;
    const WsApiSubscribeFrame in = make_frame(41, 300);
    ASSERT_TRUE(box.post(in));
    EXPECT_TRUE(box.posted());
    EXPECT_FALSE(box.taken());
    WsApiSubscribeFrame out{};
    ASSERT_TRUE(box.take(out));
    EXPECT_TRUE(same_frame(in, out));
    EXPECT_TRUE(box.posted()) << "posted stays true: it says a frame was published, not that one is waiting";
    EXPECT_TRUE(box.taken());
}

TEST(WsApiSubscribeMailbox, AFrameCanBePostedOnceAndTakenOnceWhateverHappens) {
    WsApiSubscribeMailbox box;
    ASSERT_TRUE(box.post(make_frame(1, 50)));
    EXPECT_FALSE(box.post(make_frame(2, 60))) << "a second post before the take";
    WsApiSubscribeFrame out{};
    ASSERT_TRUE(box.take(out));
    EXPECT_EQ(out.request_id, 1U) << "the first frame won, the second changed nothing";
    EXPECT_FALSE(box.take(out)) << "a second take";
    EXPECT_FALSE(box.post(make_frame(3, 70))) << "a post after the take: the same signed bytes (or any others) are never handed out again";
    EXPECT_FALSE(box.take(out));
    EXPECT_EQ(out.request_id, 1U);
}

TEST(WsApiSubscribeMailbox, ANonsenseFrameIsRefusedAndDoesNotSpendTheShot) {
    WsApiSubscribeMailbox box;
    WsApiSubscribeFrame empty = make_frame(5, 10);
    empty.length = 0;
    EXPECT_FALSE(box.post(empty)) << "no bytes";
    WsApiSubscribeFrame too_long = make_frame(6, 10);
    too_long.length = static_cast<std::uint32_t>(too_long.bytes.size()) + 1U;
    EXPECT_FALSE(box.post(too_long)) << "a length the slot cannot hold";
    EXPECT_FALSE(box.posted());
    // the shot is still there
    ASSERT_TRUE(box.post(make_frame(7, 80)));
    WsApiSubscribeFrame out{};
    ASSERT_TRUE(box.take(out));
    EXPECT_EQ(out.request_id, 7U);
}

TEST(WsApiSubscribeMailbox, AFrameOfExactlyTheLargestSizeFits) {
    WsApiSubscribeMailbox box;
    const WsApiSubscribeFrame in = make_frame(77, static_cast<std::uint32_t>(hy::kWsApiSubscribeFrameMaxBytes));
    ASSERT_TRUE(box.post(in));
    WsApiSubscribeFrame out{};
    ASSERT_TRUE(box.take(out));
    EXPECT_TRUE(same_frame(in, out));
}

TEST(WsApiSubscribeMailbox, TheStateWordAndTheSlotDoNotShareACacheLine) {
    // CLAUDE.md, Engineering Mandate 2: the coordinator flag touched by both threads gets its own line.
    static_assert(alignof(WsApiSubscribeMailbox) >= hy::kCacheLine);
    static_assert(sizeof(WsApiSubscribeMailbox) >= 2 * hy::kCacheLine);
    SUCCEED();
}
