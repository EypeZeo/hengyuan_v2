// SPDX-License-Identifier: proprietary
// strategy_spec_evaluator.hpp — Batch 5, 5b: StreamingEvaluator, the bar-by-bar DAG walker
// built on top of strategy_spec_operators.hpp's per-operator step functions.
//
// Cold path (once per loaded spec): StreamingEvaluator::init() lays out each window-class
// node's slice of the shared history pool and computes the DAG's effective warm-up
// (compute_effective_warmup(), a pure structural pass — §6: "spec 的有效 warm-up 必须能被
// 静态算出，加载时就报给调用方"). init() is also where the one aggregate capacity check this
// batch's plan calls for lives: if the sum of every window-class node's window/lag size
// exceeds kHistoryPoolCapacity (strategy_spec_operators.hpp), or any single node's window/lag
// doesn't fit a uint16_t, init() returns false and the evaluator must not be stepped.
//
// Hot path (every bar): step() walks dag_.nodes in declared order — already a valid
// topological order by construction, per 5a's forward-reference-only DAG (see
// strategy_spec_toml_parser.cpp's own comment on why no separate cycle detection is needed
// either side of the language boundary) — dispatching each node to its operator's step
// function and writing the result into current_values_[i], all in fixed-size, allocation-free
// storage.
//
// Explicit non-goal (Batch 5's stated scope, docs/STRATEGY_SPEC.md §8): nothing here is wired
// into IntentChannel/ExecutionIntent or live_submit_orchestrator.hpp's orchestrate_submit().
// This is a standalone library proven bar-consistent with Python (5c); it has no caller yet.

#pragma once

#include <hengyuan/strategy_spec_operators.hpp>
#include <hengyuan/strategy_spec_types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace hy {

namespace detail {

inline std::uint32_t node_own_warmup(SpecOp op, std::int32_t param) noexcept {
    switch (op) {
        case SpecOp::Sma:
        case SpecOp::Ema:
        case SpecOp::Stddev:
        case SpecOp::RollingMax:
        case SpecOp::RollingMin:
        case SpecOp::Roc:
        case SpecOp::Lag:
            return static_cast<std::uint32_t>(param);
        case SpecOp::Rsi:
            // Needs `window` deltas, i.e. window+1 raw prices (operators.py's rsi():
            // seed_position = start + window).
            return static_cast<std::uint32_t>(param) + 1;
        case SpecOp::CrossesAbove:
        case SpecOp::CrossesBelow:
            return 1;  // needs t AND t-1
        default:
            return 0;  // arithmetic/comparison/logical/if_then_else contribute none of their own
    }
}

inline bool is_window_class_op(SpecOp op) noexcept {
    switch (op) {
        case SpecOp::Sma:
        case SpecOp::Stddev:
        case SpecOp::RollingMax:
        case SpecOp::RollingMin:
        case SpecOp::Roc:
        case SpecOp::Lag:
            return true;
        default:
            return false;
    }
}

}  // namespace detail

// Pure structural pass, no data needed — mirrors py_core/strategy_spec/evaluator.py's
// compute_effective_warmup() (own contribution + max of NodeRef operands' already-computed
// warm-ups; RawField/Literal operands contribute 0).
inline std::uint32_t compute_effective_warmup(const SpecDag& dag) noexcept {
    std::array<std::uint32_t, kMaxIndicatorNodes> warmup{};
    for (std::size_t i = 0; i < dag.node_count; ++i) {
        const SpecNode& node = dag.nodes[i];
        std::uint32_t input_max = 0;
        for (std::uint8_t s = 0; s < node.operand_count; ++s) {
            if (node.operands[s].kind == OperandKind::NodeRef) {
                input_max = std::max(input_max, warmup[node.operands[s].node_idx]);
            }
        }
        warmup[i] = detail::node_own_warmup(node.op, node.param) + input_max;
    }
    return dag.node_count > 0 ? warmup[dag.signal_node_idx] : 0;
}

// [signal] output mapping (§4.4): boolean mode -- NaN -> 0.0, else nonzero -> 1.0/0.0.
// scaled mode -- NaN -> 0.0, else clamped to [-1.0, 1.0].
inline double apply_signal_mode(SignalMode mode, double node_value) noexcept {
    if (std::isnan(node_value)) return 0.0;
    if (mode == SignalMode::Boolean) return node_value != 0.0 ? 1.0 : 0.0;
    return std::clamp(node_value, -1.0, 1.0);
}

class StreamingEvaluator {
public:
    // Lays out the history pool and computes effective_warmup. Returns false (evaluator must
    // not be step()-ed) if any window-class node's window/lag exceeds what fits a uint16_t, or
    // the sum across all nodes exceeds kHistoryPoolCapacity -- the one aggregate capacity
    // check this batch's plan assigns to evaluator construction rather than 5a's load gate
    // (5a's SpecDag has no notion of a shared history pool; that's this file's concern).
    bool init(const SpecDag& dag) noexcept {
        reset();
        dag_ = dag;

        std::size_t pool_used = 0;
        for (std::size_t i = 0; i < dag_.node_count; ++i) {
            const SpecNode& node = dag_.nodes[i];
            if (!detail::is_window_class_op(node.op)) continue;
            if (node.param <= 0 || node.param > 0xffff) return false;
            const std::size_t capacity = static_cast<std::size_t>(node.param);
            if (pool_used + capacity > kHistoryPoolCapacity) return false;
            states_[i].window.pool_offset = static_cast<std::uint16_t>(pool_used);
            states_[i].window.capacity = static_cast<std::uint16_t>(capacity);
            states_[i].window.head = 0;
            states_[i].window.count = 0;
            pool_used += capacity;
        }

        dag_.effective_warmup = compute_effective_warmup(dag_);
        initialized_ = true;
        return true;
    }

    // Advances by one bar; returns the [signal]-mapped target position. Must only be called
    // after a successful init(). Zero heap allocation, zero I/O, zero system calls.
    double step(const Bar& bar) noexcept {
        for (std::size_t i = 0; i < dag_.node_count; ++i) {
            const SpecNode& node = dag_.nodes[i];
            double value = std::numeric_limits<double>::quiet_NaN();
            switch (node.op) {
                case SpecOp::Sma:
                    value = detail::step_sma(states_[i].window, history_pool_.data(),
                                              resolve_operand(node.operands[0], bar));
                    break;
                case SpecOp::Stddev:
                    value = detail::step_stddev(states_[i].window, history_pool_.data(),
                                                 resolve_operand(node.operands[0], bar));
                    break;
                case SpecOp::RollingMax:
                    value = detail::step_rolling_max(states_[i].window, history_pool_.data(),
                                                      resolve_operand(node.operands[0], bar));
                    break;
                case SpecOp::RollingMin:
                    value = detail::step_rolling_min(states_[i].window, history_pool_.data(),
                                                      resolve_operand(node.operands[0], bar));
                    break;
                case SpecOp::Roc:
                    value = detail::step_roc(states_[i].window, history_pool_.data(),
                                              resolve_operand(node.operands[0], bar));
                    break;
                case SpecOp::Lag:
                    value = detail::step_lag(states_[i].window, history_pool_.data(),
                                              resolve_operand(node.operands[0], bar));
                    break;
                case SpecOp::Ema:
                    value = detail::step_ema(states_[i].ema, static_cast<double>(node.param),
                                              resolve_operand(node.operands[0], bar));
                    break;
                case SpecOp::Rsi:
                    value = detail::step_rsi(states_[i].rsi, static_cast<double>(node.param),
                                              resolve_operand(node.operands[0], bar));
                    break;
                case SpecOp::Add:
                case SpecOp::Sub:
                case SpecOp::Mul:
                case SpecOp::Div:
                case SpecOp::Gt:
                case SpecOp::Lt:
                case SpecOp::Ge:
                case SpecOp::Le:
                case SpecOp::And:
                case SpecOp::Or:
                    value = detail::apply_binary(node.op, resolve_operand(node.operands[0], bar),
                                                  resolve_operand(node.operands[1], bar));
                    break;
                case SpecOp::Not:
                    value = detail::apply_not(resolve_operand(node.operands[0], bar));
                    break;
                case SpecOp::CrossesAbove:
                case SpecOp::CrossesBelow:
                    value = detail::step_crosses(node.op, states_[i].crosses,
                                                  resolve_operand(node.operands[0], bar),
                                                  resolve_operand(node.operands[1], bar));
                    break;
                case SpecOp::IfThenElse:
                    value = detail::apply_if_then_else(resolve_operand(node.operands[0], bar),
                                                        resolve_operand(node.operands[1], bar),
                                                        resolve_operand(node.operands[2], bar));
                    break;
            }
            current_values_[i] = value;
        }
        ++seen_bars_;
        return apply_signal_mode(dag_.signal_mode, current_values_[dag_.signal_node_idx]);
    }

    // For the cross-language consistency test (5c) -- inspects a specific node's value from
    // the bar just processed by step(), not just the final [signal]-mapped output.
    double node_value(std::size_t node_idx) const noexcept { return current_values_[node_idx]; }

    std::uint32_t effective_warmup() const noexcept { return dag_.effective_warmup; }

    // Bars fed through step() since init()/reset(). 批次 6 6b-0c: read-only, added so an order
    // planner can refuse to act on a signal the evaluator has not had time to define yet.
    std::uint64_t seen_bars() const noexcept { return seen_bars_; }

    // True once the [signal] node's value is safe to act on. Deliberately `seen_bars > warmup`,
    // NOT `>=`: effective_warmup() (compute_effective_warmup(), §6) is exact for window-average
    // ops (sma(n): first defined value on the n-th bar, W=n) but UNDERCOUNTS by one bar for
    // lag/roc/crosses over raw fields (lag(n): first defined value on bar n+1, W=n; crosses over
    // raw fields needs two bars, W=1) -- see test_strategy_spec_evaluator.cpp's warm-up boundary
    // tests. `>` is therefore safe for every operator and merely one bar conservative for the
    // window-average family. Why this matters: apply_signal_mode() maps a still-undefined (NaN)
    // signal to 0.0, so after a restart an acting-on-level planner would read "target position
    // zero" during warm-up and could propose selling a real holding.
    bool warmup_complete() const noexcept {
        return initialized_ && seen_bars_ > dag_.effective_warmup;
    }

    bool is_initialized() const noexcept { return initialized_; }

    // Clears all runtime state (history pool, per-node accumulators, bar counter) without
    // re-parsing the TOML -- for network-reconnect gap recovery or re-running a backtest over
    // a different historical range with the same already-loaded spec.
    void reset() noexcept {
        dag_ = SpecDag{};
        current_values_.fill(std::numeric_limits<double>::quiet_NaN());
        for (auto& state : states_) state = NodeRuntimeState{};
        history_pool_.fill(0.0);
        seen_bars_ = 0;
        initialized_ = false;
    }

private:
    double resolve_operand(const Operand& operand, const Bar& bar) const noexcept {
        switch (operand.kind) {
            case OperandKind::Literal:
                return operand.literal_value;
            case OperandKind::NodeRef:
                return current_values_[operand.node_idx];
            case OperandKind::RawFieldRef:
                switch (static_cast<RawField>(operand.raw_field_idx)) {
                    case RawField::Open:
                        return bar.open;
                    case RawField::High:
                        return bar.high;
                    case RawField::Low:
                        return bar.low;
                    case RawField::Close:
                        return bar.close;
                    case RawField::Volume:
                        return bar.volume;
                }
                break;
        }
        return std::numeric_limits<double>::quiet_NaN();  // unreachable, defensive only
    }

    SpecDag dag_{};
    bool initialized_{false};
    std::array<double, kMaxIndicatorNodes> current_values_{};
    std::array<NodeRuntimeState, kMaxIndicatorNodes> states_{};
    std::array<double, kHistoryPoolCapacity> history_pool_{};
    std::uint64_t seen_bars_{0};
};

}  // namespace hy
