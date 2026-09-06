// SPDX-License-Identifier: proprietary
// strategy_spec_operators.hpp — Batch 5, 5b: streaming (O(window) or O(1) bounded-state) C++
// implementations of docs/STRATEGY_SPEC.md §5's operator set, ported to reproduce
// py_core/indicators/operators.py bit-for-bit (§2.2's "两侧必须逐 bar 完全一致").
//
// Header-only: this IS the per-bar hot path (called once per node per bar by
// StreamingEvaluator::step(), strategy_spec_evaluator.hpp), unlike strategy_spec_toml_parser
// (cold-start spec loading, allowed to allocate). Zero heap allocation throughout -- every
// operator's runtime state is a plain, fixed-size struct; window-class operators (sma/stddev/
// rolling_max/rolling_min/roc/lag) share one fixed-capacity history pool
// (kHistoryPoolCapacity, sized generously and checked at StreamingEvaluator::init() time, not
// grown at runtime -- see that file for the aggregate capacity check).
//
// sma/stddev do a full O(window) recompute every bar, NOT an O(1) incremental running sum
// (Welford's algorithm or Σx²-(Σx)²/N are also explicitly excluded for stddev) -- this is a
// verified, load-bearing requirement, not an oversight: operators.py's own sma()/stddev() do
// the identical full recompute for exactly the reason §2.2 states ("滑动加减更快但会累积
// 路径依赖的舍入误差"), and matching Python bit-for-bit requires matching its *method*, not
// just an algebraically-equivalent result. Window sizes in this system are small (14-50 in
// every example so far), so the O(window) cost here is negligible — do not "optimize" this to
// an incremental accumulator without re-reading operators.py's own comment on why it avoids
// that on purpose.
//
// rsi()'s Wilder-smoothing recursion (avg = (avg*(window-1)+x)/window) must be translated
// verbatim, in this exact operation order — an algebraically-equivalent rearrangement (e.g.
// avg*(1-1/window) + x/window) produces a different IEEE754 rounding sequence and will NOT
// reproduce Python bit-for-bit.

#pragma once

#include <hengyuan/strategy_spec_types.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

namespace hy {

// One OHLCV bar. All fields float64 throughout the DAG, per §2.2 ("所有中间量一律 float64").
struct Bar {
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double close{0.0};
    double volume{0.0};
};

// Total capacity (in doubles) shared across every window-class node's history ring in one
// StreamingEvaluator. A generous fixed upper bound, checked defensively at
// StreamingEvaluator::init() time against the actual sum of window/lag sizes the loaded spec
// needs — never grown at runtime. See that file's own header comment for the exact contract.
inline constexpr std::size_t kHistoryPoolCapacity = 1024;

// sma/stddev/rolling_max/rolling_min/roc/lag: a slice of the shared history pool, addressed
// as a ring buffer. `capacity` is this node's own window (or lag's `n`) -- validated to fit
// uint16_t and the pool budget by StreamingEvaluator::init().
struct WindowState {
    std::uint16_t pool_offset{0};
    std::uint16_t capacity{0};
    std::uint16_t head{0};
    std::uint16_t count{0};
};

// ema: SMA-seeded, then O(1) recursion (operators.py's ema() is already incremental in
// Python -- no numerical-parity risk from also being incremental in C++, unlike sma/stddev).
struct EmaState {
    double last_value{0.0};
    double seed_sum{0.0};
    std::uint16_t seed_count{0};
    bool seeded{false};
};

// rsi: Wilder-smoothed running averages, seeded from the first `window` deltas.
struct RsiState {
    double prev_input{0.0};
    bool has_prev_input{false};
    double avg_gain{0.0};
    double avg_loss{0.0};
    double seed_gain_sum{0.0};
    double seed_loss_sum{0.0};
    std::uint16_t seed_count{0};
    bool seeded{false};
};

// crosses_above/crosses_below: only the immediately preceding bar's two resolved operand
// values are needed (operators.py's crosses_above()/crosses_below() only ever look one bar
// back) -- no window history required.
struct CrossesState {
    double prev_left{0.0};
    double prev_right{0.0};
    bool has_prev{false};
};

// Per-node runtime state, shaped to exactly what its assigned operator needs -- the 12
// stateless ops (add/sub/mul/div/gt/lt/ge/le/and/or/not/if_then_else) touch none of this
// union's members and cost nothing beyond the union's own storage (sized to its largest
// member, WindowState/EmaState/RsiState/CrossesState, all small PODs).
union NodeRuntimeState {
    WindowState window;
    EmaState ema;
    RsiState rsi;
    CrossesState crosses;
};

namespace detail {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

inline void ring_push(WindowState& state, double* pool, double value) noexcept {
    pool[state.pool_offset + state.head] = value;
    state.head = static_cast<std::uint16_t>((state.head + 1) % state.capacity);
    if (state.count < state.capacity) ++state.count;
}

// Mirrors pandas' `min_periods == window` semantics for a fixed-length rolling window: the
// entire window must be free of NaN, not just the newest value.
inline bool ring_has_nan(const WindowState& state, const double* pool) noexcept {
    for (std::uint16_t i = 0; i < state.capacity; ++i) {
        if (std::isnan(pool[state.pool_offset + i])) return true;
    }
    return false;
}

inline double step_sma(WindowState& state, double* pool, double input) noexcept {
    ring_push(state, pool, input);
    if (state.count < state.capacity || ring_has_nan(state, pool)) return kNaN;
    double sum = 0.0;
    for (std::uint16_t i = 0; i < state.capacity; ++i) sum += pool[state.pool_offset + i];
    return sum / static_cast<double>(state.capacity);
}

inline double step_stddev(WindowState& state, double* pool, double input) noexcept {
    ring_push(state, pool, input);
    if (state.count < state.capacity || ring_has_nan(state, pool)) return kNaN;
    double sum = 0.0;
    for (std::uint16_t i = 0; i < state.capacity; ++i) sum += pool[state.pool_offset + i];
    const double mean = sum / static_cast<double>(state.capacity);
    double sq_sum = 0.0;
    for (std::uint16_t i = 0; i < state.capacity; ++i) {
        const double d = pool[state.pool_offset + i] - mean;
        sq_sum += d * d;
    }
    // ddof=1 (sample stddev) -- 5a's load-time gate already enforces capacity(window) >= 2.
    return std::sqrt(sq_sum / static_cast<double>(state.capacity - 1));
}

inline double step_rolling_max(WindowState& state, double* pool, double input) noexcept {
    ring_push(state, pool, input);
    if (state.count < state.capacity || ring_has_nan(state, pool)) return kNaN;
    double m = pool[state.pool_offset];
    for (std::uint16_t i = 1; i < state.capacity; ++i) m = std::max(m, pool[state.pool_offset + i]);
    return m;
}

inline double step_rolling_min(WindowState& state, double* pool, double input) noexcept {
    ring_push(state, pool, input);
    if (state.count < state.capacity || ring_has_nan(state, pool)) return kNaN;
    double m = pool[state.pool_offset];
    for (std::uint16_t i = 1; i < state.capacity; ++i) m = std::min(m, pool[state.pool_offset + i]);
    return m;
}

// x[t]/x[t-window] - 1, denom 0 -> NaN (never inf). `state.capacity == window` (not
// window+1): the ring slot about to be overwritten by this push is exactly the value pushed
// `capacity` steps ago, i.e. x[t-window] relative to the value being pushed now.
inline double step_roc(WindowState& state, double* pool, double input) noexcept {
    const bool full = (state.count == state.capacity);
    const double x_t_minus_window = pool[state.pool_offset + state.head];
    ring_push(state, pool, input);
    if (!full) return kNaN;
    if (x_t_minus_window == 0.0) return kNaN;
    // NaN propagates naturally through division if either operand is NaN (IEEE754), matching
    // operators.py's roc() -- no separate isnan() branch needed here.
    return input / x_t_minus_window - 1.0;
}

// x.shift(n): same "read the slot about to be overwritten, then push" trick as roc(), with
// capacity == n and no arithmetic on the retrieved value.
inline double step_lag(WindowState& state, double* pool, double input) noexcept {
    const bool full = (state.count == state.capacity);
    const double delayed = pool[state.pool_offset + state.head];
    ring_push(state, pool, input);
    return full ? delayed : kNaN;
}

inline double step_ema(EmaState& state, double window, double input) noexcept {
    if (std::isnan(input)) {
        // operators.py's ema() skips a NaN prefix via _first_valid_position() before seeding,
        // then never sees NaN again post-seed (the NaN-prefix invariant). Stay defensive:
        // a NaN input never advances or corrupts the accumulator either way.
        return kNaN;
    }
    if (state.seeded) {
        const double alpha = 2.0 / (window + 1.0);
        state.last_value = alpha * input + (1.0 - alpha) * state.last_value;
        return state.last_value;
    }
    state.seed_sum += input;
    ++state.seed_count;
    if (static_cast<double>(state.seed_count) < window) return kNaN;
    state.last_value = state.seed_sum / window;
    state.seeded = true;
    return state.last_value;
}

inline double rsi_from_averages(double avg_gain, double avg_loss) noexcept {
    if (avg_loss == 0.0) return 100.0;
    const double rs = avg_gain / avg_loss;
    return 100.0 - 100.0 / (1.0 + rs);
}

inline double step_rsi(RsiState& state, double window, double input) noexcept {
    if (std::isnan(input)) return kNaN;
    if (!state.has_prev_input) {
        state.prev_input = input;
        state.has_prev_input = true;
        return kNaN;  // first observation -- no delta yet
    }
    const double delta = input - state.prev_input;
    state.prev_input = input;
    const double gain = delta > 0.0 ? delta : 0.0;
    const double loss = delta < 0.0 ? -delta : 0.0;

    if (!state.seeded) {
        state.seed_gain_sum += gain;
        state.seed_loss_sum += loss;
        ++state.seed_count;
        if (static_cast<double>(state.seed_count) < window) return kNaN;
        state.avg_gain = state.seed_gain_sum / window;
        state.avg_loss = state.seed_loss_sum / window;
        state.seeded = true;
        return rsi_from_averages(state.avg_gain, state.avg_loss);
    }
    // Exact operation order from operators.py:181-182 -- do not rearrange (see this file's
    // own header comment).
    state.avg_gain = (state.avg_gain * (window - 1.0) + gain) / window;
    state.avg_loss = (state.avg_loss * (window - 1.0) + loss) / window;
    return rsi_from_averages(state.avg_gain, state.avg_loss);
}

// add/sub/mul propagate NaN automatically via IEEE754 arithmetic, matching operators.py (no
// explicit isnan() branch there either). div/gt/lt/ge/le/and/or all need an explicit branch
// because a *comparison* against NaN evaluates to false rather than propagating, unlike
// arithmetic -- operators.py itself does this via an explicit np.where(isnan(...), NaN, ...)
// override after the fact; this ports that same override.
inline double apply_binary(SpecOp op, double left, double right) noexcept {
    switch (op) {
        case SpecOp::Add:
            return left + right;
        case SpecOp::Sub:
            return left - right;
        case SpecOp::Mul:
            return left * right;
        case SpecOp::Div:
            if (right == 0.0) return kNaN;  // NaN `right` falls through to left/right, itself NaN
            return left / right;
        case SpecOp::Gt:
            if (std::isnan(left) || std::isnan(right)) return kNaN;
            return (left > right) ? 1.0 : 0.0;
        case SpecOp::Lt:
            if (std::isnan(left) || std::isnan(right)) return kNaN;
            return (left < right) ? 1.0 : 0.0;
        case SpecOp::Ge:
            if (std::isnan(left) || std::isnan(right)) return kNaN;
            return (left >= right) ? 1.0 : 0.0;
        case SpecOp::Le:
            if (std::isnan(left) || std::isnan(right)) return kNaN;
            return (left <= right) ? 1.0 : 0.0;
        case SpecOp::And:
            if (std::isnan(left) || std::isnan(right)) return kNaN;
            return (left != 0.0 && right != 0.0) ? 1.0 : 0.0;
        case SpecOp::Or:
            if (std::isnan(left) || std::isnan(right)) return kNaN;
            return (left != 0.0 || right != 0.0) ? 1.0 : 0.0;
        default:
            return kNaN;  // unreachable given the caller's own dispatch, defensive only
    }
}

inline double apply_not(double x) noexcept {
    if (std::isnan(x)) return kNaN;
    return (x != 0.0) ? 0.0 : 1.0;
}

// cond == 1.0 (not merely != 0.0) selects `then`, matching operators.py's if_then_else()
// verbatim; NaN cond -> NaN regardless of which branch's own value would otherwise apply.
inline double apply_if_then_else(double cond, double then_value, double otherwise_value) noexcept {
    if (std::isnan(cond)) return kNaN;
    return (cond == 1.0) ? then_value : otherwise_value;
}

// a[t] > b[t] && a[t-1] <= b[t-1] (or the mirrored < / >= for crosses_below), NaN if any of
// the four needed values is NaN -- including bar 0, which has no "previous" at all.
inline double step_crosses(SpecOp op, CrossesState& state, double left, double right) noexcept {
    double result = kNaN;
    if (state.has_prev && !std::isnan(left) && !std::isnan(right) && !std::isnan(state.prev_left) &&
        !std::isnan(state.prev_right)) {
        const bool crossed = (op == SpecOp::CrossesAbove)
                                  ? (left > right && state.prev_left <= state.prev_right)
                                  : (left < right && state.prev_left >= state.prev_right);
        result = crossed ? 1.0 : 0.0;
    }
    state.prev_left = left;
    state.prev_right = right;
    state.has_prev = true;
    return result;
}

}  // namespace detail
}  // namespace hy
