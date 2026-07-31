// SPDX-License-Identifier: proprietary
// order_lifecycle.hpp — Binance order state machine + idempotency + reconciliation.
//
// Governance: L1 (pure state machine logic, no network, no secret).
// ADR-019 D8 + Architect M6:
//   ✅ Full lifecycle: Intent→Submitting→Accepted/Rejected/Ambiguous→Fill→Reconciled
//   ✅ newClientOrderId idempotency key generation
//   ✅ POST timeout → Ambiguous (not "stop and discard")
//   ✅ Ambiguous → query via same clientOrderId → reconcile
//   ✅ No blind retry from Ambiguous
//   ✅ Valid state transitions enforced
//   ✅ Unknown-escalate → kill switch or operator takeover

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace hy {

// --- Order states ---

enum class OrderState : std::uint8_t {
    Intent = 0,           // Created locally, not yet submitted
    Submitting = 1,       // HTTP request in-flight
    Accepted = 2,         // Exchange acknowledged (NEW / PARTIALLY_FILLED / FILLED)
    Rejected = 3,         // Exchange rejected (-1013, -2010, etc.)
    Ambiguous = 4,        // POST timeout — may or may not be accepted (M6)
    PartialFill = 5,      // Partially filled (still open)
    Filled = 6,           // Fully filled
    CancelRequested = 7,  // Cancel in-flight
    Cancelled = 8,        // Confirmed cancelled
    Expired = 9,          // Exchange expired the order
    Reconciled = 10,      // Final state after query confirms actual status
    EscalatedToOperator = 11, // Ambiguous could not be resolved → human takeover
};

inline const char* order_state_name(OrderState s) noexcept {
    switch (s) {
        case OrderState::Intent: return "Intent";
        case OrderState::Submitting: return "Submitting";
        case OrderState::Accepted: return "Accepted";
        case OrderState::Rejected: return "Rejected";
        case OrderState::Ambiguous: return "Ambiguous";
        case OrderState::PartialFill: return "PartialFill";
        case OrderState::Filled: return "Filled";
        case OrderState::CancelRequested: return "CancelRequested";
        case OrderState::Cancelled: return "Cancelled";
        case OrderState::Expired: return "Expired";
        case OrderState::Reconciled: return "Reconciled";
        case OrderState::EscalatedToOperator: return "EscalatedToOperator";
    }
    return "Unknown";
}

// --- State transition validation ---

enum class TransitionResult : std::uint8_t {
    Ok = 0,
    InvalidTransition = 1,
    AlreadyTerminal = 2,
};

inline bool is_terminal(OrderState s) noexcept {
    return s == OrderState::Rejected ||
           s == OrderState::Filled ||
           s == OrderState::Cancelled ||
           s == OrderState::Expired ||
           s == OrderState::Reconciled ||
           s == OrderState::EscalatedToOperator;
}

// is_terminal(EscalatedToOperator) is true (no further automatic transition ever
// leaves it), but that does NOT mean the order is done: it may still be live on
// the exchange, awaiting operator resolution. Compaction/COID-release logic that
// uses is_terminal() as its retention filter can silently drop state an escalated
// order still needs — this exact bug is docs/SPEC_INVARIANTS.md's `is_exchange_final`
// entry (SUBMITPORT spec round 9 introduced this split; round 24 separately found
// Reconciled had been wrongly included here — it is not, and must not be, in this
// set). Use is_exchange_final(), never is_terminal(), for any decision about
// whether an order can still be resting on the exchange.
inline bool is_exchange_final(OrderState s) noexcept {
    return s == OrderState::Filled ||
           s == OrderState::Cancelled ||
           s == OrderState::Rejected ||
           s == OrderState::Expired;
}

inline TransitionResult validate_transition(OrderState from, OrderState to) noexcept {
    if (is_terminal(from)) return TransitionResult::AlreadyTerminal;

    switch (from) {
        case OrderState::Intent:
            if (to == OrderState::Submitting) return TransitionResult::Ok;
            break;
        case OrderState::Submitting:
            if (to == OrderState::Accepted ||
                to == OrderState::Rejected ||
                to == OrderState::Ambiguous ||
                to == OrderState::Filled)     // immediate fill
                return TransitionResult::Ok;
            break;
        case OrderState::Accepted:
            if (to == OrderState::PartialFill ||
                to == OrderState::Filled ||
                to == OrderState::CancelRequested ||
                to == OrderState::Expired)
                return TransitionResult::Ok;
            break;
        case OrderState::Ambiguous:
            // From Ambiguous, only reconciliation or escalation
            if (to == OrderState::Reconciled ||
                to == OrderState::EscalatedToOperator)
                return TransitionResult::Ok;
            break;
        case OrderState::PartialFill:
            if (to == OrderState::Filled ||
                to == OrderState::CancelRequested ||
                to == OrderState::Expired)
                return TransitionResult::Ok;
            break;
        case OrderState::CancelRequested:
            if (to == OrderState::Cancelled ||
                to == OrderState::Filled ||   // filled before cancel arrived
                to == OrderState::Expired)
                return TransitionResult::Ok;
            break;
        default:
            break;
    }

    return TransitionResult::InvalidTransition;
}

// --- Client order ID (idempotency key) ---

static constexpr std::size_t kClientOrderIdLen = 36;

struct ClientOrderId {
    char id[kClientOrderIdLen + 1]{};

    std::string_view view() const noexcept {
        return {id, std::strlen(id)};
    }

    bool empty() const noexcept { return id[0] == '\0'; }
};

// Generate a deterministic client order ID from components.
// Format: HY-{timestamp_ms}-{seq}-{symbol_hash4}
// This is NOT a UUID — it's deterministic so the same intent always
// produces the same ID (idempotency: retry with same ID = same order).
inline ClientOrderId make_client_order_id(
    std::int64_t timestamp_ms,
    std::uint32_t sequence,
    std::uint32_t symbol_id) noexcept {

    ClientOrderId coid{};
    std::snprintf(coid.id, sizeof(coid.id),
                  "HY-%lld-%u-%04X",
                  static_cast<long long>(timestamp_ms),
                  sequence,
                  symbol_id & 0xFFFF);
    return coid;
}

// --- Order record ---

struct OrderRecord {
    ClientOrderId client_order_id{};
    std::int64_t exchange_order_id{0};  // Binance orderId, 0 if unknown
    std::uint32_t symbol_id{0};
    OrderState state{OrderState::Intent};

    std::int64_t submit_timestamp_ms{0};
    std::int64_t last_update_ms{0};

    std::int64_t intended_price_ticks{0};
    std::int64_t intended_qty_ticks{0};
    std::int64_t filled_qty_ticks{0};
    std::int64_t avg_fill_price_ticks{0};

    std::uint8_t query_attempts{0};       // reconciliation query count
    static constexpr std::uint8_t kMaxQueryAttempts = 3;

    TransitionResult transition_to(OrderState next) noexcept {
        auto result = validate_transition(state, next);
        if (result == TransitionResult::Ok) {
            state = next;
        }
        return result;
    }

    bool should_escalate() const noexcept {
        return state == OrderState::Ambiguous &&
               query_attempts >= kMaxQueryAttempts;
    }
};

// --- Reconciliation action ---

enum class ReconcileAction : std::uint8_t {
    QueryOrder = 0,       // GET /api/v3/order with same clientOrderId
    EscalateToOperator = 1, // Max queries exhausted → human takeover
    NoAction = 2,         // Not in ambiguous state
};

inline ReconcileAction determine_reconcile_action(const OrderRecord& rec) noexcept {
    if (rec.state != OrderState::Ambiguous) {
        return ReconcileAction::NoAction;
    }
    if (rec.query_attempts >= OrderRecord::kMaxQueryAttempts) {
        return ReconcileAction::EscalateToOperator;
    }
    return ReconcileAction::QueryOrder;
}

// --- In-flight registry (idempotency / no-blind-retry guard, ADR-019 D8 M6) ---
//
// A client_order_id that has been submitted and is not yet resolved to a terminal
// state must NEVER be submitted a second time. On timeout/network error the order
// is Ambiguous, and recovery is query-by-same-id + reconciliation — not a fresh
// POST. This registry is the enforcement point: register_submit() fails if the id
// is already tracked, so the orchestrator can fail-closed instead of double-sending.
static constexpr std::size_t kMaxInFlight = 64;

class InFlightRegistry {
public:
    // Attempt to record a submit for `coid`. Returns false if this id is already
    // tracked (in-flight or unresolved) or if capacity is exhausted (fail-closed).
    bool register_submit(std::string_view coid) noexcept {
        if (coid.empty()) return false;
        if (find_index(coid) != kNotFound) return false;  // already in-flight
        for (std::size_t i = 0; i < kMaxInFlight; ++i) {
            if (!slots_[i].active) {
                auto len = coid.size() < kClientOrderIdLen ? coid.size() : kClientOrderIdLen;
                std::memcpy(slots_[i].id, coid.data(), len);
                slots_[i].id[len] = '\0';
                slots_[i].active = true;
                ++count_;
                return true;
            }
        }
        return false;  // capacity exhausted → fail-closed
    }

    bool is_in_flight(std::string_view coid) const noexcept {
        return find_index(coid) != kNotFound;
    }

    // Release a resolved (terminal) order so its slot can be reused.
    void mark_resolved(std::string_view coid) noexcept {
        auto idx = find_index(coid);
        if (idx != kNotFound) {
            slots_[idx].active = false;
            slots_[idx].id[0] = '\0';
            if (count_ > 0) --count_;
        }
    }

    std::size_t count() const noexcept { return count_; }

private:
    static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

    struct Slot {
        bool active{false};
        char id[kClientOrderIdLen + 1]{};
    };

    std::size_t find_index(std::string_view coid) const noexcept {
        if (coid.empty()) return kNotFound;
        for (std::size_t i = 0; i < kMaxInFlight; ++i) {
            if (slots_[i].active) {
                std::string_view sv(slots_[i].id, std::strlen(slots_[i].id));
                if (sv == coid) return i;
            }
        }
        return kNotFound;
    }

    std::array<Slot, kMaxInFlight> slots_{};
    std::size_t count_{0};
};

}  // namespace hy
