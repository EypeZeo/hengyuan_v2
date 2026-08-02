// KeyRing unit tests — HY-KEKWRAP-v1 wrap/unwrap construction (Phase 0, 轨道
// key-rotation substrate).
//
// IMPORTANT SCOPE NOTE: HY-KEKWRAP-v1 is a bespoke construction built from
// crypto::hmac_sha256, NOT a standard library/OpenSSL AEAD (see key_ring.hpp's
// own header comment for the full construction). There is no third-party
// known-answer test vector for it -- correctness here rests entirely on
// round-trip tests (wrap then unwrap recovers the exact original key) and
// tamper tests (any single-byte corruption of wrapped_key_blob/tag/key_id is
// detected), not on comparison against a published reference implementation.

#include <gtest/gtest.h>
#include <hengyuan/key_ring.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

using hy::kKeyBlockSize;
using hy::kKekSize;
using hy::KeyRing;
using hy::KeyRingAddStatus;
using hy::KeyRingLoadStatus;
using hy::WrappedKeyRecord;

namespace {

std::array<std::byte, kKekSize> make_kek(std::uint8_t fill) {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) {
        kek[i] = static_cast<std::byte>(fill + i);
    }
    return kek;
}

// Exactly kKeyBlockSize bytes so KeyRing's internal K0 normalization is a
// pure copy (no hashing) -- keeps round-trip comparisons byte-exact and
// independent of normalize_to_block's own (already-covered-elsewhere)
// hash-if-too-long behavior.
std::array<std::byte, kKeyBlockSize> make_plaintext_key(std::uint8_t fill) {
    std::array<std::byte, kKeyBlockSize> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::byte>(fill + i * 3);
    }
    return key;
}

}  // namespace

TEST(KeyRing, AddKeyThenActiveKeyRoundTrips) {
    const auto kek = make_kek(0x01);
    KeyRing ring(kek);

    const auto plaintext = make_plaintext_key(0x10);
    WrappedKeyRecord record{};
    ASSERT_EQ(ring.add_key(7, plaintext, record), KeyRingAddStatus::Ok);
    EXPECT_EQ(record.key_id, 7u);

    std::array<std::byte, kKeyBlockSize> recovered{};
    ASSERT_TRUE(ring.active_key(7, recovered));
    EXPECT_EQ(recovered, plaintext);
}

TEST(KeyRing, WrappedBlobIsNotThePlaintext) {
    const auto kek = make_kek(0x01);
    KeyRing ring(kek);

    const auto plaintext = make_plaintext_key(0x20);
    WrappedKeyRecord record{};
    ASSERT_EQ(ring.add_key(1, plaintext, record), KeyRingAddStatus::Ok);

    // The wrapped form must not just be the plaintext copied through --
    // that would mean the "wrap" step did nothing.
    EXPECT_NE(record.wrapped_key_blob, plaintext);
}

TEST(KeyRing, LoadWrappedKeyRoundTripsAcrossIndependentRingInstances) {
    const auto kek = make_kek(0x02);
    KeyRing writer(kek);

    const auto plaintext = make_plaintext_key(0x30);
    WrappedKeyRecord record{};
    ASSERT_EQ(writer.add_key(42, plaintext, record), KeyRingAddStatus::Ok);

    // Simulates process restart: a fresh KeyRing constructed from the same
    // KEK, loading a persisted WrappedKeyRecord.
    KeyRing reader(kek);
    ASSERT_EQ(reader.load_wrapped_key(record), KeyRingLoadStatus::Ok);

    std::array<std::byte, kKeyBlockSize> recovered{};
    ASSERT_TRUE(reader.active_key(42, recovered));
    EXPECT_EQ(recovered, plaintext);
}

TEST(KeyRing, DifferentKeyIdsProduceDifferentWrappedBlobsForSamePlaintext) {
    const auto kek = make_kek(0x03);
    KeyRing ring(kek);

    const auto plaintext = make_plaintext_key(0x40);
    WrappedKeyRecord record_a{};
    WrappedKeyRecord record_b{};
    ASSERT_EQ(ring.add_key(1, plaintext, record_a), KeyRingAddStatus::Ok);
    ASSERT_EQ(ring.add_key(2, plaintext, record_b), KeyRingAddStatus::Ok);

    EXPECT_NE(record_a.wrapped_key_blob, record_b.wrapped_key_blob);
    EXPECT_NE(record_a.tag, record_b.tag);
}

TEST(KeyRing, TamperedWrappedBlobFailsVerification) {
    const auto kek = make_kek(0x04);
    KeyRing writer(kek);

    WrappedKeyRecord record{};
    ASSERT_EQ(writer.add_key(9, make_plaintext_key(0x50), record), KeyRingAddStatus::Ok);

    WrappedKeyRecord tampered = record;
    tampered.wrapped_key_blob[0] ^= std::byte{0x01};

    KeyRing reader(kek);
    EXPECT_EQ(reader.load_wrapped_key(tampered), KeyRingLoadStatus::TagMismatch);
}

TEST(KeyRing, TamperedTagFailsVerification) {
    const auto kek = make_kek(0x05);
    KeyRing writer(kek);

    WrappedKeyRecord record{};
    ASSERT_EQ(writer.add_key(9, make_plaintext_key(0x60), record), KeyRingAddStatus::Ok);

    WrappedKeyRecord tampered = record;
    tampered.tag[0] ^= std::byte{0x01};

    KeyRing reader(kek);
    EXPECT_EQ(reader.load_wrapped_key(tampered), KeyRingLoadStatus::TagMismatch);
}

TEST(KeyRing, TamperedKeyIdFailsVerification) {
    // key_id is bound into the tag domain -- swapping it (independent of the
    // wrapped blob/tag bytes themselves) must also be caught.
    const auto kek = make_kek(0x06);
    KeyRing writer(kek);

    WrappedKeyRecord record{};
    ASSERT_EQ(writer.add_key(9, make_plaintext_key(0x70), record), KeyRingAddStatus::Ok);

    WrappedKeyRecord tampered = record;
    tampered.key_id = 10;

    KeyRing reader(kek);
    EXPECT_EQ(reader.load_wrapped_key(tampered), KeyRingLoadStatus::TagMismatch);
}

TEST(KeyRing, WrongKekFailsVerification) {
    const auto kek_a = make_kek(0x07);
    const auto kek_b = make_kek(0x08);

    KeyRing writer(kek_a);
    WrappedKeyRecord record{};
    ASSERT_EQ(writer.add_key(3, make_plaintext_key(0x80), record), KeyRingAddStatus::Ok);

    KeyRing reader(kek_b);
    EXPECT_EQ(reader.load_wrapped_key(record), KeyRingLoadStatus::TagMismatch);
}

TEST(KeyRing, UnknownKeyIdLookupFails) {
    const auto kek = make_kek(0x09);
    KeyRing ring(kek);

    std::array<std::byte, kKeyBlockSize> out{};
    EXPECT_FALSE(ring.active_key(999, out));
}

TEST(KeyRing, DuplicateKeyIdIsRefused) {
    const auto kek = make_kek(0x0a);
    KeyRing ring(kek);

    WrappedKeyRecord record{};
    ASSERT_EQ(ring.add_key(5, make_plaintext_key(0x90), record), KeyRingAddStatus::Ok);
    EXPECT_EQ(ring.add_key(5, make_plaintext_key(0x91), record), KeyRingAddStatus::DuplicateKeyId);
}

TEST(KeyRing, RetireThenLookupFails) {
    const auto kek = make_kek(0x0b);
    KeyRing ring(kek);

    WrappedKeyRecord record{};
    ASSERT_EQ(ring.add_key(11, make_plaintext_key(0xa0), record), KeyRingAddStatus::Ok);

    std::array<std::byte, kKeyBlockSize> out{};
    ASSERT_TRUE(ring.active_key(11, out));

    ring.retire(11);
    EXPECT_FALSE(ring.active_key(11, out));
}

TEST(KeyRing, RetireIsNoOpForUnknownKeyId) {
    const auto kek = make_kek(0x0c);
    KeyRing ring(kek);
    ring.retire(12345);  // must not crash / must be a harmless no-op
    SUCCEED();
}

TEST(KeyRing, RetiredSlotCanBeReusedByNewKeyId) {
    const auto kek = make_kek(0x0d);
    KeyRing ring(kek);

    WrappedKeyRecord record{};
    ASSERT_EQ(ring.add_key(1, make_plaintext_key(0xb0), record), KeyRingAddStatus::Ok);
    ring.retire(1);
    EXPECT_EQ(ring.add_key(1, make_plaintext_key(0xb1), record), KeyRingAddStatus::Ok);
}

TEST(KeyRing, TableFullIsRefusedNotSilentlyOverwritten) {
    const auto kek = make_kek(0x0e);
    KeyRing ring(kek);

    for (std::uint32_t i = 0; i < hy::kMaxLiveKeys; ++i) {
        WrappedKeyRecord record{};
        ASSERT_EQ(ring.add_key(i, make_plaintext_key(static_cast<std::uint8_t>(i)), record),
                  KeyRingAddStatus::Ok)
            << "key_id " << i;
    }

    WrappedKeyRecord overflow_record{};
    EXPECT_EQ(ring.add_key(hy::kMaxLiveKeys, make_plaintext_key(0xff), overflow_record),
              KeyRingAddStatus::TableFull);
}
