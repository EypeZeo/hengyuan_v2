// TODO 1A.4: binance_listen_key_publisher.hpp unit tests, mirroring
// test_binance_clock_sync.cpp's ClockOffsetPublisher test shape.
#include <gtest/gtest.h>
#include <hengyuan/binance_listen_key_publisher.hpp>

using hy::kListenKeyBufferLen;
using hy::ListenKeyPublisher;
using hy::ListenKeySnapshot;

TEST(ListenKeyPublisher, NeverPublishedLoadsSeqZero) {
    ListenKeyPublisher pub;
    EXPECT_EQ(pub.load().seq, 0u);
}

TEST(ListenKeyPublisher, PublishLoadRoundTrip) {
    ListenKeyPublisher pub;
    EXPECT_TRUE(pub.publish("abc123listenkey", 1000, 4600000));
    ListenKeySnapshot loaded = pub.load();
    EXPECT_EQ(loaded.view(), "abc123listenkey");
    EXPECT_EQ(loaded.issued_at_ms, 1000);
    EXPECT_EQ(loaded.expires_at_ms, 4600000);
    EXPECT_EQ(loaded.seq, 1u);
}

TEST(ListenKeyPublisher, SeqIncrementsMonotonically) {
    ListenKeyPublisher pub;
    for (std::uint32_t expected = 1; expected <= 5; ++expected) {
        EXPECT_TRUE(pub.publish("k", 0, 0));
        EXPECT_EQ(pub.load().seq, expected);
    }
}

TEST(ListenKeyPublisher, LaterPublishReplacesEarlierValue) {
    ListenKeyPublisher pub;
    ASSERT_TRUE(pub.publish("first-key", 1000, 2000));
    ASSERT_TRUE(pub.publish("second-key-renewed", 5000, 6000));
    ListenKeySnapshot loaded = pub.load();
    EXPECT_EQ(loaded.view(), "second-key-renewed");
    EXPECT_EQ(loaded.issued_at_ms, 5000);
}

TEST(ListenKeyPublisher, RefusesEmptyKey) {
    ListenKeyPublisher pub;
    EXPECT_FALSE(pub.publish("", 0, 0));
    EXPECT_EQ(pub.load().seq, 0u);  // refused publish must not have taken effect
}

TEST(ListenKeyPublisher, RefusesOverlongKey) {
    ListenKeyPublisher pub;
    std::string too_long(kListenKeyBufferLen, 'x');  // exactly at the buffer size -- still too long
    EXPECT_FALSE(pub.publish(too_long, 0, 0));
    EXPECT_EQ(pub.load().seq, 0u);
}

TEST(ListenKeyPublisher, AcceptsKeyOneByteUnderBufferLen) {
    ListenKeyPublisher pub;
    std::string fits(kListenKeyBufferLen - 1, 'y');
    EXPECT_TRUE(pub.publish(fits, 0, 0));
    EXPECT_EQ(pub.load().view(), fits);
}

TEST(ListenKeyPublisher, RepeatedPublishesAdvanceSeqWithoutRefusingEarly) {
    ListenKeyPublisher pub;
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(pub.publish("k", 0, 0));
    }
    EXPECT_EQ(pub.load().seq, 1000u);
}
