#include <gtest/gtest.h>
#include <hengyuan/submit_authority.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>

namespace {

TEST(SubmitAuthorityConcurrency, ClaimThenRevokeInvalidatesTheClaimEpoch) {
    hy::SubmitAuthority authority;
    const std::uint64_t original = authority.epoch();

    ASSERT_TRUE(authority.claim(original));
    EXPECT_EQ(authority.epoch(), original + 1U);
    EXPECT_FALSE(authority.claim(original)) << "a claim consumes its epoch exactly once";

    authority.revoke();
    EXPECT_EQ(authority.epoch(), original + 2U);
    EXPECT_FALSE(authority.claim(original + 1U)) << "revocation invalidates the next epoch too";
}

TEST(SubmitAuthorityConcurrency, RevokeThenClaimRejectsTheOldEpoch) {
    hy::SubmitAuthority authority;
    const std::uint64_t original = authority.epoch();

    authority.revoke();
    const std::uint64_t current = authority.epoch();
    ASSERT_EQ(current, original + 1U);
    EXPECT_FALSE(authority.claim(original));
    EXPECT_TRUE(authority.claim(current));
}

TEST(SubmitAuthorityConcurrency, OneClaimWinsAmongThreadsUsingTheSameEpoch) {
    hy::SubmitAuthority authority;
    const std::uint64_t token = authority.epoch();
    constexpr std::size_t kThreads = 16;
    std::barrier start_line(static_cast<std::ptrdiff_t>(kThreads));
    std::atomic<std::uint32_t> successes{0};
    std::array<std::thread, kThreads> threads;

    for (auto& thread : threads) {
        thread = std::thread([&] {
            start_line.arrive_and_wait();
            if (authority.claim(token)) successes.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& thread : threads) thread.join();

    EXPECT_EQ(successes.load(std::memory_order_relaxed), 1U);
    EXPECT_EQ(authority.epoch(), token + 1U);
    EXPECT_FALSE(authority.claim(token));
}

TEST(SubmitAuthorityConcurrency, EpochAcquireObservesStatePublishedBeforeRevoke) {
    hy::SubmitAuthority authority;
    const std::uint64_t original = authority.epoch();
    std::uint64_t published_payload = 0;
    std::uint64_t observed_payload = 0;
    std::uint64_t observed_epoch = original;

    std::thread producer([&] {
        published_payload = 0x5a17c0deULL;
        authority.revoke();
    });
    std::thread observer([&] {
        do {
            observed_epoch = authority.epoch();
            if (observed_epoch == original) std::this_thread::yield();
        } while (observed_epoch == original);
        observed_payload = published_payload;
    });

    producer.join();
    observer.join();

    EXPECT_EQ(observed_epoch, original + 1U);
    EXPECT_EQ(observed_payload, 0x5a17c0deULL);
}

static_assert(!std::is_copy_constructible_v<hy::SubmitAuthority>);

}  // namespace
