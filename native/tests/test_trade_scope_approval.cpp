// SPDX-License-Identifier: proprietary
// Trade-scope grant signature, binding, encoding, and budget regressions.
#include <gtest/gtest.h>
#include <hengyuan/owner_trade_scope_approval.hpp>
#include <hengyuan/trade_scope_budget.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <openssl/x509.h>

namespace {

constexpr std::int64_t kExchangeUpperMs = 1'000'000;

struct P256Fixture {
    EVP_PKEY* key{nullptr};
    std::vector<unsigned char> public_der;

    P256Fixture() {
        key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1");
        if (!key) return;
        const int size = i2d_PUBKEY(key, nullptr);
        if (size <= 0) return;
        public_der.resize(static_cast<std::size_t>(size));
        unsigned char* cursor = public_der.data();
        if (i2d_PUBKEY(key, &cursor) != size) public_der.clear();
    }

    ~P256Fixture() { EVP_PKEY_free(key); }
    P256Fixture(const P256Fixture&) = delete;
    P256Fixture& operator=(const P256Fixture&) = delete;

    std::vector<unsigned char> sign(std::span<const std::byte> payload) const {
        std::vector<unsigned char> signature;
        if (!key) return signature;
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        if (!context) return signature;
        if (EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key) != 1 ||
            EVP_DigestSignUpdate(context, payload.data(), payload.size()) != 1) {
            EVP_MD_CTX_free(context);
            return signature;
        }
        std::size_t size = 0;
        if (EVP_DigestSignFinal(context, nullptr, &size) != 1) {
            EVP_MD_CTX_free(context);
            return signature;
        }
        signature.resize(size);
        if (EVP_DigestSignFinal(context, signature.data(), &size) != 1) signature.clear();
        else signature.resize(size);
        EVP_MD_CTX_free(context);
        return signature;
    }
};

hy::TradeScopeGrant make_grant() {
    hy::TradeScopeGrant grant{};
    grant.binding.store = {hy::StoreEnvironment::Testnet, 71, 0x1122334455667788ULL,
                           0x8877665544332211ULL};
    for (std::size_t i = 0; i < grant.binding.activation.size(); ++i) {
        grant.binding.activation[i] = static_cast<std::byte>(i + 1);
    }
    grant.binding.scope_epoch.value = 0x0102030405060708ULL;
    grant.binding.policy_version = 0x1112131415161718ULL;
    for (std::size_t i = 0; i < grant.binding.strategy_version.size(); ++i) {
        grant.binding.strategy_version[i] = static_cast<std::byte>(i + 17);
    }
    grant.binding.key_id = 0x2122232425262728ULL;
    grant.serial = 0x3132333435363738ULL;
    grant.issued_at_ms = 900'000;
    grant.not_after_ms = 1'030'000;
    constexpr std::string_view symbol = "BTCUSDT";
    std::copy(symbol.begin(), symbol.end(), grant.symbol.begin());
    grant.sides = hy::TradeScopeSides::Buy;
    grant.limits = {100'000, 250'000, 200'000, 0x1234};
    return grant;
}

hy::TradeScopePolicy policy_for(const hy::TradeScopeGrant& grant) {
    return {grant.limits, 60'000};
}

std::optional<hy::VerifiedTradeScope> verify_payload(
    std::span<const std::byte> payload, std::span<const unsigned char> signature,
    const P256Fixture& signer, const hy::TradeScopeBinding& expected,
    const hy::TradeScopePolicy& policy, std::optional<std::int64_t> exchange_upper_ms) {
    return hy::VerifiedTradeScope::verify(
        payload, signature,
        std::span<const unsigned char>(signer.public_der.data(), signer.public_der.size()),
        expected, policy, exchange_upper_ms);
}

std::optional<hy::VerifiedTradeScope> verify_grant(
    const P256Fixture& signer, const hy::TradeScopeGrant& grant,
    const hy::TradeScopeBinding& expected, const hy::TradeScopePolicy& policy,
    std::optional<std::int64_t> exchange_upper_ms = kExchangeUpperMs) {
    const auto payload = hy::encode_trade_scope_grant(grant);
    if (!payload) return std::nullopt;
    const auto signature = signer.sign(std::span<const std::byte>(*payload));
    return verify_payload(*payload, std::span<const unsigned char>(signature.data(), signature.size()),
                          signer, expected, policy, exchange_upper_ms);
}

void expect_u64_le(const hy::TradeScopeGrantBytes& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        EXPECT_EQ(std::to_integer<unsigned int>(bytes[offset + i]),
                  static_cast<unsigned int>((value >> (8 * i)) & 0xffU));
    }
}

void expect_u32_le(const hy::TradeScopeGrantBytes& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        EXPECT_EQ(std::to_integer<unsigned int>(bytes[offset + i]),
                  static_cast<unsigned int>((value >> (8 * i)) & 0xffU));
    }
}

hy::AuditRecord prepared_record(std::string_view order_id) {
    hy::AuditRecord record{};
    record.event_type = hy::AuditEventType::OrderSubmitPrepared;
    record.resulting_state = hy::OrderState::Submitting;
    record.set_client_order_id(order_id);
    return record;
}

hy::AuditRecord never_sent_record(std::string_view order_id) {
    hy::AuditRecord record{};
    record.event_type = hy::AuditEventType::OrderNeverSent;
    record.resulting_state = hy::OrderState::NeverSent;
    record.set_client_order_id(order_id);
    return record;
}

hy::AuditAppendResult acked_append() {
    return {hy::AuditAppendResult::Status::Acked, 17};
}

hy::ClosureAppendResult appended_closure(hy::AuditAppendResult append = acked_append()) {
    return {hy::ClosureAppendStatus::Appended, append};
}

std::optional<hy::TradeScopeBudget> empty_budget(const hy::VerifiedTradeScope& scope) {
    return hy::TradeScopeBudget::rebuild(scope, std::span<const hy::TradeScopeBudgetRecord>{});
}

}  // namespace

TEST(TradeScopeGrant, CanonicalBytesUseFixedOffsetsAndRoundTrip) {
    const auto grant = make_grant();
    const auto encoded = hy::encode_trade_scope_grant(grant);
    ASSERT_TRUE(encoded.has_value());

    constexpr std::size_t kDomainSize = sizeof(hy::kTradeScopeGrantDomain);
    constexpr std::size_t kActivationOffset = 42;
    constexpr std::size_t kStrategyOffset = 74;
    constexpr std::size_t kSymbolOffset = 138;
    constexpr std::size_t kSideOffset = 158;
    constexpr std::size_t kOrderTypeOffset = 159;
    constexpr std::size_t kMaxOrderOffset = 160;
    constexpr std::size_t kBudgetOffset = 168;
    constexpr std::size_t kExposureOffset = 176;
    constexpr std::size_t kPriceDeviationOffset = 184;

    ASSERT_EQ(encoded->size(), 188U);
    EXPECT_EQ(std::memcmp(encoded->data(), hy::kTradeScopeGrantDomain, kDomainSize), 0);
    EXPECT_EQ(std::to_integer<unsigned int>((*encoded)[15]), hy::kTradeScopeGrantVersion);
    EXPECT_EQ(std::to_integer<unsigned int>((*encoded)[16]), hy::kTradeScopeP256);
    EXPECT_EQ(std::to_integer<unsigned int>((*encoded)[17]),
              static_cast<unsigned int>(grant.binding.store.environment));
    expect_u64_le(*encoded, 18, grant.binding.store.account_uid);
    expect_u64_le(*encoded, 26, grant.binding.store.store_uuid_lo);
    expect_u64_le(*encoded, 34, grant.binding.store.store_uuid_hi);
    EXPECT_EQ((*encoded)[kActivationOffset], grant.binding.activation.front());
    EXPECT_EQ((*encoded)[kActivationOffset + 15], grant.binding.activation.back());
    expect_u64_le(*encoded, 58, grant.binding.scope_epoch.value);
    expect_u64_le(*encoded, 66, grant.binding.policy_version);
    EXPECT_EQ((*encoded)[kStrategyOffset], grant.binding.strategy_version.front());
    EXPECT_EQ((*encoded)[kStrategyOffset + 31], grant.binding.strategy_version.back());
    expect_u64_le(*encoded, 106, grant.binding.key_id);
    expect_u64_le(*encoded, 114, grant.serial);
    expect_u64_le(*encoded, 122, static_cast<std::uint64_t>(grant.issued_at_ms));
    expect_u64_le(*encoded, 130, static_cast<std::uint64_t>(grant.not_after_ms));
    EXPECT_EQ(std::to_integer<unsigned int>((*encoded)[kSymbolOffset]),
              static_cast<unsigned int>('B'));
    EXPECT_EQ(std::to_integer<unsigned int>((*encoded)[kSymbolOffset + 6]),
              static_cast<unsigned int>('T'));
    EXPECT_EQ((*encoded)[kSymbolOffset + 7], std::byte{0});
    EXPECT_EQ(std::to_integer<unsigned int>((*encoded)[kSideOffset]),
              static_cast<unsigned int>(hy::TradeScopeSides::Buy));
    EXPECT_EQ(std::to_integer<unsigned int>((*encoded)[kOrderTypeOffset]), 0U);
    expect_u64_le(*encoded, kMaxOrderOffset, static_cast<std::uint64_t>(grant.limits.max_order_notional));
    expect_u64_le(*encoded, kBudgetOffset, static_cast<std::uint64_t>(grant.limits.budget_notional));
    expect_u64_le(*encoded, kExposureOffset, static_cast<std::uint64_t>(grant.limits.max_exposure_notional));
    expect_u32_le(*encoded, kPriceDeviationOffset, grant.limits.max_price_deviation_bps);

    const auto decoded = hy::decode_trade_scope_grant(std::span<const std::byte>(*encoded));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, grant);
}

TEST(TradeScopeGrant, RejectsMalformedPayloadsAndSignatureTampering) {
    P256Fixture signer;
    ASSERT_NE(signer.key, nullptr);
    ASSERT_FALSE(signer.public_der.empty());
    const auto grant = make_grant();
    const auto encoded = hy::encode_trade_scope_grant(grant);
    ASSERT_TRUE(encoded.has_value());
    const auto signature = signer.sign(std::span<const std::byte>(*encoded));
    ASSERT_FALSE(signature.empty());
    const auto policy = policy_for(grant);

    std::vector<std::byte> short_payload(encoded->begin(), encoded->end() - 1);
    EXPECT_FALSE(hy::decode_trade_scope_grant(std::span<const std::byte>(short_payload)).has_value());
    std::array<std::byte, hy::TradeScopeGrantBytes{}.size() + 1> long_payload{};
    std::copy(encoded->begin(), encoded->end(), long_payload.begin());
    EXPECT_FALSE(hy::decode_trade_scope_grant(std::span<const std::byte>(long_payload)).has_value());

    const auto rejects = [&](std::size_t offset, std::byte value) {
        SCOPED_TRACE(offset);
        auto malformed = *encoded;
        malformed[offset] = value;
        EXPECT_FALSE(hy::decode_trade_scope_grant(std::span<const std::byte>(malformed)).has_value());
    };
    rejects(0, std::byte{0});                    // domain
    rejects(15, std::byte{2});                   // version
    rejects(16, std::byte{2});                   // signature algorithm
    rejects(17, std::byte{0xff});                // environment
    rejects(158, std::byte{0});                  // permission
    rejects(159, std::byte{0xff});               // order type
    rejects(146, std::byte{'X'});                // nonzero symbol padding
    rejects(186, std::byte{1});                  // high byte of u32 deviation

    auto changed_signature = signature;
    changed_signature.back() ^= 1U;
    EXPECT_FALSE(verify_payload(
        std::span<const std::byte>(*encoded),
        std::span<const unsigned char>(changed_signature.data(), changed_signature.size()),
        signer, grant.binding, policy, kExchangeUpperMs).has_value());

    auto changed_permission = *encoded;
    changed_permission[158] = static_cast<std::byte>(static_cast<std::uint8_t>(hy::TradeScopeSides::Sell));
    EXPECT_FALSE(verify_payload(
        std::span<const std::byte>(changed_permission),
        std::span<const unsigned char>(signature.data(), signature.size()),
        signer, grant.binding, policy, kExchangeUpperMs).has_value());
}

TEST(TradeScopeGrant, VerificationBindsExecutionScopeAndPermissions) {
    P256Fixture signer;
    ASSERT_NE(signer.key, nullptr);
    const auto grant = make_grant();
    const auto encoded = hy::encode_trade_scope_grant(grant);
    ASSERT_TRUE(encoded.has_value());
    const auto signature = signer.sign(std::span<const std::byte>(*encoded));
    ASSERT_FALSE(signature.empty());
    const auto policy = policy_for(grant);

    const auto verified = verify_payload(
        std::span<const std::byte>(*encoded),
        std::span<const unsigned char>(signature.data(), signature.size()),
        signer, grant.binding, policy, kExchangeUpperMs);
    ASSERT_TRUE(verified.has_value());
    EXPECT_EQ(verified->grant().sides, hy::TradeScopeSides::Buy);

    const auto rejects_binding = [&](hy::TradeScopeBinding expected) {
        EXPECT_FALSE(verify_payload(
            std::span<const std::byte>(*encoded),
            std::span<const unsigned char>(signature.data(), signature.size()),
            signer, expected, policy, kExchangeUpperMs).has_value());
    };

    auto wrong = grant.binding;
    wrong.store.environment = hy::StoreEnvironment::Production;
    rejects_binding(wrong);
    wrong = grant.binding;
    wrong.activation[0] ^= std::byte{0x80};
    rejects_binding(wrong);
    wrong = grant.binding;
    ++wrong.policy_version;
    rejects_binding(wrong);
    wrong = grant.binding;
    ++wrong.scope_epoch.value;
    rejects_binding(wrong);
    wrong = grant.binding;
    wrong.strategy_version[31] ^= std::byte{0x01};
    rejects_binding(wrong);
    wrong = grant.binding;
    ++wrong.key_id;
    rejects_binding(wrong);
    wrong = grant.binding;
    ++wrong.store.account_uid;
    rejects_binding(wrong);
}

TEST(TradeScopeGrant, EnforcesAbsoluteExpiryValidityAndPolicyLimits) {
    P256Fixture signer;
    ASSERT_NE(signer.key, nullptr);
    auto grant = make_grant();
    auto policy = policy_for(grant);
    policy.limits = {200'000, 400'000, 300'000, 500};
    policy.max_validity_ms = 60'000;

    grant.not_after_ms = kExchangeUpperMs + policy.max_validity_ms;
    grant.limits = policy.limits;
    EXPECT_TRUE(verify_grant(signer, grant, grant.binding, policy).has_value());

    auto expired = grant;
    expired.not_after_ms = kExchangeUpperMs;
    EXPECT_FALSE(verify_grant(signer, expired, expired.binding, policy).has_value());
    auto beyond_validity = grant;
    ++beyond_validity.not_after_ms;
    EXPECT_FALSE(verify_grant(signer, beyond_validity, beyond_validity.binding, policy).has_value());
    EXPECT_FALSE(verify_grant(signer, grant, grant.binding, policy, std::nullopt).has_value());

    auto over_limit = grant;
    ++over_limit.limits.max_order_notional;
    EXPECT_FALSE(verify_grant(signer, over_limit, over_limit.binding, policy).has_value());
    over_limit = grant;
    ++over_limit.limits.budget_notional;
    EXPECT_FALSE(verify_grant(signer, over_limit, over_limit.binding, policy).has_value());
    over_limit = grant;
    ++over_limit.limits.max_exposure_notional;
    EXPECT_FALSE(verify_grant(signer, over_limit, over_limit.binding, policy).has_value());
    over_limit = grant;
    ++over_limit.limits.max_price_deviation_bps;
    EXPECT_FALSE(verify_grant(signer, over_limit, over_limit.binding, policy).has_value());
}

TEST(TradeScopeBudget, ReservesAndReleasesOnlyUnpreparedOrders) {
    P256Fixture signer;
    ASSERT_NE(signer.key, nullptr);
    const auto grant = make_grant();
    const auto scope = verify_grant(signer, grant, grant.binding, policy_for(grant));
    ASSERT_TRUE(scope.has_value());
    auto budget = empty_budget(*scope);
    ASSERT_TRUE(budget.has_value());

    EXPECT_TRUE(budget->reserve("order-a", 60'000));
    EXPECT_EQ(budget->used_notional(), 60'000);
    EXPECT_EQ(budget->available_notional(), 190'000);
    EXPECT_FALSE(budget->reserve("order-a", 1));
    EXPECT_FALSE(budget->reserve("", 1));
    EXPECT_FALSE(budget->reserve("bad id", 1));
    EXPECT_FALSE(budget->reserve("zero", 0));
    EXPECT_FALSE(budget->reserve("negative", -1));
    EXPECT_FALSE(budget->reserve("too-large", grant.limits.max_order_notional + 1));
    EXPECT_FALSE(budget->release_unprepared("missing"));

    EXPECT_TRUE(budget->release_unprepared("order-a"));
    EXPECT_EQ(budget->used_notional(), 0);
    EXPECT_EQ(budget->available_notional(), grant.limits.budget_notional);
    EXPECT_FALSE(budget->release_unprepared("order-a"));
    EXPECT_TRUE(budget->reserve("order-a", 60'000));
    EXPECT_TRUE(budget->begin_preparation("order-a"));
    EXPECT_FALSE(budget->release_unprepared("order-a"));

    auto moved = std::move(*budget);
    EXPECT_EQ(moved.used_notional(), 60'000);
    EXPECT_EQ(moved.available_notional(), 190'000);
    EXPECT_FALSE(budget->reserve("after-move", 1));
}

TEST(TradeScopeBudget, PreparedAndUncertainClosuresKeepReservation) {
    P256Fixture signer;
    ASSERT_NE(signer.key, nullptr);
    const auto grant = make_grant();
    const auto scope = verify_grant(signer, grant, grant.binding, policy_for(grant));
    ASSERT_TRUE(scope.has_value());

    auto uncertain_prepared = empty_budget(*scope);
    ASSERT_TRUE(uncertain_prepared.has_value());
    ASSERT_TRUE(uncertain_prepared->reserve("prepared-unknown", 40'000));
    ASSERT_TRUE(uncertain_prepared->begin_preparation("prepared-unknown"));
    EXPECT_FALSE(uncertain_prepared->acknowledge_prepared(
        prepared_record("prepared-unknown"),
        {hy::AuditAppendResult::Status::Failed, 17}));
    EXPECT_TRUE(uncertain_prepared->rebuild_required());
    EXPECT_EQ(uncertain_prepared->used_notional(), 40'000);
    EXPECT_EQ(uncertain_prepared->available_notional(), 0);
    EXPECT_FALSE(uncertain_prepared->release_unprepared("prepared-unknown"));

    auto confirmed = empty_budget(*scope);
    ASSERT_TRUE(confirmed.has_value());
    ASSERT_TRUE(confirmed->reserve("never-sent", 30'000));
    ASSERT_TRUE(confirmed->begin_preparation("never-sent"));
    ASSERT_TRUE(confirmed->acknowledge_prepared(prepared_record("never-sent"), acked_append()));
    EXPECT_FALSE(confirmed->release_unprepared("never-sent"));

    const auto valid_closure = appended_closure();
    EXPECT_FALSE(confirmed->acknowledge_never_sent(never_sent_record("unknown-order"), valid_closure));
    auto invalid_closure_record = never_sent_record("never-sent");
    invalid_closure_record.exchange_order_id = 19;
    EXPECT_FALSE(confirmed->acknowledge_never_sent(invalid_closure_record, valid_closure));
    invalid_closure_record = never_sent_record("never-sent");
    invalid_closure_record.event_type = hy::AuditEventType::OrderSubmitted;
    EXPECT_FALSE(confirmed->acknowledge_never_sent(invalid_closure_record, valid_closure));
    invalid_closure_record = never_sent_record("never-sent");
    invalid_closure_record.resulting_state = hy::OrderState::Submitting;
    EXPECT_FALSE(confirmed->acknowledge_never_sent(invalid_closure_record, valid_closure));
    invalid_closure_record = never_sent_record("never-sent");
    invalid_closure_record.filled_qty_ticks = 1;
    EXPECT_FALSE(confirmed->acknowledge_never_sent(invalid_closure_record, valid_closure));
    EXPECT_FALSE(confirmed->acknowledge_never_sent(
        never_sent_record("never-sent"), {hy::ClosureAppendStatus::Refused, {}}));
    EXPECT_EQ(confirmed->used_notional(), 30'000);

    EXPECT_TRUE(confirmed->acknowledge_never_sent(never_sent_record("never-sent"), valid_closure));
    EXPECT_EQ(confirmed->used_notional(), 0);
    EXPECT_EQ(confirmed->available_notional(), grant.limits.budget_notional);
    EXPECT_FALSE(confirmed->acknowledge_never_sent(never_sent_record("never-sent"), valid_closure));

    auto uncertain_closure = empty_budget(*scope);
    ASSERT_TRUE(uncertain_closure.has_value());
    ASSERT_TRUE(uncertain_closure->reserve("closure-unknown", 20'000));
    ASSERT_TRUE(uncertain_closure->begin_preparation("closure-unknown"));
    ASSERT_TRUE(uncertain_closure->acknowledge_prepared(prepared_record("closure-unknown"), acked_append()));
    EXPECT_FALSE(uncertain_closure->acknowledge_never_sent(
        never_sent_record("closure-unknown"),
        appended_closure({hy::AuditAppendResult::Status::Failed, 23})));
    EXPECT_TRUE(uncertain_closure->rebuild_required());
    EXPECT_EQ(uncertain_closure->used_notional(), 20'000);
    EXPECT_EQ(uncertain_closure->available_notional(), 0);
}

TEST(TradeScopeBudget, ReplayKeepsUnknownOrdersAndRejectsDuplicateEvents) {
    P256Fixture signer;
    ASSERT_NE(signer.key, nullptr);
    const auto grant = make_grant();
    const auto scope = verify_grant(signer, grant, grant.binding, policy_for(grant));
    ASSERT_TRUE(scope.has_value());

    const std::array unresolved_records{
        hy::TradeScopeBudgetRecord{hy::TradeScopeBudgetEvent::Prepared, grant.serial, "order-a", 40'000},
        hy::TradeScopeBudgetRecord{hy::TradeScopeBudgetEvent::Prepared, grant.serial, "order-b", 20'000},
    };
    auto rebuilt = hy::TradeScopeBudget::rebuild(*scope, unresolved_records);
    ASSERT_TRUE(rebuilt.has_value());
    EXPECT_EQ(rebuilt->used_notional(), 60'000);
    EXPECT_EQ(rebuilt->available_notional(), grant.limits.budget_notional - 60'000);
    EXPECT_FALSE(rebuilt->begin_preparation("order-a"));
    EXPECT_FALSE(rebuilt->release_unprepared("order-a"));

    const std::array wrong_serial{
        hy::TradeScopeBudgetRecord{hy::TradeScopeBudgetEvent::Prepared, grant.serial + 1, "order-a", 10'000},
    };
    EXPECT_FALSE(hy::TradeScopeBudget::rebuild(*scope, wrong_serial).has_value());

    const std::array duplicate_charge{
        hy::TradeScopeBudgetRecord{hy::TradeScopeBudgetEvent::Prepared, grant.serial, "order-a", 10'000},
        hy::TradeScopeBudgetRecord{hy::TradeScopeBudgetEvent::Prepared, grant.serial, "order-a", 10'000},
    };
    EXPECT_FALSE(hy::TradeScopeBudget::rebuild(*scope, duplicate_charge).has_value());

    const std::array duplicate_refund{
        hy::TradeScopeBudgetRecord{hy::TradeScopeBudgetEvent::Prepared, grant.serial, "order-a", 10'000},
        hy::TradeScopeBudgetRecord{hy::TradeScopeBudgetEvent::Refund, grant.serial, "order-a", 0},
        hy::TradeScopeBudgetRecord{hy::TradeScopeBudgetEvent::Refund, grant.serial, "order-a", 0},
    };
    EXPECT_FALSE(hy::TradeScopeBudget::rebuild(*scope, duplicate_refund).has_value());

    const std::array unknown_refund{
        hy::TradeScopeBudgetRecord{hy::TradeScopeBudgetEvent::Refund, grant.serial, "unknown", 0},
    };
    EXPECT_FALSE(hy::TradeScopeBudget::rebuild(*scope, unknown_refund).has_value());

    const std::array resolved_records{
        hy::TradeScopeBudgetRecord{hy::TradeScopeBudgetEvent::Prepared, grant.serial, "order-a", 40'000},
        hy::TradeScopeBudgetRecord{hy::TradeScopeBudgetEvent::Refund, grant.serial, "order-a", 0},
    };
    auto resolved = hy::TradeScopeBudget::rebuild(*scope, resolved_records);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->used_notional(), 0);
    EXPECT_EQ(resolved->available_notional(), grant.limits.budget_notional);
}

TEST(TradeScopeBudget, AcceptsIntegerBoundaryWithoutOverflow) {
    P256Fixture signer;
    ASSERT_NE(signer.key, nullptr);
    auto grant = make_grant();
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    grant.limits = {kMax, kMax, kMax, 10'000};
    const auto scope = verify_grant(signer, grant, grant.binding, policy_for(grant));
    ASSERT_TRUE(scope.has_value());

    auto budget = empty_budget(*scope);
    ASSERT_TRUE(budget.has_value());
    EXPECT_TRUE(budget->reserve("max-order", kMax));
    EXPECT_EQ(budget->used_notional(), kMax);
    EXPECT_EQ(budget->available_notional(), 0);
    EXPECT_FALSE(budget->reserve("one-more", 1));
    EXPECT_TRUE(budget->release_unprepared("max-order"));
    EXPECT_EQ(budget->used_notional(), 0);
    EXPECT_EQ(budget->available_notional(), kMax);
    EXPECT_TRUE(budget->reserve("one", 1));
}
