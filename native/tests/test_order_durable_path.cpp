// SPDX-License-Identifier: proprietary
// test_order_durable_path.cpp -- private-stream migration M4 (order_durable_path.hpp): which frames make a WebSocket observation of an order
// replayable, and which observations are refused.
//
// Four layers. (1) The table of the design (docs/PRIVATE_STREAM_MIGRATION_DESIGN.md 6.3), cell by cell, and the fill rules case by case.
// (2) A property over every shadow, every state and a grid of fills, against an oracle restated here from the recovery scan: whatever the planner says
// Write must replay, Stale must be old, and a refusal must leave no legal route on the table. (3) A differential against the REAL recovery scan
// (DurableAuditSink over a real file): every Write plan replays, and every refusal is either a frame the scan itself calls Corrupt or a fill that
// contradicts itself. (4) A walk: many orders, random observation sequences, the shadow advanced exactly as a publisher would advance it, and at the end
// the reopened log agrees with the shadow of every unfinished order.

#include <gtest/gtest.h>
#include <hengyuan/binance_user_data_event.hpp>
#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/order_durable_path.hpp>
#include <hengyuan/order_tracker.hpp>
#include <hengyuan/startup_recovery.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __linux__
#include <unistd.h>
#endif

namespace hy {
// gtest prints an enum class as "1-byte object <02>" unless it is told how: a failing table cell should say Write / Stale / Unreachable.
inline void PrintTo(PathStatus s, std::ostream* os) {
    *os << (s == PathStatus::Write ? "Write" : s == PathStatus::Stale ? "Stale" : "Unreachable");
}
}  // namespace hy

using namespace hy;

namespace {

constexpr std::int64_t kIntended = 10;
constexpr std::int64_t kNowMs = 1'700'000'000'000;

constexpr std::array<OrderState, 12> kAllStates{OrderState::Intent,    OrderState::Submitting,      OrderState::Accepted,  OrderState::Rejected,
                                                OrderState::Ambiguous, OrderState::PartialFill,     OrderState::Filled,    OrderState::CancelRequested,
                                                OrderState::Cancelled, OrderState::Expired,         OrderState::Reconciled, OrderState::EscalatedToOperator};

// The seven states a WebSocket execution report can name (map_binance_order_status()), in the column order of the table below.
constexpr std::array<OrderState, 7> kObservable{OrderState::Accepted, OrderState::PartialFill, OrderState::Filled,  OrderState::CancelRequested,
                                                 OrderState::Cancelled, OrderState::Rejected,    OrderState::Expired};

constexpr std::array<std::int64_t, 7> kCumulatives{-1, 0, 3, 4, 7, kIntended, kIntended + 1};
constexpr std::array<std::int64_t, 3> kShadowFills{0, 4, kIntended};

constexpr DurableShadow exact(OrderState s, std::int64_t filled = 0) { return DurableShadow{s, false, filled}; }
constexpr DurableShadow recovered(std::int64_t filled = 0) { return DurableShadow{OrderState::Ambiguous, true, filled}; }

DurablePath plan(const DurableShadow& log, OrderState target, std::int64_t cumulative) { return plan_observation(log, target, cumulative, kIntended); }

bool is_observable(OrderState s) { return std::find(kObservable.begin(), kObservable.end(), s) != kObservable.end(); }

}  // namespace

// --- 1. the table of the design -----------------------------------------------------------------------------------------------------------

namespace {

// Rows: what the log holds. Columns: what the exchange reports, in kObservable's order -- Accepted PartialFill Filled CancelRequested Cancelled Rejected
// Expired. W = one frame is enough; A = an Ambiguous frame has to go first; . = cannot be written. Written out by hand from validate_transition().
struct TableRow {
    OrderState from;
    bool maybe_submitting;
    const char* cells;
};

constexpr std::array<TableRow, 13> kTable{{
    {OrderState::Intent, false, "......."},               // nothing was sent: an observation of it is a contradiction
    {OrderState::Submitting, false, "WWWAAWA"},           // Cancelled / Expired / CancelRequested are only reachable from Ambiguous
    {OrderState::Accepted, false, "WWWWW.W"},             // never Rejected (a submit-time status), never an Ambiguous frame (not true of it)
    {OrderState::Rejected, false, ".....W."},
    {OrderState::Ambiguous, false, "WWWWWWW"},            // every target is a legal edge from Ambiguous
    {OrderState::PartialFill, false, ".WWWW.W"},          // never back to Accepted
    {OrderState::Filled, false, "..W...."},
    {OrderState::CancelRequested, false, "..WWW.W"},      // KNOWN LIMIT: no PartialFill edge out of CancelRequested
    {OrderState::Cancelled, false, "....W.."},
    {OrderState::Expired, false, "......W"},
    {OrderState::Reconciled, false, "......."},           // unreachable by design: nothing ever targets it
    {OrderState::EscalatedToOperator, false, "......."},  // terminal for validate_transition(): only the operator tool leaves it
    {OrderState::Ambiguous, true, "WWWAAWA"},             // a recovered order: Submitting or Ambiguous -- what replays from BOTH goes as it stands
}};

}  // namespace

TEST(DurablePath, TheStatePathTableOfTheDesign) {
    static_assert(kTable.size() == kAllStates.size() + 1, "one row per state, plus the recovered Ambiguous");
    for (const TableRow& row : kTable) {
        ASSERT_EQ(std::strlen(row.cells), kObservable.size());
        const DurableShadow log{row.from, row.maybe_submitting, 0};
        for (std::size_t i = 0; i < kObservable.size(); ++i) {
            const DurablePath p = state_path(log, kObservable[i]);
            SCOPED_TRACE(std::string(order_state_name(row.from)) + (row.maybe_submitting ? " (Submitting or Ambiguous)" : "") + " -> " +
                         order_state_name(kObservable[i]));
            switch (row.cells[i]) {
                case '.':
                    EXPECT_EQ(p.status, PathStatus::Unreachable);
                    EXPECT_FALSE(p.ambiguous_first);
                    break;
                case 'W':
                    EXPECT_EQ(p.status, PathStatus::Write);
                    EXPECT_FALSE(p.ambiguous_first);
                    break;
                case 'A':
                    EXPECT_EQ(p.status, PathStatus::Write);
                    EXPECT_TRUE(p.ambiguous_first);
                    break;
                default:
                    ADD_FAILURE() << "bad cell";
            }
        }
    }
}

TEST(DurablePath, NothingTheExchangeDoesNotSayIsEverATarget) {
    for (const OrderState target : kAllStates) {
        if (is_observable(target)) continue;
        for (const TableRow& row : kTable) {
            const DurableShadow log{row.from, row.maybe_submitting, 0};
            EXPECT_EQ(state_path(log, target).status, PathStatus::Unreachable) << order_state_name(row.from) << " -> " << order_state_name(target);
            EXPECT_EQ(plan(log, target, 0).status, PathStatus::Unreachable) << order_state_name(row.from) << " -> " << order_state_name(target);
        }
    }
}

TEST(DurablePath, TheFillDecidesBetweenNothingNewAndSomethingToWrite) {
    // the same state and the same cumulative fill: nothing to record
    EXPECT_EQ(plan(exact(OrderState::Accepted), OrderState::Accepted, 0).status, PathStatus::Stale);
    EXPECT_EQ(plan(exact(OrderState::PartialFill, 4), OrderState::PartialFill, 4).status, PathStatus::Stale);
    // more fill in the same state: one frame
    const DurablePath more = plan(exact(OrderState::PartialFill, 4), OrderState::PartialFill, 6);
    EXPECT_EQ(more.status, PathStatus::Write);
    EXPECT_FALSE(more.ambiguous_first);
    // a later state with no new fill: one frame
    EXPECT_EQ(plan(exact(OrderState::PartialFill, 4), OrderState::Cancelled, 4).status, PathStatus::Write);
    // an older observation delivered late (a NEW after a PARTIALLY_FILLED, a FILLED event repeated, a lower cumulative): stale
    EXPECT_EQ(plan(exact(OrderState::PartialFill, 4), OrderState::Accepted, 0).status, PathStatus::Stale);
    EXPECT_EQ(plan(exact(OrderState::Filled, kIntended), OrderState::PartialFill, 7).status, PathStatus::Stale);
    EXPECT_EQ(plan(exact(OrderState::Filled, kIntended), OrderState::Filled, kIntended).status, PathStatus::Stale);
    EXPECT_EQ(plan(exact(OrderState::PartialFill, 4), OrderState::PartialFill, 3).status, PathStatus::Stale);
    EXPECT_EQ(plan(exact(OrderState::Cancelled, 4), OrderState::PartialFill, 4).status, PathStatus::Stale) << "an old event after the cancel";
    // a lower fill with a LATER state contradicts the log
    EXPECT_EQ(plan(exact(OrderState::PartialFill, 4), OrderState::Filled, 3).status, PathStatus::Unreachable);
    // a state the log cannot be followed by, whatever the fill says
    EXPECT_EQ(plan(exact(OrderState::Filled, kIntended), OrderState::Cancelled, kIntended).status, PathStatus::Unreachable) << "both final, and different";
    EXPECT_EQ(plan(exact(OrderState::Rejected), OrderState::Accepted, 0).status, PathStatus::Unreachable) << "a NEW for an order the submit rejected";
    // a recovered order's first observation is never stale, whatever it says: legal from both possible last frames, it goes as it stands ...
    const DurablePath first = plan(recovered(), OrderState::Accepted, 0);
    EXPECT_EQ(first.status, PathStatus::Write);
    EXPECT_FALSE(first.ambiguous_first);
    // ... and one only Ambiguous reaches gets the Ambiguous frame in front of it
    const DurablePath cancelled = plan(recovered(), OrderState::Cancelled, 0);
    EXPECT_EQ(cancelled.status, PathStatus::Write);
    EXPECT_TRUE(cancelled.ambiguous_first);
}

TEST(DurablePath, AFillPastTheIntendedQuantityOrWithoutOneIsNeverWritten) {
    EXPECT_EQ(plan(exact(OrderState::Accepted), OrderState::Filled, kIntended).status, PathStatus::Write);
    EXPECT_EQ(plan(exact(OrderState::Accepted), OrderState::Filled, kIntended + 1).status, PathStatus::Unreachable) << "an overfill";
    EXPECT_EQ(plan(exact(OrderState::Accepted), OrderState::PartialFill, kIntended + 1).status, PathStatus::Unreachable) << "an overfill, a state change";
    EXPECT_EQ(plan(exact(OrderState::PartialFill, 4), OrderState::PartialFill, kIntended + 1).status, PathStatus::Unreachable) << "an overfill, same state";
    EXPECT_EQ(plan_observation(exact(OrderState::Accepted), OrderState::PartialFill, 1, 0).status, PathStatus::Unreachable) << "no intended quantity known";
    EXPECT_EQ(plan_observation(exact(OrderState::Accepted), OrderState::PartialFill, 1, -5).status, PathStatus::Unreachable);
    EXPECT_EQ(plan_observation(exact(OrderState::Accepted), OrderState::Cancelled, 0, 0).status, PathStatus::Write) << "no fill, no need for one";
    EXPECT_EQ(plan(exact(OrderState::Accepted), OrderState::PartialFill, -1).status, PathStatus::Unreachable) << "a negative cumulative fill";
    // a log that already holds an overfill never gets another frame: a repeat of it is nothing new, anything that would add a frame is refused
    EXPECT_EQ(plan_observation(exact(OrderState::PartialFill, 9), OrderState::PartialFill, 9, 5).status, PathStatus::Stale);
    EXPECT_EQ(plan_observation(exact(OrderState::PartialFill, 9), OrderState::Filled, 9, 5).status, PathStatus::Unreachable);
    EXPECT_EQ(plan_observation(exact(OrderState::PartialFill, 9), OrderState::PartialFill, 10, 5).status, PathStatus::Unreachable);
}

TEST(DurablePath, AResolutionMayAlsoEscalateButOnlyWhereTheStateMachineLetsIt) {
    const auto escalate = [](const DurableShadow& log) { return plan_resolution(log, OrderState::EscalatedToOperator, 0, kIntended); };
    // an order that is Ambiguous goes straight to the operator; one that may still be Submitting, or is, needs the Ambiguous frame first
    EXPECT_EQ(escalate(exact(OrderState::Ambiguous)).status, PathStatus::Write);
    EXPECT_FALSE(escalate(exact(OrderState::Ambiguous)).ambiguous_first);
    EXPECT_EQ(escalate(recovered()).status, PathStatus::Write);
    EXPECT_TRUE(escalate(recovered()).ambiguous_first);
    EXPECT_EQ(escalate(exact(OrderState::Submitting)).status, PathStatus::Write);
    EXPECT_TRUE(escalate(exact(OrderState::Submitting)).ambiguous_first);
    // a live order is never escalated (Accepted / PartialFill -> Ambiguous is not an edge), and a final one has nowhere to go
    for (const OrderState s : {OrderState::Accepted, OrderState::PartialFill, OrderState::CancelRequested, OrderState::Filled, OrderState::Cancelled,
                               OrderState::Rejected, OrderState::Expired, OrderState::Intent}) {
        EXPECT_EQ(escalate(exact(s)).status, PathStatus::Unreachable) << order_state_name(s);
    }
    EXPECT_EQ(escalate(exact(OrderState::EscalatedToOperator)).status, PathStatus::Stale) << "already escalated";
    // an exchange report is never an escalation
    EXPECT_EQ(plan_observation(exact(OrderState::Ambiguous), OrderState::EscalatedToOperator, 0, kIntended).status, PathStatus::Unreachable);
    EXPECT_EQ(state_path(exact(OrderState::Ambiguous), OrderState::EscalatedToOperator).status, PathStatus::Unreachable);
    // and nothing else outside the seven is a resolution either
    for (const OrderState s : {OrderState::Intent, OrderState::Submitting, OrderState::Ambiguous, OrderState::Reconciled}) {
        EXPECT_EQ(plan_resolution(exact(OrderState::Accepted), s, 0, kIntended).status, PathStatus::Unreachable) << order_state_name(s);
    }
}

TEST(DurablePath, AResolutionAndAnObservationAgreeOnEveryStateThatBothCanName) {
    for (const OrderState from : kAllStates) {
        for (const bool maybe : {false, true}) {
            if (maybe && from != OrderState::Ambiguous) continue;
            for (const std::int64_t shadow_filled : kShadowFills) {
                for (const OrderState target : kObservable) {
                    for (const std::int64_t cumulative : kCumulatives) {
                        const DurableShadow log{from, maybe, shadow_filled};
                        const DurablePath o = plan(log, target, cumulative);
                        const DurablePath r = plan_resolution(log, target, cumulative, kIntended);
                        ASSERT_EQ(o.status, r.status);
                        ASSERT_EQ(o.ambiguous_first, r.ambiguous_first);
                    }
                }
            }
        }
    }
}

TEST(DurablePath, AnUnboundedFillBoundLetsAResolutionThroughWhereAZeroBoundRefusesIt) {
    constexpr std::int64_t kUnbounded = std::numeric_limits<std::int64_t>::max();
    EXPECT_EQ(plan_resolution(exact(OrderState::Accepted), OrderState::Filled, 1000, kUnbounded).status, PathStatus::Write);
    EXPECT_EQ(plan_resolution(exact(OrderState::Accepted), OrderState::Filled, 1000, 0).status, PathStatus::Unreachable) << "0 = unknown: refuse every fill";
    EXPECT_EQ(plan_resolution(exact(OrderState::Accepted), OrderState::Cancelled, 0, 0).status, PathStatus::Write) << "no fill, no bound needed";
}

TEST(DurablePath, TheShadowAfterAnAckIsExactAndTheFillOnlyGoesUp) {
    const DurableShadow rec = shadow_of_recovered(OrderState::Ambiguous, 0);
    EXPECT_EQ(rec.state, OrderState::Ambiguous);
    EXPECT_TRUE(rec.maybe_submitting);
    EXPECT_EQ(rec.filled_qty_ticks, 0);
    // every other state the scan reports is exact
    for (const OrderState s : {OrderState::Accepted, OrderState::PartialFill, OrderState::CancelRequested, OrderState::EscalatedToOperator}) {
        const DurableShadow d = shadow_of_recovered(s, 3);
        EXPECT_EQ(d.state, s);
        EXPECT_FALSE(d.maybe_submitting);
        EXPECT_EQ(d.filled_qty_ticks, 3);
    }
    // one acknowledged frame makes the recovered shadow exact, whatever it was
    const DurableShadow acked = after_ack(rec, OrderState::Ambiguous, 0);
    EXPECT_FALSE(acked.maybe_submitting);
    EXPECT_EQ(acked.state, OrderState::Ambiguous);
    const DurableShadow next = after_ack(acked, OrderState::PartialFill, 4);
    EXPECT_EQ(next.state, OrderState::PartialFill);
    EXPECT_EQ(next.filled_qty_ticks, 4);
    // the high-water mark, like the scan's own: a frame that carries less fill does not lower it
    EXPECT_EQ(after_ack(next, OrderState::Cancelled, 2).filled_qty_ticks, 4);
    EXPECT_EQ(after_ack(next, OrderState::Cancelled, 9).filled_qty_ticks, 9);
}

TEST(DurablePath, EverythingIsConstexpr) {
    static_assert(state_path(DurableShadow{OrderState::Submitting, false, 0}, OrderState::Cancelled).ambiguous_first);
    static_assert(plan_observation(DurableShadow{OrderState::Accepted, false, 0}, OrderState::Accepted, 0, 10).status == PathStatus::Stale);
    static_assert(plan_observation(DurableShadow{OrderState::Accepted, false, 0}, OrderState::Filled, 11, 10).status == PathStatus::Unreachable);
    static_assert(after_ack(shadow_of_recovered(OrderState::Ambiguous, 0), OrderState::Ambiguous, 0).maybe_submitting == false);
    SUCCEED();
}

// --- 2. the property -------------------------------------------------------------------------------------------------------------------------

namespace {

// The recovery scan's rule for the next frame of an order, restated from DurableAuditSink::run_recovery_scan_impl() on purpose -- the test must not share
// the code it checks. The same state again: the fill never goes down and never passes the intended quantity. A different state: validate_transition()
// alone, with no fill check at all.
bool scan_accepts(OrderState at, std::int64_t at_fill, OrderState next, std::int64_t next_fill) {
    if (next == at) return next_fill >= at_fill && next_fill <= kIntended;
    return validate_transition(at, next) == TransitionResult::Ok;
}

// The writer-side rule on top of the scan's: the frame's own fill is a consistent one, for a state change as well as for a repeat.
bool fill_is_consistent(const DurableShadow& log, std::int64_t cumulative) { return cumulative >= log.filled_qty_ticks && cumulative <= kIntended; }

// The states the log's last frame may be: one, or Submitting and Ambiguous for a recovered order.
std::size_t possible_last_states(const DurableShadow& log, std::array<OrderState, 2>& out) {
    if (log.maybe_submitting) {
        out = {OrderState::Submitting, OrderState::Ambiguous};
        return 2;
    }
    out = {log.state, log.state};
    return 1;
}

// Does writing `path` for the observation replay from EVERY state the log may hold?
bool replays(const DurableShadow& log, const DurablePath& path, OrderState target, std::int64_t cumulative) {
    std::array<OrderState, 2> starts{};
    const std::size_t n = possible_last_states(log, starts);
    for (std::size_t i = 0; i < n; ++i) {
        OrderState at = starts[i];
        if (path.ambiguous_first) {
            if (!scan_accepts(at, log.filled_qty_ticks, OrderState::Ambiguous, log.filled_qty_ticks)) return false;
            at = OrderState::Ambiguous;
        }
        if (!scan_accepts(at, log.filled_qty_ticks, target, cumulative)) return false;
    }
    return true;
}

// Is there ANY legal route -- the frame itself, or one intermediate frame of any state that carries no new fill -- from what the log may hold to the
// observation? Wider than the planner, which only ever bridges with Ambiguous: a refusal is shown to leave nothing on the table.
bool route_exists(const DurableShadow& log, OrderState target, std::int64_t cumulative) {
    std::array<OrderState, 2> starts{};
    const std::size_t n = possible_last_states(log, starts);
    for (std::size_t i = 0; i < n; ++i) {
        bool ok = scan_accepts(starts[i], log.filled_qty_ticks, target, cumulative);
        for (const OrderState via : kAllStates) {
            if (ok) break;
            ok = scan_accepts(starts[i], log.filled_qty_ticks, via, log.filled_qty_ticks) && scan_accepts(via, log.filled_qty_ticks, target, cumulative);
        }
        if (!ok) return false;
    }
    return true;
}

}  // namespace

TEST(DurablePath, EveryPlanIsSoundAgainstTheScansOwnRules) {
    std::size_t written = 0, bridged = 0, stale = 0, refused = 0;
    for (const OrderState from : kAllStates) {
        for (const bool maybe : {false, true}) {
            if (maybe && from != OrderState::Ambiguous) continue;
            for (const std::int64_t shadow_filled : kShadowFills) {
                for (const OrderState target : kAllStates) {
                    for (const std::int64_t cumulative : kCumulatives) {
                        const DurableShadow log{from, maybe, shadow_filled};
                        const DurablePath p = plan(log, target, cumulative);
                        SCOPED_TRACE(std::string(order_state_name(from)) + (maybe ? "(maybe Submitting)" : "") + " filled " + std::to_string(shadow_filled) +
                                     " -> " + order_state_name(target) + " cumulative " + std::to_string(cumulative));
                        if (p.status == PathStatus::Write) {
                            ++written;
                            if (p.ambiguous_first) {
                                ++bridged;
                                EXPECT_TRUE(maybe || from == OrderState::Submitting) << "an Ambiguous frame only where it is true";
                            }
                            EXPECT_TRUE(is_observable(target));
                            EXPECT_TRUE(fill_is_consistent(log, cumulative)) << "a Write plan with a fill that contradicts itself";
                            EXPECT_TRUE(replays(log, p, target, cumulative)) << "a Write plan that the scan would call Corrupt";
                        } else if (p.status == PathStatus::Stale) {
                            ++stale;
                            EXPECT_FALSE(p.ambiguous_first);
                            EXPECT_TRUE(is_observable(target));
                            EXPECT_FALSE(maybe) << "a recovered order's first observation is never stale";
                            EXPECT_LE(cumulative, shadow_filled) << "stale means nothing new to record";
                            EXPECT_TRUE(target == from || validate_transition(target, from) == TransitionResult::Ok) << "stale means the log is already past it";
                        } else {
                            ++refused;
                            EXPECT_FALSE(p.ambiguous_first);
                            // completeness: a refusal is explained -- not something the exchange says, a fill that contradicts itself, an order nothing
                            // was ever sent for, or no legal route at all from what the log holds
                            const bool explained = !is_observable(target) || !fill_is_consistent(log, cumulative) || from == OrderState::Intent ||
                                                   !route_exists(log, target, cumulative);
                            EXPECT_TRUE(explained) << "refused, yet a legal route exists";
                        }
                    }
                }
            }
        }
    }
    EXPECT_GT(written, 200u);
    EXPECT_GT(bridged, 20u);
    EXPECT_GT(stale, 50u);
    EXPECT_GT(refused, 500u);
}

// --- 3. the differential against the real recovery scan ------------------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kKeyId = 1;

struct Frame {
    OrderState state;
    std::int64_t filled;
    AuditEventType event;
};

AuditEventType natural_event(OrderState s) {
    switch (s) {
        case OrderState::Intent: return AuditEventType::OrderIntentCreated;
        case OrderState::Submitting: return AuditEventType::OrderSubmitPrepared;
        case OrderState::Accepted: return AuditEventType::OrderAccepted;
        case OrderState::Rejected: return AuditEventType::OrderRejected;
        case OrderState::Ambiguous: return AuditEventType::OrderAmbiguous;
        case OrderState::PartialFill: return AuditEventType::OrderPartialFill;
        case OrderState::Filled: return AuditEventType::OrderFilled;
        case OrderState::CancelRequested: return AuditEventType::OrderCancelRequested;
        case OrderState::Cancelled: return AuditEventType::OrderCancelled;
        case OrderState::Expired: return AuditEventType::OrderExpired;
        case OrderState::Reconciled: return AuditEventType::OrderReconciled;
        case OrderState::EscalatedToOperator: return AuditEventType::OrderEscalated;
    }
    return AuditEventType::OrderIntentCreated;
}

// A frame an earlier part of the order's life wrote: its own event type.
Frame history_frame(OrderState s, std::int64_t filled = 0) { return Frame{s, filled, natural_event(s)}; }
// The frame a WebSocket observation becomes.
Frame observed_frame(OrderState s, std::int64_t filled) { return Frame{s, filled, AuditEventType::UserDataStreamEventObserved}; }
// The frame a plan may put in front of it (it carries the shadow's own fill).
Frame ambiguous_frame(std::int64_t filled) { return Frame{OrderState::Ambiguous, filled, AuditEventType::OrderAmbiguous}; }

struct OrderFrames {
    std::string coid;
    std::vector<Frame> frames;  // after the Intent frame, which every order gets first
};

bool replayed(RecoveryScanStatus s) { return s == RecoveryScanStatus::Clean || s == RecoveryScanStatus::Recovered; }

}  // namespace

class DurablePathReplayTest : public ::testing::Test {
protected:
    std::string stem_;
    unsigned used_{0};
    std::unique_ptr<KeyRing> ring_;
    std::vector<std::string> paths_;

    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        stem_ = std::string(tmp) + "hy_durpath_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter);
#else
        stem_ = "/tmp/hy_durpath_" + std::to_string(getpid()) + "_" + std::to_string(counter);
#endif
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x30 + i);
        ring_ = std::make_unique<KeyRing>(kek);
        static const char kKey[] = "order-durable-path-test-key";
        std::vector<std::byte> key(sizeof(kKey) - 1);
        std::memcpy(key.data(), kKey, key.size());
        WrappedKeyRecord rec{};
        ASSERT_EQ(ring_->add_key(kKeyId, key, rec), KeyRingAddStatus::Ok);
    }

    void TearDown() override {
        for (const std::string& base : paths_) {
            for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".keyrotations", ".keyrotations.lock", ".keyrotations.tip", ".keyrotations.tip.tmp",
                                       ".storeid", ".storeid.lock", ".storeid.tip", ".storeid.tip.tmp"}) {
                std::remove((base + suffix).c_str());
            }
        }
    }

    // A log name nothing has used (re-creating a name just deleted races a virus scanner on a Windows host: see test_durable_audit_sink.cpp's
    // use_fresh_base_path()).
    std::string fresh() {
        paths_.push_back(stem_ + "_" + std::to_string(++used_) + ".audit.log");
        return paths_.back();
    }

    static AuditRecord record_for(const std::string& coid, const Frame& f) {
        AuditRecord r{};
        r.timestamp_ms = kNowMs;
        r.event_type = f.event;
        r.mode = ExecutionMode::Live;
        r.symbol_id = 1;
        r.set_client_order_id(coid.c_str());
        r.qty_ticks = kIntended;
        r.price_ticks = 500'000;
        r.resulting_state = f.state;
        r.filled_qty_ticks = f.filled;
        r.side = OrderSide::Buy;
        return r;
    }

    // Appends one frame; the sink validates nothing, which is what lets the recovery scan be the judge.
    bool append(DurableAuditSink& sink, const std::string& coid, const Frame& f) {
        if (sink.append_durable(record_for(coid, f), kNowMs).acked()) return true;
        ADD_FAILURE() << "setup: the sink refused a frame of " << coid;
        return false;
    }

    std::unique_ptr<DurableAuditSink> reopen(const std::string& path) { return std::make_unique<DurableAuditSink>(path, *ring_, kKeyId); }

    // Writes every order (its Intent frame, then its frames) into a fresh log. Returns the path, or an empty string if something refused.
    std::string write_log(const std::vector<OrderFrames>& orders) {
        const std::string path = fresh();
        DurableAuditSink sink(path, *ring_, kKeyId);
        if (!sink.is_open() || sink.fenced()) {
            ADD_FAILURE() << "setup: the sink did not open";
            return {};
        }
        for (const OrderFrames& o : orders) {
            if (!append(sink, o.coid, history_frame(OrderState::Intent))) return {};
            for (const Frame& f : o.frames) {
                if (!append(sink, o.coid, f)) return {};
            }
        }
        return path;
    }

    // What the recovery scan makes of a log holding these orders.
    RecoveryScanStatus scan_status(const std::vector<OrderFrames>& orders) {
        const std::string path = write_log(orders);
        if (path.empty()) return RecoveryScanStatus::IoError;
        return reopen(path)->recovery_status();
    }
};

TEST_F(DurablePathReplayTest, EveryWritePlanReplaysAndEveryRefusalIsExplained) {
    struct Start {
        std::string name;
        std::vector<Frame> history;  // after Intent
        DurableShadow log;
    };
    const std::vector<Start> starts{
        {"Submitting", {history_frame(OrderState::Submitting)}, exact(OrderState::Submitting)},
        {"Accepted", {history_frame(OrderState::Submitting), history_frame(OrderState::Accepted)}, exact(OrderState::Accepted)},
        {"PartialFill(4)",
         {history_frame(OrderState::Submitting), history_frame(OrderState::Accepted), history_frame(OrderState::PartialFill, 4)},
         exact(OrderState::PartialFill, 4)},
        {"Ambiguous", {history_frame(OrderState::Submitting), history_frame(OrderState::Ambiguous)}, exact(OrderState::Ambiguous)},
        {"CancelRequested",
         {history_frame(OrderState::Submitting), history_frame(OrderState::Accepted), history_frame(OrderState::CancelRequested)},
         exact(OrderState::CancelRequested)},
        {"Filled(10)", {history_frame(OrderState::Submitting), history_frame(OrderState::Filled, kIntended)}, exact(OrderState::Filled, kIntended)},
        {"Rejected", {history_frame(OrderState::Submitting), history_frame(OrderState::Rejected)}, exact(OrderState::Rejected)},
        // a recovered order: the log holds Submitting (the scan remaps it) -- or Ambiguous -- and the shadow is the same for both
        {"recovered, last frame Submitting", {history_frame(OrderState::Submitting)}, recovered()},
        {"recovered, last frame Ambiguous", {history_frame(OrderState::Submitting), history_frame(OrderState::Ambiguous)}, recovered()},
    };

    struct Case {
        std::string what;
        OrderFrames order;
    };
    std::vector<Case> to_replay;   // Write plans: the log must replay
    std::vector<Case> to_corrupt;  // refusals of a state the scan itself cannot take: the naive single frame must make the log Corrupt
    std::size_t stale = 0, fill_refusals = 0;
    unsigned next_id = 0;
    for (const Start& s : starts) {
        // The fills worth a real log, each file append being a real fsync: no new fill, a partial one, the full one, and one more than the order has
        // (which only ever exercises the refusal's explanation). The property test above covers the whole grid in memory; this one confirms that the
        // oracle it uses is the scan's own.
        std::vector<std::int64_t> fills{s.log.filled_qty_ticks};
        if (s.log.filled_qty_ticks + 3 < kIntended) fills.push_back(s.log.filled_qty_ticks + 3);
        if (s.log.filled_qty_ticks < kIntended) fills.push_back(kIntended);
        fills.push_back(kIntended + 1);
        for (const OrderState target : kObservable) {
            for (const std::int64_t cumulative : fills) {
                const DurablePath p = plan(s.log, target, cumulative);
                Case c{s.name + " -> " + order_state_name(target) + " cumulative " + std::to_string(cumulative),
                       OrderFrames{"HY-DP-" + std::to_string(next_id++), s.history}};
                if (p.status == PathStatus::Write) {
                    if (p.ambiguous_first) c.order.frames.push_back(ambiguous_frame(s.log.filled_qty_ticks));
                    c.order.frames.push_back(observed_frame(target, cumulative));
                    to_replay.push_back(std::move(c));
                } else if (p.status == PathStatus::Unreachable) {
                    if (!(cumulative >= s.log.filled_qty_ticks && cumulative <= kIntended)) {
                        ++fill_refusals;  // a fill that contradicts itself: the scan may or may not see it, the writer never writes it
                    } else if (cumulative == s.log.filled_qty_ticks) {
                        c.order.frames.push_back(observed_frame(target, cumulative));  // what a publisher without this header would have written
                        to_corrupt.push_back(std::move(c));
                    }
                } else {
                    ++stale;
                }
            }
        }
    }

    // Write plans, many orders to a log (a log holds up to kMaxInFlight unfinished orders), so one scan judges a whole batch; a failing batch is
    // taken apart to name its order.
    constexpr std::size_t kBatch = 40;
    for (std::size_t begin = 0; begin < to_replay.size(); begin += kBatch) {
        const std::size_t end = std::min(begin + kBatch, to_replay.size());
        std::vector<OrderFrames> batch;
        for (std::size_t i = begin; i < end; ++i) batch.push_back(to_replay[i].order);
        if (replayed(scan_status(batch))) continue;
        ADD_FAILURE() << "a batch of Write plans did not replay; naming the orders";
        for (std::size_t i = begin; i < end; ++i) {
            EXPECT_TRUE(replayed(scan_status({to_replay[i].order}))) << to_replay[i].what << ": a Write plan that the scan calls Corrupt";
        }
    }
    // Refusals: one log each (a Corrupt log is Corrupt for every order in it).
    for (const Case& c : to_corrupt) {
        EXPECT_EQ(scan_status({c.order}), RecoveryScanStatus::Corrupt) << c.what << ": refused, yet the scan takes the single frame";
    }
    // Measured (the grid is fixed): 128 Write plans, 12 refusals of a state the scan cannot take, 63 overfills, 11 stale. The floors are about half: far from
    // a test that checks nothing, close enough to notice one that stops reaching a kind of plan.
    EXPECT_GT(to_replay.size(), 60u);
    EXPECT_GT(to_corrupt.size(), 6u);
    EXPECT_GT(fill_refusals, 30u);
    EXPECT_GT(stale, 5u);
}

// --- 4. the walk ---------------------------------------------------------------------------------------------------------------------------

namespace {

struct Lcg {
    std::uint64_t state;
    std::uint32_t next() noexcept {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state >> 33);
    }
};

}  // namespace

TEST_F(DurablePathReplayTest, ARandomWalkOfObservationsLeavesALogThatReplaysAndAShadowEqualToIt) {
    constexpr std::size_t kOrders = 32;  // fewer than kMaxInFlight: every unfinished order is a checkpoint
    constexpr std::size_t kEvents = 6;
    struct Order {
        std::string coid;
        DurableShadow shadow;
    };
    std::vector<Order> orders;
    std::size_t wrote = 0, bridged = 0, stale = 0, refused = 0;
    const std::string path = fresh();
    {
        DurableAuditSink sink(path, *ring_, kKeyId);
        ASSERT_TRUE(sink.is_open());
        ASSERT_FALSE(sink.fenced());
        Lcg rng{0x9E3779B97F4A7C15ULL};
        for (std::size_t k = 0; k < kOrders; ++k) {
            Order o{"HY-WALK-" + std::to_string(k), DurableShadow{}};
            ASSERT_TRUE(append(sink, o.coid, history_frame(OrderState::Intent)));
            ASSERT_TRUE(append(sink, o.coid, history_frame(OrderState::Submitting)));
            switch (k % 4) {
                case 0:  // the submit's result never landed, and the process still knows it
                    o.shadow = exact(OrderState::Submitting);
                    break;
                case 1:  // acknowledged by the exchange
                    ASSERT_TRUE(append(sink, o.coid, history_frame(OrderState::Accepted)));
                    o.shadow = exact(OrderState::Accepted);
                    break;
                case 2:  // a restart: the log holds Submitting, the process only knows "Ambiguous, maybe Submitting"
                    o.shadow = recovered();
                    break;
                default:  // a restart: the log holds Ambiguous, same shadow
                    ASSERT_TRUE(append(sink, o.coid, history_frame(OrderState::Ambiguous)));
                    o.shadow = recovered();
                    break;
            }
            for (std::size_t e = 0; e < kEvents; ++e) {
                const OrderState target = kObservable[rng.next() % kObservable.size()];
                const std::int64_t cumulative = static_cast<std::int64_t>(rng.next() % static_cast<std::uint32_t>(kIntended + 2));  // 0 .. kIntended + 1
                const DurablePath p = plan(o.shadow, target, cumulative);
                if (p.status == PathStatus::Stale) {
                    ++stale;
                    continue;
                }
                if (p.status == PathStatus::Unreachable) {
                    ++refused;
                    continue;
                }
                if (p.ambiguous_first) {
                    ASSERT_TRUE(append(sink, o.coid, ambiguous_frame(o.shadow.filled_qty_ticks)));
                    o.shadow = after_ack(o.shadow, OrderState::Ambiguous, o.shadow.filled_qty_ticks);
                    ++bridged;
                }
                ASSERT_TRUE(append(sink, o.coid, observed_frame(target, cumulative)));
                o.shadow = after_ack(o.shadow, target, cumulative);
                ++wrote;
            }
            orders.push_back(std::move(o));
        }
    }

    const std::unique_ptr<DurableAuditSink> reopened = reopen(path);
    ASSERT_TRUE(replayed(reopened->recovery_status())) << "the walk left a log that does not replay (status " << static_cast<int>(reopened->recovery_status())
                                                       << ")";
    for (const Order& o : orders) {
        const OrderRecoveryCheckpoint* found = nullptr;
        for (const OrderRecoveryCheckpoint& cp : reopened->recovered_checkpoints()) {
            if (std::strncmp(cp.client_order_id.id, o.coid.c_str(), kClientOrderIdLen) == 0) found = &cp;
        }
        if (is_exchange_final(o.shadow.state)) {
            EXPECT_EQ(found, nullptr) << o.coid << ": a finished order is not a checkpoint";
            continue;
        }
        ASSERT_NE(found, nullptr) << o.coid << ": an unfinished order is a checkpoint";
        // the scan reports an order whose last frame is Submitting as Ambiguous
        const OrderState expected = o.shadow.state == OrderState::Submitting ? OrderState::Ambiguous : o.shadow.state;
        EXPECT_EQ(found->resulting_state, expected) << o.coid;
        EXPECT_EQ(found->filled_qty_ticks, o.shadow.filled_qty_ticks) << o.coid << ": the shadow's fill is the log's";
    }
    // Measured on this seed (it is deterministic): 50 frames written; the floors are about half.
    EXPECT_GT(wrote, 25u);
    EXPECT_GT(bridged, 2u);
    EXPECT_GT(stale, 10u);
    EXPECT_GT(refused, 10u);
}

// --- 5. the publishers over a real log, both writers ---------------------------------------------------------------------------------------------

namespace {

AuditAppendResult sink_append(void* user_data, const AuditRecord& rec, std::int64_t now_ms) noexcept {
    return static_cast<DurableAuditSink*>(user_data)->append_durable(rec, now_ms);
}
bool sink_fenced(void* user_data) noexcept { return static_cast<DurableAuditSink*>(user_data)->fenced(); }
DurableOrderAuditPort port_of(DurableAuditSink& sink) { return DurableOrderAuditPort{&sink_append, &sink_fenced, &sink}; }

// Symbol 1 is the one record_for() writes; every scale 0, so a raw "10" is 10 ticks.
SymbolRules plain_rules(std::uint32_t) {
    SymbolRules r{};
    r.rules_version = 1;
    return r;
}

UserDataWsEvent ws_report(const std::string& coid, OrderState status, std::int64_t cumulative) {
    UserDataWsEvent ev{};
    ev.kind = UserDataEventKind::ExecutionReport;
    std::snprintf(ev.coid.id, sizeof(ev.coid.id), "%s", coid.c_str());
    ev.exchange_order_id = 555;
    ev.order_status = status;
    ev.order_status_known = true;
    ev.side = OrderSide::Buy;
    ev.side_known = true;
    std::snprintf(ev.cumulative_filled_qty_raw, sizeof(ev.cumulative_filled_qty_raw), "%lld", static_cast<long long>(cumulative));
    std::snprintf(ev.cumulative_quote_qty_raw, sizeof(ev.cumulative_quote_qty_raw), "%lld", static_cast<long long>(cumulative * 100));
    return ev;
}

ReconcileEvent resolution(const std::string& coid, InFlightHandle handle, OrderState resulting, std::int64_t cumulative, OrderState prior) {
    ReconcileEvent ev{};
    ev.handle = handle;
    std::snprintf(ev.coid.id, sizeof(ev.coid.id), "%s", coid.c_str());
    ev.resulting_state = resulting;
    ev.prior_state = prior;
    ev.symbol_id = 1;
    ev.side = OrderSide::Buy;
    ev.exchange_order_id = 555;
    ev.filled_qty_ticks = cumulative;
    ev.fill_delta_qty_ticks = cumulative;
    return ev;
}

}  // namespace

TEST_F(DurablePathReplayTest, AStreamReportForARecoveredSubmittingOrderLeavesALogThatReplaysAndClosesTheOrder) {
    // the previous process died with the order's last frame Submitting: the scan hands it back as an Ambiguous checkpoint
    const std::string path = write_log({OrderFrames{"HY-REC", {history_frame(OrderState::Submitting)}}});
    ASSERT_FALSE(path.empty());
    {
        DurableAuditSink sink(path, *ring_, kKeyId);
        ASSERT_EQ(sink.recovered_checkpoints().size(), 1u);
        ASSERT_EQ(sink.recovered_checkpoints()[0].resulting_state, OrderState::Ambiguous);
        InFlightRegistry in_flight;
        ToReconcileRing to_reconcile;
        PositionTruth truth;
        OrderFillContext fills;
        ASSERT_TRUE(apply_recovery(sink.recovered_checkpoints(), in_flight, to_reconcile, truth, fills, &plain_rules).ok());
        const OrderFillContextEntry* e = fills.find("HY-REC");
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->shadow.state, OrderState::Ambiguous);
        EXPECT_TRUE(e->shadow.maybe_submitting) << "the checkpoint cannot say whether the last frame was Submitting or Ambiguous";
        EXPECT_EQ(e->intended_qty_ticks, kIntended) << "the quantity the order was sent with comes from its Intent frame";

        UserDataWsEventRing events;
        UserDataDurablePublisher publisher(port_of(sink));
        AuditRingSink audit;
        ASSERT_TRUE(events.try_push(ws_report("HY-REC", OrderState::Cancelled, 0)));  // CANCELED: not reachable from Submitting

        drain_user_data_events(in_flight, &audit, events, 5000, &truth, &fills, nullptr, nullptr, &publisher);

        EXPECT_EQ(publisher.stats().acked_frames, 2u) << "the Ambiguous re-record, then the cancellation";
        EXPECT_EQ(publisher.stats().refused_contradiction, 0u);
        EXPECT_EQ(fills.find("HY-REC")->shadow.state, OrderState::Cancelled);
    }
    const std::unique_ptr<DurableAuditSink> after = reopen(path);
    EXPECT_EQ(after->recovery_status(), RecoveryScanStatus::Clean) << "Submitting -> Cancelled written bare would make the whole log Corrupt";
    EXPECT_TRUE(after->recovered_checkpoints().empty()) << "Cancelled is exchange-final: nothing to recover";
}

TEST_F(DurablePathReplayTest, AStreamFillFollowedByAStaleRestAnswerLeavesALogThatReplays) {
    // The order is live (the log holds Accepted). The stream delivers FILLED; the reconcile thread, which polled a moment earlier, then reports PartialFill(4)
    // and only later the Filled it has now seen -- both with the prior_state the reconcile thread knew, which is out of date.
    const std::string path = write_log({OrderFrames{"HY-A", {history_frame(OrderState::Submitting), history_frame(OrderState::Accepted)}}});
    ASSERT_FALSE(path.empty());
    {
        DurableAuditSink sink(path, *ring_, kKeyId);
        InFlightRegistry in_flight;
        ToReconcileRing to_reconcile;
        PositionTruth truth;
        OrderFillContext fills;
        ASSERT_TRUE(apply_recovery(sink.recovered_checkpoints(), in_flight, to_reconcile, truth, fills, &plain_rules).ok());
        UserDataWsEventRing ws_ring;
        ReconcileEventRing rec_ring;
        UserDataDurablePublisher ws(port_of(sink));
        ReconcileDurablePublisher rec(port_of(sink));
        AuditRingSink audit;
        ReconcileIngress ingress{};
        ASSERT_TRUE(to_reconcile.try_pop(ingress));  // the handle the reconcile thread would hold

        ASSERT_TRUE(ws_ring.try_push(ws_report("HY-A", OrderState::Filled, 10)));
        drain_user_data_events(in_flight, &audit, ws_ring, 5000, &truth, &fills, nullptr, nullptr, &ws);
        EXPECT_EQ(ws.stats().written, 1u);
        EXPECT_EQ(truth.net_qty_ticks(1), 10);

        ASSERT_TRUE(rec_ring.try_push(resolution("HY-A", ingress.handle, OrderState::PartialFill, 4, OrderState::Accepted)));
        drain_reconcile_events(in_flight, &audit, rec_ring, 5001, &truth, &fills, &rec);
        EXPECT_EQ(rec.stale_skipped(), 1u) << "a PartialFill after the Filled the log already holds would make the whole log Corrupt";
        EXPECT_TRUE(in_flight.is_in_flight("HY-A")) << "not final: not released by an older answer";

        ASSERT_TRUE(rec_ring.try_push(resolution("HY-A", ingress.handle, OrderState::Filled, 10, OrderState::PartialFill)));
        drain_reconcile_events(in_flight, &audit, rec_ring, 5002, &truth, &fills, &rec);
        EXPECT_EQ(rec.stale_skipped(), 2u) << "the log already says Filled(10)";
        EXPECT_EQ(rec.refused(), 0u);
        EXPECT_FALSE(in_flight.is_in_flight("HY-A")) << "and a final answer still releases the slot";
        EXPECT_EQ(truth.net_qty_ticks(1), 10) << "the fill was credited once";
    }
    const std::unique_ptr<DurableAuditSink> after = reopen(path);
    EXPECT_EQ(after->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_TRUE(after->recovered_checkpoints().empty());
}

TEST_F(DurablePathReplayTest, TwoWritersSharingTheShadowsLeaveALogThatReplaysAndShadowsEqualToIt) {
    // Many orders, the stream and the reconcile path each saying random (often contradictory, often old) things about them -- the reconcile path with a
    // prior_state picked at random, because the shadow, not that, decides. Whatever each publisher wrote, the reopened log must replay and agree with the
    // shadow of every unfinished order.
    constexpr std::size_t kOrders = 24;
    constexpr std::size_t kSteps = 160;
    const std::string path = fresh();
    struct Order {
        std::string coid;
        InFlightHandle handle;
        bool released{false};
    };
    std::vector<Order> orders;
    std::size_t ws_written = 0, rec_written_or_skipped = 0, rec_refused = 0, ws_refused = 0;
    std::vector<std::pair<std::string, DurableShadow>> final_shadows;
    {
        DurableAuditSink sink(path, *ring_, kKeyId);
        ASSERT_TRUE(sink.is_open());
        InFlightRegistry in_flight;
        PositionTruth truth;
        OrderFillContext fills;
        AuditRingSink audit;
        UserDataWsEventRing ws_ring;
        ReconcileEventRing rec_ring;
        UserDataDurablePublisher ws(port_of(sink));
        ReconcileDurablePublisher rec(port_of(sink));
        Lcg rng{0xD1B54A32D192ED03ULL};

        for (std::size_t k = 0; k < kOrders; ++k) {
            Order o{"HY-TW-" + std::to_string(k), {}, false};
            ASSERT_TRUE(append(sink, o.coid, history_frame(OrderState::Intent)));
            ASSERT_TRUE(append(sink, o.coid, history_frame(OrderState::Submitting)));
            DurableShadow shadow = exact(OrderState::Submitting);
            switch (k % 3) {
                case 1:
                    ASSERT_TRUE(append(sink, o.coid, history_frame(OrderState::Accepted)));
                    shadow = exact(OrderState::Accepted);
                    break;
                case 2:
                    ASSERT_TRUE(append(sink, o.coid, history_frame(OrderState::Ambiguous)));
                    shadow = exact(OrderState::Ambiguous);
                    break;
                default:
                    break;
            }
            o.handle = in_flight.register_submit_handle(o.coid);
            ASSERT_TRUE(o.handle.valid());
            ASSERT_TRUE(fills.track(o.coid, 1, OrderSide::Buy, 0, 0, 0, kIntended));
            ASSERT_TRUE(fills.seed_shadow(o.coid, shadow));
            orders.push_back(o);
        }

        constexpr std::array<OrderState, 8> kResolvable{OrderState::Accepted, OrderState::PartialFill,     OrderState::Filled,    OrderState::CancelRequested,
                                                         OrderState::Cancelled, OrderState::Rejected,        OrderState::Expired,   OrderState::EscalatedToOperator};
        constexpr std::array<OrderState, 4> kPriors{OrderState::Ambiguous, OrderState::Accepted, OrderState::PartialFill, OrderState::Intent};
        for (std::size_t step = 0; step < kSteps; ++step) {
            Order& o = orders[rng.next() % orders.size()];
            const std::int64_t cumulative = static_cast<std::int64_t>(rng.next() % static_cast<std::uint32_t>(kIntended + 2));
            if (rng.next() % 2 == 0) {
                ASSERT_TRUE(ws_ring.try_push(ws_report(o.coid, kObservable[rng.next() % kObservable.size()], cumulative)));
                drain_user_data_events(in_flight, &audit, ws_ring, 5000, &truth, &fills, nullptr, nullptr, &ws);
            } else if (!o.released) {
                const OrderState result = kResolvable[rng.next() % kResolvable.size()];
                ASSERT_TRUE(rec_ring.try_push(resolution(o.coid, o.handle, result, result == OrderState::EscalatedToOperator ? 0 : cumulative,
                                                          kPriors[rng.next() % kPriors.size()])));
                drain_reconcile_events(in_flight, &audit, rec_ring, 5000, &truth, &fills, &rec);
                if (is_exchange_final(result)) o.released = true;  // the drain releases a final answer's slot, whatever the log could take of it
            }
        }
        ws_written = static_cast<std::size_t>(ws.stats().written);
        ws_refused = static_cast<std::size_t>(ws.stats().refused_contradiction);
        rec_written_or_skipped = static_cast<std::size_t>(rec.acked_frames() + rec.stale_skipped());
        rec_refused = static_cast<std::size_t>(rec.refused());
        EXPECT_FALSE(ws.blocked());
        EXPECT_FALSE(rec.blocked());
        for (const Order& o : orders) {
            // an order a final answer released has no context entry any more: its shadow is whatever the log holds, recomputed below from the scan itself
            const OrderFillContextEntry* e = fills.find(o.coid);
            if (e != nullptr) final_shadows.emplace_back(o.coid, e->shadow);
        }
    }

    const std::unique_ptr<DurableAuditSink> reopened = reopen(path);
    ASSERT_TRUE(replayed(reopened->recovery_status())) << "the two writers left a log that does not replay (status "
                                                       << static_cast<int>(reopened->recovery_status()) << ")";
    for (const auto& [coid, shadow] : final_shadows) {
        const OrderRecoveryCheckpoint* found = nullptr;
        for (const OrderRecoveryCheckpoint& cp : reopened->recovered_checkpoints()) {
            if (std::strncmp(cp.client_order_id.id, coid.c_str(), kClientOrderIdLen) == 0) found = &cp;
        }
        if (is_exchange_final(shadow.state)) {
            EXPECT_EQ(found, nullptr) << coid << ": a finished order is not a checkpoint";
            continue;
        }
        ASSERT_NE(found, nullptr) << coid << ": an unfinished order is a checkpoint";
        const OrderState expected = shadow.state == OrderState::Submitting ? OrderState::Ambiguous : shadow.state;
        EXPECT_EQ(found->resulting_state, expected) << coid;
        EXPECT_EQ(found->filled_qty_ticks, shadow.filled_qty_ticks) << coid << ": the shadow's fill is the log's";
    }
    // Both writers must actually have written, skipped and refused things (a walk that only ever did one of them proves little).
    EXPECT_GT(ws_written, 5u);
    EXPECT_GT(ws_refused + rec_refused, 5u);
    EXPECT_GT(rec_written_or_skipped, 5u);
}
