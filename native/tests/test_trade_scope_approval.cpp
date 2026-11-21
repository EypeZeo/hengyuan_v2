// SPDX-License-Identifier: proprietary
// Trade-scope grant signature, binding, encoding, and budget regressions.
#include <gtest/gtest.h>
#include <hengyuan/account_truth.hpp>
#include <hengyuan/binance_clock_sync.hpp>
#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/durable_kill_switch.hpp>
#include <hengyuan/owner_trade_scope_approval.hpp>
#include <hengyuan/store_binding_writer.hpp>
#include <hengyuan/trade_scope_budget.hpp>
#include <hengyuan/trade_scope_execution.hpp>
#include <hengyuan/trade_scope_owner_key.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <openssl/x509.h>

namespace {

constexpr std::int64_t kExchangeUpperMs = 1'000'000;
constexpr std::int64_t kAccountRiskTestDayStart = 20'000LL * hy::account_risk_audit::kUtcDayMs;
constexpr std::uint64_t kAccountRiskTestKeyId = 0x4143434f554e5452ULL;

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
    grant.issued_at_ms = kExchangeUpperMs;
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

hy::AccountRiskPolicy make_account_risk_policy(const hy::OperatorResetScope& store, std::uint64_t key_id,
                                              hy::AccountRiskLimits limits) {
    hy::AccountRiskPolicy policy{};
    policy.store = store;
    policy.key_id = key_id;
    policy.limits = limits;
    return policy;
}

std::optional<hy::VerifiedAccountRiskPolicy> verify_account_risk_policy_payload(
    std::span<const std::byte> payload, std::span<const unsigned char> signature, const P256Fixture& signer,
    const hy::OperatorResetScope& expected_store, std::uint64_t expected_key_id, std::uint64_t expected_version) {
    return hy::VerifiedAccountRiskPolicy::verify(
        payload, signature, std::span<const unsigned char>(signer.public_der.data(), signer.public_der.size()),
        expected_store, expected_key_id, expected_version);
}

std::optional<hy::VerifiedAccountRiskPolicy> sign_and_verify_account_risk_policy(
    const hy::AccountRiskPolicy& policy, const P256Fixture& signer) {
    const auto payload = hy::encode_account_risk_policy(policy);
    if (!payload) return std::nullopt;
    const auto signature = signer.sign(std::span<const std::byte>(*payload));
    if (signature.empty()) return std::nullopt;
    return verify_account_risk_policy_payload(
        std::span<const std::byte>(*payload), std::span<const unsigned char>(signature.data(), signature.size()),
        signer, policy.store, policy.key_id, policy.limits.version);
}

hy::TradeScopeGrant make_account_period_scope(const hy::OperatorResetScope& store, std::uint64_t policy_version,
                                             std::uint64_t serial, hy::TradeScopeSides sides) {
    auto grant = make_grant();
    grant.binding.store = store;
    grant.binding.policy_version = policy_version;
    grant.serial = serial;
    grant.sides = sides;
    grant.limits = {100'000'000, 250'000'000, 200'000'000, 10'000};
    return grant;
}

hy::AuditRecord make_account_intent_record(std::string_view order_id, std::int64_t price, std::int64_t quantity,
                                           std::uint32_t symbol_id, hy::OrderSide side,
                                           std::int64_t timestamp_ms) {
    hy::AuditRecord record{};
    record.timestamp_ms = timestamp_ms;
    record.event_type = hy::AuditEventType::OrderIntentCreated;
    record.mode = hy::ExecutionMode::Live;
    record.symbol_id = symbol_id;
    record.set_client_order_id(order_id);
    record.price_ticks = price;
    record.qty_ticks = quantity;
    record.resulting_state = hy::OrderState::Intent;
    record.side = side;
    return record;
}

hy::AuditRecord make_account_period_prepared(const hy::AuditRecord& intent, bool restricted_exit,
                                             std::uint64_t authorization, const hy::SymbolRules& rules,
                                             std::int64_t exchange_ms) {
    auto record = intent;
    hy::account_risk_audit::tag_prepared(record, restricted_exit, authorization, rules, exchange_ms);
    record.resulting_state = hy::OrderState::Submitting;
    return record;
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

class TradeScopeExecutionTest : public ::testing::Test {
protected:
    static constexpr std::uint32_t kAuditKeyId = 1;
    static constexpr std::uint64_t kAccountUid = 71;
    static constexpr std::uint32_t kSymbolId = 7;
    static constexpr hy::TradeScopeLimits kRuntimeLimits{100'000'000, 250'000'000, 200'000'000, 100};

    void SetUp() override {
        static std::atomic<std::uint64_t> sequence{0};
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        directory_ = std::filesystem::temp_directory_path() /
                     ("hy-trade-scope-execution-" + std::to_string(ticks) + "-" +
                      std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        ASSERT_TRUE(std::filesystem::create_directory(directory_));
        audit_path_ = (directory_ / "audit.log").string();

        std::array<std::byte, hy::kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x20 + i);
        key_ring_ = std::make_unique<hy::KeyRing>(kek);
        std::array<std::byte, hy::kKeyBlockSize> key{};
        for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::byte>(0x51 + i);
        hy::WrappedKeyRecord wrapped{};
        ASSERT_EQ(key_ring_->add_key(kAuditKeyId, key, wrapped), hy::KeyRingAddStatus::Ok);

        sink_ = std::make_unique<hy::DurableAuditSink>(audit_path_, *key_ring_, kAuditKeyId,
                                                       hy::kMaxIntentOrphans, hy::AuditSinkOpenMode::Trading,
                                                       hy::NeverSentWrites::Enabled);
        ASSERT_TRUE(sink_->is_open());
        ASSERT_TRUE(sink_->trade_scope_writes_available());
        const auto bind_now = hy::fetch_clock_pair();
        ASSERT_TRUE(hy::store_may_be_used(hy::ensure_store_binding(
            *sink_, audit_path_, *key_ring_, hy::StoreEnvironment::Testnet, kAccountUid, bind_now.system_ms())));
        const auto store_binding = sink_->read_binding();
        ASSERT_EQ(store_binding.status, hy::StoreBindingStatus::Bound);
        ASSERT_EQ(store_binding.binding.environment, hy::StoreEnvironment::Testnet);
        ASSERT_EQ(store_binding.binding.account_uid, kAccountUid);

        hy::DurableKillSwitchConfig kill_config{audit_path_, hy::StoreEnvironment::Testnet, kAccountUid,
                                                sink_->store_uuid_lo(), sink_->store_uuid_hi(), kAuditKeyId, true};
        kill_store_ = std::make_unique<hy::DurableKillSwitchStore>(kill_config, *key_ring_);
        ASSERT_TRUE(kill_store_->recover(kill_switch_));
        ASSERT_EQ(kill_switch_.persistence_health(), hy::KillPersistenceHealth::Ready);
        ASSERT_EQ(kill_switch_.state(), hy::KillState::Normal);

        const auto clock_sample = hy::fetch_clock_pair();
        hy::ClockOffsetSnapshot clock_snapshot{};
        clock_snapshot.error_bound_ms = 1;
        clock_snapshot.system_at_fetch_ms = clock_sample.system_ms();
        clock_snapshot.steady_at_fetch_ms = clock_sample.steady_ms();
        ASSERT_TRUE(clock_.publish(clock_snapshot));

        binding_ = make_grant().binding;
        binding_.store = {hy::StoreEnvironment::Testnet, kAccountUid, sink_->store_uuid_lo(), sink_->store_uuid_hi()};
        binding_.scope_epoch = kill_switch_.snapshot().scope_epoch;
        execution_ = std::make_unique<hy::TradeScopeExecution>(*sink_, kill_switch_, authority_, clock_, binding_);
        ASSERT_TRUE(execution_->current_binding().has_value());
        ASSERT_TRUE(execution_->exchange_upper().has_value());
    }

    void TearDown() override {
        execution_.reset();
        kill_store_.reset();
        sink_.reset();
        key_ring_.reset();
        if (!directory_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(directory_, error);
        }
    }

    std::optional<hy::VerifiedTradeScope> signed_scope(
        hy::TradeScopeBinding binding, std::uint64_t serial,
        hy::TradeScopeSides sides = hy::TradeScopeSides::Buy,
        hy::TradeScopeLimits limits = kRuntimeLimits) const {
        const auto upper = execution_->exchange_upper();
        if (!upper || *upper <= 0 || *upper > std::numeric_limits<std::int64_t>::max() - 30'000) return std::nullopt;
        auto grant = make_grant();
        grant.binding = binding;
        grant.serial = serial;
        grant.issued_at_ms = *upper;
        grant.not_after_ms = *upper + 30'000;
        grant.sides = sides;
        grant.limits = limits;
        const auto payload = hy::encode_trade_scope_grant(grant);
        if (!payload) return std::nullopt;
        const auto signature = signer_.sign(std::span<const std::byte>(*payload));
        if (signature.empty()) return std::nullopt;
        return verify_payload(std::span<const std::byte>(*payload),
                              std::span<const unsigned char>(signature.data(), signature.size()), signer_, binding,
                              hy::TradeScopePolicy{limits, 60'000}, upper);
    }

    hy::SymbolRules rules_for(std::string_view symbol = "BTCUSDT") const {
        hy::SymbolRules rules{};
        std::copy(symbol.begin(), symbol.end(), rules.symbol);
        rules.price_scale = 2;
        rules.qty_scale = 2;
        return rules;
    }

    hy::ExposureLimits exposure(std::int64_t current = 0) const {
        hy::ExposureLimits limits{};
        limits.total_exposure_notional_cap = 300'000'000;
        limits.current_exposure_notional = current;
        return limits;
    }

    std::int64_t now_ms() const { return hy::fetch_clock_pair().system_ms(); }

    std::filesystem::path directory_;
    std::string audit_path_;
    std::unique_ptr<hy::KeyRing> key_ring_;
    std::unique_ptr<hy::DurableAuditSink> sink_;
    std::unique_ptr<hy::DurableKillSwitchStore> kill_store_;
    hy::KillSwitch kill_switch_;
    hy::SubmitAuthority authority_;
    hy::ClockOffsetPublisher clock_;
    hy::TradeScopeBinding binding_{};
    P256Fixture signer_;
    std::unique_ptr<hy::TradeScopeExecution> execution_;
};

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

TEST_F(TradeScopeExecutionTest, AccountRiskPolicySignatureBindsStoreVersionAndLimits) {
    ASSERT_NE(signer_.key, nullptr);
    ASSERT_FALSE(signer_.public_der.empty());
    const auto policy = make_account_risk_policy(
        binding_.store, kAccountRiskTestKeyId, {77, 250'000'000, 0, 0, 0});
    const auto payload = hy::encode_account_risk_policy(policy);
    ASSERT_TRUE(payload.has_value());
    const auto signature = signer_.sign(std::span<const std::byte>(*payload));
    ASSERT_FALSE(signature.empty());
    const auto signature_view = std::span<const unsigned char>(signature.data(), signature.size());

    const auto verified = verify_account_risk_policy_payload(
        std::span<const std::byte>(*payload), signature_view, signer_, policy.store, policy.key_id,
        policy.limits.version);
    ASSERT_TRUE(verified.has_value());
    EXPECT_EQ(verified->policy(), policy);

    auto wrong_store = policy.store;
    ++wrong_store.account_uid;
    EXPECT_FALSE(verify_account_risk_policy_payload(
        std::span<const std::byte>(*payload), signature_view, signer_, wrong_store, policy.key_id,
        policy.limits.version).has_value());
    EXPECT_FALSE(verify_account_risk_policy_payload(
        std::span<const std::byte>(*payload), signature_view, signer_, policy.store, policy.key_id + 1,
        policy.limits.version).has_value());
    EXPECT_FALSE(verify_account_risk_policy_payload(
        std::span<const std::byte>(*payload), signature_view, signer_, policy.store, policy.key_id,
        policy.limits.version + 1).has_value());

    constexpr std::size_t kStoreFieldsOffset = sizeof(hy::kAccountRiskDomain) + 2 + sizeof(hy::kAccountRiskSymbol);
    constexpr std::size_t kPolicyVersionOffset = kStoreFieldsOffset + 4 * sizeof(std::uint64_t);
    constexpr std::size_t kDailyCapOffset = kStoreFieldsOffset + 5 * sizeof(std::uint64_t);
    auto changed_store_payload = *payload;
    changed_store_payload[kStoreFieldsOffset] ^= std::byte{1};
    auto changed_store = policy.store;
    changed_store.account_uid ^= 1;
    EXPECT_FALSE(verify_account_risk_policy_payload(
        std::span<const std::byte>(changed_store_payload), signature_view, signer_, changed_store, policy.key_id,
        policy.limits.version).has_value());

    auto changed_version_payload = *payload;
    changed_version_payload[kPolicyVersionOffset] ^= std::byte{1};
    EXPECT_FALSE(verify_account_risk_policy_payload(
        std::span<const std::byte>(changed_version_payload), signature_view, signer_, policy.store, policy.key_id,
        policy.limits.version - 1).has_value());

    auto changed_limit_payload = *payload;
    changed_limit_payload[kDailyCapOffset] ^= std::byte{1};
    EXPECT_FALSE(verify_account_risk_policy_payload(
        std::span<const std::byte>(changed_limit_payload), signature_view, signer_, policy.store, policy.key_id,
        policy.limits.version).has_value());

    auto foreign_policy = policy;
    ++foreign_policy.store.account_uid;
    const auto foreign_payload = hy::encode_account_risk_policy(foreign_policy);
    ASSERT_TRUE(foreign_payload.has_value());
    const auto foreign_signature = signer_.sign(std::span<const std::byte>(*foreign_payload));
    ASSERT_FALSE(foreign_signature.empty());
    const auto verified_foreign = verify_account_risk_policy_payload(
        std::span<const std::byte>(*foreign_payload),
        std::span<const unsigned char>(foreign_signature.data(), foreign_signature.size()), signer_,
        foreign_policy.store, foreign_policy.key_id, foreign_policy.limits.version);
    ASSERT_TRUE(verified_foreign.has_value());
    EXPECT_FALSE(sink_->append_account_risk_policy(*verified_foreign, kSymbolId, kAccountRiskTestDayStart).acked());
    EXPECT_FALSE(sink_->account_risk().policy().has_value());
}

TEST_F(TradeScopeExecutionTest, AccountPeriodCapSurvivesScopeReplacementAndResetsAtUtcDayBoundary) {
    ASSERT_NE(signer_.key, nullptr);
    const auto policy = make_account_risk_policy(
        binding_.store, kAccountRiskTestKeyId, {81, 100'000'000, 0, 0, 0});
    const auto verified_policy = sign_and_verify_account_risk_policy(policy, signer_);
    ASSERT_TRUE(verified_policy.has_value());
    ASSERT_TRUE(sink_->append_account_risk_policy(*verified_policy, kSymbolId, kAccountRiskTestDayStart).acked());

    const auto first_scope = make_account_period_scope(binding_.store, policy.limits.version, 100,
                                                       hy::TradeScopeSides::Buy);
    ASSERT_TRUE(sink_->append_trade_scope_install(first_scope, kSymbolId, kAccountRiskTestDayStart + 1).acked());
    const auto first_time = kAccountRiskTestDayStart + 10;
    const auto first_intent = make_account_intent_record("HY-ACCOUNT-DAY-1", 100, 100, kSymbolId,
                                                         hy::OrderSide::Buy, first_time);
    ASSERT_TRUE(sink_->append_durable(first_intent, first_time).acked());
    const auto first_prepared = make_account_period_prepared(
        first_intent, false, first_scope.serial, rules_for(), first_time);
    ASSERT_EQ(first_prepared.event_type, hy::AuditEventType::OrderAccountScopedPrepared);
    ASSERT_TRUE(sink_->append_durable(first_prepared, first_time).acked());
    ASSERT_EQ(sink_->account_risk().day_used(), policy.limits.daily_quote_cap);

    const auto replacement = make_account_period_scope(binding_.store, policy.limits.version, 101,
                                                       hy::TradeScopeSides::Buy);
    ASSERT_TRUE(sink_->append_trade_scope_install(replacement, kSymbolId, kAccountRiskTestDayStart + 20).acked());
    const auto capped_time = kAccountRiskTestDayStart + 30;
    const auto capped_intent = make_account_intent_record("HY-ACCOUNT-DAY-2", 100, 100, kSymbolId,
                                                          hy::OrderSide::Buy, capped_time);
    ASSERT_TRUE(sink_->append_durable(capped_intent, capped_time).acked());
    const auto capped_prepared = make_account_period_prepared(
        capped_intent, false, replacement.serial, rules_for(), capped_time);
    const auto frames_before_cap_refusal = sink_->frame_count();
    EXPECT_FALSE(sink_->append_durable(capped_prepared, capped_time).acked());
    EXPECT_EQ(sink_->frame_count(), frames_before_cap_refusal);
    EXPECT_FALSE(sink_->fenced());
    ASSERT_NE(sink_->trade_scope_balance(replacement.serial), nullptr);
    EXPECT_EQ(sink_->trade_scope_balance(replacement.serial)->usage.used_notional, 0);

    execution_.reset();
    sink_.reset();
    sink_ = std::make_unique<hy::DurableAuditSink>(audit_path_, *key_ring_, kAuditKeyId,
                                                   hy::kMaxIntentOrphans, hy::AuditSinkOpenMode::Trading,
                                                   hy::NeverSentWrites::Enabled);
    ASSERT_TRUE(sink_->is_open());
    EXPECT_EQ(sink_->recovery_status(), hy::RecoveryScanStatus::Recovered);
    ASSERT_TRUE(sink_->account_risk().policy().has_value());
    EXPECT_EQ(sink_->account_risk().policy()->limits.version, policy.limits.version);
    EXPECT_EQ(sink_->account_risk().latest_day(), hy::account_risk_audit::utc_day(first_time));
    EXPECT_EQ(sink_->account_risk().day_used(), policy.limits.daily_quote_cap);
    EXPECT_EQ(sink_->recovered_intent_orphans(), 1U);
    EXPECT_EQ(sink_->active_trade_scope_serial(), 0U);
    EXPECT_EQ(sink_->trade_scope_high_water(), replacement.serial);
    ASSERT_NE(sink_->trade_scope_balance(first_scope.serial), nullptr);
    EXPECT_EQ(sink_->trade_scope_balance(first_scope.serial)->usage.used_notional, 100'000'000);
    ASSERT_NE(sink_->trade_scope_balance(replacement.serial), nullptr);
    EXPECT_EQ(sink_->trade_scope_balance(replacement.serial)->usage.used_notional, 0);

    const auto next_day = kAccountRiskTestDayStart + hy::account_risk_audit::kUtcDayMs;
    const auto next_scope = make_account_period_scope(binding_.store, policy.limits.version, 102,
                                                      hy::TradeScopeSides::Buy);
    ASSERT_TRUE(sink_->append_trade_scope_install(next_scope, kSymbolId, next_day).acked());
    const auto next_day_intent = make_account_intent_record("HY-ACCOUNT-NEXT-DAY", 100, 100, kSymbolId,
                                                            hy::OrderSide::Buy, next_day);
    ASSERT_TRUE(sink_->append_durable(next_day_intent, next_day).acked());
    const auto next_day_prepared = make_account_period_prepared(
        next_day_intent, false, next_scope.serial, rules_for(), next_day);
    ASSERT_TRUE(sink_->append_durable(next_day_prepared, next_day).acked());
    EXPECT_EQ(sink_->account_risk().latest_day(), hy::account_risk_audit::utc_day(next_day));
    EXPECT_EQ(sink_->account_risk().day_used(), policy.limits.daily_quote_cap);
    ASSERT_NE(sink_->trade_scope_balance(next_scope.serial), nullptr);
    EXPECT_EQ(sink_->trade_scope_balance(next_scope.serial)->usage.used_notional, 100'000'000);

    const auto same_day_replacement = make_account_period_scope(binding_.store, policy.limits.version, 103,
                                                                hy::TradeScopeSides::Buy);
    ASSERT_TRUE(sink_->append_trade_scope_install(same_day_replacement, kSymbolId, next_day + 1).acked());
    const auto second_capped_time = next_day + 2;
    const auto second_capped_intent = make_account_intent_record(
        "HY-ACCOUNT-NEXT-DAY-2", 100, 100, kSymbolId, hy::OrderSide::Buy, second_capped_time);
    ASSERT_TRUE(sink_->append_durable(second_capped_intent, second_capped_time).acked());
    const auto second_capped_prepared = make_account_period_prepared(
        second_capped_intent, false, same_day_replacement.serial, rules_for(), second_capped_time);
    EXPECT_FALSE(sink_->append_durable(second_capped_prepared, second_capped_time).acked());
}

TEST_F(TradeScopeExecutionTest, DurableNeverSentRefundsAccountAndScopeChargesAfterReplay) {
    ASSERT_NE(signer_.key, nullptr);
    const auto policy = make_account_risk_policy(
        binding_.store, kAccountRiskTestKeyId, {82, 300'000'000, 0, 0, 0});
    const auto verified_policy = sign_and_verify_account_risk_policy(policy, signer_);
    ASSERT_TRUE(verified_policy.has_value());
    ASSERT_TRUE(sink_->append_account_risk_policy(*verified_policy, kSymbolId, kAccountRiskTestDayStart).acked());
    const auto scope = make_account_period_scope(binding_.store, policy.limits.version, 200,
                                                 hy::TradeScopeSides::Buy);
    ASSERT_TRUE(sink_->append_trade_scope_install(scope, kSymbolId, kAccountRiskTestDayStart + 1).acked());

    const auto prepared_time = kAccountRiskTestDayStart + 10;
    const auto intent = make_account_intent_record("HY-ACCOUNT-REFUND", 100, 100, kSymbolId,
                                                   hy::OrderSide::Buy, prepared_time);
    ASSERT_TRUE(sink_->append_durable(intent, prepared_time).acked());
    const auto prepared = make_account_period_prepared(intent, false, scope.serial, rules_for(), prepared_time);
    ASSERT_TRUE(sink_->append_durable(prepared, prepared_time).acked());
    ASSERT_EQ(sink_->account_risk().day_used(), 100'000'000);
    ASSERT_NE(sink_->trade_scope_balance(scope.serial), nullptr);
    ASSERT_EQ(sink_->trade_scope_balance(scope.serial)->usage.used_notional, 100'000'000);

    const auto closure_time = kAccountRiskTestDayStart + 20;
    auto target = hy::never_sent::class_a_target_of(intent);
    target.state = hy::OrderState::Submitting;
    const auto closure = hy::never_sent::class_a_closure_of(
        intent, hy::never_sent::Reason::AccountPeriodChanged, closure_time);
    const auto closed = sink_->append_never_sent(target, closure, closure_time);
    ASSERT_EQ(closed.status, hy::NeverSentAppendStatus::Appended);
    EXPECT_EQ(sink_->account_risk().day_used(), 0);
    EXPECT_EQ(sink_->account_risk().base_reserved(), 0);
    EXPECT_EQ(sink_->trade_scope_balance(scope.serial)->usage.used_notional, 0);

    execution_.reset();
    sink_.reset();
    sink_ = std::make_unique<hy::DurableAuditSink>(audit_path_, *key_ring_, kAuditKeyId,
                                                   hy::kMaxIntentOrphans, hy::AuditSinkOpenMode::Trading,
                                                   hy::NeverSentWrites::Enabled);
    ASSERT_TRUE(sink_->is_open());
    EXPECT_EQ(sink_->recovered_never_sent(), 1U);
    ASSERT_TRUE(sink_->account_risk().policy().has_value());
    EXPECT_EQ(sink_->account_risk().day_used(), 0);
    EXPECT_EQ(sink_->account_risk().base_reserved(), 0);
    EXPECT_EQ(sink_->trade_scope_high_water(), scope.serial);
    EXPECT_EQ(sink_->active_trade_scope_serial(), 0U);
    ASSERT_NE(sink_->trade_scope_balance(scope.serial), nullptr);
    EXPECT_EQ(sink_->trade_scope_balance(scope.serial)->usage.used_notional, 0);
}

TEST_F(TradeScopeExecutionTest, RestrictedExitAndOrdinaryScopedSellShareDurableBaseReservation) {
    ASSERT_NE(signer_.key, nullptr);
    const auto policy = make_account_risk_policy(
        binding_.store, kAccountRiskTestKeyId, {83, 300'000'000, 200'000'000, 100'000'000, 100});
    const auto verified_policy = sign_and_verify_account_risk_policy(policy, signer_);
    ASSERT_TRUE(verified_policy.has_value());
    ASSERT_TRUE(sink_->append_account_risk_policy(*verified_policy, kSymbolId, kAccountRiskTestDayStart).acked());
    const auto scope = make_account_period_scope(binding_.store, policy.limits.version, 300,
                                                 hy::TradeScopeSides::Sell);
    ASSERT_TRUE(sink_->append_trade_scope_install(scope, kSymbolId, kAccountRiskTestDayStart + 1).acked());

    const auto ordinary_time = kAccountRiskTestDayStart + 10;
    const auto ordinary_intent = make_account_intent_record("HY-ORDINARY-SELL", 100, 100, kSymbolId,
                                                            hy::OrderSide::Sell, ordinary_time);
    ASSERT_TRUE(sink_->append_durable(ordinary_intent, ordinary_time).acked());
    const auto ordinary_prepared = make_account_period_prepared(
        ordinary_intent, false, scope.serial, rules_for(), ordinary_time);
    ASSERT_EQ(ordinary_prepared.event_type, hy::AuditEventType::OrderAccountScopedPrepared);
    ASSERT_TRUE(sink_->append_durable(ordinary_prepared, ordinary_time).acked());
    EXPECT_EQ(sink_->account_risk().day_used(), 100'000'000);
    EXPECT_EQ(sink_->account_risk().base_reserved(), 100'000'000);
    ASSERT_NE(sink_->trade_scope_balance(scope.serial), nullptr);
    EXPECT_EQ(sink_->trade_scope_balance(scope.serial)->usage.used_notional, 100'000'000);

    const auto exit_time = kAccountRiskTestDayStart + 20;
    const auto exit_intent = make_account_intent_record("HY-RESTRICTED-EXIT", 100, 100, kSymbolId,
                                                        hy::OrderSide::Sell, exit_time);
    ASSERT_TRUE(sink_->append_durable(exit_intent, exit_time).acked());
    const auto exit_prepared = make_account_period_prepared(
        exit_intent, true, policy.limits.version, rules_for(), exit_time);
    ASSERT_EQ(exit_prepared.event_type, hy::AuditEventType::OrderRestrictedExitPrepared);
    ASSERT_TRUE(sink_->append_durable(exit_prepared, exit_time).acked());
    EXPECT_EQ(sink_->account_risk().day_used(), 100'000'000);
    EXPECT_EQ(sink_->account_risk().base_reserved(), 200'000'000);
    EXPECT_EQ(sink_->trade_scope_balance(scope.serial)->usage.used_notional, 100'000'000);

    const auto blocked_time = kAccountRiskTestDayStart + 30;
    const auto blocked_intent = make_account_intent_record("HY-EXIT-BASE-CAP", 100, 100, kSymbolId,
                                                           hy::OrderSide::Sell, blocked_time);
    ASSERT_TRUE(sink_->append_durable(blocked_intent, blocked_time).acked());
    const auto blocked_prepared = make_account_period_prepared(
        blocked_intent, true, policy.limits.version, rules_for(), blocked_time);
    const auto frames_before_blocked_exit = sink_->frame_count();
    EXPECT_FALSE(sink_->append_durable(blocked_prepared, blocked_time).acked());
    EXPECT_EQ(sink_->frame_count(), frames_before_blocked_exit);
    EXPECT_EQ(sink_->account_risk().base_reserved(), 200'000'000);

    execution_.reset();
    sink_.reset();
    sink_ = std::make_unique<hy::DurableAuditSink>(audit_path_, *key_ring_, kAuditKeyId,
                                                   hy::kMaxIntentOrphans, hy::AuditSinkOpenMode::Trading,
                                                   hy::NeverSentWrites::Enabled);
    ASSERT_TRUE(sink_->is_open());
    ASSERT_EQ(sink_->recovery_status(), hy::RecoveryScanStatus::Recovered);
    EXPECT_EQ(sink_->recovered_checkpoints().size(), 2U);
    EXPECT_EQ(sink_->recovered_intent_orphans(), 1U);
    ASSERT_TRUE(sink_->account_risk().policy().has_value());
    EXPECT_EQ(sink_->account_risk().latest_day(), hy::account_risk_audit::utc_day(ordinary_time));
    EXPECT_EQ(sink_->account_risk().day_used(), 100'000'000);
    EXPECT_EQ(sink_->account_risk().base_reserved(), 200'000'000);
    EXPECT_EQ(sink_->active_trade_scope_serial(), 0U);
    ASSERT_NE(sink_->trade_scope_balance(scope.serial), nullptr);
    EXPECT_EQ(sink_->trade_scope_balance(scope.serial)->usage.used_notional, 100'000'000);
}

TEST_F(TradeScopeExecutionTest, RefusesGrantWithDifferentActivation) {
    ASSERT_NE(signer_.key, nullptr);
    auto wrong_binding = binding_;
    wrong_binding.activation[0] ^= std::byte{0x80};
    const auto scope = signed_scope(wrong_binding, 1);
    ASSERT_TRUE(scope.has_value());

    const auto authority_epoch = authority_.epoch();
    EXPECT_EQ(execution_->install(*scope, rules_for(), kSymbolId, now_ms()), hy::TradeScopeChange::Refused);
    EXPECT_EQ(sink_->trade_scope_high_water(), 0U);
    EXPECT_EQ(authority_.epoch(), authority_epoch);
    EXPECT_EQ(execution_->active(), nullptr);
}

TEST_F(TradeScopeExecutionTest, TargetedRevocationAdvancesHighWaterAndInvalidatesOldClaim) {
    ASSERT_NE(signer_.key, nullptr);
    const auto first = signed_scope(binding_, 40);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(execution_->install(*first, rules_for(), kSymbolId, now_ms()), hy::TradeScopeChange::Applied);
    ASSERT_TRUE(execution_->permits_claim(40));

    const auto stale_claim_epoch = authority_.epoch();
    EXPECT_EQ(execution_->revoke(42, now_ms()), hy::TradeScopeChange::Applied);
    EXPECT_EQ(authority_.claim(stale_claim_epoch), false);
    EXPECT_EQ(sink_->trade_scope_high_water(), 42U);
    EXPECT_EQ(sink_->active_trade_scope_serial(), 40U);
    ASSERT_NE(execution_->active(), nullptr);
    EXPECT_EQ(execution_->active()->serial, 40U);
    EXPECT_TRUE(execution_->permits_claim(40));

    const auto skipped = signed_scope(binding_, 41);
    ASSERT_TRUE(skipped.has_value());
    EXPECT_EQ(execution_->install(*skipped, rules_for(), kSymbolId, now_ms()), hy::TradeScopeChange::Refused);
    const auto after_high_water = signed_scope(binding_, 43);
    ASSERT_TRUE(after_high_water.has_value());
    EXPECT_EQ(execution_->install(*after_high_water, rules_for(), kSymbolId, now_ms()),
              hy::TradeScopeChange::Applied);
    EXPECT_EQ(sink_->trade_scope_high_water(), 43U);
    EXPECT_TRUE(execution_->permits_claim(43));
    EXPECT_FALSE(execution_->permits_claim(40));
}

TEST_F(TradeScopeExecutionTest, ReplacementInvalidatesThePreviousScope) {
    ASSERT_NE(signer_.key, nullptr);
    const auto old_scope = signed_scope(binding_, 50, hy::TradeScopeSides::Buy);
    ASSERT_TRUE(old_scope.has_value());
    ASSERT_EQ(execution_->install(*old_scope, rules_for(), kSymbolId, now_ms()), hy::TradeScopeChange::Applied);
    ASSERT_TRUE(execution_->permits_claim(50));

    const auto replacement = signed_scope(binding_, 51, hy::TradeScopeSides::Sell);
    ASSERT_TRUE(replacement.has_value());
    EXPECT_EQ(execution_->install(*replacement, rules_for(), kSymbolId, now_ms()),
              hy::TradeScopeChange::Applied);

    ASSERT_NE(execution_->active(), nullptr);
    EXPECT_EQ(execution_->active()->serial, 51U);
    EXPECT_EQ(execution_->active()->sides, hy::TradeScopeSides::Sell);
    EXPECT_FALSE(execution_->permits_claim(50));
    EXPECT_TRUE(execution_->permits_claim(51));
}

TEST_F(TradeScopeExecutionTest, SignedScopeAuthorizesOnlyPermittedOrderAndGrantExposure) {
    ASSERT_NE(signer_.key, nullptr);
    const auto scope = signed_scope(binding_, 60, hy::TradeScopeSides::Buy, kRuntimeLimits);
    ASSERT_TRUE(scope.has_value());
    ASSERT_EQ(execution_->install(*scope, rules_for(), kSymbolId, now_ms()), hy::TradeScopeChange::Applied);

    const auto rules = rules_for();
    EXPECT_TRUE(execution_->authorizes(60, rules, hy::OrderSide::Buy, hy::OrderType::Limit,
                                       10'000, 1, 10'000, exposure()));
    EXPECT_FALSE(execution_->authorizes(60, rules_for("ETHUSDT"), hy::OrderSide::Buy, hy::OrderType::Limit,
                                        10'000, 1, 10'000, exposure()));
    EXPECT_FALSE(execution_->authorizes(60, rules, hy::OrderSide::Sell, hy::OrderType::Limit,
                                        10'000, 1, 10'000, exposure()));
    EXPECT_FALSE(execution_->authorizes(60, rules, hy::OrderSide::Buy, static_cast<hy::OrderType>(255),
                                        10'000, 1, 10'000, exposure()));
    EXPECT_FALSE(execution_->authorizes(60, rules, hy::OrderSide::Buy, hy::OrderType::Limit,
                                        10'000, 1, 10'000, exposure(100'000'001)));
    EXPECT_FALSE(execution_->authorizes(60, rules, hy::OrderSide::Buy, hy::OrderType::Limit,
                                        10'001, 1, 10'001, exposure()));
    EXPECT_FALSE(execution_->authorizes(60, rules, hy::OrderSide::Buy, hy::OrderType::Limit,
                                        10'000, 1, 9'899, exposure()));

    EXPECT_TRUE(execution_->can_reserve(60, rules, 10'000, 1));
    EXPECT_FALSE(execution_->can_reserve(60, rules, 10'001, 1));
}

TEST_F(TradeScopeExecutionTest, ProductionKeyRequiresTheCurrentStoresFirstOrderDeclaration) {
    namespace files = hy::spot_owner_key_store;
    const files::StoreFiles production{(directory_ / "production.log").string(), kAuditKeyId,
                                      (directory_ / "production.kek").string(), (directory_ / "production.keys").string()};
    const auto public_path = (directory_ / "owner.der").string();
    const auto draft_path = (directory_ / "first-order.grant").string();
    const auto signature_path = production.log + ".first-order.sig";
    const auto owner_key_id = std::uint64_t{41};
    ASSERT_FALSE(signer_.public_der.empty());

    std::array<std::byte, hy::kKekSize> kek{};
    std::array<std::byte, hy::kKeyBlockSize> audit_key{};
    kek.fill(std::byte{0x51});
    audit_key.fill(std::byte{0x62});
    hy::KeyRing keys(kek);
    hy::WrappedKeyRecord wrapped{};
    ASSERT_EQ(keys.add_key(kAuditKeyId, audit_key, wrapped), hy::KeyRingAddStatus::Ok);
    ASSERT_EQ(files::publish_file(production.kek, kek), hy::ExportStatus::Ok);
    ASSERT_EQ(files::publish_file(production.wrapped_keys, hy::encode_audit_key_file(std::span(&wrapped, 1))),
              hy::ExportStatus::Ok);
    ASSERT_EQ(files::publish_file(public_path, std::as_bytes(std::span(signer_.public_der))), hy::ExportStatus::Ok);
    {
        hy::DurableAuditSink sink(production.log, keys, kAuditKeyId);
        ASSERT_TRUE(sink.is_open());
        ASSERT_TRUE(hy::store_may_be_used(hy::ensure_store_binding(
            sink, production.log, keys, hy::StoreEnvironment::Production, kAccountUid, now_ms())));
    }
    ASSERT_EQ(hy::TradeScopeOwnerKey::enroll(production, hy::StoreEnvironment::Production, owner_key_id, public_path),
              hy::ExportStatus::Ok);
    ASSERT_EQ(hy::TradeScopeOwnerKey::publish_first_order_draft(production, draft_path), hy::ExportStatus::Ok);
    auto store = files::open_existing_store(production, hy::StoreEnvironment::Production);
    ASSERT_NE(store.sink, nullptr);
    EXPECT_FALSE(hy::TradeScopeOwnerKey::load(production.log, *store.sink));
    const hy::OperatorResetScope wrong_store{hy::StoreEnvironment::Testnet, kAccountUid,
                                           store.sink->store_uuid_lo(), store.sink->store_uuid_hi()};
    const auto wrong_signature = signer_.sign(hy::scope_first_order_declaration(wrong_store, owner_key_id));
    ASSERT_FALSE(wrong_signature.empty());
    ASSERT_EQ(files::publish_file(signature_path, std::as_bytes(std::span(wrong_signature))), hy::ExportStatus::Ok);
    EXPECT_FALSE(hy::TradeScopeOwnerKey::load(production.log, *store.sink));
    ASSERT_TRUE(std::filesystem::remove(signature_path));

    std::vector<std::byte> declaration;
    ASSERT_EQ(hy::read_file_bounded(draft_path, declaration, hy::ScopeFirstOrderBytes{}.size()), hy::ReadFileStatus::Ok);
    const auto signature = signer_.sign(declaration);
    ASSERT_FALSE(signature.empty());
    ASSERT_EQ(files::publish_file(signature_path, std::as_bytes(std::span(signature))), hy::ExportStatus::Ok);
    const auto owner = hy::TradeScopeOwnerKey::load(production.log, *store.sink);
    ASSERT_TRUE(owner.has_value());
    EXPECT_EQ(owner->subject().environment, hy::StoreEnvironment::Production);
    EXPECT_EQ(owner->key_id(), owner_key_id);
}
