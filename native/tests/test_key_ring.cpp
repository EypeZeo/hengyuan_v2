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
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

using hy::kKeyBlockSize;
using hy::kKekSize;
using hy::KeyRing;
using hy::KeyRingAddStatus;
using hy::KeyRingLoadStatus;
using hy::PinnedKeyHandle;
using hy::PinResult;
using hy::PinStatus;
using hy::RetireStatus;
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

    EXPECT_EQ(ring.retire(11), RetireStatus::Retired);
    EXPECT_FALSE(ring.active_key(11, out));
}

TEST(KeyRing, RetireIsNotFoundForUnknownKeyId) {
    const auto kek = make_kek(0x0c);
    KeyRing ring(kek);
    EXPECT_EQ(ring.retire(12345), RetireStatus::NotFound);  // must not crash
}

TEST(KeyRing, RetiredSlotCanBeReusedByNewKeyId) {
    const auto kek = make_kek(0x0d);
    KeyRing ring(kek);

    WrappedKeyRecord record{};
    ASSERT_EQ(ring.add_key(1, make_plaintext_key(0xb0), record), KeyRingAddStatus::Ok);
    ASSERT_EQ(ring.retire(1), RetireStatus::Retired);
    EXPECT_EQ(ring.add_key(1, make_plaintext_key(0xb1), record), KeyRingAddStatus::Ok);
}

TEST(KeyRing, RetireSecondTimeIsNotFoundNotRetired) {
    const auto kek = make_kek(0x0f);
    KeyRing ring(kek);
    WrappedKeyRecord record{};
    ASSERT_EQ(ring.add_key(1, make_plaintext_key(0xb0), record), KeyRingAddStatus::Ok);
    ASSERT_EQ(ring.retire(1), RetireStatus::Retired);
    EXPECT_EQ(ring.retire(1), RetireStatus::NotFound);  // already gone, not a second Retired
}

// --- pin_key / retire interaction (Round D review P0-4) ---

TEST(KeyRingPin, PinKeyReturnsKeyBytesMatchingActiveKey) {
    const auto kek = make_kek(0x10);
    KeyRing ring(kek);
    WrappedKeyRecord record{};
    ASSERT_EQ(ring.add_key(7, make_plaintext_key(0xc0), record), KeyRingAddStatus::Ok);

    std::array<std::byte, kKeyBlockSize> expected{};
    ASSERT_TRUE(ring.active_key(7, expected));

    const PinResult pin = ring.pin_key(7);
    ASSERT_EQ(pin.status, PinStatus::Pinned);
    ASSERT_TRUE(pin.handle.has_value());
    EXPECT_EQ(pin.handle->key_id(), 7u);
    ASSERT_EQ(pin.handle->key_bytes().size(), expected.size());
    EXPECT_EQ(0, std::memcmp(pin.handle->key_bytes().data(), expected.data(), expected.size()));
}

TEST(KeyRingPin, PinKeyNotFoundForUnknownKeyId) {
    const auto kek = make_kek(0x11);
    KeyRing ring(kek);
    const PinResult pin = ring.pin_key(999);
    EXPECT_EQ(pin.status, PinStatus::NotFound);
    EXPECT_FALSE(pin.handle.has_value());
}

TEST(KeyRingPin, RetireRefusedWhilePinHeld) {
    const auto kek = make_kek(0x12);
    KeyRing ring(kek);
    WrappedKeyRecord record{};
    ASSERT_EQ(ring.add_key(3, make_plaintext_key(0xd0), record), KeyRingAddStatus::Ok);

    PinResult pin = ring.pin_key(3);
    ASSERT_EQ(pin.status, PinStatus::Pinned);

    EXPECT_EQ(ring.retire(3), RetireStatus::KeyPinned);

    std::array<std::byte, kKeyBlockSize> out{};
    EXPECT_TRUE(ring.active_key(3, out)) << "still active -- retire must have been refused, not silently applied";
}

TEST(KeyRingPin, RetireSucceedsAfterHandleReleased) {
    const auto kek = make_kek(0x13);
    KeyRing ring(kek);
    WrappedKeyRecord record{};
    ASSERT_EQ(ring.add_key(4, make_plaintext_key(0xe0), record), KeyRingAddStatus::Ok);

    {
        PinResult pin = ring.pin_key(4);
        ASSERT_EQ(pin.status, PinStatus::Pinned);
        // handle destructs at end of this scope, releasing the pin
    }

    EXPECT_EQ(ring.retire(4), RetireStatus::Retired);
}

TEST(KeyRingPin, MoveTransfersPinOwnershipWithoutDoubleRelease) {
    const auto kek = make_kek(0x14);
    KeyRing ring(kek);
    WrappedKeyRecord record{};
    ASSERT_EQ(ring.add_key(5, make_plaintext_key(0xf0), record), KeyRingAddStatus::Ok);

    PinResult pin = ring.pin_key(5);
    ASSERT_EQ(pin.status, PinStatus::Pinned);
    PinnedKeyHandle moved(std::move(*pin.handle));
    EXPECT_EQ(moved.key_id(), 5u);

    // pin.handle's contained object is now moved-from (ring_==nullptr, so
    // its destructor at scope exit is a safe no-op); `moved` is what
    // actually still holds the pin. If move had double-released instead of
    // transferring ownership, this retire() would wrongly succeed while
    // `moved` is still alive.
    EXPECT_EQ(ring.retire(5), RetireStatus::KeyPinned);
}

TEST(KeyRingPin, SelfMoveAssignmentDoesNotCorruptOrDoubleRelease) {
    const auto kek = make_kek(0x15);
    KeyRing ring(kek);
    WrappedKeyRecord record{};
    ASSERT_EQ(ring.add_key(6, make_plaintext_key(0x20), record), KeyRingAddStatus::Ok);

    PinResult pin = ring.pin_key(6);
    ASSERT_EQ(pin.status, PinStatus::Pinned);
    PinnedKeyHandle& handle = *pin.handle;
    // Indirect through a pointer so the compiler can't statically detect
    // (and warn on) an obviously-self move -- this still exercises the
    // real self-move-assignment code path at runtime.
    PinnedKeyHandle* self_ptr = &handle;
    handle = std::move(*self_ptr);
    EXPECT_EQ(handle.key_id(), 6u);
    EXPECT_EQ(ring.retire(6), RetireStatus::KeyPinned) << "self-move must not have silently released the pin";
}

// Round D review P0-2: ~KeyRing() must fail-closed in ALL build types (not
// a debug-only assert) when a PinnedKeyHandle is still alive at destruction
// time -- verifying this actually happens, not just documenting the
// contract. Local scoping alone would destroy `pin` before `ring` (reverse
// declaration order), which would NOT exercise the violation -- std::
// unique_ptr<KeyRing>::reset() forces `ring` to be destroyed first, while
// `pin`'s PinnedKeyHandle is still alive, which is the actual misuse this
// destructor must catch.
TEST(KeyRingDeathTest, DestroyedWithLivePinTerminates) {
    const auto kek = make_kek(0x16);
    EXPECT_DEATH(
        {
            auto ring = std::make_unique<KeyRing>(kek);
            WrappedKeyRecord record{};
            if (ring->add_key(1, make_plaintext_key(0x30), record) != KeyRingAddStatus::Ok) std::abort();
            PinResult pin = ring->pin_key(1);
            if (pin.status != PinStatus::Pinned) std::abort();
            ring.reset();  // destroys KeyRing while pin.handle is still alive -- must terminate()
        },
        "");
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
