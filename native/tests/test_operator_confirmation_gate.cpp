// SPDX-License-Identifier: proprietary
#include <hengyuan/operator_confirmation_gate.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <limits>

namespace hy {
namespace {

OperatorOrderChallenge challenge() {
    return {12345, OperatorEnvironment::Testnet, 7, OrderSide::Buy, OrderType::Limit,
            100, 2, 200, 200};
}

OperatorInputResult line(const char* text) {
    OperatorInputResult result{};
    result.status = OperatorInputStatus::Line;
    result.size = std::strlen(text);
    if (result.size > result.bytes.size()) return {OperatorInputStatus::TooLong};
    std::memcpy(result.bytes.data(), text, result.size);
    return result;
}

TEST(OperatorConfirmationGate, ExactAccountAndOrderMayBeConsumedOnce) {
    OperatorConfirmationGate gate(challenge(), 100);
    ASSERT_EQ(gate.state(), OperatorConfirmationState::AwaitingInput);
    ASSERT_TRUE(gate.submit_input(line("CONFIRM"), 100));

    ASSERT_TRUE(gate.consume(12345, OperatorEnvironment::Testnet, 7, OrderSide::Buy,
                             OrderType::Limit, 100, 2, 200, 150));
    EXPECT_EQ(gate.state(), OperatorConfirmationState::Consumed);
    EXPECT_FALSE(gate.consume(12345, OperatorEnvironment::Testnet, 7, OrderSide::Buy,
                              OrderType::Limit, 100, 2, 200, 150));
    EXPECT_FALSE(gate.submit_input(line("CONFIRM"), 150));
}

TEST(OperatorConfirmationGate, WrongAccountOrOrderPermanentlyDenies) {
    for (int mutation = 0; mutation < 7; ++mutation) {
        OperatorConfirmationGate gate(challenge(), 100);
        ASSERT_TRUE(gate.submit_input(line("CONFIRM"), 100));
        const auto confirmation = gate.consume(
            mutation == 0 ? 12346u : 12345u,
            mutation == 5 ? OperatorEnvironment::Production : OperatorEnvironment::Testnet,
            mutation == 1 ? 8u : 7u,
            mutation == 2 ? OrderSide::Sell : OrderSide::Buy,
            mutation == 3 ? static_cast<OrderType>(1) : OrderType::Limit,
            mutation == 4 ? 101 : 100, 2, mutation == 6 ? 201 : 200, 150);
        EXPECT_FALSE(confirmation) << mutation;
        EXPECT_EQ(gate.state(), OperatorConfirmationState::Denied) << mutation;
        EXPECT_FALSE(gate.consume(12345, OperatorEnvironment::Testnet, 7, OrderSide::Buy,
                                  OrderType::Limit, 100, 2, 200, 150));
    }
}

TEST(OperatorConfirmationGate, ExpiredAndDuplicateInputFailClosed) {
    OperatorConfirmationGate expired(challenge(), 100);
    EXPECT_FALSE(expired.submit_input(line("CONFIRM"), 201));
    EXPECT_EQ(expired.state(), OperatorConfirmationState::Expired);

    OperatorConfirmationGate expired_after_input(challenge(), 100);
    ASSERT_TRUE(expired_after_input.submit_input(line("CONFIRM"), 100));
    EXPECT_FALSE(expired_after_input.consume(12345, OperatorEnvironment::Testnet, 7, OrderSide::Buy,
                                             OrderType::Limit, 100, 2, 200, 201));
    EXPECT_EQ(expired_after_input.state(), OperatorConfirmationState::Expired);

    OperatorConfirmationGate repeated(challenge(), 100);
    ASSERT_TRUE(repeated.submit_input(line("CONFIRM"), 100));
    EXPECT_FALSE(repeated.submit_input(line("CONFIRM"), 101));
    EXPECT_EQ(repeated.state(), OperatorConfirmationState::Denied);
    EXPECT_FALSE(repeated.consume(12345, OperatorEnvironment::Testnet, 7, OrderSide::Buy,
                                  OrderType::Limit, 100, 2, 200, 101));

    OperatorConfirmationGate backward_before_input(challenge(), 100);
    EXPECT_FALSE(backward_before_input.submit_input(line("CONFIRM"), 99));
    EXPECT_EQ(backward_before_input.state(), OperatorConfirmationState::Denied);

    OperatorConfirmationGate backward_after_input(challenge(), 100);
    ASSERT_TRUE(backward_after_input.submit_input(line("CONFIRM"), 150));
    EXPECT_FALSE(backward_after_input.consume(12345, OperatorEnvironment::Testnet, 7, OrderSide::Buy,
                                              OrderType::Limit, 100, 2, 200, 149));
    EXPECT_EQ(backward_after_input.state(), OperatorConfirmationState::Denied);
}

TEST(OperatorConfirmationGate, InputFailureAndWrongWordCannotAuthorize) {
    for (OperatorInputStatus status : {OperatorInputStatus::Timeout,
                                       OperatorInputStatus::EndOfInput,
                                       OperatorInputStatus::Error,
                                       OperatorInputStatus::TooLong}) {
        OperatorConfirmationGate gate(challenge(), 100);
        OperatorInputResult result{};
        result.status = status;
        EXPECT_FALSE(gate.submit_input(result, 100));
        EXPECT_EQ(gate.state(), OperatorConfirmationState::Denied);
    }
    OperatorConfirmationGate wrong_word(challenge(), 100);
    EXPECT_FALSE(wrong_word.submit_input(line("confirm"), 100));
    EXPECT_EQ(wrong_word.state(), OperatorConfirmationState::Denied);

    OperatorConfirmationGate forged_length(challenge(), 100);
    auto malformed = line("CONFIRM");
    malformed.size = 8;
    EXPECT_FALSE(forged_length.submit_input(malformed, 100));
    EXPECT_EQ(forged_length.state(), OperatorConfirmationState::Denied);
}

TEST(OperatorConfirmationGate, InvalidChallengeNeverPrompts) {
    auto bad = challenge();
    bad.account_uid = 0;
    EXPECT_EQ(OperatorConfirmationGate(bad, 100).state(), OperatorConfirmationState::Invalid);
    bad = challenge();
    bad.max_notional = 199;
    EXPECT_EQ(OperatorConfirmationGate(bad, 100).state(), OperatorConfirmationState::Invalid);
    bad = challenge();
    bad.price_ticks = std::numeric_limits<std::int64_t>::max();
    EXPECT_EQ(OperatorConfirmationGate(bad, 100).state(), OperatorConfirmationState::Invalid);
    bad = challenge();
    bad.type = static_cast<OrderType>(1);
    EXPECT_EQ(OperatorConfirmationGate(bad, 100).state(), OperatorConfirmationState::Invalid);
    bad = challenge();
    bad.environment = static_cast<OperatorEnvironment>(255);
    EXPECT_EQ(OperatorConfirmationGate(bad, 100).state(), OperatorConfirmationState::Invalid);
    bad = challenge();
    bad.valid_until_ms = 99;
    EXPECT_EQ(OperatorConfirmationGate(bad, 100).state(), OperatorConfirmationState::Invalid);
    EXPECT_EQ(OperatorConfirmationGate(challenge(), -1).state(), OperatorConfirmationState::Invalid);
}

}  // namespace
}  // namespace hy
