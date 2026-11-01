// SPDX-License-Identifier: proprietary
// test_store_binding.cpp -- D5 slice R5: the audit store's signed record of the exchange environment and the account it belongs to
// (store_binding.hpp: format, reader, judgement; store_binding_writer.hpp: write-once, the trading process's policy), and
// DurableAuditSink::frame_count(), which the policy uses to tell a new store from a legacy one.
//
// What these pin:
//   * the file: 70 bytes at fixed offsets, every byte of it protected by the MAC, bound to the store by its uuid, signed under the key
//     it names -- and a file that exists but cannot be believed is Unreadable, never "no binding";
//   * write once: a second bind with the same values writes nothing, with other values is a Conflict, and a file that cannot be read is
//     never overwritten; what was written reads back;
//   * the judgement: environment before account, an absent binding and an unreadable one are different answers;
//   * the trading process's policy: a store without frames is bound on first use, a legacy Testnet store is bound and says so, a legacy
//     Production store is refused, a bound store must match both, and only an open, unfenced, Trading-mode sink is accepted.

#include <gtest/gtest.h>
#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/store_binding.hpp>
#include <hengyuan/store_binding_writer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __linux__
#include <unistd.h>
#endif

using namespace hy;

namespace {

constexpr std::uint32_t kKeyId = 1;
constexpr std::uint32_t kKeyId2 = 2;
constexpr std::uint64_t kUid = 123'456'789;
constexpr std::int64_t kNow = 1'790'000'000'000;
// A store identity that is nobody's: the binding primitives take the uuid as a value, so most tests need no sink at all.
constexpr std::uint64_t kUuidLo = 0x1122334455667788ull;
constexpr std::uint64_t kUuidHi = 0x99AABBCCDDEEFF00ull;

std::vector<std::byte> bytes_of(const char* text) {
    const std::size_t n = std::strlen(text);
    std::vector<std::byte> k(n);
    std::memcpy(k.data(), text, n);
    return k;
}

// The same KEK and the same key 1 every time; `with_key2` adds a second key, so a ring without it is "the ring that does not know key 2".
std::unique_ptr<KeyRing> make_ring(bool with_key2) {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x40 + i);
    auto ring = std::make_unique<KeyRing>(kek);
    WrappedKeyRecord rec{};
    EXPECT_EQ(ring->add_key(kKeyId, bytes_of("store-binding-test-key-one"), rec), KeyRingAddStatus::Ok);
    if (with_key2) {  // (braces: EXPECT_EQ is an if/else, and GCC's -Werror=dangling-else refuses a bare one)
        EXPECT_EQ(ring->add_key(kKeyId2, bytes_of("store-binding-test-key-two"), rec), KeyRingAddStatus::Ok);
    }
    return ring;
}

AuditRecord intent_record(int n) {
    AuditRecord r{};
    r.timestamp_ms = 1000;
    r.event_type = AuditEventType::OrderIntentCreated;
    r.mode = ExecutionMode::Live;
    r.symbol_id = 0;
    r.set_client_order_id("HY-1789999399000-" + std::to_string(n) + "-0A0A");
    r.price_ticks = 500'000;
    r.qty_ticks = 100;
    r.resulting_state = OrderState::Intent;
    r.side = OrderSide::Sell;
    return r;
}

std::vector<std::byte> file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    if (!raw.empty()) std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

std::uint64_t le64(const std::vector<std::byte>& b, std::size_t at) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b[at + i])) << (8 * i);
    return v;
}

std::uint32_t le32(const std::vector<std::byte>& b, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[at + i])) << (8 * i);
    return v;
}

}  // namespace

class StoreBindingTest : public ::testing::Test {
protected:
    std::string stem_;
    std::string path_;  // the current store's base path
    std::vector<std::string> used_paths_;
    std::unique_ptr<KeyRing> ring_;      // keys 1 and 2
    std::unique_ptr<KeyRing> ring_one_;  // key 1 only

    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        stem_ = std::string(tmp) + "hy_storebind_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter);
#else
        stem_ = "/tmp/hy_storebind_" + std::to_string(getpid()) + "_" + std::to_string(counter);
#endif
        ring_ = make_ring(true);
        ring_one_ = make_ring(false);
        fresh_path();
    }

    void TearDown() override { remove_all(); }

    void remove_all() {
        std::error_code ec;
        for (const std::string& base : used_paths_) {
            for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".keyrotations", ".keyrotations.lock", ".keyrotations.tip",
                                       ".keyrotations.tip.tmp", ".storeid", ".storeid.lock", ".storeid.tip", ".storeid.tip.tmp",
                                       ".binding", ".binding.lock", ".binding.tip", ".binding.tip.tmp"}) {
                std::filesystem::remove(base + suffix, ec);
            }
        }
        used_paths_.clear();
    }

    // A new store name: a scenario that needs a second store gets a second name (deleting a file and re-creating one of the same name a
    // moment later races a real-time virus scanner on a Windows host).
    const std::string& fresh_path() {
        path_ = stem_ + "_" + std::to_string(used_paths_.size() + 1) + ".audit.log";
        used_paths_.push_back(path_);
        return path_;
    }

    std::unique_ptr<DurableAuditSink> open_sink(AuditSinkOpenMode mode = AuditSinkOpenMode::Trading) {
        return std::make_unique<DurableAuditSink>(path_, *ring_, kKeyId, kMaxIntentOrphans, mode);
    }

    void append_frames(DurableAuditSink& sink, int count) {
        for (int i = 0; i < count; ++i) ASSERT_TRUE(sink.append_durable(intent_record(i + 1), kNow + i).acked());
    }

    std::string binding_file() const { return store_binding_file_path(path_); }

    // The raw binding file, as written by the production writer.
    std::vector<std::byte> raw_binding() const { return file_bytes(binding_file()); }

    // Puts `bytes` where the binding lives, through the store's own atomic write (as the writer would, with whatever content).
    void plant_binding(const std::vector<std::byte>& bytes) {
        DurableLogStore store = detail::make_binding_store(path_);
        ASSERT_TRUE(store.write_tip_anchor(bytes));
    }

    // A validly signed file whose fields the test chooses (the writer refuses to write most of them).
    std::vector<std::byte> signed_file(std::uint64_t lo, std::uint64_t hi, StoreEnvironment env, std::uint64_t uid, std::uint32_t key_id = kKeyId) {
        std::array<std::byte, kKeyBlockSize> key{};
        EXPECT_TRUE(ring_->active_key(key_id, key));
        std::array<std::byte, kStoreBindingSize> buf{};
        detail::encode_store_binding(buf, lo, hi, StoreBinding{env, uid, kNow}, key_id, std::span<const std::byte>(key.data(), key.size()));
        return std::vector<std::byte>(buf.begin(), buf.end());
    }

    StoreBindingRead read(const KeyRing& ring) const { return read_store_binding(path_, ring, kUuidLo, kUuidHi); }
    StoreBindingRead read() const { return read(*ring_); }

    BindStatus bind(StoreEnvironment env, std::uint64_t uid = kUid, std::int64_t at = kNow, std::uint32_t key_id = kKeyId) {
        return bind_store(path_, *ring_, key_id, kUuidLo, kUuidHi, StoreBinding{env, uid, at});
    }
};

// --- the file ---------------------------------------------------------------------------------------------------------------------------

static_assert(kStoreBindingSize == 70, "the on-disk size is part of the format");
static_assert(kStoreBindingFormatVersion == 1);

TEST_F(StoreBindingTest, TheFileIsSeventyBytesAtFixedOffsets) {
    ASSERT_EQ(bind_store(path_, *ring_, kKeyId2, kUuidLo, kUuidHi, StoreBinding{StoreEnvironment::Production, 0x0102030405060708ull, 0x1112131415161718}),
              BindStatus::Written);
    const std::vector<std::byte> raw = raw_binding();
    ASSERT_EQ(raw.size(), 70u);
    EXPECT_EQ(std::to_integer<int>(raw[0]), 1) << "format version";
    EXPECT_EQ(le64(raw, 1), kUuidLo);
    EXPECT_EQ(le64(raw, 9), kUuidHi);
    EXPECT_EQ(std::to_integer<int>(raw[17]), 2) << "Production is 2";
    EXPECT_EQ(le64(raw, 18), 0x0102030405060708ull);
    EXPECT_EQ(le64(raw, 26), 0x1112131415161718ull);
    EXPECT_EQ(le32(raw, 34), kKeyId2) << "the key id the file is signed under";
    // the last 32 bytes are the MAC, and they are not zero
    bool any = false;
    for (std::size_t i = 38; i < 70; ++i) any = any || raw[i] != std::byte{0};
    EXPECT_TRUE(any);
    EXPECT_EQ(store_binding_file_path("x/audit.log"), "x/audit.log.binding.tip");
}

TEST_F(StoreBindingTest, EnvironmentCodesAreOneAndTwoAndZeroIsNoEnvironment) {
    EXPECT_EQ(static_cast<int>(StoreEnvironment::Testnet), 1);
    EXPECT_EQ(static_cast<int>(StoreEnvironment::Production), 2);
    EXPECT_TRUE(is_valid_store_environment(StoreEnvironment::Testnet));
    EXPECT_TRUE(is_valid_store_environment(StoreEnvironment::Production));
    for (const int v : {0, 3, 255}) EXPECT_FALSE(is_valid_store_environment(static_cast<StoreEnvironment>(v))) << v;
    EXPECT_EQ(store_environment_name(StoreEnvironment::Testnet), "TESTNET");
    EXPECT_EQ(store_environment_name(StoreEnvironment::Production), "PRODUCTION");
    EXPECT_EQ(store_environment_name(static_cast<StoreEnvironment>(0)), "");
}

TEST_F(StoreBindingTest, EveryByteOfTheFileIsProtectedByTheMac) {
    const std::vector<std::byte> good = signed_file(kUuidLo, kUuidHi, StoreEnvironment::Production, kUid);
    std::array<std::byte, kKeyBlockSize> key{};
    ASSERT_TRUE(ring_->active_key(kKeyId, key));
    const std::span<const std::byte> hmac_key(key.data(), key.size());
    detail::DecodedStoreBinding decoded{};
    ASSERT_TRUE(detail::decode_store_binding(good, hmac_key, decoded)) << "the control: the untouched file decodes";
    EXPECT_EQ(decoded.uuid_lo, kUuidLo);
    EXPECT_EQ(decoded.uuid_hi, kUuidHi);
    EXPECT_EQ(decoded.binding.environment, StoreEnvironment::Production);
    EXPECT_EQ(decoded.binding.account_uid, kUid);
    EXPECT_EQ(decoded.binding.bound_at_ms, kNow);
    EXPECT_EQ(decoded.key_id, kKeyId);
    for (std::size_t i = 0; i < good.size(); ++i) {
        for (const std::uint8_t flip : {std::uint8_t{0x01}, std::uint8_t{0x80}}) {
            std::vector<std::byte> bad = good;
            bad[i] ^= static_cast<std::byte>(flip);
            detail::DecodedStoreBinding out{};
            EXPECT_FALSE(detail::decode_store_binding(bad, hmac_key, out)) << "byte " << i << " flip " << static_cast<int>(flip);
        }
    }
}

TEST_F(StoreBindingTest, TheKeyIdIsReadBeforeTheMacCanBeChecked) {
    const std::vector<std::byte> good = signed_file(kUuidLo, kUuidHi, StoreEnvironment::Testnet, kUid, kKeyId2);
    std::uint32_t id = 0;
    ASSERT_TRUE(detail::peek_store_binding_key_id(good, id));
    EXPECT_EQ(id, kKeyId2);
    // 38 bytes are enough to name the key, 37 are not
    EXPECT_TRUE(detail::peek_store_binding_key_id(std::span<const std::byte>(good.data(), 38), id));
    EXPECT_FALSE(detail::peek_store_binding_key_id(std::span<const std::byte>(good.data(), 37), id));
    EXPECT_FALSE(detail::peek_store_binding_key_id(std::span<const std::byte>(good.data(), 0), id));
}

// --- the reader ---------------------------------------------------------------------------------------------------------------------------

TEST_F(StoreBindingTest, NoFileIsUnbound) {
    const StoreBindingRead r = read();
    EXPECT_EQ(r.status, StoreBindingStatus::Unbound);
    EXPECT_EQ(StoreBindingRead{}.status, StoreBindingStatus::Unbound) << "a default-constructed read is 'no file'";
    // and it needs no key to say so
    EXPECT_EQ(read(*make_ring(false)).status, StoreBindingStatus::Unbound);
}

TEST_F(StoreBindingTest, WhatWasBoundReadsBackWhateverTheEnvironmentAndTheClock) {
    for (const StoreEnvironment env : {StoreEnvironment::Testnet, StoreEnvironment::Production}) {
        for (const std::int64_t at : {std::int64_t{0}, kNow, std::int64_t{-1}, std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
            fresh_path();
            ASSERT_EQ(bind(env, kUid, at), BindStatus::Written);
            const StoreBindingRead r = read();
            ASSERT_EQ(r.status, StoreBindingStatus::Bound);
            EXPECT_EQ(r.binding.environment, env);
            EXPECT_EQ(r.binding.account_uid, kUid);
            EXPECT_EQ(r.binding.bound_at_ms, at) << "informational, but signed and read back exactly";
        }
    }
    fresh_path();
    ASSERT_EQ(bind(StoreEnvironment::Production, std::numeric_limits<std::uint64_t>::max()), BindStatus::Written);
    EXPECT_EQ(read().binding.account_uid, std::numeric_limits<std::uint64_t>::max()) << "the whole 64 bits of the uid";
}

TEST_F(StoreBindingTest, ASizeThatIsNotSeventyIsUnreadable) {
    const std::vector<std::byte> good = signed_file(kUuidLo, kUuidHi, StoreEnvironment::Production, kUid);
    for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{37}, std::size_t{38}, std::size_t{53}, std::size_t{69}, std::size_t{71}, std::size_t{140}}) {
        std::vector<std::byte> shaped(size, std::byte{0});
        if (size != 0) std::memcpy(shaped.data(), good.data(), std::min(size, good.size()));  // (an empty vector's data() may be null: UBSan)
        plant_binding(shaped);
        EXPECT_EQ(read().status, StoreBindingStatus::Unreadable) << size;
    }
    plant_binding(good);
    EXPECT_EQ(read().status, StoreBindingStatus::Bound) << "the control: the right size, the right content";
}

TEST_F(StoreBindingTest, ATamperedFileIsUnreadableNotUnbound) {
    std::vector<std::byte> good = signed_file(kUuidLo, kUuidHi, StoreEnvironment::Testnet, kUid);
    good[18] ^= std::byte{0x01};  // the account uid, one bit: the file now says another account
    plant_binding(good);
    EXPECT_EQ(read().status, StoreBindingStatus::Unreadable);
    // all zeros, all ones, a copy of the store identity's shape
    plant_binding(std::vector<std::byte>(70, std::byte{0}));
    EXPECT_EQ(read().status, StoreBindingStatus::Unreadable);
    plant_binding(std::vector<std::byte>(70, std::byte{0xFF}));
    EXPECT_EQ(read().status, StoreBindingStatus::Unreadable);
}

TEST_F(StoreBindingTest, TheFileOfAnotherStoreIsUnreadable) {
    ASSERT_EQ(bind(StoreEnvironment::Production), BindStatus::Written);
    ASSERT_EQ(read().status, StoreBindingStatus::Bound);
    EXPECT_EQ(read_store_binding(path_, *ring_, kUuidLo, kUuidHi + 1).status, StoreBindingStatus::Unreadable) << "another uuid, high half";
    EXPECT_EQ(read_store_binding(path_, *ring_, kUuidLo + 1, kUuidHi).status, StoreBindingStatus::Unreadable) << "another uuid, low half";
    EXPECT_EQ(read_store_binding(path_, *ring_, kUuidHi, kUuidLo).status, StoreBindingStatus::Unreadable) << "the halves swapped";
}

TEST_F(StoreBindingTest, AStoreWithoutAnIdentityHasNothingAFileCouldBelongTo) {
    ASSERT_EQ(bind(StoreEnvironment::Production), BindStatus::Written);
    EXPECT_EQ(read_store_binding(path_, *ring_, 0, 0).status, StoreBindingStatus::Unreadable) << "a file exists, and there is no store for it";
    // and a file that claims uuid 0/0 does not belong to an identity-less store either
    plant_binding(signed_file(0, 0, StoreEnvironment::Production, kUid));
    EXPECT_EQ(read_store_binding(path_, *ring_, 0, 0).status, StoreBindingStatus::Unreadable);
    fresh_path();
    EXPECT_EQ(read_store_binding(path_, *ring_, 0, 0).status, StoreBindingStatus::Unbound) << "no file stays 'no file'";
}

TEST_F(StoreBindingTest, AKeyTheRingDoesNotHaveIsUnreadable) {
    ASSERT_EQ(bind(StoreEnvironment::Production, kUid, kNow, kKeyId2), BindStatus::Written);
    EXPECT_EQ(read(*ring_).status, StoreBindingStatus::Bound);
    EXPECT_EQ(read(*ring_one_).status, StoreBindingStatus::Unreadable) << "signed under key 2, which this ring does not hold";
    // and a file signed under key 1 reads under either ring, because the key is the one the file names
    fresh_path();
    ASSERT_EQ(bind(StoreEnvironment::Production, kUid, kNow, kKeyId), BindStatus::Written);
    EXPECT_EQ(read(*ring_).status, StoreBindingStatus::Bound);
    EXPECT_EQ(read(*ring_one_).status, StoreBindingStatus::Bound);
}

TEST_F(StoreBindingTest, AValidlySignedFileThatNamesNoEnvironmentOrNoAccountIsUnreadable) {
    for (const int env : {0, 3, 255}) {
        plant_binding(signed_file(kUuidLo, kUuidHi, static_cast<StoreEnvironment>(env), kUid));
        EXPECT_EQ(read().status, StoreBindingStatus::Unreadable) << "environment " << env;
    }
    plant_binding(signed_file(kUuidLo, kUuidHi, StoreEnvironment::Production, 0));
    EXPECT_EQ(read().status, StoreBindingStatus::Unreadable) << "account uid 0";
    plant_binding(signed_file(kUuidLo, kUuidHi, StoreEnvironment::Production, kUid));
    EXPECT_EQ(read().status, StoreBindingStatus::Bound) << "the control";
}

TEST_F(StoreBindingTest, AVersionOtherThanOneIsUnreadableEvenWithAMacThatWouldVerify) {
    // a file with version byte 2 and a MAC computed over THAT content: the version, not the MAC, refuses it
    std::vector<std::byte> v2 = signed_file(kUuidLo, kUuidHi, StoreEnvironment::Production, kUid);
    v2[0] = std::byte{2};
    std::array<std::byte, kKeyBlockSize> key{};
    ASSERT_TRUE(ring_->active_key(kKeyId, key));
    const auto mac = crypto::hmac_sha256(std::span<const std::byte>(key.data(), key.size()), std::span<const std::byte>(v2.data(), 38));
    std::memcpy(v2.data() + 38, mac.bytes.data(), 32);
    plant_binding(v2);
    EXPECT_EQ(read().status, StoreBindingStatus::Unreadable);
}

TEST_F(StoreBindingTest, ADirectoryWhereTheFileShouldBeIsUnreadableNotUnbound) {
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directory(binding_file(), ec)) << ec.message();
    EXPECT_EQ(read().status, StoreBindingStatus::Unreadable) << "something is there and it cannot be read: not 'no binding'";
    EXPECT_EQ(bind(StoreEnvironment::Production), BindStatus::Unreadable) << "and it is not written over";
    std::filesystem::remove(binding_file(), ec);
}

// --- the writer -----------------------------------------------------------------------------------------------------------------------------

TEST_F(StoreBindingTest, ABindingIsWrittenOnceAndNeverChanged) {
    ASSERT_EQ(bind(StoreEnvironment::Production, kUid, kNow), BindStatus::Written);
    const std::vector<std::byte> first = raw_binding();
    EXPECT_EQ(bind(StoreEnvironment::Production, kUid, kNow + 5000), BindStatus::AlreadyBound) << "the same environment and account: nothing to do, whatever the clock says";
    EXPECT_EQ(raw_binding(), first) << "and the file, with its original bound_at, is untouched";
    EXPECT_EQ(bind(StoreEnvironment::Testnet, kUid), BindStatus::Conflict) << "another environment";
    EXPECT_EQ(raw_binding(), first);
    EXPECT_EQ(bind(StoreEnvironment::Production, kUid + 1), BindStatus::Conflict) << "another account";
    EXPECT_EQ(raw_binding(), first);
    EXPECT_EQ(bind(StoreEnvironment::Testnet, kUid + 1), BindStatus::Conflict) << "both";
    EXPECT_EQ(raw_binding(), first);
    const StoreBindingRead r = read();
    ASSERT_EQ(r.status, StoreBindingStatus::Bound);
    EXPECT_EQ(r.binding.environment, StoreEnvironment::Production);
    EXPECT_EQ(r.binding.account_uid, kUid);
    EXPECT_EQ(r.binding.bound_at_ms, kNow);
}

TEST_F(StoreBindingTest, ABindingFileThatCannotBeBelievedIsNeverOverwritten) {
    plant_binding(std::vector<std::byte>(70, std::byte{0x5A}));
    const std::vector<std::byte> junk = raw_binding();
    EXPECT_EQ(bind(StoreEnvironment::Testnet), BindStatus::Unreadable);
    EXPECT_EQ(bind(StoreEnvironment::Production), BindStatus::Unreadable);
    EXPECT_EQ(raw_binding(), junk);
    // another store's file is the same: it is not ours to replace
    fresh_path();
    ASSERT_EQ(bind_store(path_, *ring_, kKeyId, kUuidLo + 7, kUuidHi, StoreBinding{StoreEnvironment::Production, kUid, kNow}), BindStatus::Written);
    const std::vector<std::byte> theirs = raw_binding();
    EXPECT_EQ(bind(StoreEnvironment::Production), BindStatus::Unreadable);
    EXPECT_EQ(raw_binding(), theirs);
}

TEST_F(StoreBindingTest, BadArgumentsWriteNothing) {
    for (const int env : {0, 3, 255}) {
        EXPECT_EQ(bind_store(path_, *ring_, kKeyId, kUuidLo, kUuidHi, StoreBinding{static_cast<StoreEnvironment>(env), kUid, kNow}), BindStatus::BadArguments) << env;
    }
    EXPECT_EQ(bind(StoreEnvironment::Production, 0), BindStatus::BadArguments) << "account uid 0";
    EXPECT_EQ(bind_store(path_, *ring_, kKeyId, 0, 0, StoreBinding{StoreEnvironment::Production, kUid, kNow}), BindStatus::StoreNotIdentified);
    EXPECT_FALSE(std::filesystem::exists(binding_file()));
    // a store whose identity has only one zero half is an identity
    EXPECT_EQ(bind_store(path_, *ring_, kKeyId, 0, 5, StoreBinding{StoreEnvironment::Production, kUid, kNow}), BindStatus::Written);
    EXPECT_EQ(read_store_binding(path_, *ring_, 0, 5).status, StoreBindingStatus::Bound);
}

TEST_F(StoreBindingTest, AKeyTheRingLacksWritesNothing) {
    EXPECT_EQ(bind(StoreEnvironment::Production, kUid, kNow, 9), BindStatus::WriteFailed);
    EXPECT_FALSE(std::filesystem::exists(binding_file()));
    // and it is signed under the key it is given, not under another one the ring happens to hold
    EXPECT_EQ(bind(StoreEnvironment::Production, kUid, kNow, kKeyId2), BindStatus::Written);
    EXPECT_EQ(le32(raw_binding(), 34), kKeyId2);
}

TEST_F(StoreBindingTest, AWriteThatFailsLeavesNoBindingAndIsReportedAsFailed) {
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directory(binding_file() + ".tmp", ec)) << ec.message();  // the temp file cannot be created
    EXPECT_EQ(bind(StoreEnvironment::Production), BindStatus::WriteFailed);
    EXPECT_FALSE(std::filesystem::exists(binding_file())) << "no half-written binding";
    EXPECT_EQ(read().status, StoreBindingStatus::Unbound);
    std::filesystem::remove(binding_file() + ".tmp", ec);
    EXPECT_EQ(bind(StoreEnvironment::Production), BindStatus::Written) << "and the next attempt works";
}

TEST_F(StoreBindingTest, EveryStatusHasAName) {
    for (const auto& [s, n] : {std::pair{BindStatus::NotRun, "NotRun"}, std::pair{BindStatus::Written, "Written"},
                               std::pair{BindStatus::AlreadyBound, "AlreadyBound"}, std::pair{BindStatus::Conflict, "Conflict"},
                               std::pair{BindStatus::Unreadable, "Unreadable"}, std::pair{BindStatus::StoreNotIdentified, "StoreNotIdentified"},
                               std::pair{BindStatus::BadArguments, "BadArguments"}, std::pair{BindStatus::WriteFailed, "WriteFailed"}}) {
        EXPECT_EQ(bind_status_name(s), n);
    }
    EXPECT_EQ(bind_status_name(static_cast<BindStatus>(200)), "Unknown");
    EXPECT_EQ(static_cast<int>(BindStatus::NotRun), 0) << "the zero value is not a verdict";
    EXPECT_EQ(static_cast<int>(EnsureBindingStatus::NotRun), 0);
    EXPECT_EQ(store_binding_status_name(StoreBindingStatus::Unbound), "Unbound");
    EXPECT_EQ(store_binding_status_name(StoreBindingStatus::Bound), "Bound");
    EXPECT_EQ(store_binding_status_name(StoreBindingStatus::Unreadable), "Unreadable");
    EXPECT_EQ(store_binding_status_name(static_cast<StoreBindingStatus>(9)), "Unknown");
    for (const auto& [j, n] : {std::pair{BindingJudgement::Match, "Match"}, std::pair{BindingJudgement::EnvironmentMismatch, "EnvironmentMismatch"},
                               std::pair{BindingJudgement::AccountMismatch, "AccountMismatch"}, std::pair{BindingJudgement::Unbound, "Unbound"},
                               std::pair{BindingJudgement::Unreadable, "Unreadable"}}) {
        EXPECT_EQ(binding_judgement_name(j), n);
    }
    EXPECT_EQ(binding_judgement_name(static_cast<BindingJudgement>(9)), "Unknown");
    for (const auto& [s, n] : {std::pair{EnsureBindingStatus::NotRun, "NotRun"}, std::pair{EnsureBindingStatus::Verified, "Verified"},
                               std::pair{EnsureBindingStatus::BoundNew, "BoundNew"}, std::pair{EnsureBindingStatus::BoundLegacyTestnet, "BoundLegacyTestnet"},
                               std::pair{EnsureBindingStatus::EnvironmentMismatch, "EnvironmentMismatch"},
                               std::pair{EnsureBindingStatus::AccountMismatch, "AccountMismatch"}, std::pair{EnsureBindingStatus::Unreadable, "Unreadable"},
                               std::pair{EnsureBindingStatus::UnboundProductionStoreWithHistory, "UnboundProductionStoreWithHistory"},
                               std::pair{EnsureBindingStatus::StoreNotIdentified, "StoreNotIdentified"},
                               std::pair{EnsureBindingStatus::WriteFailed, "WriteFailed"}, std::pair{EnsureBindingStatus::BadArguments, "BadArguments"},
                               std::pair{EnsureBindingStatus::StoreNotUsable, "StoreNotUsable"}}) {
        EXPECT_EQ(ensure_binding_status_name(s), n);
    }
    EXPECT_EQ(ensure_binding_status_name(static_cast<EnsureBindingStatus>(200)), "Unknown");
}

// --- the judgement -------------------------------------------------------------------------------------------------------------------------

TEST(StoreBindingJudgement, TheTruthTable) {
    const auto bound = [](StoreEnvironment e, std::uint64_t uid) {
        StoreBindingRead r;
        r.status = StoreBindingStatus::Bound;
        r.binding = StoreBinding{e, uid, 0};
        return r;
    };
    constexpr StoreEnvironment T = StoreEnvironment::Testnet;
    constexpr StoreEnvironment P = StoreEnvironment::Production;
    EXPECT_EQ(judge_store_binding(bound(P, 7), P, 7), BindingJudgement::Match);
    EXPECT_EQ(judge_store_binding(bound(T, 7), T, 7), BindingJudgement::Match);
    EXPECT_EQ(judge_store_binding(bound(P, 7), T, 7), BindingJudgement::EnvironmentMismatch);
    EXPECT_EQ(judge_store_binding(bound(T, 7), P, 7), BindingJudgement::EnvironmentMismatch);
    EXPECT_EQ(judge_store_binding(bound(P, 7), P, 8), BindingJudgement::AccountMismatch);
    EXPECT_EQ(judge_store_binding(bound(T, 7), T, 8), BindingJudgement::AccountMismatch);
    EXPECT_EQ(judge_store_binding(bound(P, 7), T, 8), BindingJudgement::EnvironmentMismatch) << "the environment is said first";
    EXPECT_EQ(judge_store_binding(bound(T, 7), P, 8), BindingJudgement::EnvironmentMismatch);
    // all 64 bits of the account count
    EXPECT_EQ(judge_store_binding(bound(P, 7), P, 7 + (std::uint64_t{1} << 63)), BindingJudgement::AccountMismatch);
    EXPECT_EQ(judge_store_binding(bound(P, 7), P, 7 + (std::uint64_t{1} << 32)), BindingJudgement::AccountMismatch);
    // no file and an unreadable file are different answers, whatever the process says it is
    for (const StoreEnvironment e : {T, P}) {
        EXPECT_EQ(judge_store_binding(StoreBindingRead{}, e, 7), BindingJudgement::Unbound);
        EXPECT_EQ(judge_store_binding(StoreBindingRead{StoreBindingStatus::Unreadable, {}}, e, 7), BindingJudgement::Unreadable);
        // a stale binding left in an Unbound/Unreadable read is not consulted
        EXPECT_EQ(judge_store_binding(StoreBindingRead{StoreBindingStatus::Unbound, StoreBinding{e, 7, 0}}, e, 7), BindingJudgement::Unbound);
        EXPECT_EQ(judge_store_binding(StoreBindingRead{StoreBindingStatus::Unreadable, StoreBinding{e, 7, 0}}, e, 7), BindingJudgement::Unreadable);
    }
    static_assert(judge_store_binding(StoreBindingRead{}, StoreEnvironment::Production, 1) == BindingJudgement::Unbound, "pure and constexpr");
}

// --- the trading process's policy ----------------------------------------------------------------------------------------------------------

TEST_F(StoreBindingTest, AStoreWithoutFramesIsBoundOnFirstUseAndVerifiedAfterwards) {
    for (const StoreEnvironment env : {StoreEnvironment::Testnet, StoreEnvironment::Production}) {
        fresh_path();
        auto sink = open_sink();
        ASSERT_TRUE(sink->is_open());
        ASSERT_EQ(sink->frame_count(), 0u);
        ASSERT_FALSE(sink->store_uuid_lo() == 0 && sink->store_uuid_hi() == 0) << "the sink established an identity";
        EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, env, kUid, kNow), EnsureBindingStatus::BoundNew);
        EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, env, kUid, kNow + 1), EnsureBindingStatus::Verified);
        const StoreBindingRead r = read_store_binding(path_, *ring_, sink->store_uuid_lo(), sink->store_uuid_hi());
        ASSERT_EQ(r.status, StoreBindingStatus::Bound);
        EXPECT_EQ(r.binding.environment, env);
        EXPECT_EQ(r.binding.account_uid, kUid);
        EXPECT_EQ(r.binding.bound_at_ms, kNow) << "the first call's clock: the second did not rewrite it";
        EXPECT_TRUE(store_may_be_used(EnsureBindingStatus::BoundNew));
        EXPECT_TRUE(store_may_be_used(EnsureBindingStatus::Verified));
    }
}

TEST_F(StoreBindingTest, ALegacyTestnetStoreWithHistoryIsBoundAndSaysSo) {
    auto sink = open_sink();
    append_frames(*sink, 3);
    ASSERT_EQ(sink->frame_count(), 3u);
    EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Testnet, kUid, kNow), EnsureBindingStatus::BoundLegacyTestnet);
    EXPECT_TRUE(store_may_be_used(EnsureBindingStatus::BoundLegacyTestnet));
    EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Testnet, kUid, kNow), EnsureBindingStatus::Verified) << "bound now, like any other";
}

TEST_F(StoreBindingTest, ALegacyProductionStoreWithHistoryIsRefusedAndNothingIsWritten) {
    auto sink = open_sink();
    append_frames(*sink, 1);
    EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Production, kUid, kNow), EnsureBindingStatus::UnboundProductionStoreWithHistory);
    EXPECT_FALSE(store_may_be_used(EnsureBindingStatus::UnboundProductionStoreWithHistory));
    EXPECT_FALSE(std::filesystem::exists(binding_file())) << "a store is not bound to Production after the fact";
    EXPECT_EQ(read_store_binding(path_, *ring_, sink->store_uuid_lo(), sink->store_uuid_hi()).status, StoreBindingStatus::Unbound);
    // asking again changes nothing, and a Testnet claim on the same store is still the legacy Testnet case
    EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Production, kUid, kNow), EnsureBindingStatus::UnboundProductionStoreWithHistory);
}

TEST_F(StoreBindingTest, ABoundStoreMustMatchBothTheEnvironmentAndTheAccount) {
    auto sink = open_sink();
    ASSERT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Production, kUid, kNow), EnsureBindingStatus::BoundNew);
    const std::vector<std::byte> bound = raw_binding();
    EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Testnet, kUid, kNow), EnsureBindingStatus::EnvironmentMismatch);
    EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Production, kUid + 1, kNow), EnsureBindingStatus::AccountMismatch);
    EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Testnet, kUid + 1, kNow), EnsureBindingStatus::EnvironmentMismatch);
    EXPECT_EQ(raw_binding(), bound) << "a refusal changes nothing";
    EXPECT_FALSE(store_may_be_used(EnsureBindingStatus::EnvironmentMismatch));
    EXPECT_FALSE(store_may_be_used(EnsureBindingStatus::AccountMismatch));
    // the same with frames in the store: history does not soften a mismatch
    append_frames(*sink, 2);
    EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Testnet, kUid, kNow), EnsureBindingStatus::EnvironmentMismatch);
    EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Production, kUid, kNow), EnsureBindingStatus::Verified);
}

TEST_F(StoreBindingTest, ABindingThatCannotBeReadRefusesEveryProcessWhateverItIs) {
    auto sink = open_sink();
    plant_binding(std::vector<std::byte>(70, std::byte{0x33}));
    for (const StoreEnvironment env : {StoreEnvironment::Testnet, StoreEnvironment::Production}) {
        EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, env, kUid, kNow), EnsureBindingStatus::Unreadable);
    }
    EXPECT_FALSE(store_may_be_used(EnsureBindingStatus::Unreadable));
    append_frames(*sink, 1);
    EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Testnet, kUid, kNow), EnsureBindingStatus::Unreadable) << "with history too";
}

TEST_F(StoreBindingTest, ABindingFromAnotherStoreRefusesThisOne) {
    auto sink = open_sink();
    // the file is perfectly signed, for a store that is not this one
    plant_binding(signed_file(sink->store_uuid_lo() + 1, sink->store_uuid_hi(), StoreEnvironment::Production, kUid));
    EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Production, kUid, kNow), EnsureBindingStatus::Unreadable);
}

TEST_F(StoreBindingTest, BadArgumentsAndStoresWithoutAnIdentityAreRefusedBeforeAnythingIsRead) {
    EXPECT_EQ(ensure_store_binding(path_, *ring_, kKeyId, kUuidLo, kUuidHi, false, static_cast<StoreEnvironment>(0), kUid, kNow), EnsureBindingStatus::BadArguments);
    EXPECT_EQ(ensure_store_binding(path_, *ring_, kKeyId, kUuidLo, kUuidHi, false, static_cast<StoreEnvironment>(3), kUid, kNow), EnsureBindingStatus::BadArguments);
    EXPECT_EQ(ensure_store_binding(path_, *ring_, kKeyId, kUuidLo, kUuidHi, false, StoreEnvironment::Production, 0, kNow), EnsureBindingStatus::BadArguments);
    EXPECT_EQ(ensure_store_binding(path_, *ring_, kKeyId, 0, 0, false, StoreEnvironment::Production, kUid, kNow), EnsureBindingStatus::StoreNotIdentified);
    EXPECT_FALSE(std::filesystem::exists(binding_file()));
    EXPECT_FALSE(store_may_be_used(EnsureBindingStatus::BadArguments));
    EXPECT_FALSE(store_may_be_used(EnsureBindingStatus::StoreNotIdentified));
    EXPECT_FALSE(store_may_be_used(EnsureBindingStatus::WriteFailed));
    EXPECT_FALSE(store_may_be_used(EnsureBindingStatus::StoreNotUsable));
    EXPECT_FALSE(store_may_be_used(EnsureBindingStatus::NotRun));
}

TEST_F(StoreBindingTest, ABadClaimIsRefusedAsOneEvenForABoundStoreAndAMissingIdentityEvenNextToAFile) {
    ASSERT_EQ(bind(StoreEnvironment::Production), BindStatus::Written);
    // the claim is looked at before the file: an environment that is none, or no account, is a bad claim -- not a "mismatch" with the binding
    EXPECT_EQ(ensure_store_binding(path_, *ring_, kKeyId, kUuidLo, kUuidHi, false, static_cast<StoreEnvironment>(0), kUid, kNow), EnsureBindingStatus::BadArguments);
    EXPECT_EQ(ensure_store_binding(path_, *ring_, kKeyId, kUuidLo, kUuidHi, false, static_cast<StoreEnvironment>(3), kUid, kNow), EnsureBindingStatus::BadArguments);
    EXPECT_EQ(ensure_store_binding(path_, *ring_, kKeyId, kUuidLo, kUuidHi, false, StoreEnvironment::Production, 0, kNow), EnsureBindingStatus::BadArguments);
    // and a store with no identity is not identified, whatever file lies next to it (which would otherwise read as Unreadable)
    EXPECT_EQ(ensure_store_binding(path_, *ring_, kKeyId, 0, 0, false, StoreEnvironment::Production, kUid, kNow), EnsureBindingStatus::StoreNotIdentified);
}

TEST_F(StoreBindingTest, AWriteFailureIsAFailureAndTheStoreMustNotBeUsed) {
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directory(binding_file() + ".tmp", ec)) << ec.message();
    EXPECT_EQ(ensure_store_binding(path_, *ring_, kKeyId, kUuidLo, kUuidHi, false, StoreEnvironment::Testnet, kUid, kNow), EnsureBindingStatus::WriteFailed);
    EXPECT_EQ(ensure_store_binding(path_, *ring_, 9, kUuidLo, kUuidHi, false, StoreEnvironment::Testnet, kUid, kNow), EnsureBindingStatus::WriteFailed) << "a key the ring lacks";
    std::filesystem::remove(binding_file() + ".tmp", ec);
}

TEST_F(StoreBindingTest, TheFullPolicyOnPlainValues) {
    // history x environment x what the file says: the whole table through the value overload
    struct Row {
        bool history;
        StoreEnvironment env;
        EnsureBindingStatus want;
    };
    for (const Row& row : {Row{false, StoreEnvironment::Testnet, EnsureBindingStatus::BoundNew}, Row{false, StoreEnvironment::Production, EnsureBindingStatus::BoundNew},
                           Row{true, StoreEnvironment::Testnet, EnsureBindingStatus::BoundLegacyTestnet},
                           Row{true, StoreEnvironment::Production, EnsureBindingStatus::UnboundProductionStoreWithHistory}}) {
        fresh_path();
        EXPECT_EQ(ensure_store_binding(path_, *ring_, kKeyId, kUuidLo, kUuidHi, row.history, row.env, kUid, kNow), row.want)
            << (row.history ? "history " : "no history ") << store_environment_name(row.env);
        const bool wrote = row.want == EnsureBindingStatus::BoundNew || row.want == EnsureBindingStatus::BoundLegacyTestnet;
        EXPECT_EQ(std::filesystem::exists(binding_file()), wrote);
    }
}

// --- the sink -------------------------------------------------------------------------------------------------------------------------------

TEST_F(StoreBindingTest, FrameCountIsTheNumberOfFramesTheLogHoldsAndSurvivesARestart) {
    {
        auto sink = open_sink();
        ASSERT_TRUE(sink->is_open());
        EXPECT_EQ(sink->frame_count(), 0u);
        append_frames(*sink, 1);
        EXPECT_EQ(sink->frame_count(), 1u);
        append_frames(*sink, 2);
        EXPECT_EQ(sink->frame_count(), 3u);
    }
    auto again = open_sink();
    ASSERT_TRUE(again->is_open());
    ASSERT_FALSE(again->fenced());
    EXPECT_EQ(again->frame_count(), 3u) << "recovery counted what the log holds";
    // an OperatorOpen scan counts the same
    again.reset();
    auto operator_open = open_sink(AuditSinkOpenMode::OperatorOpen);
    ASSERT_TRUE(operator_open->is_open());
    EXPECT_EQ(operator_open->frame_count(), 3u);
}

TEST_F(StoreBindingTest, TheSinkOverloadAcceptsOnlyAnOpenUnfencedTradingSink) {
    {
        // an operator-mode sink: it holds the lock, but it is not a trading process
        auto op = open_sink(AuditSinkOpenMode::OperatorOpen);
        ASSERT_TRUE(op->is_open());
        EXPECT_EQ(ensure_store_binding(*op, path_, *ring_, StoreEnvironment::Production, kUid, kNow), EnsureBindingStatus::StoreNotUsable);
        EXPECT_FALSE(std::filesystem::exists(binding_file())) << "and nothing was written for it";
    }
    {
        // a second sink over the same store: the first holds the lock, so this one is not open
        auto first = open_sink();
        ASSERT_TRUE(first->is_open());
        auto second = open_sink();
        ASSERT_FALSE(second->is_open());
        EXPECT_EQ(ensure_store_binding(*second, path_, *ring_, StoreEnvironment::Production, kUid, kNow), EnsureBindingStatus::StoreNotUsable);
        EXPECT_FALSE(std::filesystem::exists(binding_file()));
    }
}

TEST_F(StoreBindingTest, AFencedSinkIsNotUsable) {
    {
        auto sink = open_sink();
        append_frames(*sink, 2);
    }
    {
        // damage the first frame of the log: recovery cannot account for it and the sink fences
        std::fstream f(path_, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.good());
        char original = 0;
        f.seekg(10);
        f.read(&original, 1);
        ASSERT_TRUE(f.good());
        const char flipped = static_cast<char>(~original);
        f.seekp(10);
        f.write(&flipped, 1);
        ASSERT_TRUE(f.good());
    }
    auto sink = open_sink();
    ASSERT_TRUE(sink->is_open());
    ASSERT_TRUE(sink->fenced()) << "the precondition: a damaged log fences the sink";
    EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Testnet, kUid, kNow), EnsureBindingStatus::StoreNotUsable);
    EXPECT_FALSE(std::filesystem::exists(binding_file()));
}

TEST_F(StoreBindingTest, ABindingSignedBeforeAKeyRotationStillVerifiesAfterIt) {
    auto sink = open_sink();
    ASSERT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Production, kUid, kNow), EnsureBindingStatus::BoundNew);
    ASSERT_EQ(le32(raw_binding(), 34), kKeyId);
    ASSERT_TRUE(sink->rotate_active_key(kKeyId2, kNow + 10));
    ASSERT_EQ(sink->active_key_id(), kKeyId2);
    EXPECT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Production, kUid, kNow + 20), EnsureBindingStatus::Verified)
        << "the file names key 1 and the ring still holds it";
    EXPECT_EQ(le32(raw_binding(), 34), kKeyId) << "and it was not rewritten under the new key";
}

TEST_F(StoreBindingTest, ABindingIsSignedUnderTheSinksActiveKey) {
    auto sink = std::make_unique<DurableAuditSink>(path_, *ring_, kKeyId2);
    ASSERT_TRUE(sink->is_open());
    ASSERT_EQ(ensure_store_binding(*sink, path_, *ring_, StoreEnvironment::Testnet, kUid, kNow), EnsureBindingStatus::BoundNew);
    EXPECT_EQ(le32(raw_binding(), 34), kKeyId2);
}
