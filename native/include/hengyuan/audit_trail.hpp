// SPDX-License-Identifier: proprietary
// audit_trail.hpp — Append-only audit trail for live execution events.
//
// Governance: L1 (data structures + policy, no DB, no file I/O).
// ADR-019 D10:
//   ✅ Append-only event types (submit, fill, reject, kill, state change)
//   ✅ dry-run vs live event separation (tagged, not mixed)
//   ✅ Audit unavailable → fail-closed (no new orders without audit)
//   ✅ No secret in audit records
//   ✅ Serialization to fixed-size records for CSV/binary append

#pragma once

#include <hengyuan/order_lifecycle.hpp>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace hy {

// --- Audit event types ---

enum class AuditEventType : std::uint8_t {
    OrderIntentCreated = 0,
    OrderSubmitted = 1,
    OrderAccepted = 2,
    OrderRejected = 3,
    OrderAmbiguous = 4,
    OrderFilled = 5,
    OrderPartialFill = 6,
    OrderCancelRequested = 7,
    OrderCancelled = 8,
    OrderExpired = 9,
    OrderReconciled = 10,
    OrderEscalated = 11,
    KillSwitchTriggered = 12,
    KillSwitchRearmAttempt = 13,
    PreTradeRejected = 14,
    PreflightFailed = 15,
    AccountTruthRefreshed = 16,
    AccountTruthStale = 17,
    ClockSkewDetected = 18,
    RateLimitApproaching = 19,
    OrderSubmitPrepared = 20,
};

inline const char* audit_event_name(AuditEventType t) noexcept {
    switch (t) {
        case AuditEventType::OrderIntentCreated: return "ORDER_INTENT_CREATED";
        case AuditEventType::OrderSubmitted: return "ORDER_SUBMITTED";
        case AuditEventType::OrderAccepted: return "ORDER_ACCEPTED";
        case AuditEventType::OrderRejected: return "ORDER_REJECTED";
        case AuditEventType::OrderAmbiguous: return "ORDER_AMBIGUOUS";
        case AuditEventType::OrderFilled: return "ORDER_FILLED";
        case AuditEventType::OrderPartialFill: return "ORDER_PARTIAL_FILL";
        case AuditEventType::OrderCancelRequested: return "ORDER_CANCEL_REQUESTED";
        case AuditEventType::OrderCancelled: return "ORDER_CANCELLED";
        case AuditEventType::OrderExpired: return "ORDER_EXPIRED";
        case AuditEventType::OrderReconciled: return "ORDER_RECONCILED";
        case AuditEventType::OrderEscalated: return "ORDER_ESCALATED";
        case AuditEventType::KillSwitchTriggered: return "KILL_SWITCH_TRIGGERED";
        case AuditEventType::KillSwitchRearmAttempt: return "KILL_SWITCH_REARM_ATTEMPT";
        case AuditEventType::PreTradeRejected: return "PRE_TRADE_REJECTED";
        case AuditEventType::PreflightFailed: return "PREFLIGHT_FAILED";
        case AuditEventType::AccountTruthRefreshed: return "ACCOUNT_TRUTH_REFRESHED";
        case AuditEventType::AccountTruthStale: return "ACCOUNT_TRUTH_STALE";
        case AuditEventType::ClockSkewDetected: return "CLOCK_SKEW_DETECTED";
        case AuditEventType::RateLimitApproaching: return "RATE_LIMIT_APPROACHING";
        case AuditEventType::OrderSubmitPrepared: return "ORDER_SUBMIT_PREPARED";
    }
    return "UNKNOWN";
}

// --- Execution mode tag (dry-run vs live, never mixed) ---

enum class ExecutionMode : std::uint8_t {
    DryRun = 0,
    Live = 1,
};

// --- Audit record (fixed-size, append-only) ---

struct AuditRecord {
    std::int64_t timestamp_ms{0};
    AuditEventType event_type{};
    ExecutionMode mode{ExecutionMode::DryRun};
    std::uint32_t symbol_id{0};
    char client_order_id[kClientOrderIdLen + 1]{};
    std::int64_t exchange_order_id{0};
    std::int64_t price_ticks{0};
    std::int64_t qty_ticks{0};
    std::int64_t detail_code{0};  // binance error code, pre-trade check code, etc.
    char detail_msg[64]{};        // short sanitized message (no secrets)

    // Precise post-event order state and fill progress. Added for the durable
    // audit log's recovery_scan() (docs/SPEC_INVARIANTS.md's "durable 审计日志"
    // entry): without these, a replay can tell THAT an order was reconciled but
    // not WHICH of the six exchange-final states it actually landed in, nor how
    // much filled. event_type alone is not enough -- e.g. OrderReconciled is
    // emitted for Accepted/PartialFill/Filled/Cancelled/Rejected/Expired alike
    // (order_tracker.hpp's drain_reconcile_events()), so the specific outcome
    // would otherwise be lost the instant the record is written.
    OrderState resulting_state{OrderState::Intent};
    std::int64_t filled_qty_ticks{0};
    std::int64_t avg_fill_price_ticks{0};

    // TODO 1A.3 follow-up (PositionTruth): direction was never durably recorded
    // before this field -- durable_control_plane.hpp's OrderRecoveryCheckpoint
    // deliberately scoped it out ("Deliberately NOT chasing the spec text's
    // other proposed fields (side, ...)"), which meant a crash-recovered
    // OrderRecord silently defaulted to Buy for every order regardless of its
    // real side. Appended at the end, matching this struct's own convention
    // for resulting_state/filled_qty_ticks/avg_fill_price_ticks above (added
    // as a trailing block, not inserted earlier) -- the minimal diff, and
    // consistent with how durable_frame_codec.hpp encodes/decodes fields in
    // declaration order.
    OrderSide side{OrderSide::Buy};

    void set_client_order_id(std::string_view coid) noexcept {
        auto len = coid.size() < kClientOrderIdLen ? coid.size() : kClientOrderIdLen;
        std::memcpy(client_order_id, coid.data(), len);
        client_order_id[len] = '\0';
    }

    void set_detail(const char* msg) noexcept {
        std::strncpy(detail_msg, msg, sizeof(detail_msg) - 1);
        detail_msg[sizeof(detail_msg) - 1] = '\0';
    }
};

// --- Audit sink interface (append-only, fail-closed) ---

enum class AuditSinkStatus : std::uint8_t {
    Available = 0,
    Unavailable = 1,
};

// In-memory ring buffer audit sink for testing / first path.
// Production may replace with CSV file / DB writer.
static constexpr std::size_t kAuditRingCapacity = 1024;

class AuditRingSink {
public:
    // Fail-closed once the ring is at capacity: this in-memory sink is a
    // testing / first-path stand-in (see class comment above), not a
    // production append-only store. Silently overwriting the oldest record
    // to make room for a new one would let audit history for older orders
    // disappear while still reporting Available — a fail-open path that
    // violates ADR-019 D10 (audit unavailable -> no new orders). Once full,
    // this sink correctly reports Unavailable instead of wrapping.
    AuditSinkStatus status() const noexcept {
        if (!available_) return AuditSinkStatus::Unavailable;
        if (write_pos_ >= kAuditRingCapacity) return AuditSinkStatus::Unavailable;
        return AuditSinkStatus::Available;
    }

    bool append(const AuditRecord& rec) noexcept {
        if (status() != AuditSinkStatus::Available) return false;
        records_[write_pos_ % kAuditRingCapacity] = rec;
        ++write_pos_;
        return true;
    }

    bool is_full() const noexcept { return write_pos_ >= kAuditRingCapacity; }

    std::size_t count() const noexcept { return write_pos_; }

    const AuditRecord* last() const noexcept {
        if (write_pos_ == 0) return nullptr;
        return &records_[(write_pos_ - 1) % kAuditRingCapacity];
    }

    // write_pos_ never exceeds kAuditRingCapacity (append() refuses once full,
    // see status()/is_full()), so every index in [0, write_pos_) is still present
    // — nothing is ever silently overwritten.
    const AuditRecord* at(std::size_t index) const noexcept {
        if (index >= write_pos_) return nullptr;
        return &records_[index % kAuditRingCapacity];
    }

    void set_available(bool v) noexcept { available_ = v; }

    void clear() noexcept {
        write_pos_ = 0;
        available_ = true;
    }

private:
    bool available_{true};
    std::size_t write_pos_{0};
    std::array<AuditRecord, kAuditRingCapacity> records_{};
};

// --- Gate: audit unavailable → no new orders (ADR-019 D10) ---

inline bool can_submit_order(const AuditRingSink& sink) noexcept {
    return sink.status() == AuditSinkStatus::Available;
}

}  // namespace hy
