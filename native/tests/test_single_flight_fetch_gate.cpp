// 批次 6 6b-0f-3a: single_flight_fetch_gate.hpp tests -- Boost-free, no network.
//
// The gate runs the Fetcher on a REAL worker thread, so these tests use a fetcher that blocks until
// the test releases it: that makes "in flight", "completed", "never a second fetch while one runs"
// and "poll() never blocks" deterministic instead of timing-dependent. The gate's clock is injected
// (poll(..., now_ms)), so cooldowns are asserted with synthetic milliseconds and no sleeping. The
// binary carries the `concurrency` label: worker thread + mailbox is exactly what TSan is for.

#include <gtest/gtest.h>
#include <hengyuan/single_flight_fetch_gate.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using hy::FetchGateState;
using hy::SingleFlightFetchGate;

namespace {

struct Req {
    int id{0};
    std::string tag;
};

struct Res {
    int fetch_no{0};
    int req_id{0};
    std::string tag;
    std::int64_t cooldown_ms{0};
};

constexpr std::int64_t kFailureCooldown = 5000;

// A fetcher the test drives by hand: fetch #n blocks until allow_next() has been called n times.
class Controlled {
public:
    Res operator()(const Req& r) {
        const int n = ++calls;
        std::unique_lock<std::mutex> lock(m_);
        // Time-bounded: if a failed ASSERT abandons a test while this fetch is still blocked, the
        // gate's destructor would otherwise join a worker nobody will ever release and hang the run.
        cv_.wait_for(lock, std::chrono::seconds(10), [&] { return allowed_ >= n; });
        if (throw_next_) {
            throw_next_ = false;
            throw std::runtime_error("fetcher failed");
        }
        Res out;
        out.fetch_no = n;
        out.req_id = r.id;
        out.tag = r.tag;
        out.cooldown_ms = next_cooldown_ms_;
        ++finished;
        return out;
    }

    // Applies to the NEXT fetch to be released.
    void plan(std::int64_t cooldown_ms, bool throws = false) {
        std::lock_guard<std::mutex> lock(m_);
        next_cooldown_ms_ = cooldown_ms;
        throw_next_ = throws;
    }

    void allow_next() {
        {
            std::lock_guard<std::mutex> lock(m_);
            ++allowed_;
        }
        cv_.notify_all();
    }

    std::atomic<int> calls{0};     // fetches that have ENTERED the fetcher
    std::atomic<int> finished{0};  // fetches that returned normally

private:
    std::mutex m_;
    std::condition_variable cv_;
    int allowed_{0};
    std::int64_t next_cooldown_ms_{0};
    bool throw_next_{false};
};

using Gate = SingleFlightFetchGate<Req, Res>;

Gate::CooldownFn from_outcome() {
    return [](const Res& r) { return r.cooldown_ms; };
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Polls (with the wanted flag as given) until an outcome appears or the timeout hits.
std::optional<Res> poll_for_outcome(Gate& gate, const Req& req, std::int64_t now_ms, bool wanted = true) {
    std::optional<Res> out;
    wait_until([&] {
        out = gate.poll(wanted, req, now_ms);
        return out.has_value();
    });
    return out;
}

}  // namespace

TEST(SingleFlightFetchGate, NothingIsStartedUnlessAFetchIsWanted) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); });
    for (int i = 0; i < 200; ++i) EXPECT_FALSE(gate.poll(false, Req{1, "x"}, i).has_value());
    EXPECT_EQ(gate.state(), FetchGateState::Idle);
    EXPECT_EQ(gate.stats().fetches_started, 0U);
    EXPECT_EQ(c.calls.load(), 0);
}

TEST(SingleFlightFetchGate, WantedStartsExactlyOneFetchAndNeverASecondWhileItRuns) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); });
    gate.poll(true, Req{1, "x"}, 0);
    EXPECT_EQ(gate.state(), FetchGateState::InFlight);
    EXPECT_TRUE(gate.in_flight());
    ASSERT_TRUE(wait_until([&] { return c.calls.load() == 1; }));

    for (int i = 1; i <= 500; ++i) EXPECT_FALSE(gate.poll(true, Req{i, "y"}, i).has_value());
    EXPECT_EQ(gate.stats().fetches_started, 1U);
    EXPECT_EQ(c.calls.load(), 1) << "single-flight violated: a second fetch started while the first was running";

    c.allow_next();
    ASSERT_TRUE(poll_for_outcome(gate, Req{1, "x"}, 1000).has_value());
}

TEST(SingleFlightFetchGate, PollNeverBlocksOnARunningFetch) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); });
    gate.poll(true, Req{1, "x"}, 0);
    ASSERT_TRUE(wait_until([&] { return c.calls.load() == 1; }));

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20'000; ++i) (void)gate.poll(true, Req{1, "x"}, i);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::milliseconds(1500)) << "poll() waited for the fetch";

    c.allow_next();
    ASSERT_TRUE(poll_for_outcome(gate, Req{1, "x"}, 0).has_value());
}

TEST(SingleFlightFetchGate, ACompletedOutcomeIsDeliveredExactlyOnce) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); });
    gate.poll(true, Req{42, "alpha"}, 0);
    c.allow_next();

    const auto out = poll_for_outcome(gate, Req{42, "alpha"}, 10);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->fetch_no, 1);
    EXPECT_EQ(out->req_id, 42);
    EXPECT_EQ(out->tag, "alpha");
    EXPECT_EQ(gate.state(), FetchGateState::Idle);
    EXPECT_EQ(gate.stats().results_delivered, 1U);

    // Nothing is wanted now: no re-delivery, no new fetch.
    for (int i = 0; i < 50; ++i) EXPECT_FALSE(gate.poll(false, Req{42, "alpha"}, 20 + i).has_value());
    EXPECT_EQ(c.calls.load(), 1);
    EXPECT_EQ(gate.stats().results_delivered, 1U);
}

TEST(SingleFlightFetchGate, TheNextCycleReturnsItsOwnOutcomeNotAStaleOne) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); });

    gate.poll(true, Req{1, "first"}, 0);
    c.allow_next();
    const auto first = poll_for_outcome(gate, Req{1, "first"}, 0);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->fetch_no, 1);

    gate.poll(true, Req{2, "second"}, 1);  // no cooldown: starts immediately
    ASSERT_EQ(gate.state(), FetchGateState::InFlight);
    c.allow_next();
    const auto second = poll_for_outcome(gate, Req{2, "second"}, 1);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->fetch_no, 2);
    EXPECT_EQ(second->req_id, 2);
    EXPECT_EQ(second->tag, "second");
}

TEST(SingleFlightFetchGate, TheRequestIsCopiedByValueBeforePollReturns) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); });
    {
        auto req = std::make_unique<Req>(Req{7, std::string(64, 'o') + "riginal"});  // beyond SSO
        gate.poll(true, *req, 0);
        req->tag = std::string(64, 'M') + "UTATED";
        req->id = -1;
    }  // the caller's request is gone; the worker may only have started now
    c.allow_next();
    const auto out = poll_for_outcome(gate, Req{}, 0);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->req_id, 7);
    EXPECT_EQ(out->tag, std::string(64, 'o') + "riginal");
}

// --- cooldown ------------------------------------------------------------------------------------------

TEST(SingleFlightFetchGate, TheOutcomeDecidesTheCooldownAndItsBoundaryIsExact) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); }, from_outcome());
    c.plan(/*cooldown_ms=*/60'000);
    gate.poll(true, Req{1, "x"}, 1000);
    c.allow_next();
    ASSERT_TRUE(poll_for_outcome(gate, Req{1, "x"}, 2000).has_value());  // collected at t=2000

    EXPECT_EQ(gate.state(), FetchGateState::Cooldown);
    EXPECT_FALSE(gate.poll(true, Req{1, "x"}, 2000).has_value());
    EXPECT_FALSE(gate.poll(true, Req{1, "x"}, 62'000 - 1).has_value());
    // fetches_started is bumped synchronously on the polling thread; the fetcher's own counter is
    // bumped by the worker at some later moment, so it cannot prove that NO fetch was started.
    EXPECT_EQ(gate.stats().fetches_started, 1U) << "a fetch started inside the cooldown";
    EXPECT_EQ(gate.state(), FetchGateState::Cooldown);

    gate.poll(true, Req{1, "x"}, 62'000);  // cooldown elapsed exactly: a new fetch starts in the same call
    EXPECT_EQ(gate.state(), FetchGateState::InFlight);
    ASSERT_TRUE(wait_until([&] { return c.calls.load() == 2; }));
    c.allow_next();
    ASSERT_TRUE(poll_for_outcome(gate, Req{1, "x"}, 62'000).has_value());
}

TEST(SingleFlightFetchGate, AZeroCooldownOutcomeAllowsTheNextFetchImmediately) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); }, from_outcome());
    c.plan(0);
    gate.poll(true, Req{1, "x"}, 500);
    c.allow_next();
    ASSERT_TRUE(poll_for_outcome(gate, Req{1, "x"}, 500).has_value());
    EXPECT_EQ(gate.state(), FetchGateState::Idle);
    EXPECT_EQ(gate.stats().cooldowns_imposed, 0U);

    gate.poll(true, Req{1, "x"}, 500);  // the very same millisecond
    EXPECT_EQ(gate.state(), FetchGateState::InFlight);
    c.allow_next();
    ASSERT_TRUE(poll_for_outcome(gate, Req{1, "x"}, 500).has_value());
}

TEST(SingleFlightFetchGate, WithoutACooldownFunctionThereIsNeverACooldown) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); });  // no CooldownFn
    c.plan(999'999);                                  // ignored: nobody reads it
    gate.poll(true, Req{1, "x"}, 0);
    c.allow_next();
    ASSERT_TRUE(poll_for_outcome(gate, Req{1, "x"}, 0).has_value());
    EXPECT_EQ(gate.state(), FetchGateState::Idle);
}

TEST(SingleFlightFetchGate, TheCallerCanImposeACooldownOnAnOutcomeItRejected) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); });
    gate.poll(true, Req{1, "x"}, 0);
    c.allow_next();
    ASSERT_TRUE(poll_for_outcome(gate, Req{1, "x"}, 100).has_value());

    gate.impose_cooldown(100, 3000);
    EXPECT_EQ(gate.state(), FetchGateState::Cooldown);
    EXPECT_FALSE(gate.poll(true, Req{1, "x"}, 3099).has_value());
    EXPECT_EQ(gate.stats().fetches_started, 1U);
    gate.poll(true, Req{1, "x"}, 3100);
    EXPECT_EQ(gate.state(), FetchGateState::InFlight);
    c.allow_next();
    ASSERT_TRUE(poll_for_outcome(gate, Req{1, "x"}, 3100).has_value());
}

TEST(SingleFlightFetchGate, ImposingACooldownExtendsButNeverShortensOne) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); });
    gate.impose_cooldown(0, 1000);
    gate.impose_cooldown(0, 200);  // shorter: must not pull the deadline in
    gate.poll(true, Req{1, "x"}, 999);
    EXPECT_EQ(gate.stats().fetches_started, 0U);
    EXPECT_EQ(gate.state(), FetchGateState::Cooldown);

    gate.impose_cooldown(100, 5000);  // longer: until 5100
    gate.poll(true, Req{1, "x"}, 5099);
    EXPECT_EQ(gate.stats().fetches_started, 0U);
    gate.poll(true, Req{1, "x"}, 5100);
    EXPECT_EQ(gate.state(), FetchGateState::InFlight);
    c.allow_next();
    ASSERT_TRUE(poll_for_outcome(gate, Req{1, "x"}, 5100).has_value());
}

TEST(SingleFlightFetchGate, NonPositiveCooldownsAreIgnored) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); });
    gate.impose_cooldown(0, 0);
    gate.impose_cooldown(0, -5000);
    EXPECT_EQ(gate.state(), FetchGateState::Idle);
    EXPECT_EQ(gate.stats().cooldowns_imposed, 0U);
}

TEST(SingleFlightFetchGate, ACooldownCannotBeImposedWhileAFetchIsInFlight) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); });
    gate.poll(true, Req{1, "x"}, 0);
    ASSERT_TRUE(wait_until([&] { return c.calls.load() == 1; }));
    gate.impose_cooldown(0, 10'000);
    EXPECT_EQ(gate.state(), FetchGateState::InFlight);
    EXPECT_EQ(gate.stats().cooldowns_imposed, 0U);
    c.allow_next();
    ASSERT_TRUE(poll_for_outcome(gate, Req{1, "x"}, 0).has_value());
    EXPECT_EQ(gate.state(), FetchGateState::Idle) << "the ignored request leaked into a cooldown";
}

TEST(SingleFlightFetchGate, CooldownArithmeticSaturatesInsteadOfWrapping) {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); });
    gate.impose_cooldown(kMax - 5, 1000);  // would overflow
    EXPECT_EQ(gate.state(), FetchGateState::Cooldown);
    gate.poll(true, Req{1, "x"}, kMax - 5);
    EXPECT_EQ(gate.stats().fetches_started, 0U) << "an overflowed deadline released the cooldown at once";
    EXPECT_EQ(gate.state(), FetchGateState::Cooldown);
    gate.poll(true, Req{1, "x"}, kMax);  // saturated deadline reached
    EXPECT_EQ(gate.state(), FetchGateState::InFlight);
    c.allow_next();
    ASSERT_TRUE(poll_for_outcome(gate, Req{1, "x"}, kMax).has_value());
}

// --- lifecycle ---------------------------------------------------------------------------------------------------

TEST(SingleFlightFetchGate, AResultIsStillDeliveredWhenNothingIsWantedAnyMore) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); });
    gate.poll(true, Req{1, "x"}, 0);
    ASSERT_TRUE(wait_until([&] { return c.calls.load() == 1; }));
    c.allow_next();
    // `wanted` only matters when idle: the finished fetch is collected regardless, and not restarted.
    const auto out = poll_for_outcome(gate, Req{1, "x"}, 0, /*wanted=*/false);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(gate.state(), FetchGateState::Idle);
    EXPECT_EQ(gate.stats().fetches_started, 1U);
}

TEST(SingleFlightFetchGate, AFetcherThatThrowsIsContainedAndCoolsDown) {
    Controlled c;
    Gate gate([&c](const Req& r) { return c(r); }, from_outcome(), kFailureCooldown);
    c.plan(0, /*throws=*/true);
    gate.poll(true, Req{1, "x"}, 100);
    c.allow_next();

    // The outcome is "no result": nothing is delivered, the process must not have terminated.
    ASSERT_TRUE(wait_until([&] {
        (void)gate.poll(false, Req{1, "x"}, 200);
        return gate.state() != FetchGateState::InFlight;
    }));
    EXPECT_EQ(gate.stats().fetcher_exceptions, 1U);
    EXPECT_EQ(gate.stats().results_delivered, 0U);
    EXPECT_EQ(gate.state(), FetchGateState::Cooldown);

    EXPECT_FALSE(gate.poll(true, Req{1, "x"}, 200 + kFailureCooldown - 1).has_value());
    EXPECT_EQ(gate.stats().fetches_started, 1U);

    gate.poll(true, Req{1, "x"}, 200 + kFailureCooldown);  // recovers: a normal fetch works afterwards
    ASSERT_EQ(gate.state(), FetchGateState::InFlight);
    c.allow_next();
    const auto out = poll_for_outcome(gate, Req{1, "x"}, 200 + kFailureCooldown);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->fetch_no, 2);
}

TEST(SingleFlightFetchGate, TheDestructorJoinsAFetchThatIsStillRunning) {
    Controlled c;
    auto gate = std::make_unique<Gate>([&c](const Req& r) { return c(r); });
    gate->poll(true, Req{1, "x"}, 0);
    ASSERT_TRUE(wait_until([&] { return c.calls.load() == 1; }));

    std::thread releaser([&c] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        c.allow_next();
    });
    gate.reset();  // must wait for the running fetch (no detached thread outliving the gate)
    EXPECT_EQ(c.finished.load(), 1) << "the gate was destroyed while its worker was still running";
    releaser.join();
}

TEST(SingleFlightFetchGate, ALargeOutcomeSurvivesTheMailboxIntact) {
    struct BigReq {};
    struct Big {
        std::array<unsigned char, 65'536> bytes{};
        std::uint64_t checksum{0};
    };
    SingleFlightFetchGate<BigReq, Big> gate([](const BigReq&) {
        Big b;
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < b.bytes.size(); ++i) {
            b.bytes[i] = static_cast<unsigned char>((i * 31U + 7U) & 0xFFU);
            sum += b.bytes[i];
        }
        b.checksum = sum;
        return b;
    });

    std::optional<Big> out;
    gate.poll(true, BigReq{}, 0);
    ASSERT_TRUE(wait_until([&] {
        out = gate.poll(false, BigReq{}, 0);
        return out.has_value();
    }));
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < out->bytes.size(); ++i) {
        ASSERT_EQ(out->bytes[i], static_cast<unsigned char>((i * 31U + 7U) & 0xFFU)) << "byte " << i;
        sum += out->bytes[i];
    }
    EXPECT_EQ(sum, out->checksum);
}

TEST(SingleFlightFetchGate, StateNamesAreDistinct) {
    EXPECT_STRNE(hy::fetch_gate_state_name(FetchGateState::Idle), hy::fetch_gate_state_name(FetchGateState::InFlight));
    EXPECT_STRNE(hy::fetch_gate_state_name(FetchGateState::Idle), hy::fetch_gate_state_name(FetchGateState::Cooldown));
    EXPECT_STRNE(hy::fetch_gate_state_name(FetchGateState::InFlight),
                 hy::fetch_gate_state_name(FetchGateState::Cooldown));
    for (FetchGateState s : {FetchGateState::Idle, FetchGateState::InFlight, FetchGateState::Cooldown}) {
        EXPECT_STRNE(hy::fetch_gate_state_name(s), "?");
    }
}
