// SPDX-License-Identifier: proprietary
// spot_rate_limit_budget.hpp — TODOLIST Stage 1A · TODO 1A.2: local safety-margin rate
// limiter across Binance's three spot REST rate-limit dimensions (REQUEST_WEIGHT,
// RAW_REQUESTS, ORDERS), partitioned into StrategyBudget/ReconciliationReserve/
// EmergencyReserve lanes (default 80/10/10).
//
// Governance: L4, extends transport_policy.hpp.
//
// docs/BINANCE_PRIVATE_REST_L4_SPEC.md §7 (lines 1386-2449) specifies a considerably
// larger, server-time-bucket-aligned, header-corrected, multi-interval-set design
// (RequestWeightTrackerSet/RawRequestsTrackerSet/OrderCountTrackerSet, BucketIdentity,
// correct_from_response_header()) that is entirely spec-only in this repo. This file is
// a deliberately scoped first slice: single-interval-per-dimension, steady_clock-only
// (transport_policy.hpp::RequestWeightTracker's existing engine, parameterized rather
// than replaced), wired to the one real order-placing call site that exists today
// (live_submit_orchestrator.hpp's Gate 8/12c). The 80/10/10 StrategyBudget/
// ReconciliationReserve/EmergencyReserve split has NO counterpart anywhere in §7 (which
// only has a flat additive safety_pad) -- it is this batch's own policy design, not a
// spec transcription.
//
// Explicit non-goals for this slice (see the batch's own plan for full reasoning):
// §9.1's actor/scheduler, BucketIdentity/server-time-aligned fixed buckets, response
// header parsing (X-MBX-USED-WEIGHT-*/X-MBX-ORDER-COUNT-*), multi-interval sets (this
// file models exactly one interval per dimension -- Binance's real ORDERS limit has
// BOTH a 10s and a 1-day window; only the 10s one is tracked here, a named residual
// risk), EndpointWeightConfig operator-reload, bootstrap/FreezeProbeCredit/
// TimeResyncCredit/Retry-After handling, and rate-limit awareness for query_order()/
// fetch_account()/fetch_exchange_info() (those run on threads other than the one this
// tracker is owned by -- see "THREAD OWNERSHIP" below).
//
// THREAD OWNERSHIP (load-bearing, not a convention comment, mirrors order_tracker.hpp's
// own wording): SpotRateLimitTracker/PartitionedRateBudget carry NO internal
// synchronization. In this batch they are owned EXCLUSIVELY by the hot/submit thread
// (the same thread that already owns InFlightRegistry and calls orchestrate_submit())
// -- no other thread may touch them. Binance enforces these limits account-and-IP-wide,
// not per-subsystem, so the *correct* long-term answer once query_order()/
// fetch_account() need rate-limit awareness too is a shared, cross-thread-reachable
// source of truth (this repo already has a precedent for that shape:
// binance_clock_sync.hpp's ClockOffsetPublisher, mutex + by-value copy/swap) -- that is
// a real, separate design fork for a future batch, not something this file attempts.

#include <hengyuan/transport_policy.hpp>

#include <cstdint>
#include <limits>
#include <type_traits>

namespace hy {

// --- Endpoint enum: numeric values pinned to match L4 spec §7.4's PrivateRestEndpoint
// (docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2313) exactly, checked build-time by
// tools/spec_enum_diff.py (CI Spec Verification). GetRateLimitOrder=3 has no real call
// site in this codebase this batch (no caller wires GET /api/v3/rateLimit/order yet --
// see this file's header comment on scope) but MUST keep the spec's numeric slot: a
// codebase-only subset enum (omitting it and shifting GetExchangeInfo/GetServerTime
// down by one) is exactly the VALUE_CONFLICT this tool exists to catch, since it
// silently reinterprets any wire/persisted discriminator sharing this numbering.
// Transcribe the spec block verbatim rather than adjusting the spec to match the code.
enum class PrivateRestEndpoint : std::uint8_t {
    PostOrder = 0,           // POST /api/v3/order        (live_submit_orchestrator.hpp Gate 12c)
    GetOrder = 1,             // GET  /api/v3/order         (order_tracker.hpp QueryPort, reconcile thread)
    GetAccount = 2,           // GET  /api/v3/account
    GetRateLimitOrder = 3,    // GET  /api/v3/rateLimit/order -- reserved spec slot, no caller this batch
    GetExchangeInfo = 4,      // GET  /api/v3/exchangeInfo
    GetServerTime = 5,        // GET  /api/v3/time
};
inline constexpr std::size_t kPrivateRestEndpointCount = 6;

// Binance spot REST documented weights (IP-scoped), pinned to match L4 spec §7.4's
// kPinnedEndpointWeight values for these 6 endpoints exactly.
struct EndpointWeightTable {
    std::uint32_t weights[kPrivateRestEndpointCount] = {
        /*PostOrder*/ 1u,
        /*GetOrder*/ 4u,
        /*GetAccount*/ 20u,
        /*GetRateLimitOrder*/ 40u,
        /*GetExchangeInfo*/ 20u,
        /*GetServerTime*/ 1u,
    };
};
static_assert(std::is_trivially_copyable_v<EndpointWeightTable>);
static_assert(std::is_standard_layout_v<EndpointWeightTable>);

// Unknown/out-of-range endpoint -> UINT32_MAX (refuse), never a guessed weight.
inline std::uint32_t endpoint_weight(const EndpointWeightTable& table,
                                      PrivateRestEndpoint ep) noexcept {
    const auto idx = static_cast<std::size_t>(ep);
    if (idx >= kPrivateRestEndpointCount) [[unlikely]] {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return table.weights[idx];
}

// --- TODO 1A.2's StrategyBudget/ReconciliationReserve/EmergencyReserve split ---
enum class RateLimitLane : std::uint8_t {
    Strategy = 0,        // ordinary order flow -- Gate 8/12c PostOrder (this batch's only
                          // real caller)
    Reconciliation = 1,  // reserved for GetOrder polling / GetAccount / GetExchangeInfo
                          // refresh -- no caller wired this batch, lane exists and is
                          // independently tested
    Emergency = 2,        // reachable ONLY via an explicit RateLimitLane::Emergency call
                          // -- EmergencyExit/EmergencyCancel-class calls, mirroring the
                          // disk-reserve "only Emergency* may use the reserved space"
                          // philosophy elsewhere in this system. No such REST call exists
                          // yet, so this lane has no real caller this batch either.
};
inline constexpr std::size_t kRateLimitLaneCount = 3;

struct RateLimitBudgetSplit {
    std::uint32_t strategy_pct{80};
    std::uint32_t reconciliation_pct{10};
    std::uint32_t emergency_pct{10};

    static constexpr RateLimitBudgetSplit default_split() noexcept { return {}; }

    // Checked add: three uint32_t percentages summing to exactly 100. Rejects any
    // other total (including a sum that would overflow before comparison) rather
    // than silently accepting a split that doesn't partition the whole budget.
    bool is_valid() const noexcept {
        if (strategy_pct > 100 || reconciliation_pct > 100 || emergency_pct > 100) {
            return false;
        }
        const std::uint32_t sum1 = strategy_pct + reconciliation_pct;
        if (sum1 < strategy_pct) return false;  // overflow (unreachable given the
                                                  // >100 guards above, checked anyway)
        const std::uint32_t sum2 = sum1 + emergency_pct;
        if (sum2 < sum1) return false;
        return sum2 == 100;
    }
};
static_assert(std::is_trivially_copyable_v<RateLimitBudgetSplit>);
static_assert(std::is_standard_layout_v<RateLimitBudgetSplit>);

// One rate-limit dimension (REQUEST_WEIGHT, RAW_REQUESTS, or ORDERS), partitioned into
// three independent RequestWeightTracker lanes. A lane's try_consume/can_send/rollback
// NEVER reads or writes another lane's state -- Strategy exhausting its 80% cannot
// borrow into Reconciliation's or Emergency's 10%.
//
// The safety margin is subtracted exactly ONCE, at the outer available_total level,
// before the 80/10/10 split -- each lane's own RequestWeightTracker is then constructed
// with safety_margin=0. Splitting a margin that's already been subtracted once again
// per-lane would silently shrink the real usable budget to less than intended.
//
// Floor division (lane_limit = available_total * pct / 100) is the load-bearing safety
// property: it guarantees sum(lane_limit) <= available_total always, so partitioning can
// only ever be MORE conservative than one unpartitioned tracker at the same limit, never
// less -- fail-closed by construction, not by convention.
class PartitionedRateBudget {
public:
    using Clock = RequestWeightTracker::Clock;
    using TimePoint = Clock::time_point;

    // Returns false (and leaves every lane's budget at zero -- RequestWeightTracker's
    // own default-constructed state) if !split.is_valid(). Callers MUST check the
    // return value; this is not a "best effort, ignore bad input" reset.
    bool reset(std::uint32_t limit, std::uint32_t safety_margin,
               RateLimitBudgetSplit split = RateLimitBudgetSplit::default_split(),
               TimePoint now = Clock::now(), std::uint32_t window_seconds = 60) noexcept {
        if (!split.is_valid()) [[unlikely]] {
            // Explicit zero-limit reset, not default-construction: RequestWeightTracker's
            // OWN default state is limit_=6000/safety_=500 (its class defaults), which is
            // very much sendable -- {} here would silently fail OPEN instead of closed.
            lanes_[0].reset(0, 0, now, window_seconds);
            lanes_[1].reset(0, 0, now, window_seconds);
            lanes_[2].reset(0, 0, now, window_seconds);
            return false;
        }
        const std::uint32_t available_total = safety_margin < limit ? (limit - safety_margin) : 0;
        const std::uint32_t pct[kRateLimitLaneCount] = {
            split.strategy_pct, split.reconciliation_pct, split.emergency_pct};
        for (std::size_t i = 0; i < kRateLimitLaneCount; ++i) {
            const std::uint32_t lane_limit =
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(available_total) * pct[i]) / 100);
            lanes_[i].reset(lane_limit, 0, now, window_seconds);
        }
        return true;
    }

    bool can_send(RateLimitLane lane, std::uint32_t weight, TimePoint now = Clock::now()) const noexcept {
        return lane_tracker(lane).can_send(weight, now);
    }

    bool try_consume(RateLimitLane lane, std::uint32_t weight, TimePoint now = Clock::now()) noexcept {
        return lane_tracker(lane).try_consume(weight, now);
    }

    void rollback(RateLimitLane lane, std::uint32_t weight, TimePoint now = Clock::now()) noexcept {
        lane_tracker(lane).rollback(weight, now);
    }

    std::uint32_t remaining(RateLimitLane lane, TimePoint now = Clock::now()) const noexcept {
        return lane_tracker(lane).remaining(now);
    }

private:
    // Bounds-guards the enum-to-index conversion (defense in depth against a caller
    // constructing an out-of-range RateLimitLane via static_cast -- matches this
    // codebase's established fail-closed handling of untrusted/out-of-range enum-like
    // input elsewhere, e.g. map_binance_order_status()). Falls back to lane 0 rather
    // than indexing out of bounds.
    RequestWeightTracker& lane_tracker(RateLimitLane lane) noexcept {
        const auto idx = static_cast<std::size_t>(lane);
        return lanes_[idx < kRateLimitLaneCount ? idx : 0];
    }
    const RequestWeightTracker& lane_tracker(RateLimitLane lane) const noexcept {
        const auto idx = static_cast<std::size_t>(lane);
        return lanes_[idx < kRateLimitLaneCount ? idx : 0];
    }

    RequestWeightTracker lanes_[kRateLimitLaneCount]{};
};
static_assert(std::is_trivially_copyable_v<PartitionedRateBudget>);
static_assert(std::is_standard_layout_v<PartitionedRateBudget>);

// The "Spot RateLimit" tracker (TODO 1A.2's "现货限流" object): three
// PartitionedRateBudget dimensions -- REQUEST_WEIGHT, RAW_REQUESTS, ORDERS.
class SpotRateLimitTracker {
public:
    using Clock = RequestWeightTracker::Clock;
    using TimePoint = Clock::time_point;

    // Window defaults follow Binance's typical documented intervals for each
    // dimension: REQUEST_WEIGHT ~60s, RAW_REQUESTS ~300s, ORDERS ~10s (the SHORTER of
    // Binance's two real ORDERS windows -- the 1-day window is not modeled, see this
    // file's header comment).
    //
    // Returns false if any of the three RateLimitBudgetSplit values is invalid --
    // propagated from PartitionedRateBudget::reset(), same "caller must check" contract.
    bool configure(std::uint32_t weight_limit, std::uint32_t weight_safety_margin,
                   std::uint32_t raw_limit, std::uint32_t raw_safety_margin,
                   std::uint32_t orders_limit, std::uint32_t orders_safety_margin,
                   RateLimitBudgetSplit split = RateLimitBudgetSplit::default_split(),
                   TimePoint now = Clock::now(), std::uint32_t weight_window_s = 60,
                   std::uint32_t raw_window_s = 300, std::uint32_t orders_window_s = 10) noexcept {
        const bool ok_w = weight_.reset(weight_limit, weight_safety_margin, split, now, weight_window_s);
        const bool ok_r = raw_.reset(raw_limit, raw_safety_margin, split, now, raw_window_s);
        const bool ok_o = orders_.reset(orders_limit, orders_safety_margin, split, now, orders_window_s);
        return ok_w && ok_r && ok_o;
    }

    // Non-consuming pre-check for POST /api/v3/order -- all 3 dimensions.
    bool can_send_order(RateLimitLane lane, std::uint32_t weight, TimePoint now = Clock::now()) const noexcept {
        return weight_.can_send(lane, weight, now) && raw_.can_send(lane, 1, now) &&
               orders_.can_send(lane, 1, now);
    }

    // Atomic reserve for POST /api/v3/order: weight -> raw -> orders, rolling back
    // prior reservations on any later failure. Reduced-fidelity mirror of L4 spec
    // §7.2's try_reserve_all_budgets() (steady_clock-only, no BucketIdentity/
    // server-time correction).
    bool try_reserve_order(RateLimitLane lane, std::uint32_t weight, TimePoint now = Clock::now()) noexcept {
        if (!weight_.try_consume(lane, weight, now)) {
            return false;
        }
        if (!raw_.try_consume(lane, 1, now)) {
            weight_.rollback(lane, weight, now);
            return false;
        }
        if (!orders_.try_consume(lane, 1, now)) {
            raw_.rollback(lane, 1, now);
            weight_.rollback(lane, weight, now);
            return false;
        }
        return true;
    }

    // Non-order-placing calls (GetOrder/GetAccount/GetExchangeInfo/GetServerTime):
    // weight + raw only, never touches ORDERS.
    bool try_reserve_weight_only(RateLimitLane lane, PrivateRestEndpoint ep,
                                  const EndpointWeightTable& table,
                                  TimePoint now = Clock::now()) noexcept {
        const std::uint32_t w = endpoint_weight(table, ep);
        if (w == std::numeric_limits<std::uint32_t>::max()) [[unlikely]] {
            return false;  // unknown endpoint -- refuse, never guess a weight
        }
        if (!weight_.try_consume(lane, w, now)) {
            return false;
        }
        if (!raw_.try_consume(lane, 1, now)) {
            weight_.rollback(lane, w, now);
            return false;
        }
        return true;
    }

private:
    PartitionedRateBudget weight_{};
    PartitionedRateBudget raw_{};
    PartitionedRateBudget orders_{};
};
static_assert(std::is_trivially_copyable_v<SpotRateLimitTracker>);
static_assert(std::is_standard_layout_v<SpotRateLimitTracker>);

}  // namespace hy
