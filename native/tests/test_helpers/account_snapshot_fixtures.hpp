// Test-only helpers for AccountSnapshot (account_truth.hpp, audit P1-003 / SAFE-02):
//   * make_account(): the fixtures' short way to a valid snapshot through AccountSnapshotBuilder;
//   * AccountSnapshotTestHooks: the friend that can write the private fields, to corrupt a value on purpose and
//     prove the accessors' clamps (find()/asset_count()/asset_name()) and the callers' handling of a snapshot
//     the factory would never have produced (a zero timestamp with can_trade set, ...).
//
// AccountSnapshotTestHooks is defined ONLY here, under native/tests/. Production code must never contain a class
// of this name: it would be a back door to every private field the factory exists to protect.
#pragma once

#include <hengyuan/account_truth.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string_view>

namespace hy {

class AccountSnapshotTestHooks {
public:
    static void set_raw_count(AccountSnapshot& s, std::size_t n) { s.count_ = n; }
    static void set_raw_timestamp(AccountSnapshot& s, std::int64_t ts_ms) { s.timestamp_ms_ = ts_ms; }
    static void set_raw_can_trade(AccountSnapshot& s, bool can_trade) { s.can_trade_ = can_trade; }

    // Overwrites slot `i`'s whole name buffer with `bytes` (at most kAssetNameLen of them; the rest is zeroed, and
    // nothing guarantees a terminator) and sets the stored length to `stored_len`, whatever the bytes say.
    static void set_raw_name(AccountSnapshot& s, std::size_t i, std::string_view bytes, std::uint8_t stored_len) {
        AssetBalance& slot = s.assets_[i];
        std::memset(slot.asset_, 0, sizeof(slot.asset_));
        std::memcpy(slot.asset_, bytes.data(),
                    bytes.size() < sizeof(slot.asset_) ? bytes.size() : sizeof(slot.asset_));
        slot.name_len_ = stored_len;
    }
    static void set_raw_balance(AccountSnapshot& s, std::size_t i, std::int64_t free_ticks,
                                std::int64_t locked_ticks) {
        s.assets_[i].free_ticks_ = free_ticks;
        s.assets_[i].locked_ticks_ = locked_ticks;
    }
};

namespace test_helpers {

// A valid snapshot for a fixture. The fixtures' data is constant, so a refusal is a test failure here (and the
// never-fetched default snapshot comes back, which fails the test that uses it far from the cause otherwise).
inline AccountSnapshot make_account(std::int64_t timestamp_ms, bool can_trade,
                                    std::initializer_list<AssetBalanceInput> assets) {
    AccountSnapshotBuilder builder;
    builder.set_can_trade(can_trade);
    for (const AssetBalanceInput& a : assets) {
        const AccountSnapshotError e = builder.add_asset(a.name, a.free_ticks, a.locked_ticks);
        if (e != AccountSnapshotError::None) {
            ADD_FAILURE() << "fixture asset '" << a.name << "' refused: " << account_snapshot_error_name(e);
            return AccountSnapshot{};
        }
    }
    AccountSnapshot out;
    const AccountSnapshotError e = builder.build(timestamp_ms, out);
    if (e != AccountSnapshotError::None) {
        ADD_FAILURE() << "fixture snapshot refused: " << account_snapshot_error_name(e);
        return AccountSnapshot{};
    }
    return out;
}

}  // namespace test_helpers
}  // namespace hy
