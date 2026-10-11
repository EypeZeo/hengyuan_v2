// SAFE-02 / audit P1-003 / fault case FI-033: the account snapshot's safe boundary.
//
// What these tests pin, none of it needing a network:
//   * the type properties: AssetBalance and AccountSnapshot expose no writable field and cannot be made by
//     brace-initialisation, so the only way to a non-empty snapshot is AccountSnapshotBuilder;
//   * the factory's refusals (empty / overlong / unprintable / duplicate names, negative and overflowing balances,
//     capacity, a non-positive timestamp), each leaving the builder and the output untouched;
//   * the accessors' own clamps -- find()/asset_count()/asset_name() stay inside their arrays even for a value the
//     factory would never produce (made through the test hook), which is what ASan watches in the sanitizer job;
//   * check_freshness()/validate_pre_trade() on a snapshot dated in the future, with a bounded, clamped tolerance;
//   * a deterministic fuzz of the factory (fixed seed): whatever sequence of assets is thrown at it, a snapshot that
//     comes out satisfies every promise, and a refusal changes nothing.
// The parse path (parse_account_response) has its own fuzz in test_binance_private_rest.cpp (it needs Boost).

#include <gtest/gtest.h>
#include <hengyuan/account_truth.hpp>

#include "test_helpers/account_snapshot_fixtures.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using hy::AccountSnapshot;
using hy::AccountSnapshotBuilder;
using hy::AccountSnapshotError;
using hy::AccountSnapshotTestHooks;
using hy::AssetBalance;
using hy::AssetBalanceInput;
using hy::ExposureLimits;
using hy::FreshnessStatus;
using hy::PreTradeCheck;
using hy::SymbolRules;
using hy::check_freshness;
using hy::kMaxAssets;
using hy::kAssetNameLen;
using hy::test_helpers::make_account;

namespace {

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

// --- the type properties, stated as expressions the compiler either accepts or not ---

template <class T>
constexpr bool can_write_timestamp = requires(T t) { t.timestamp_ms = 1; };
template <class T>
constexpr bool can_write_can_trade = requires(T t) { t.can_trade = true; };
template <class T>
constexpr bool can_write_asset_count = requires(T t) { t.asset_count = 1; };
template <class T>
constexpr bool can_write_through_assets = requires(T t) { t.assets[0].free_ticks = 1; };
template <class T>
constexpr bool can_write_free_ticks = requires(T t) { t.free_ticks = 1; };
template <class T>
constexpr bool can_write_locked_ticks = requires(T t) { t.locked_ticks = 1; };
template <class T>
constexpr bool can_write_name_bytes = requires(T t) { t.asset[0] = 'X'; };
template <class T>
constexpr bool can_write_private_count = requires(T t) { t.count_ = 1; };
template <class T>
constexpr bool can_write_private_timestamp = requires(T t) { t.timestamp_ms_ = 1; };
template <class T>
constexpr bool can_write_private_balance = requires(T t) { t.free_ticks_ = 1; };

// What the types looked like before SAFE-02: the positive control for every concept above.
struct LegacyBalanceShape {
    char asset[kAssetNameLen]{};
    std::int64_t free_ticks{0};
    std::int64_t locked_ticks{0};
};
struct LegacySnapshotShape {
    std::int64_t timestamp_ms{0};
    std::size_t asset_count{0};
    LegacyBalanceShape assets[kMaxAssets]{};
    bool can_trade{false};
};

// A snapshot of `n` distinct assets A0.., each with 1 free tick.
AccountSnapshot n_assets(std::size_t n, std::int64_t ts = 1000) {
    AccountSnapshotBuilder b;
    for (std::size_t i = 0; i < n; ++i) {
        const std::string name = "A" + std::to_string(i);
        if (b.add_asset(name, 1, 0) != AccountSnapshotError::None) ADD_FAILURE() << "unexpected refusal of " << name;
    }
    AccountSnapshot out;
    if (b.build(ts, out) != AccountSnapshotError::None) ADD_FAILURE() << "unexpected refusal of the snapshot";
    return out;
}

SymbolRules btcusdt_rules() {
    SymbolRules r{};
    std::memcpy(r.symbol, "BTCUSDT", 8);
    r.is_trading = true;
    r.min_price_ticks = 100;
    r.max_price_ticks = 10'000'000;
    r.tick_size_ticks = 100;
    r.min_qty_ticks = 10;
    r.max_qty_ticks = 1'000'000;
    r.step_size_ticks = 10;
    r.min_notional_ticks = 1000;
    r.price_scale = 0;
    r.qty_scale = 8;
    return r;
}

ExposureLimits limits() {
    ExposureLimits l{};
    l.single_order_notional_cap = 50'000;
    l.total_exposure_notional_cap = 200'000;
    l.freshness_max_age_ms = 30'000;
    return l;
}

}  // namespace

// ================================================================== type properties

TEST(AccountSnapshotTypes, ThePositiveControlSeesTheOldPublicShape) {
    EXPECT_TRUE(can_write_timestamp<LegacySnapshotShape>);
    EXPECT_TRUE(can_write_can_trade<LegacySnapshotShape>);
    EXPECT_TRUE(can_write_asset_count<LegacySnapshotShape>);
    EXPECT_TRUE(can_write_through_assets<LegacySnapshotShape>);
    EXPECT_TRUE(can_write_free_ticks<LegacyBalanceShape>);
    EXPECT_TRUE(can_write_locked_ticks<LegacyBalanceShape>);
    EXPECT_TRUE(can_write_name_bytes<LegacyBalanceShape>);
}

TEST(AccountSnapshotTypes, NoFieldOfASnapshotOrABalanceCanBeWrittenFromOutside) {
    EXPECT_FALSE(can_write_timestamp<AccountSnapshot>);
    EXPECT_FALSE(can_write_can_trade<AccountSnapshot>);
    EXPECT_FALSE(can_write_asset_count<AccountSnapshot>);
    EXPECT_FALSE(can_write_through_assets<AccountSnapshot>);
    EXPECT_FALSE(can_write_private_count<AccountSnapshot>);
    EXPECT_FALSE(can_write_private_timestamp<AccountSnapshot>);
    EXPECT_FALSE(can_write_free_ticks<AssetBalance>);
    EXPECT_FALSE(can_write_locked_ticks<AssetBalance>);
    EXPECT_FALSE(can_write_name_bytes<AssetBalance>);
    EXPECT_FALSE(can_write_private_balance<AssetBalance>);
}

TEST(AccountSnapshotTypes, NeitherTypeCanBeBraceInitialisedFromItsParts) {
    EXPECT_FALSE(std::is_aggregate_v<AccountSnapshot>);
    EXPECT_FALSE(std::is_aggregate_v<AssetBalance>);
    EXPECT_FALSE((std::is_constructible_v<AccountSnapshot, std::int64_t, std::size_t, bool>));
    EXPECT_FALSE((std::is_constructible_v<AssetBalance, const char*, std::int64_t, std::int64_t>));
    EXPECT_TRUE(std::is_default_constructible_v<AccountSnapshot>);
}

TEST(AccountSnapshotTypes, StillAPlainFixedSizeValueTheHotPathMayCopy) {
    EXPECT_TRUE(std::is_trivially_copyable_v<AccountSnapshot>);
    EXPECT_TRUE(std::is_trivially_copyable_v<AssetBalance>);
    EXPECT_TRUE(std::is_trivially_destructible_v<AccountSnapshot>);
    // The memory budget of the 32-slot capacity (SAFE-02 keeps it): 32 balances of 32 bytes plus the header.
    static_assert(sizeof(AssetBalance) <= 32, "an asset balance must stay within half a cache line");
    static_assert(sizeof(AccountSnapshot) <= 1100, "the snapshot's footprint is part of the capacity decision");
    EXPECT_LE(sizeof(AccountSnapshot), 1100u);
}

// ================================================================== the factory

TEST(AccountSnapshotBuilderRules, AValidAssetIsStoredExactly) {
    AccountSnapshotBuilder b;
    ASSERT_EQ(b.add_asset("USDT", 150, 25), AccountSnapshotError::None);
    b.set_can_trade(true);
    AccountSnapshot s;
    ASSERT_EQ(b.build(1234, s), AccountSnapshotError::None);
    EXPECT_EQ(s.timestamp_ms(), 1234);
    EXPECT_TRUE(s.can_trade());
    ASSERT_EQ(s.asset_count(), 1u);
    const AssetBalance* a = s.find("USDT");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->asset_name(), "USDT");
    EXPECT_EQ(a->free_ticks(), 150);
    EXPECT_EQ(a->locked_ticks(), 25);
    std::int64_t total = 0;
    ASSERT_TRUE(a->total(total));
    EXPECT_EQ(total, 175);
}

TEST(AccountSnapshotBuilderRules, AnEmptyNameIsRefused) {
    AccountSnapshotBuilder b;
    EXPECT_EQ(b.add_asset("", 1, 0), AccountSnapshotError::AssetNameEmpty);
    EXPECT_EQ(b.asset_count(), 0u);
}

TEST(AccountSnapshotBuilderRules, ElevenCharactersFitAndTwelveAreRefusedNotTruncated) {
    static_assert(kAssetNameLen == 12, "the boundary below is written for a 12-byte buffer");
    AccountSnapshotBuilder b;
    EXPECT_EQ(b.add_asset("ELEVENCHARS", 1, 0), AccountSnapshotError::None);
    EXPECT_EQ(b.add_asset("TWELVELETTRS", 1, 0), AccountSnapshotError::AssetNameTooLong);
    EXPECT_EQ(b.add_asset(std::string(200, 'X'), 1, 0), AccountSnapshotError::AssetNameTooLong);
    EXPECT_EQ(b.asset_count(), 1u);
    AccountSnapshot s;
    ASSERT_EQ(b.build(1, s), AccountSnapshotError::None);
    EXPECT_NE(s.find("ELEVENCHARS"), nullptr);
    EXPECT_EQ(s.find("TWELVELETTR"), nullptr) << "the 12-character name must not survive as an 11-character prefix";
}

TEST(AccountSnapshotBuilderRules, AByteOutsidePrintableAsciiIsRefused) {
    const std::string bad_names[] = {
        "BT C",                          // space
        "BTC\t",                         // tab
        std::string("BTC\0X", 5),        // embedded NUL: the name would end early
        std::string("\0", 1),            // a lone NUL
        "BTC\x7f",                       // DEL
        "BTC\x80",                       // a UTF-8 continuation byte
        "BT\xc3\xa9",                    // a real non-ASCII character
        "\x01" "BTC",                    // control byte first
    };
    for (const std::string& name : bad_names) {
        AccountSnapshotBuilder b;
        EXPECT_EQ(b.add_asset(name, 1, 0), AccountSnapshotError::AssetNameInvalid) << "name of " << name.size() << " bytes";
        EXPECT_EQ(b.asset_count(), 0u);
    }
    AccountSnapshotBuilder printable;  // the boundary bytes themselves are fine
    EXPECT_EQ(printable.add_asset("!", 1, 0), AccountSnapshotError::None);   // 0x21
    EXPECT_EQ(printable.add_asset("~", 1, 0), AccountSnapshotError::None);   // 0x7E
}

TEST(AccountSnapshotBuilderRules, ADuplicateNameIsRefused) {
    AccountSnapshotBuilder b;
    ASSERT_EQ(b.add_asset("BTC", 1, 0), AccountSnapshotError::None);
    ASSERT_EQ(b.add_asset("ETH", 1, 0), AccountSnapshotError::None);
    EXPECT_EQ(b.add_asset("BTC", 99, 0), AccountSnapshotError::DuplicateAsset);
    EXPECT_EQ(b.add_asset("ETH", 99, 0), AccountSnapshotError::DuplicateAsset);
    EXPECT_EQ(b.asset_count(), 2u);
    AccountSnapshot s;
    ASSERT_EQ(b.build(1, s), AccountSnapshotError::None);
    EXPECT_EQ(s.find("BTC")->free_ticks(), 1) << "the refused duplicate must not have replaced the first entry";
}

TEST(AccountSnapshotBuilderRules, ANegativeBalanceIsRefused) {
    AccountSnapshotBuilder b;
    EXPECT_EQ(b.add_asset("BTC", -1, 0), AccountSnapshotError::NegativeBalance);
    EXPECT_EQ(b.add_asset("BTC", 0, -1), AccountSnapshotError::NegativeBalance);
    EXPECT_EQ(b.add_asset("BTC", kMin, 0), AccountSnapshotError::NegativeBalance);
    EXPECT_EQ(b.asset_count(), 0u);
}

TEST(AccountSnapshotBuilderRules, FreePlusLockedThatDoesNotFitInt64IsRefused) {
    AccountSnapshotBuilder b;
    EXPECT_EQ(b.add_asset("BTC", kMax, 1), AccountSnapshotError::BalanceOverflow);
    EXPECT_EQ(b.add_asset("BTC", 1, kMax), AccountSnapshotError::BalanceOverflow);
    EXPECT_EQ(b.add_asset("BTC", kMax / 2 + 1, kMax / 2 + 1), AccountSnapshotError::BalanceOverflow);
    EXPECT_EQ(b.asset_count(), 0u);
    // the largest sum that does fit is accepted, and total() agrees
    ASSERT_EQ(b.add_asset("ETH", kMax - 5, 5), AccountSnapshotError::None);
    AccountSnapshot s;
    ASSERT_EQ(b.build(1, s), AccountSnapshotError::None);
    std::int64_t total = 0;
    ASSERT_TRUE(s.find("ETH")->total(total));
    EXPECT_EQ(total, kMax);
}

TEST(AccountSnapshotBuilderRules, ThirtyTwoAssetsFitAndTheThirtyThirdIsRefusedWithoutDisturbingTheRest) {
    AccountSnapshotBuilder b;
    for (std::size_t i = 0; i < kMaxAssets; ++i) {
        ASSERT_EQ(b.add_asset("A" + std::to_string(i), static_cast<std::int64_t>(i) + 1, 0), AccountSnapshotError::None) << i;
    }
    EXPECT_EQ(b.asset_count(), kMaxAssets);
    EXPECT_EQ(b.add_asset("ONEMORE", 1, 0), AccountSnapshotError::CapacityExceeded);
    EXPECT_EQ(b.add_asset("ONEMORE", 1, 0), AccountSnapshotError::CapacityExceeded) << "refusals are repeatable";
    EXPECT_EQ(b.asset_count(), kMaxAssets);
    AccountSnapshot s;
    ASSERT_EQ(b.build(1, s), AccountSnapshotError::None);
    ASSERT_EQ(s.asset_count(), kMaxAssets);
    EXPECT_EQ(s.find("ONEMORE"), nullptr);
    for (std::size_t i = 0; i < kMaxAssets; ++i) {
        const AssetBalance* a = s.find("A" + std::to_string(i));
        ASSERT_NE(a, nullptr) << i;
        EXPECT_EQ(a->free_ticks(), static_cast<std::int64_t>(i) + 1);
    }
}

TEST(AccountSnapshotBuilderRules, ARefusalChangesNothingAndTheBuilderKeepsWorking) {
    AccountSnapshotBuilder b;
    ASSERT_EQ(b.add_asset("BTC", 10, 0), AccountSnapshotError::None);
    EXPECT_NE(b.add_asset("BAD NAME", 1, 0), AccountSnapshotError::None);
    EXPECT_NE(b.add_asset("ETH", -5, 0), AccountSnapshotError::None);
    ASSERT_EQ(b.add_asset("ETH", 20, 0), AccountSnapshotError::None);
    AccountSnapshot s;
    ASSERT_EQ(b.build(1, s), AccountSnapshotError::None);
    ASSERT_EQ(s.asset_count(), 2u);
    EXPECT_EQ(s.assets()[0].asset_name(), "BTC");
    EXPECT_EQ(s.assets()[1].asset_name(), "ETH");
    EXPECT_EQ(s.assets()[1].free_ticks(), 20);
}

TEST(AccountSnapshotBuilderRules, ANonPositiveTimestampIsRefusedAndTheOutputIsLeftAlone) {
    AccountSnapshotBuilder b;
    ASSERT_EQ(b.add_asset("BTC", 1, 0), AccountSnapshotError::None);
    for (const std::int64_t bad : {std::int64_t{0}, std::int64_t{-1}, kMin}) {
        AccountSnapshot out = make_account(777, true, {{"KEEP", 5, 0}});
        EXPECT_EQ(b.build(bad, out), AccountSnapshotError::TimestampNotPositive) << bad;
        EXPECT_EQ(out.timestamp_ms(), 777) << "out must be untouched on a refusal";
        EXPECT_NE(out.find("KEEP"), nullptr);
    }
    AccountSnapshot ok;
    EXPECT_EQ(b.build(1, ok), AccountSnapshotError::None);  // the smallest valid stamp
    EXPECT_EQ(b.build(kMax, ok), AccountSnapshotError::None);
}

TEST(AccountSnapshotBuilderRules, OneBuilderCanStampTheSameAssetsTwice) {
    AccountSnapshotBuilder b;
    b.set_can_trade(true);
    ASSERT_EQ(b.add_asset("BTC", 3, 4), AccountSnapshotError::None);
    AccountSnapshot first, second;
    ASSERT_EQ(b.build(100, first), AccountSnapshotError::None);
    ASSERT_EQ(b.build(200, second), AccountSnapshotError::None);
    EXPECT_EQ(first.timestamp_ms(), 100);
    EXPECT_EQ(second.timestamp_ms(), 200);
    EXPECT_EQ(first.find("BTC")->free_ticks(), second.find("BTC")->free_ticks());
}

TEST(AccountSnapshotBuilderRules, StartingFromASnapshotKeepsItsAssetsAndDropsItsStamp) {
    const AccountSnapshot base = make_account(900, true, {{"USDT", 999, 1}, {"BTC", 5, 0}});
    AccountSnapshotBuilder from_base(base);
    EXPECT_EQ(from_base.asset_count(), 2u);
    AccountSnapshot restamped;
    ASSERT_EQ(from_base.build(42, restamped), AccountSnapshotError::None);
    EXPECT_EQ(restamped.timestamp_ms(), 42);
    EXPECT_TRUE(restamped.can_trade());
    ASSERT_EQ(restamped.asset_count(), 2u);
    EXPECT_EQ(restamped.find("USDT")->free_ticks(), 999);
    EXPECT_EQ(base.timestamp_ms(), 900) << "the base snapshot is not touched";
    // the builder can still be extended, and the factory's rules still apply to the additions
    EXPECT_EQ(from_base.add_asset("USDT", 1, 0), AccountSnapshotError::DuplicateAsset);
    EXPECT_EQ(from_base.add_asset("ETH", 1, 0), AccountSnapshotError::None);
    // and a zero stamp is as refused here as anywhere
    AccountSnapshot none;
    EXPECT_EQ(from_base.build(0, none), AccountSnapshotError::TimestampNotPositive);
}

TEST(AccountSnapshotBuilderRules, StartingFromACorruptedSnapshotNeverCopiesMoreThanTheCapacity) {
    AccountSnapshot corrupt = n_assets(kMaxAssets);
    AccountSnapshotTestHooks::set_raw_count(corrupt, 5000);
    AccountSnapshotBuilder b(corrupt);
    EXPECT_EQ(b.asset_count(), kMaxAssets);
    EXPECT_EQ(b.add_asset("ONEMORE", 1, 0), AccountSnapshotError::CapacityExceeded);
}

TEST(AccountSnapshotBuilderRules, TheShortcutStopsAtTheFirstRefusal) {
    const AssetBalanceInput good[] = {{"USDT", 5, 0}, {"BTC", 6, 1}};
    const AssetBalanceInput bad_second[] = {{"USDT", 5, 0}, {"BAD NAME", 6, 1}};
    AccountSnapshot out = make_account(777, true, {{"KEEP", 5, 0}});
    EXPECT_EQ(hy::make_account_snapshot(10, true, good, out), AccountSnapshotError::None);
    EXPECT_EQ(out.asset_count(), 2u);
    EXPECT_EQ(hy::make_account_snapshot(11, true, bad_second, out), AccountSnapshotError::AssetNameInvalid);
    EXPECT_EQ(out.timestamp_ms(), 10) << "a refusal leaves the previous output as it was";
}

TEST(AccountSnapshotBuilderRules, EveryErrorHasAName) {
    for (const auto e : {AccountSnapshotError::None, AccountSnapshotError::AssetNameEmpty,
                         AccountSnapshotError::AssetNameTooLong, AccountSnapshotError::AssetNameInvalid,
                         AccountSnapshotError::DuplicateAsset, AccountSnapshotError::NegativeBalance,
                         AccountSnapshotError::BalanceOverflow, AccountSnapshotError::CapacityExceeded,
                         AccountSnapshotError::TimestampNotPositive}) {
        const std::string_view name = hy::account_snapshot_error_name(e);
        EXPECT_FALSE(name.empty());
        EXPECT_NE(name, "Unknown");
    }
}

// ================================================================== the accessors

TEST(AccountSnapshotAccessors, ADefaultSnapshotIsNeverFetched) {
    const AccountSnapshot s;
    EXPECT_EQ(s.timestamp_ms(), 0);
    EXPECT_FALSE(s.can_trade());
    EXPECT_EQ(s.asset_count(), 0u);
    EXPECT_TRUE(s.assets().empty());
    EXPECT_EQ(s.find("USDT"), nullptr);
    EXPECT_EQ(check_freshness(s.timestamp_ms(), 1000, 30'000), FreshnessStatus::NeverFetched);
}

TEST(AccountSnapshotAccessors, FindMatchesTheWholeNameOnly) {
    const AccountSnapshot s = make_account(1000, true, {{"BTC", 1, 0}, {"BTCUP", 2, 0}});
    ASSERT_NE(s.find("BTC"), nullptr);
    EXPECT_EQ(s.find("BTC")->free_ticks(), 1);
    EXPECT_EQ(s.find("BTCUP")->free_ticks(), 2);
    EXPECT_EQ(s.find("BT"), nullptr);
    EXPECT_EQ(s.find("BTCU"), nullptr);
    EXPECT_EQ(s.find("btc"), nullptr);
    EXPECT_EQ(s.find(""), nullptr);
}

TEST(AccountSnapshotAccessors, TotalIsCheckedEvenForAValueTheFactoryWouldNeverMake) {
    AccountSnapshot s = make_account(1000, true, {{"BTC", 1, 1}});
    std::int64_t total = 12345;
    AccountSnapshotTestHooks::set_raw_balance(s, 0, kMax, 1);
    EXPECT_FALSE(s.assets()[0].total(total));
    AccountSnapshotTestHooks::set_raw_balance(s, 0, -1, 5);
    EXPECT_FALSE(s.assets()[0].total(total));
    AccountSnapshotTestHooks::set_raw_balance(s, 0, 5, -1);
    EXPECT_FALSE(s.assets()[0].total(total));
    EXPECT_EQ(total, 12345) << "a failed total() leaves out alone";
    AccountSnapshotTestHooks::set_raw_balance(s, 0, kMax, 0);
    ASSERT_TRUE(s.assets()[0].total(total));
    EXPECT_EQ(total, kMax);
}

TEST(AccountSnapshotAccessors, ACountBeyondTheCapacityIsClampedByEveryReader) {
    AccountSnapshot s = n_assets(kMaxAssets);
    AccountSnapshotTestHooks::set_raw_count(s, 1'000'000);
    EXPECT_EQ(s.asset_count(), kMaxAssets);
    EXPECT_EQ(s.assets().size(), kMaxAssets);
    // find() of a name that is not there walks every slot it is allowed to and not one more (ASan would see it)
    EXPECT_EQ(s.find("NOT-THERE"), nullptr);
    EXPECT_NE(s.find("A31"), nullptr);
    AccountSnapshotTestHooks::set_raw_count(s, std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(s.asset_count(), kMaxAssets);
    EXPECT_EQ(s.find("NOT-THERE"), nullptr);
    AccountSnapshotTestHooks::set_raw_count(s, 3);  // an in-range count is left alone
    EXPECT_EQ(s.asset_count(), 3u);
}

TEST(AccountSnapshotAccessors, ANameWhoseTerminatorIsGoneIsStillReadByItsStoredLengthAndClamped) {
    AccountSnapshot s = make_account(1000, true, {{"BTC", 1, 0}});
    // all 12 bytes are letters: nothing ends the string -- strlen() would run on into the next field
    AccountSnapshotTestHooks::set_raw_name(s, 0, "ABCDEFGHIJKL", 12);
    EXPECT_EQ(s.assets()[0].asset_name(), "ABCDEFGHIJK") << "the length is clamped to the buffer minus the terminator";
    AccountSnapshotTestHooks::set_raw_name(s, 0, "ABCDEFGHIJKL", 255);
    EXPECT_EQ(s.assets()[0].asset_name().size(), kAssetNameLen - 1);
    AccountSnapshotTestHooks::set_raw_name(s, 0, "ABCDEFGHIJKL", 3);
    EXPECT_EQ(s.assets()[0].asset_name(), "ABC") << "the stored length decides, not a terminator that may not be there";
    AccountSnapshotTestHooks::set_raw_name(s, 0, "ABC", 0);
    EXPECT_TRUE(s.assets()[0].asset_name().empty());
    EXPECT_EQ(s.find("ABCDEFGHIJKL"), nullptr);
}

TEST(AccountSnapshotAccessors, CopiesKeepTheInvariants) {
    const AccountSnapshot original = n_assets(7);
    AccountSnapshot copy = original;
    AccountSnapshot assigned;
    assigned = copy;
    for (const AccountSnapshot* s : {&copy, &assigned}) {
        EXPECT_EQ(s->asset_count(), 7u);
        EXPECT_EQ(s->timestamp_ms(), original.timestamp_ms());
        EXPECT_NE(s->find("A6"), nullptr);
        EXPECT_EQ(s->find("A7"), nullptr);
    }
}

// ================================================================== a snapshot from the future

TEST(FutureTimestamp, WithinTheToleranceItIsClockErrorBeyondItIsRefused) {
    const std::int64_t now = 1'000'000;
    EXPECT_EQ(check_freshness(now + 2500, now, 30'000), FreshnessStatus::Fresh);          // the default tolerance, inclusive
    EXPECT_EQ(check_freshness(now + 2501, now, 30'000), FreshnessStatus::FromTheFuture);
    EXPECT_EQ(check_freshness(now + 1, now, 30'000), FreshnessStatus::Fresh);
    EXPECT_EQ(check_freshness(now + 3'600'000, now, 30'000), FreshnessStatus::FromTheFuture)
        << "an hour ahead used to be judged fresh for an hour and 30 seconds";
    EXPECT_EQ(check_freshness(now, now, 30'000), FreshnessStatus::Fresh);
}

TEST(FutureTimestamp, TheToleranceIsTheCallersBoundedByAHardCap) {
    const std::int64_t now = 1'000'000;
    EXPECT_EQ(check_freshness(now + 100, now, 30'000, 100), FreshnessStatus::Fresh);
    EXPECT_EQ(check_freshness(now + 101, now, 30'000, 100), FreshnessStatus::FromTheFuture);
    // a misconfigured, huge tolerance cannot reopen the hole: it is clamped to kMaxFutureSkewCapMs
    EXPECT_EQ(check_freshness(now + hy::kMaxFutureSkewCapMs, now, 30'000, kMax), FreshnessStatus::Fresh);
    EXPECT_EQ(check_freshness(now + hy::kMaxFutureSkewCapMs + 1, now, 30'000, kMax), FreshnessStatus::FromTheFuture);
    // a negative tolerance means "none": any lead at all is refused
    EXPECT_EQ(check_freshness(now + 1, now, 30'000, -5), FreshnessStatus::FromTheFuture);
    EXPECT_EQ(check_freshness(now, now, 30'000, -5), FreshnessStatus::Fresh);
    static_assert(hy::kDefaultMaxFutureSkewMs > 0 && hy::kDefaultMaxFutureSkewMs <= hy::kMaxFutureSkewCapMs);
}

TEST(FutureTimestamp, ArithmeticAtTheExtremesNeitherWrapsNorCallsAFutureSnapshotFresh) {
    EXPECT_EQ(check_freshness(1, kMax, 30'000), FreshnessStatus::Stale);                  // age ~ INT64_MAX: stale, no wrap
    EXPECT_EQ(check_freshness(kMax, 0, 30'000), FreshnessStatus::FromTheFuture);          // lead = INT64_MAX
    EXPECT_EQ(check_freshness(1, kMin, 30'000), FreshnessStatus::FromTheFuture);          // lead overflows int64
    EXPECT_EQ(check_freshness(kMax, kMin, 30'000), FreshnessStatus::FromTheFuture);
    EXPECT_EQ(check_freshness(kMax, kMax, 30'000), FreshnessStatus::Fresh);
    EXPECT_EQ(check_freshness(1, 0, 30'000), FreshnessStatus::Fresh);                     // a clock that has not started yet
    EXPECT_EQ(check_freshness(0, kMax, 30'000), FreshnessStatus::NeverFetched);
    EXPECT_EQ(check_freshness(-5, 100, 30'000), FreshnessStatus::NeverFetched);
}

TEST(FutureTimestamp, TheOrdinaryVerdictsAreUnchanged) {
    EXPECT_EQ(check_freshness(1'000'000, 1'020'000, 30'000), FreshnessStatus::Fresh);
    EXPECT_EQ(check_freshness(1'000'000, 1'030'000, 30'000), FreshnessStatus::Fresh);    // exact boundary
    EXPECT_EQ(check_freshness(1'000'000, 1'030'001, 30'000), FreshnessStatus::Stale);
    EXPECT_EQ(check_freshness(0, 1'000'000, 30'000), FreshnessStatus::NeverFetched);
}

TEST(FutureTimestamp, PreTradeNamesItAndHonoursTheLimitsTolerance) {
    const SymbolRules rules = btcusdt_rules();
    ExposureLimits l = limits();
    const std::int64_t now = 1'000;

    const AccountSnapshot in_the_future = make_account(now + 60'000, true, {{"USDT", 100'000, 0}});
    EXPECT_EQ(hy::validate_pre_trade(rules, 1000, 10, in_the_future, "USDT", now, l), PreTradeCheck::BalanceFromFuture);
    EXPECT_EQ(hy::validate_pre_trade(rules, hy::OrderSide::Sell, 1000, 10, in_the_future, "BTC", "USDT", now, l),
              PreTradeCheck::BalanceFromFuture);

    const AccountSnapshot slightly_ahead = make_account(now + 2000, true, {{"USDT", 100'000, 0}});
    EXPECT_EQ(hy::validate_pre_trade(rules, 1000, 10, slightly_ahead, "USDT", now, l), PreTradeCheck::Ok);
    l.max_future_skew_ms = 1000;  // the caller derived a tighter bound from a live clock error bound
    EXPECT_EQ(hy::validate_pre_trade(rules, 1000, 10, slightly_ahead, "USDT", now, l), PreTradeCheck::BalanceFromFuture);

    // it is not reported as "stale": operators read the audit detail code, which is the enumerator's number
    EXPECT_EQ(static_cast<int>(PreTradeCheck::BalanceFromFuture), 16);
    EXPECT_NE(PreTradeCheck::BalanceFromFuture, PreTradeCheck::BalanceStale);
    EXPECT_EQ(static_cast<int>(PreTradeCheck::BalanceStale), 12) << "the existing codes keep their numbers";
}

TEST(FutureTimestamp, TheDefaultLimitsCarryTheDefaultTolerance) {
    const ExposureLimits l{};
    EXPECT_EQ(l.max_future_skew_ms, hy::kDefaultMaxFutureSkewMs);
}

// The two bounds are numbers with a stated derivation (two honest exchange-calibrated readings differ by up to
// 2 * 1100 ms; the cap is the most any caller may ask for), so they are pinned, not only used symbolically: a
// change has to come with a reviewer who has read that derivation.
TEST(FutureTimestamp, TheBoundsArePinnedNumbers) {
    EXPECT_EQ(hy::kDefaultMaxFutureSkewMs, 2'500);
    EXPECT_EQ(hy::kMaxFutureSkewCapMs, 10'000);
}

// ================================================================== FI-033: deterministic fuzz of the factory

namespace {

struct Xorshift64 {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::uint64_t below(std::uint64_t n) { return next() % n; }
};

std::string random_name(Xorshift64& r, const std::vector<std::string>& earlier) {
    if (!earlier.empty() && r.below(5) == 0) return earlier[r.below(earlier.size())];
    std::string name;
    const std::uint64_t len = r.below(16);
    for (std::uint64_t i = 0; i < len; ++i) {
        const std::uint64_t k = r.below(48);
        if (k < 40) name.push_back(static_cast<char>('A' + r.below(26)));
        else if (k == 40) name.push_back(' ');
        else if (k == 41) name.push_back('\0');
        else if (k == 42) name.push_back(static_cast<char>(1 + r.below(31)));
        else if (k == 43) name.push_back('\x7f');
        else name.push_back(static_cast<char>(0x80 | r.below(0x80)));
    }
    return name;
}

std::int64_t random_balance(Xorshift64& r) {
    switch (r.below(8)) {
        case 0: return 0;
        case 1: return kMax;
        case 2: return kMax / 2 + 1;
        case 3: return -static_cast<std::int64_t>(r.below(1000)) - 1;
        case 4: return kMin;
        default: return static_cast<std::int64_t>(r.below(1'000'000'000));
    }
}

bool snapshot_keeps_every_promise(const AccountSnapshot& s) {
    const auto assets = s.assets();
    if (assets.size() != s.asset_count() || assets.size() > kMaxAssets || s.timestamp_ms() <= 0) return false;
    for (std::size_t i = 0; i < assets.size(); ++i) {
        const std::string_view name = assets[i].asset_name();
        if (name.empty() || name.size() >= kAssetNameLen) return false;
        for (const char ch : name) {
            const auto c = static_cast<unsigned char>(ch);
            if (c < 0x21 || c > 0x7E) return false;
        }
        std::int64_t total = 0;
        if (assets[i].free_ticks() < 0 || assets[i].locked_ticks() < 0 || !assets[i].total(total)) return false;
        for (std::size_t j = 0; j < i; ++j) {
            if (assets[j].asset_name() == name) return false;
        }
        if (s.find(name) != &assets[i]) return false;
    }
    return true;
}

}  // namespace

TEST(AccountSnapshotFuzz, FI033NoSequenceOfAssetsMakesABrokenSnapshotOrDisturbsABuilderThatRefused) {
    Xorshift64 rng{0xD1B54A32D192ED03ULL};
    std::size_t accepted = 0, refused = 0, capacity_refusals = 0;
    for (int round = 0; round < 3000; ++round) {
        AccountSnapshotBuilder builder;
        std::vector<std::string> names;
        // Half the rounds are adversarial (names and balances from the nasty generators); the other half are
        // mostly valid and run past the capacity, so the 33rd-asset refusal is reached too.
        const bool adversarial = rng.below(2) == 0;
        const std::uint64_t adds = adversarial ? rng.below(12) : 26 + rng.below(14);
        for (std::uint64_t i = 0; i < adds; ++i) {
            const bool nasty = adversarial || rng.below(12) == 0;
            const std::string name = nasty ? random_name(rng, names) : "Q" + std::to_string(rng.below(1'000'000));
            const std::int64_t free_ticks =
                nasty ? random_balance(rng) : static_cast<std::int64_t>(rng.below(1'000'000'000));
            const std::int64_t locked_ticks =
                nasty ? random_balance(rng) : static_cast<std::int64_t>(rng.below(1'000'000));
            const std::size_t before = builder.asset_count();
            const AccountSnapshotError e = builder.add_asset(name, free_ticks, locked_ticks);
            if (e == AccountSnapshotError::None) {
                ++accepted;
                names.push_back(name);
                ASSERT_EQ(builder.asset_count(), before + 1);
            } else {
                ++refused;
                if (e == AccountSnapshotError::CapacityExceeded) ++capacity_refusals;
                ASSERT_EQ(builder.asset_count(), before) << "a refusal must change nothing";
            }
        }
        AccountSnapshot out;
        const std::int64_t ts = rng.below(10) == 0 ? 0 : static_cast<std::int64_t>(rng.below(1'000'000)) + 1;
        const AccountSnapshotError e = builder.build(ts, out);
        if (ts == 0) {
            ASSERT_EQ(e, AccountSnapshotError::TimestampNotPositive);
            ASSERT_EQ(out.timestamp_ms(), 0);  // untouched: still the default
            continue;
        }
        ASSERT_EQ(e, AccountSnapshotError::None);
        ASSERT_TRUE(snapshot_keeps_every_promise(out));
        ASSERT_EQ(out.asset_count(), builder.asset_count());
    }
    // the generator must reach the interesting outcomes, or the checks above ran on nothing
    EXPECT_GT(accepted, 5000u);
    EXPECT_GT(refused, 2000u);
    EXPECT_GT(capacity_refusals, 50u);
}

TEST(AccountSnapshotFuzz, FI033ReadersStayInsideTheirArraysWhateverTheStoredCountAndLengthSay) {
    Xorshift64 rng{0x2545F4914F6CDD1DULL};
    AccountSnapshot s = n_assets(kMaxAssets);
    for (int i = 0; i < 20'000; ++i) {
        AccountSnapshotTestHooks::set_raw_count(s, static_cast<std::size_t>(rng.next()));
        const std::size_t slot = rng.below(kMaxAssets);
        std::string bytes;
        for (std::size_t k = 0; k < kAssetNameLen; ++k) bytes.push_back(static_cast<char>(rng.below(256)));
        AccountSnapshotTestHooks::set_raw_name(s, slot, bytes, static_cast<std::uint8_t>(rng.below(256)));
        ASSERT_LE(s.asset_count(), kMaxAssets);
        ASSERT_EQ(s.assets().size(), s.asset_count());
        for (const AssetBalance& a : s.assets()) {
            ASSERT_LT(a.asset_name().size(), kAssetNameLen);
        }
        static_cast<void>(s.find("A1"));
        static_cast<void>(s.find(bytes));
    }
}

// ================================================================== budget (SAFE-02 evidence, not a gate)

// Run with --gtest_also_run_disabled_tests to print the numbers quoted in the PR. A timing assertion would be
// flaky on a shared runner; the factory is called once per account refresh (seconds apart), not per tick.
TEST(AccountSnapshotBudget, DISABLED_BuildingAFullSnapshotCostsMicroseconds) {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < kMaxAssets; ++i) names.push_back("ASSET" + std::to_string(i));
    constexpr int kRounds = 20'000;
    std::int64_t sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < kRounds; ++r) {
        AccountSnapshotBuilder b;
        b.set_can_trade(true);
        for (const std::string& n : names) {
            if (b.add_asset(n, 1'000'000 + r, 7) != AccountSnapshotError::None) FAIL();
        }
        AccountSnapshot out;
        if (b.build(1000 + r, out) != AccountSnapshotError::None) FAIL();
        sink += out.find("ASSET31")->free_ticks();
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns_per_snapshot =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / kRounds;
    std::printf("[budget] building a full %zu-asset snapshot through the factory: %.0f ns (%.2f us); sink=%lld\n",
                kMaxAssets, ns_per_snapshot, ns_per_snapshot / 1000.0, static_cast<long long>(sink));
    SUCCEED();
}
