// Private-stream migration M5a: the account refresh path (account_refresh.hpp) and its hook in the sweep (barrier_sweep.hpp SweepAccountStep,
// order_tracker.hpp poll_once()). Single-threaded and deterministic; the same code under two real threads is test_account_refresh_concurrency.cpp.
//
// The contract under test, in the order the tests below take it:
//   * AccountRefresher fetches at once on its first tick, then once per period (sooner after a fetch that did not deliver), stamps an update with the
//     clock reading it was given -- the start of the fetch -- tags it with epoch 0 (the periodic refresh answers no sweep), and never fetches with a clock
//     the holder would refuse; a full ring drops the new update;
//   * its sweep step ALWAYS fetches -- an update from before the sweep's last answer is what a sweep must not rest on -- and tags the update with the epoch;
//   * AccountHolder keeps the newest update by fetch start, re-stamps the snapshot with that start, answers refreshed_by(epoch) from the epoch tags alone
//     (never from a clock reading, never from a periodic update, never for a later epoch than the one fetched) after draining the ring itself, and judges
//     "fresh enough to trade" with check_freshness() and the pre-trade limits -- the predicate validate_pre_trade() uses;
//   * poll_once() runs the job's account step once per epoch, after every order is answered and BEFORE the marker, a full event ring retries the marker
//     and not the step, and a newer epoch gets a step of its own;
//   * end to end: the barrier completes only on the account the sweep itself fetched; a failed or refused step fetch leaves it Sweeping, whatever
//     periodic updates -- older or newer than the request -- are on the ring.
#include <gtest/gtest.h>

#include <hengyuan/account_refresh.hpp>
#include <hengyuan/kill_switch.hpp>
#include <hengyuan/order_tracker.hpp>
#include <hengyuan/startup_recovery.hpp>
#include <hengyuan/submit_readiness.hpp>
#include <hengyuan/sweep_pump.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

using namespace hy;

namespace {

AccountSnapshot make_snapshot(std::int64_t usdt_free, bool can_trade = true, std::int64_t stamp = 1'700'000'000'000) {
    AccountSnapshotBuilder b;
    b.set_can_trade(can_trade);
    EXPECT_EQ(b.add_asset("USDT", usdt_free, 7), AccountSnapshotError::None);
    EXPECT_EQ(b.add_asset("BTC", 3), AccountSnapshotError::None);
    AccountSnapshot out{};
    EXPECT_EQ(b.build(stamp, out), AccountSnapshotError::None);
    return out;
}

// The scripted exchange side of the port: the next outcomes in order (the last one repeats), and the snapshot to return.
struct FakeAccountPort {
    std::vector<AccountFetchOutcome> outcomes{AccountFetchOutcome::Fetched};
    std::size_t calls{0};
    std::int64_t next_usdt{1'000};

    static AccountFetchOutcome call(void* self, AccountSnapshot& out) noexcept {
        auto* p = static_cast<FakeAccountPort*>(self);
        const std::size_t i = p->calls < p->outcomes.size() ? p->calls : p->outcomes.size() - 1;
        ++p->calls;
        const AccountFetchOutcome o = p->outcomes[i];
        if (o == AccountFetchOutcome::Fetched) out = make_snapshot(p->next_usdt++);
        return o;
    }
    [[nodiscard]] AccountFetchPort port() { return AccountFetchPort{&FakeAccountPort::call, this}; }
};

std::vector<AccountUpdate> drain(AccountUpdateRing& ring) {
    std::vector<AccountUpdate> out;
    AccountUpdate u{};
    while (ring.try_pop(u)) out.push_back(u);
    return out;
}

constexpr AccountRefreshPolicy kPolicy{10'000, 2'000};

// Runs the refresher's sweep step the way SweepJob does.
void run_step(AccountRefresher& r, std::uint64_t epoch, std::int64_t sweep_started_ms, std::int64_t now_ms) {
    const SweepAccountStep step = r.sweep_step();
    step.fn(step.user_data, epoch, sweep_started_ms, now_ms);
}

// --- AccountRefresher ------------------------------------------------------------------------------------------------------------

class AccountRefresherTest : public ::testing::Test {
protected:
    AccountUpdateRing ring_;
    FakeAccountPort fake_;
    AccountRefresher refresher_{ring_, fake_.port(), kPolicy};
};

}  // namespace

TEST_F(AccountRefresherTest, TheFirstTickFetchesAtOnceStampsTheStartOfTheFetchAndTagsNoSweep) {
    refresher_.tick(5'000);
    const std::vector<AccountUpdate> updates = drain(ring_);
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0].fetch_started_ms, 5'000) << "the stamp is the clock reading the fetch started at, as given";
    EXPECT_EQ(updates[0].sweep_epoch, 0U) << "the periodic refresh answers no sweep";
    ASSERT_NE(updates[0].snapshot.find("USDT"), nullptr);
    EXPECT_EQ(updates[0].snapshot.find("USDT")->free_ticks(), 1'000);
    EXPECT_EQ(refresher_.stats().attempts, 1U);
    EXPECT_EQ(refresher_.stats().delivered, 1U);
}

TEST_F(AccountRefresherTest, AfterADeliveryItWaitsAFullPeriod) {
    refresher_.tick(5'000);
    refresher_.tick(5'000 + kPolicy.period_ms - 1);
    EXPECT_EQ(fake_.calls, 1U) << "one millisecond early";
    refresher_.tick(5'000 + kPolicy.period_ms);
    EXPECT_EQ(fake_.calls, 2U) << "due exactly at the period";
    EXPECT_EQ(drain(ring_).size(), 2U);
}

TEST_F(AccountRefresherTest, AFetchThatDoesNotDeliverIsRetriedSoonerThanThePeriodButNeverInATightLoop) {
    fake_.outcomes = {AccountFetchOutcome::Failed, AccountFetchOutcome::NotSent, AccountFetchOutcome::Fetched};
    refresher_.tick(5'000);  // Failed
    EXPECT_EQ(refresher_.stats().failed, 1U);
    refresher_.tick(5'000 + kPolicy.retry_ms - 1);
    EXPECT_EQ(fake_.calls, 1U) << "no retry before the retry interval";
    refresher_.tick(5'000 + kPolicy.retry_ms);  // NotSent
    EXPECT_EQ(fake_.calls, 2U);
    EXPECT_EQ(refresher_.stats().not_sent, 1U);
    EXPECT_EQ(refresher_.stats().delivered, 0U) << "nothing delivered yet";
    refresher_.tick(5'000 + 2 * kPolicy.retry_ms);  // Fetched
    EXPECT_EQ(fake_.calls, 3U);
    EXPECT_EQ(drain(ring_).size(), 1U);
    refresher_.tick(5'000 + 3 * kPolicy.retry_ms);
    EXPECT_EQ(fake_.calls, 3U) << "after a delivery the full period applies again, not the retry interval";
}

TEST_F(AccountRefresherTest, AFullRingDropsTheNewUpdateAndCountsItAsNotDelivered) {
    std::size_t filled = 0;
    while (ring_.try_push(AccountUpdate{static_cast<std::int64_t>(filled + 1), 0, make_snapshot(1)})) ++filled;
    ASSERT_GE(filled, kAccountUpdateRingCapacity - 1) << "the ring holds at least capacity - 1 updates";
    refresher_.tick(5'000);
    EXPECT_EQ(fake_.calls, 1U) << "the port was asked: the ring's state is only known on the push";
    EXPECT_EQ(refresher_.stats().ring_full, 1U);
    EXPECT_EQ(refresher_.stats().delivered, 0U) << "a dropped update is not a delivery";
    const std::vector<AccountUpdate> old_updates = drain(ring_);
    ASSERT_EQ(old_updates.size(), filled) << "the old updates are intact";
    for (std::size_t i = 0; i < old_updates.size(); ++i) EXPECT_EQ(old_updates[i].fetch_started_ms, static_cast<std::int64_t>(i + 1)) << "FIFO";
    refresher_.tick(5'000 + kPolicy.retry_ms);
    EXPECT_EQ(fake_.calls, 2U) << "a drop schedules the quick retry";
    EXPECT_EQ(refresher_.stats().delivered, 1U) << "...and with room again it is delivered";
}

TEST_F(AccountRefresherTest, ANonPositiveClockIsNeverFetchedWith) {
    refresher_.tick(0);
    refresher_.tick(-7);
    EXPECT_EQ(fake_.calls, 0U) << "the holder would refuse the stamp: do not spend a REST call on it";
    EXPECT_EQ(refresher_.stats().bad_clock, 1U) << "the second tick is inside the retry interval of the first";
    EXPECT_EQ(refresher_.stats().attempts, 0U);
    EXPECT_TRUE(drain(ring_).empty());
}

TEST_F(AccountRefresherTest, AnUnwiredPortIsNotSent) {
    AccountUpdateRing ring;
    AccountRefresher unwired{ring, AccountFetchPort{}, kPolicy};
    unwired.tick(5'000);
    EXPECT_EQ(unwired.stats().not_sent, 1U);
    EXPECT_TRUE(drain(ring).empty());
}

TEST(AccountRefresherPolicy, AnInvalidPolicyIsClampedNotLooped) {
    AccountUpdateRing ring;
    FakeAccountPort fake;
    AccountRefresher refresher{ring, fake.port(), AccountRefreshPolicy{0, -5}};
    refresher.tick(100);
    EXPECT_EQ(fake.calls, 1U);
    refresher.tick(100);
    EXPECT_EQ(fake.calls, 1U) << "even a zero period does not make the same millisecond due again";
    refresher.tick(101);
    EXPECT_EQ(fake.calls, 2U) << "...but the next millisecond is";
}

TEST(AccountRefresherPolicy, TheNextDueTimeSaturatesInsteadOfWrapping) {
    AccountUpdateRing ring;
    FakeAccountPort fake;
    AccountRefresher refresher{ring, fake.port(), kPolicy};
    const std::int64_t near_max = std::numeric_limits<std::int64_t>::max() - 5;
    refresher.tick(near_max);
    refresher.tick(near_max + 1);
    EXPECT_EQ(fake.calls, 1U) << "a wrapped due time would be in the past and fetch again at once";
}

// --- the sweep step ----------------------------------------------------------------------------------------------------------------

TEST_F(AccountRefresherTest, ASweepStepFetchesAndTagsTheUpdateWithItsEpoch) {
    refresher_.tick(1'000);  // a periodic update from before the sweep
    ASSERT_EQ(drain(ring_).size(), 1U);
    run_step(refresher_, /*epoch=*/5, /*sweep_started_ms=*/2'000, /*now_ms=*/2'500);
    const std::vector<AccountUpdate> updates = drain(ring_);
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0].sweep_epoch, 5U);
    EXPECT_EQ(updates[0].fetch_started_ms, 2'500) << "the step's fetch is stamped with the tick's clock";
    EXPECT_EQ(refresher_.stats().sweep_steps, 1U);
    EXPECT_EQ(refresher_.stats().sweep_delivered, 1U);
}

TEST_F(AccountRefresherTest, ASweepStepFetchesEvenWhenAPeriodicUpdateWasJustDelivered) {
    refresher_.tick(3'000);
    ASSERT_EQ(drain(ring_).size(), 1U);
    run_step(refresher_, 2, /*sweep_started_ms=*/3'000, /*now_ms=*/3'000);  // the very same clock reading
    EXPECT_EQ(fake_.calls, 2U) << "the periodic fetch ran BEFORE this tick's queries; only the step's runs after the sweep's last answer";
    const std::vector<AccountUpdate> updates = drain(ring_);
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0].sweep_epoch, 2U);
}

TEST_F(AccountRefresherTest, AFailedFetchForASweepDeliversNothingAndTheRetryComesSoon) {
    fake_.outcomes = {AccountFetchOutcome::Failed, AccountFetchOutcome::Fetched};
    run_step(refresher_, 1, 2'000, 2'100);
    EXPECT_TRUE(drain(ring_).empty());
    EXPECT_EQ(refresher_.stats().failed, 1U);
    EXPECT_EQ(refresher_.stats().sweep_delivered, 0U);
    refresher_.tick(2'100 + kPolicy.retry_ms - 1);
    EXPECT_EQ(fake_.calls, 1U);
    refresher_.tick(2'100 + kPolicy.retry_ms);
    EXPECT_EQ(fake_.calls, 2U) << "the sweep's failed fetch scheduled the quick retry for the periodic step";
}

TEST_F(AccountRefresherTest, ADeliveredSweepFetchPostponesThePeriodicOne) {
    run_step(refresher_, 1, 2'000, 2'100);
    refresher_.tick(2'100 + kPolicy.period_ms - 1);
    EXPECT_EQ(fake_.calls, 1U) << "the account was just fetched for the sweep";
    refresher_.tick(2'100 + kPolicy.period_ms);
    EXPECT_EQ(fake_.calls, 2U);
}

TEST_F(AccountRefresherTest, TheStepIsTheTrampolineToThisRefresher) {
    const SweepAccountStep step = refresher_.sweep_step();
    EXPECT_NE(step.fn, nullptr);
    EXPECT_EQ(step.user_data, &refresher_);
}

// --- AccountHolder -------------------------------------------------------------------------------------------------------------------

namespace {

class AccountHolderTest : public ::testing::Test {
protected:
    AccountUpdateRing ring_;
    AccountHolder holder_{ring_};

    void push(std::int64_t started_ms, std::int64_t usdt_free = 1'000, bool can_trade = true, std::uint64_t sweep_epoch = 0) {
        ASSERT_TRUE(ring_.try_push(AccountUpdate{started_ms, sweep_epoch, make_snapshot(usdt_free, can_trade)}));
    }
};

}  // namespace

TEST_F(AccountHolderTest, BeforeTheFirstUpdateItHoldsNothingAndSaysNeverFetched) {
    holder_.poll();
    EXPECT_FALSE(holder_.has_snapshot());
    EXPECT_FALSE(holder_.refreshed_by(1));
    EXPECT_FALSE(holder_.fresh(1'000, ExposureLimits{}));
    EXPECT_EQ(holder_.view().timestamp_ms(), 0);
    EXPECT_FALSE(holder_.view().can_trade());
    EXPECT_EQ(holder_.fetch_started_ms(), 0);
    EXPECT_EQ(holder_.refreshed_epoch(), 0U);
    EXPECT_EQ(check_freshness(holder_.view().timestamp_ms(), 1'000, 30'000), FreshnessStatus::NeverFetched) << "what validate_pre_trade() sees";
}

TEST_F(AccountHolderTest, AnUpdateIsRestampedWithItsFetchStartAndKeepsItsAssets) {
    push(40'000, 1'234);
    holder_.poll();
    ASSERT_TRUE(holder_.has_snapshot());
    EXPECT_EQ(holder_.view().timestamp_ms(), 40'000) << "re-stamped from the exchange epoch of the port's snapshot to the steady clock";
    EXPECT_EQ(holder_.fetch_started_ms(), 40'000);
    ASSERT_NE(holder_.view().find("USDT"), nullptr);
    EXPECT_EQ(holder_.view().find("USDT")->free_ticks(), 1'234);
    EXPECT_EQ(holder_.view().find("USDT")->locked_ticks(), 7);
    ASSERT_NE(holder_.view().find("BTC"), nullptr);
    EXPECT_TRUE(holder_.view().can_trade());
    EXPECT_EQ(holder_.stats().applied, 1U);
}

TEST_F(AccountHolderTest, TheCanTradeFlagOfTheExchangeIsKept) {
    push(40'000, 1'000, /*can_trade=*/false);
    holder_.poll();
    EXPECT_FALSE(holder_.view().can_trade()) << "an account the exchange says cannot trade must not become tradable by being re-stamped";
}

TEST_F(AccountHolderTest, RefreshedByIsTrueForTheEpochOfASweepUpdateAndForEarlierOnesOnly) {
    push(1'000, 1'000, true, /*sweep_epoch=*/3);
    EXPECT_TRUE(holder_.refreshed_by(3));
    EXPECT_TRUE(holder_.refreshed_by(2)) << "fetched by a LATER sweep, whose step also ran after the answers of the earlier one";
    EXPECT_TRUE(holder_.refreshed_by(1));
    EXPECT_FALSE(holder_.refreshed_by(4)) << "the sweep of epoch 4 has not fetched anything";
    EXPECT_FALSE(holder_.refreshed_by(0)) << "0 is no epoch";
    EXPECT_EQ(holder_.refreshed_epoch(), 3U);
}

TEST_F(AccountHolderTest, APeriodicUpdateNeverAnswersASweepHoweverNewItIs) {
    push(1'000, 1, true, /*sweep_epoch=*/1);
    push(5'000, 2, true, /*sweep_epoch=*/0);  // a periodic update, much newer
    holder_.poll();
    EXPECT_EQ(holder_.fetch_started_ms(), 5'000) << "the periodic update is the held snapshot, for trading";
    EXPECT_TRUE(holder_.refreshed_by(1));
    EXPECT_FALSE(holder_.refreshed_by(2)) << "...but it is not the answer to the sweep of epoch 2";
}

TEST_F(AccountHolderTest, WithOnlyPeriodicUpdatesNoSweepIsEverAnswered) {
    push(1'000);
    push(2'000);
    push(1'000'000);
    for (std::uint64_t e = 1; e <= 5; ++e) EXPECT_FALSE(holder_.refreshed_by(e)) << e;
}

TEST_F(AccountHolderTest, RefreshedByDrainsTheRingItself) {
    push(1'000, 1'000, true, 2);
    EXPECT_FALSE(holder_.has_snapshot()) << "nothing polled yet";
    EXPECT_TRUE(holder_.refreshed_by(2)) << "the marker path: the update sitting on the ring counts without a separate poll()";
    EXPECT_TRUE(holder_.has_snapshot());
}

TEST_F(AccountHolderTest, TheNewestUpdateWinsAndAnOlderOneArrivingLaterIsRefused) {
    push(2'000, 2);
    push(1'500, 1);  // cannot happen on a FIFO ring from one producer; the holder refuses it all the same
    push(2'000, 3);  // the same start: the later arrival is at least as new
    holder_.poll();
    EXPECT_EQ(holder_.fetch_started_ms(), 2'000);
    EXPECT_EQ(holder_.view().find("USDT")->free_ticks(), 3);
    EXPECT_EQ(holder_.stats().applied, 2U);
    EXPECT_EQ(holder_.stats().stale, 1U);
}

TEST_F(AccountHolderTest, ARefusedUpdateAnswersNoSweep) {
    push(0, 3, true, 7);   // refused by the builder (no positive stamp); nothing is held yet, so it is not 'stale' either
    push(-5, 4, true, 8);
    push(2'000, 1, true, 5);
    push(1'500, 2, true, 9);  // refused as stale: the sweep it claims to answer is not answered by it
    holder_.poll();
    EXPECT_EQ(holder_.refreshed_epoch(), 5U);
    EXPECT_FALSE(holder_.refreshed_by(6));
    EXPECT_EQ(holder_.stats().stale, 1U);
    EXPECT_EQ(holder_.stats().rejected, 2U);
}

TEST_F(AccountHolderTest, AnUpdateWithANonPositiveStampIsRejectedAndTheHeldOneStays) {
    push(0);
    push(-5);
    holder_.poll();
    EXPECT_FALSE(holder_.has_snapshot());
    EXPECT_EQ(holder_.stats().rejected, 2U);
    push(3'000);
    push(0);
    holder_.poll();
    EXPECT_EQ(holder_.fetch_started_ms(), 3'000);
}

TEST_F(AccountHolderTest, FreshUsesThePreTradePredicateAndLimits) {
    push(100'000);
    holder_.poll();
    const ExposureLimits limits{};  // 30 s window, the default future skew
    EXPECT_TRUE(holder_.fresh(100'000, limits));
    EXPECT_TRUE(holder_.fresh(100'000 + limits.freshness_max_age_ms, limits)) << "exactly at the window";
    EXPECT_FALSE(holder_.fresh(100'000 + limits.freshness_max_age_ms + 1, limits));
    EXPECT_TRUE(holder_.fresh(100'000 - limits.max_future_skew_ms, limits)) << "a reading ahead of the caller's clock by the tolerated skew";
    EXPECT_FALSE(holder_.fresh(100'000 - limits.max_future_skew_ms - 1, limits)) << "ahead by more: not a fresh reading from anywhere";
    ExposureLimits tight{};
    tight.freshness_max_age_ms = 500;
    EXPECT_TRUE(holder_.fresh(100'500, tight));
    EXPECT_FALSE(holder_.fresh(100'501, tight));
    // the same answer as the function validate_pre_trade() calls, over a grid
    for (std::int64_t now = 90'000; now <= 140'000; now += 777) {
        EXPECT_EQ(holder_.fresh(now, limits),
                  check_freshness(holder_.view().timestamp_ms(), now, limits.freshness_max_age_ms, limits.max_future_skew_ms) == FreshnessStatus::Fresh)
            << now;
    }
}

TEST_F(AccountHolderTest, TheProbeIsRefreshedByOfThisHolder) {
    push(1'000, 1'000, true, 4);
    const SweepAccountProbe probe = holder_.sweep_probe();
    EXPECT_TRUE(probe.refreshed_by(4));
    EXPECT_FALSE(probe.refreshed_by(5));
    EXPECT_EQ(probe.user_data, &holder_);
}

TEST_F(AccountHolderTest, ThePointerToTheViewIsStableAcrossUpdates) {
    const AccountSnapshot* wired = &holder_.view();
    push(1'000, 1);
    holder_.poll();
    EXPECT_EQ(&holder_.view(), wired) << "OrchestratorContext::account is wired once and keeps pointing at the held snapshot";
    EXPECT_EQ(wired->find("USDT")->free_ticks(), 1);
    push(2'000, 2);
    holder_.poll();
    EXPECT_EQ(wired->find("USDT")->free_ticks(), 2);
}

// --- poll_once() and the account step ---------------------------------------------------------------------------------------------

namespace {

OrderRecord ambiguous_record(const char* coid) {
    OrderRecord rec{};
    std::snprintf(rec.client_order_id.id, sizeof(rec.client_order_id.id), "%s", coid);
    rec.symbol_id = 1;
    rec.intended_price_ticks = 5000;
    rec.intended_qty_ticks = 100;
    rec.exchange_order_id = 555;
    rec.state = OrderState::Submitting;
    rec.transition_to(OrderState::Ambiguous);
    return rec;
}

struct ScriptedExchange {
    bool answers{true};  // false = every query is inconclusive
};

QueryResult scripted_query(const OrderExpectation& expected, void* user_data) {
    if (!static_cast<ScriptedExchange*>(user_data)->answers) return QueryResult{};
    return QueryResult{QueryOutcome::Found, OrderState::Filled, 555, expected.intended_qty_ticks, 5000};
}

// A recording step: how often it ran, with what, and whether the marker was already on the event ring when it ran.
struct StepLog {
    ReconcileEventRing* events{nullptr};
    std::vector<std::tuple<std::uint64_t, std::int64_t, std::int64_t>> calls;  // (epoch, sweep_started_ms, now_ms)
    bool marker_was_already_there{false};
    bool resolution_was_already_there{false};
    bool look{true};  // look at (and so consume) what is on the event ring when the step runs; a test about a FULL ring must leave it alone
};

void recording_step(void* user_data, std::uint64_t epoch, std::int64_t sweep_started_ms, std::int64_t now_ms) noexcept {
    auto* log = static_cast<StepLog*>(user_data);
    log->calls.emplace_back(epoch, sweep_started_ms, now_ms);
    if (!log->look) return;
    // The reconcile thread is the producer of this ring and the test is single-threaded, so popping here is only a way to LOOK at what is in it.
    ReconcileEvent ev{};
    while (log->events->try_pop(ev)) {
        if (ev.kind == ReconcileEventKind::SweepDone) log->marker_was_already_there = true;
        else log->resolution_was_already_there = true;
    }
}

class SweepStepTest : public ::testing::Test {
protected:
    OrderTracker tracker_;
    ToReconcileRing inbound_;
    ReconcileEventRing outbound_;
    SweepRequestRing requests_;
    StepLog log_{&outbound_, {}, false, false, true};
    SweepJob job_{requests_, SweepAccountStep{&recording_step, &log_}};
    ScriptedExchange exchange_;
    QueryPort port_{&scripted_query, &exchange_};
    ReconcilePollPolicy policy_{};
    std::size_t next_slot_{0};

    void track(const OrderRecord& rec) { ASSERT_TRUE(tracker_.track(InFlightHandle{next_slot_++, 1}, rec, 0)); }
    void post(std::uint64_t epoch) { ASSERT_TRUE(requests_.try_push(SweepRequest{epoch})); }
    void tick(std::int64_t now_ms) { poll_once(tracker_, inbound_, outbound_, port_, policy_, now_ms, OrderRecord::kClockUnset, &job_); }
    std::vector<ReconcileEvent> drain_events() {
        std::vector<ReconcileEvent> out;
        ReconcileEvent ev{};
        while (outbound_.try_pop(ev)) out.push_back(ev);
        return out;
    }
};

}  // namespace

TEST_F(SweepStepTest, TheStepRunsOnceAfterEveryOrderIsAnsweredAndBeforeTheMarker) {
    track(ambiguous_record("HY-A"));
    post(1);
    tick(1'000);
    ASSERT_EQ(log_.calls.size(), 1U);
    EXPECT_EQ(std::get<0>(log_.calls[0]), 1U) << "the epoch of the sweep";
    EXPECT_EQ(std::get<1>(log_.calls[0]), 1'000) << "the reconcile clock reading at which the sweep began";
    EXPECT_EQ(std::get<2>(log_.calls[0]), 1'000);
    EXPECT_TRUE(log_.resolution_was_already_there) << "the step comes after the sweep's own resolutions";
    EXPECT_FALSE(log_.marker_was_already_there) << "...and BEFORE the marker: what the step publishes is in front of the marker";
    const std::vector<ReconcileEvent> events = drain_events();
    ASSERT_EQ(events.size(), 1U) << "the marker (the step's look consumed the resolution)";
    EXPECT_EQ(events[0].kind, ReconcileEventKind::SweepDone);
    EXPECT_EQ(job_.stats().account_steps, 1U);
}

TEST_F(SweepStepTest, TheStepDoesNotRunWhileAnOrderIsStillWaitingForAnAnswer) {
    track(ambiguous_record("HY-A"));
    exchange_.answers = false;
    post(1);
    tick(1'000);
    tick(1'300);
    EXPECT_TRUE(log_.calls.empty());
    EXPECT_EQ(job_.stats().account_steps, 0U);
    exchange_.answers = true;
    tick(1'600);  // inside the sweep retry interval of the query at 1300: the order is not asked yet
    EXPECT_TRUE(log_.calls.empty());
    tick(2'000);
    ASSERT_EQ(log_.calls.size(), 1U);
    EXPECT_EQ(std::get<1>(log_.calls[0]), 1'000) << "the sweep's start, not the tick that finished it";
    EXPECT_EQ(std::get<2>(log_.calls[0]), 2'000);
}

TEST_F(SweepStepTest, AFullEventRingRetriesTheMarkerAndNotTheStep) {
    log_.look = false;
    post(1);
    ReconcileEvent filler{};
    filler.kind = ReconcileEventKind::SweepDone;
    filler.sweep_epoch = 99;
    while (outbound_.try_push(filler)) {
    }
    tick(1'000);
    EXPECT_EQ(log_.calls.size(), 1U) << "the step ran: the sweep was complete";
    EXPECT_EQ(job_.stats().marker_push_failures, 1U);
    EXPECT_TRUE(job_.running());
    tick(1'100);
    EXPECT_EQ(log_.calls.size(), 1U) << "a second tick with the marker still due must not run the step again";
    EXPECT_EQ(job_.stats().marker_push_failures, 2U);
    ReconcileEvent ev{};
    ASSERT_TRUE(outbound_.try_pop(ev));  // make room
    tick(1'200);
    EXPECT_EQ(log_.calls.size(), 1U);
    EXPECT_FALSE(job_.running());
    EXPECT_EQ(job_.stats().account_steps, 1U);
}

TEST_F(SweepStepTest, ANewerEpochGetsAStepOfItsOwn) {
    post(1);
    tick(1'000);
    drain_events();
    post(2);
    tick(2'000);
    ASSERT_EQ(log_.calls.size(), 2U);
    EXPECT_EQ(std::get<0>(log_.calls[1]), 2U);
    EXPECT_EQ(std::get<1>(log_.calls[1]), 2'000);
    post(2);  // a repeated epoch is dropped by the job, so there is no new sweep and no new step
    tick(3'000);
    EXPECT_EQ(log_.calls.size(), 2U);
}

TEST_F(SweepStepTest, WithoutASweepNothingRuns) {
    track(ambiguous_record("HY-A"));
    tick(1'000);
    tick(2'000);
    EXPECT_TRUE(log_.calls.empty());
}

TEST(SweepStepAbsent, AJobWithoutAStepSweepsAsBefore) {
    OrderTracker tracker;
    ToReconcileRing inbound;
    ReconcileEventRing outbound;
    SweepRequestRing requests;
    SweepJob job{requests};
    ScriptedExchange exchange;
    const QueryPort port{&scripted_query, &exchange};
    ASSERT_TRUE(requests.try_push(SweepRequest{1}));
    poll_once(tracker, inbound, outbound, port, ReconcilePollPolicy{}, 1'000, OrderRecord::kClockUnset, &job);
    ReconcileEvent ev{};
    ASSERT_TRUE(outbound.try_pop(ev));
    EXPECT_EQ(ev.kind, ReconcileEventKind::SweepDone);
    EXPECT_EQ(job.stats().account_steps, 0U);
    EXPECT_EQ(job.stats().sweeps_completed, 1U);
}

// --- end to end: refresher -> ring -> holder -> pump -> barrier ------------------------------------------------------------------------

namespace {

struct Script {
    FeedObservation feed{};
    RecoveryObservation recovery{};
    bool clock_ready{true};
};
FeedObservation read_feed(void* p) noexcept { return static_cast<Script*>(p)->feed; }
RecoveryObservation read_recovery(void* p) noexcept { return static_cast<Script*>(p)->recovery; }
bool read_clock(void* p) noexcept { return static_cast<Script*>(p)->clock_ready; }

struct Durable {
    std::uint64_t frames{0};
};
AuditAppendResult durable_append(void* p, const AuditRecord&, std::int64_t) noexcept {
    auto* d = static_cast<Durable*>(p);
    AuditAppendResult r{};
    r.status = AuditAppendResult::Status::Acked;
    r.sequence = ++d->frames;
    return r;
}

// The whole path with the REAL components, single-threaded. Hot side: readiness (scripted feed), the pump with the holder's probe, the drain. Reconcile
// side: an empty tracker (so a sweep completes at once and the account is the only thing it waits for), a job with the refresher's step.
class AccountPathTest : public ::testing::Test {
protected:
    // hot
    KillSwitch kill_switch_;
    StartupRecovery recovery_{60'000};
    Script script_;
    BarrierPolicy barrier_policy_{};
    SubmitReadiness readiness_{recovery_, kill_switch_, ReadinessProbe{&read_feed, &read_recovery, &read_clock, &script_}, barrier_policy_};
    AccountUpdateRing account_ring_;
    AccountHolder holder_{account_ring_};
    SweepRequestRing requests_;
    SweepPump pump_{readiness_, requests_, holder_.sweep_probe()};
    InFlightRegistry in_flight_;
    PositionTruth truth_;
    OrderFillContext fill_context_;
    AuditRingSink audit_;
    Durable durable_;
    ReconcileDurablePublisher publisher_{DurableOrderAuditPort{&durable_append, nullptr, &durable_}};
    // reconcile
    OrderTracker tracker_;
    ToReconcileRing inbound_;
    ReconcileEventRing events_;
    FakeAccountPort fake_;
    AccountRefresher refresher_{account_ring_, fake_.port(), kPolicy};
    SweepJob job_{requests_, refresher_.sweep_step()};
    ScriptedExchange exchange_;
    QueryPort query_port_{&scripted_query, &exchange_};

    AccountPathTest() {
        audit_.set_available(true);
        script_.feed.generation = 1;
        script_.feed.connected = true;
    }

    void hot_cycle(std::int64_t now_ms) {
        script_.feed.last_activity_ms = now_ms;
        script_.feed.sampled_at_ms = now_ms;
        pump_.service();
        const SweepSink sink = pump_.sink();
        drain_reconcile_events(in_flight_, &audit_, events_, now_ms, &truth_, &fill_context_, &publisher_, &sink);
    }
    void reconcile_tick(std::int64_t now_ms) {
        poll_once(tracker_, inbound_, events_, query_port_, ReconcilePollPolicy{}, now_ms, OrderRecord::kClockUnset, &job_);
    }
    [[nodiscard]] BarrierState state() const { return readiness_.barrier().state(); }
};

}  // namespace

TEST_F(AccountPathTest, TheBarrierCompletesOnTheAccountTheSweepItselfFetched) {
    hot_cycle(1'000);  // the connected generation -> Sweeping -> the request goes out
    ASSERT_EQ(pump_.requested_epoch(), 1U);
    EXPECT_EQ(state(), BarrierState::Sweeping);

    reconcile_tick(1'010);  // takes the request, nothing to ask, the step fetches the account (tagged epoch 1), then the marker
    EXPECT_EQ(fake_.calls, 1U);
    EXPECT_EQ(refresher_.stats().sweep_delivered, 1U);

    hot_cycle(1'020);  // pops the marker: the holder drains the ring and finds the update of epoch 1
    EXPECT_EQ(state(), BarrierState::Reconciled);
    EXPECT_EQ(pump_.stats().reports_account_stale, 0U);
    EXPECT_EQ(holder_.refreshed_epoch(), 1U);
    EXPECT_EQ(holder_.fetch_started_ms(), 1'010);
    EXPECT_EQ(holder_.view().timestamp_ms(), 1'010);
}

TEST_F(AccountPathTest, AFailedStepFetchLeavesTheBarrierSweepingAndTheNextSweepRecovers) {
    fake_.outcomes = {AccountFetchOutcome::Failed, AccountFetchOutcome::Fetched};
    hot_cycle(1'000);
    reconcile_tick(1'010);  // the fetch fails: no update; the marker is pushed all the same
    hot_cycle(1'020);
    EXPECT_EQ(state(), BarrierState::Sweeping) << "an answered sweep with no account fetched by it does not complete the barrier";
    EXPECT_EQ(pump_.stats().markers_received, 1U);
    EXPECT_EQ(pump_.stats().reports_account_stale, 1U);
    EXPECT_FALSE(holder_.has_snapshot());

    hot_cycle(2'100);  // the barrier asks again after resweep_after_ms, under a new epoch
    EXPECT_EQ(pump_.requested_epoch(), 2U);
    reconcile_tick(2'110);
    hot_cycle(2'120);
    EXPECT_EQ(state(), BarrierState::Reconciled);
    EXPECT_EQ(holder_.refreshed_epoch(), 2U);
    EXPECT_EQ(holder_.fetch_started_ms(), 2'110);
}

TEST_F(AccountPathTest, AnUpdateFromBeforeTheRequestIsNotEnough) {
    fake_.outcomes = {AccountFetchOutcome::Fetched, AccountFetchOutcome::NotSent};
    refresher_.tick(500);   // a periodic update, stamped 500, sitting on the ring
    hot_cycle(1'000);       // the request at 1000
    reconcile_tick(1'010);  // the step asks the port: NotSent
    hot_cycle(1'020);
    EXPECT_TRUE(holder_.has_snapshot());
    EXPECT_EQ(holder_.fetch_started_ms(), 500);
    EXPECT_EQ(state(), BarrierState::Sweeping);
    EXPECT_EQ(pump_.stats().reports_account_stale, 1U);
}

// The hole a clock comparison leaves: a periodic update fetched AFTER the request was posted -- but before the sweep's last order was answered -- is
// newer than the request and older than the answers. It must not complete the barrier when the sweep's own fetch is missing.
TEST_F(AccountPathTest, APeriodicUpdateFetchedAfterTheRequestIsNotEnoughEither) {
    fake_.outcomes = {AccountFetchOutcome::Fetched, AccountFetchOutcome::Failed};
    hot_cycle(1'000);        // the request, posted at 1000
    refresher_.tick(1'005);  // the periodic refresh runs after it: stamped 1005, newer than the request
    reconcile_tick(1'010);   // the sweep: the step's own fetch fails
    hot_cycle(1'020);
    ASSERT_TRUE(holder_.has_snapshot());
    EXPECT_EQ(holder_.fetch_started_ms(), 1'005) << "the held snapshot is the periodic one, newer than the request";
    EXPECT_EQ(state(), BarrierState::Sweeping) << "...and it must not stand in for the sweep's own account";
    EXPECT_EQ(pump_.stats().reports_account_stale, 1U);
    EXPECT_EQ(holder_.refreshed_epoch(), 0U);
}

TEST_F(AccountPathTest, TheStepFetchesEvenIfAPeriodicFetchRanInTheSameTick) {
    hot_cycle(1'000);
    refresher_.tick(1'010);  // the periodic step of this very tick runs before poll_once() takes the request
    reconcile_tick(1'010);
    EXPECT_EQ(fake_.calls, 2U) << "the periodic fetch preceded the queries of the tick; the sweep's fetch comes after them";
    hot_cycle(1'020);
    EXPECT_EQ(state(), BarrierState::Reconciled);
    EXPECT_EQ(holder_.refreshed_epoch(), 1U);
}
