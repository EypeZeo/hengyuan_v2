// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/account_truth.hpp>
#include <hengyuan/operator_input_reader.hpp>

#include <cstdint>
#include <limits>

namespace hy {

enum class OperatorEnvironment : std::uint8_t { Testnet, Production };

// Account UID must come from an authenticated account response. The current
// fetch_account() parser discards uid, so no live caller can arm this gate yet.
struct OperatorOrderChallenge {
    // valid_until_ms and every now_ms passed to the gate use one monotonic
    // clock origin; an epoch timestamp is not interchangeable with them.
    std::uint64_t account_uid{0};
    OperatorEnvironment environment{static_cast<OperatorEnvironment>(255)};
    std::uint32_t symbol_id{0};
    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Limit};
    std::int64_t price_ticks{0};
    std::int64_t qty_ticks{0};
    std::int64_t max_notional{0};
    std::int64_t valid_until_ms{0};
};

enum class OperatorConfirmationState : std::uint8_t {
    Invalid,
    AwaitingInput,
    Confirmed,
    Denied,
    Expired,
    Consumed,
};

// Single-owner, one-shot control-plane gate. The caller keeps exclusive ownership
// of stdin; this class only receives a complete line or an input failure.
class OperatorConfirmationGate {
public:
    explicit OperatorConfirmationGate(const OperatorOrderChallenge& challenge,
                                      std::int64_t now_ms) noexcept {
        if (challenge.account_uid == 0 ||
            (challenge.environment != OperatorEnvironment::Testnet &&
             challenge.environment != OperatorEnvironment::Production) ||
            (challenge.side != OrderSide::Buy && challenge.side != OrderSide::Sell) ||
            challenge.type != OrderType::Limit ||
            now_ms < 0 || challenge.price_ticks <= 0 || challenge.qty_ticks <= 0 ||
            challenge.max_notional <= 0 || challenge.valid_until_ms < now_ms ||
            challenge.price_ticks > std::numeric_limits<std::int64_t>::max() / challenge.qty_ticks ||
            challenge.price_ticks * challenge.qty_ticks > challenge.max_notional) {
            return;
        }
        account_uid_ = challenge.account_uid;
        environment_ = challenge.environment;
        order_ = {challenge.symbol_id, challenge.side, challenge.type, challenge.price_ticks,
                  challenge.qty_ticks, challenge.max_notional, challenge.valid_until_ms};
        issued_at_ms_ = now_ms;
        state_ = OperatorConfirmationState::AwaitingInput;
    }

    OperatorConfirmationGate(const OperatorConfirmationGate&) = delete;
    OperatorConfirmationGate& operator=(const OperatorConfirmationGate&) = delete;

    [[nodiscard]] OperatorConfirmationState state() const noexcept { return state_; }

    // Timeout, EOF, I/O error, and any line other than the exact ASCII word
    // CONFIRM permanently deny this challenge. A second input never creates
    // a second authorization.
    [[nodiscard]] bool submit_input(const OperatorInputResult& input,
                                    std::int64_t now_ms) noexcept {
        if (state_ == OperatorConfirmationState::Confirmed) {
            state_ = OperatorConfirmationState::Denied;
            return false;
        }
        if (state_ != OperatorConfirmationState::AwaitingInput) return false;
        if (now_ms < issued_at_ms_) {
            state_ = OperatorConfirmationState::Denied;
            return false;
        }
        if (now_ms > order_.valid_until_ms) {
            state_ = OperatorConfirmationState::Expired;
            return false;
        }
        if (input.status != OperatorInputStatus::Line || input.size != 7 ||
            input.line() != "CONFIRM") {
            state_ = OperatorConfirmationState::Denied;
            return false;
        }
        state_ = OperatorConfirmationState::Confirmed;
        confirmed_at_ms_ = now_ms;
        return true;
    }

    // account_uid is the independently obtained identity for the order being
    // authorized, not the operator's own text input. Mismatch consumes the
    // challenge and fails closed. This pure gate deliberately does not return
    // an OrchestratorContext/OrderConfirmation: that runtime bridge must also
    // bind the verified account identity before it can authorize a live send.
    [[nodiscard]] bool consume(
        std::uint64_t account_uid, OperatorEnvironment environment,
        std::uint32_t symbol_id, OrderSide side,
        OrderType type, std::int64_t price_ticks, std::int64_t qty_ticks,
        std::int64_t max_notional,
        std::int64_t now_ms) noexcept {
        if (state_ != OperatorConfirmationState::Confirmed) return false;
        if (now_ms < confirmed_at_ms_) {
            state_ = OperatorConfirmationState::Denied;
            return false;
        }
        if (now_ms > order_.valid_until_ms) {
            state_ = OperatorConfirmationState::Expired;
            return false;
        }
        if (environment != environment_ || account_uid != account_uid_ ||
            symbol_id != order_.symbol_id || side != order_.side || type != order_.type ||
            price_ticks != order_.price_ticks || qty_ticks != order_.qty_ticks ||
            max_notional != order_.max_notional) {
            state_ = OperatorConfirmationState::Denied;
            return false;
        }
        state_ = OperatorConfirmationState::Consumed;
        return true;
    }

private:
    struct StoredOrder {
        std::uint32_t symbol_id{0};
        OrderSide side{OrderSide::Buy};
        OrderType type{OrderType::Limit};
        std::int64_t price_ticks{0};
        std::int64_t qty_ticks{0};
        std::int64_t max_notional{0};
        std::int64_t valid_until_ms{0};
    };
    std::uint64_t account_uid_{0};
    OperatorEnvironment environment_{static_cast<OperatorEnvironment>(255)};
    StoredOrder order_{};
    std::int64_t issued_at_ms_{0};
    std::int64_t confirmed_at_ms_{0};
    OperatorConfirmationState state_{OperatorConfirmationState::Invalid};
};

}  // namespace hy
