// Tests for seal_started_migration_cleanup_loader.hpp's two read-only
// loaders — SealExportStartedMigrationLoader ("seal-export-started.mig") and
// SealStartedCleanupTombstoneLoader ("seal-export-started.clr").
//
// The loaders are templates over the lease type specifically so they can be
// driven here against a duck-typed mock: the real CandidateLease read
// methods are private and friend-only, so real-file tests are impossible
// until the coordinator-side lease integration lands. Every arm of
// load_and_validate_intent()'s structure (LeaseIoOutcome -> ReadFixedStatus
// -> key_id peek -> pin -> decode -> copy-out) is exercised for BOTH wire
// types. Governance: L1 (no file I/O anywhere in this file).
#include <gtest/gtest.h>
#include <hengyuan/compaction_lease.hpp>  // LeaseIoOutcome / LeaseReadResult / compaction_detail::ReadFixedStatus
#include <hengyuan/key_ring.hpp>
#include <hengyuan/seal_export_migration_cleanup_abandon_codec.hpp>
#include <hengyuan/seal_started_migration_cleanup_loader.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <vector>

using namespace hy;

namespace {

constexpr std::uint32_t kTestKeyId = 7;

std::array<std::byte, kKekSize> make_kek(std::uint8_t fill) {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) {
        kek[i] = static_cast<std::byte>(fill + static_cast<std::uint8_t>(i));
    }
    return kek;
}

void fill_bytes(std::uint8_t (&arr)[32], std::uint8_t seed) {
    for (int i = 0; i < 32; ++i) {
        arr[i] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(i));
    }
}

// Duck-typed stand-in for CandidateLease — same method names/signatures as
// the real (friend-only, coordinator-added) private read methods. The
// coordinator-supplied shape, kept verbatim.
struct MockCandidateLeaseForMigrationCleanup {
    LeaseIoOutcome next_outcome = LeaseIoOutcome::Ok;
    compaction_detail::ReadFixedStatus next_read_status = compaction_detail::ReadFixedStatus::Ok;
    std::vector<std::byte> canned_bytes;

    LeaseReadResult read_seal_export_started_migration(
        std::span<std::byte, kSealExportStartedMigrationWireBytes> out) noexcept {
        return fill(out);
    }
    LeaseReadResult read_seal_started_cleanup_tombstone(
        std::span<std::byte, kSealStartedCleanupWireBytes> out) noexcept {
        return fill(out);
    }

private:
    template <std::size_t N>
    LeaseReadResult fill(std::span<std::byte, N> out) noexcept {
        LeaseReadResult r{};
        r.outcome = next_outcome;
        if (next_outcome != LeaseIoOutcome::Ok) return r;
        r.read.status = next_read_status;
        if (next_read_status == compaction_detail::ReadFixedStatus::Ok) {
            const std::size_t n = std::min(out.size(), canned_bytes.size());
            std::memcpy(out.data(), canned_bytes.data(), n);
        }
        return r;
    }
};

// A shape-valid SealExportStartedMigrationWire for the test key id.
SealExportStartedMigrationWire make_sample_migration() {
    SealExportStartedMigrationWire v{};
    v.format_version = kSealExportStartedMigrationFormatVersion;
    v.total_bytes = kSealExportStartedMigrationWireBytes;
    v.store_uuid_lo = 0x1111111111111111ull;
    v.store_uuid_hi = 0x2222222222222222ull;
    v.candidate_id = 100;
    v.request_id = 200;
    v.legacy_kek_key_id = 5;
    v.v2_kek_key_id = kTestKeyId;
    fill_bytes(v.legacy_file_digest, 0x10);
    fill_bytes(v.v2_file_digest, 0x20);
    fill_bytes(v.legacy_mac, 0x30);
    fill_bytes(v.v2_mac, 0x40);
    return v;
}

// A shape-valid NativeV2 SealStartedCleanupTombstoneWire for the test key id
// (present_mask == 0b001; digest_V/digest_M all-zero, per the L4 spec).
SealStartedCleanupTombstoneWire make_sample_cleanup() {
    SealStartedCleanupTombstoneWire v{};
    v.format_version = kSealStartedCleanupFormatVersion;
    v.total_bytes = kSealStartedCleanupWireBytes;
    v.store_uuid_lo = 0x1111111111111111ull;
    v.store_uuid_hi = 0x2222222222222222ull;
    v.candidate_id = 100;
    v.request_id = 200;
    v.kek_key_id = kTestKeyId;
    v.started_kind = kSealStartedKindNativeV2;
    v.present_mask = 0b001;
    v.phase = kSealStartedCleanupPhaseAuthorized;
    v.reserved0 = 0;
    v.source_generation = 5;
    v.baseline_tip_seq = 42;
    fill_bytes(v.baseline_tip_mac, 0x10);
    v.baseline_key_id = 3;
    v.new_generation = 6;
    v.new_final_seq = 99;
    fill_bytes(v.new_final_tip_mac, 0x20);
    v.new_key_id = 4;
    fill_bytes(v.content_root, 0x30);
    fill_bytes(v.digest_L, 0x40);
    std::memset(v.digest_V, 0, sizeof(v.digest_V));
    std::memset(v.digest_M, 0, sizeof(v.digest_M));
    return v;
}

template <std::size_t N>
std::vector<std::byte> to_vector(const std::array<std::byte, N>& a) {
    return std::vector<std::byte>(a.begin(), a.end());
}

std::array<std::byte, kSealExportStartedMigrationWireBytes> encode_migration(
    const SealExportStartedMigrationWire& v, std::span<const std::byte> hmac_key) {
    std::array<std::byte, kSealExportStartedMigrationWireBytes> out{};
    encode_seal_export_started_migration_wire(out, v, hmac_key);
    return out;
}

std::array<std::byte, kSealStartedCleanupWireBytes> encode_cleanup(
    const SealStartedCleanupTombstoneWire& v, std::span<const std::byte> hmac_key) {
    std::array<std::byte, kSealStartedCleanupWireBytes> out{};
    encode_seal_started_cleanup_tombstone_wire(out, v, hmac_key);
    return out;
}

void expect_migration_equal(const SealExportStartedMigrationWire& got,
                            const SealExportStartedMigrationWire& want) {
    EXPECT_EQ(got.format_version, want.format_version);
    EXPECT_EQ(got.total_bytes, want.total_bytes);
    EXPECT_EQ(got.store_uuid_lo, want.store_uuid_lo);
    EXPECT_EQ(got.store_uuid_hi, want.store_uuid_hi);
    EXPECT_EQ(got.candidate_id, want.candidate_id);
    EXPECT_EQ(got.request_id, want.request_id);
    EXPECT_EQ(got.legacy_kek_key_id, want.legacy_kek_key_id);
    EXPECT_EQ(got.v2_kek_key_id, want.v2_kek_key_id);
    EXPECT_EQ(std::memcmp(got.legacy_file_digest, want.legacy_file_digest, sizeof(got.legacy_file_digest)), 0);
    EXPECT_EQ(std::memcmp(got.v2_file_digest, want.v2_file_digest, sizeof(got.v2_file_digest)), 0);
    EXPECT_EQ(std::memcmp(got.legacy_mac, want.legacy_mac, sizeof(got.legacy_mac)), 0);
    EXPECT_EQ(std::memcmp(got.v2_mac, want.v2_mac, sizeof(got.v2_mac)), 0);
}

void expect_cleanup_equal(const SealStartedCleanupTombstoneWire& got,
                          const SealStartedCleanupTombstoneWire& want) {
    EXPECT_EQ(got.format_version, want.format_version);
    EXPECT_EQ(got.total_bytes, want.total_bytes);
    EXPECT_EQ(got.store_uuid_lo, want.store_uuid_lo);
    EXPECT_EQ(got.store_uuid_hi, want.store_uuid_hi);
    EXPECT_EQ(got.candidate_id, want.candidate_id);
    EXPECT_EQ(got.request_id, want.request_id);
    EXPECT_EQ(got.kek_key_id, want.kek_key_id);
    EXPECT_EQ(got.started_kind, want.started_kind);
    EXPECT_EQ(got.present_mask, want.present_mask);
    EXPECT_EQ(got.phase, want.phase);
    EXPECT_EQ(got.reserved0, want.reserved0);
    EXPECT_EQ(got.source_generation, want.source_generation);
    EXPECT_EQ(got.baseline_tip_seq, want.baseline_tip_seq);
    EXPECT_EQ(std::memcmp(got.baseline_tip_mac, want.baseline_tip_mac, sizeof(got.baseline_tip_mac)), 0);
    EXPECT_EQ(got.baseline_key_id, want.baseline_key_id);
    EXPECT_EQ(got.new_generation, want.new_generation);
    EXPECT_EQ(got.new_final_seq, want.new_final_seq);
    EXPECT_EQ(std::memcmp(got.new_final_tip_mac, want.new_final_tip_mac, sizeof(got.new_final_tip_mac)), 0);
    EXPECT_EQ(got.new_key_id, want.new_key_id);
    EXPECT_EQ(std::memcmp(got.content_root, want.content_root, sizeof(got.content_root)), 0);
    EXPECT_EQ(std::memcmp(got.digest_L, want.digest_L, sizeof(got.digest_L)), 0);
    EXPECT_EQ(std::memcmp(got.digest_V, want.digest_V, sizeof(got.digest_V)), 0);
    EXPECT_EQ(std::memcmp(got.digest_M, want.digest_M, sizeof(got.digest_M)), 0);
}

}  // namespace

// ===========================================================================
// SealExportStartedMigrationLoader
// ===========================================================================

TEST(SealExportStartedMigrationLoader, RoundTripLoadsAllFields) {
    const auto kek = make_kek(0x10);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(kTestKeyId, kek, rec), KeyRingAddStatus::Ok);

    const auto v = make_sample_migration();
    const PinResult pin = ring.pin_key(kTestKeyId);
    ASSERT_EQ(pin.status, PinStatus::Pinned);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.canned_bytes = to_vector(encode_migration(v, pin.handle->key_bytes()));

    SealExportStartedMigrationLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealExportStartedMigrationWire out{};
    ASSERT_EQ(loader.load(ring, out), LoadStatus::Ok);
    expect_migration_equal(out, v);
}

TEST(SealExportStartedMigrationLoader, WrongOwnerMapsToLeaseNotHeld) {
    const auto kek = make_kek(0x11);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_outcome = LeaseIoOutcome::WrongOwner;
    SealExportStartedMigrationLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealExportStartedMigrationWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::LeaseNotHeld);
}

TEST(SealExportStartedMigrationLoader, NotHeldMapsToLeaseNotHeld) {
    const auto kek = make_kek(0x12);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_outcome = LeaseIoOutcome::NotHeld;
    SealExportStartedMigrationLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealExportStartedMigrationWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::LeaseNotHeld);
}

TEST(SealExportStartedMigrationLoader, CandidateFencedMapsToCandidateFenced) {
    const auto kek = make_kek(0x13);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_outcome = LeaseIoOutcome::CandidateFenced;
    SealExportStartedMigrationLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealExportStartedMigrationWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::CandidateFenced);
}

TEST(SealExportStartedMigrationLoader, DirectoryIdentityChangedMapsToDirectoryIdentityChanged) {
    const auto kek = make_kek(0x14);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_outcome = LeaseIoOutcome::DirectoryIdentityChanged;
    SealExportStartedMigrationLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealExportStartedMigrationWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::DirectoryIdentityChanged);
}

TEST(SealExportStartedMigrationLoader, NotFoundMapsToNotFound) {
    const auto kek = make_kek(0x15);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_read_status = compaction_detail::ReadFixedStatus::NotFound;
    SealExportStartedMigrationLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealExportStartedMigrationWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::NotFound);
}

TEST(SealExportStartedMigrationLoader, WrongSizeMapsToCorrupt) {
    const auto kek = make_kek(0x16);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_read_status = compaction_detail::ReadFixedStatus::WrongSize;
    SealExportStartedMigrationLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealExportStartedMigrationWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::Corrupt);
}

TEST(SealExportStartedMigrationLoader, NotRegularFileMapsToCorrupt) {
    const auto kek = make_kek(0x17);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_read_status = compaction_detail::ReadFixedStatus::NotRegularFile;
    SealExportStartedMigrationLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealExportStartedMigrationWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::Corrupt);
}

TEST(SealExportStartedMigrationLoader, IoErrorMapsToIoError) {
    const auto kek = make_kek(0x18);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_read_status = compaction_detail::ReadFixedStatus::IoError;
    SealExportStartedMigrationLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealExportStartedMigrationWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::IoError);
}

TEST(SealExportStartedMigrationLoader, TamperedMacMapsToCorrupt) {
    const auto kek = make_kek(0x19);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(kTestKeyId, kek, rec), KeyRingAddStatus::Ok);

    const auto v = make_sample_migration();
    const PinResult pin = ring.pin_key(kTestKeyId);
    ASSERT_EQ(pin.status, PinStatus::Pinned);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.canned_bytes = to_vector(encode_migration(v, pin.handle->key_bytes()));
    mock.canned_bytes.back() ^= std::byte{0x01};  // flip one MAC trailer byte

    SealExportStartedMigrationLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealExportStartedMigrationWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::Corrupt);
}

TEST(SealExportStartedMigrationLoader, UnknownKekKeyIdMapsToKeyNotFound) {
    const auto kek = make_kek(0x1A);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(kTestKeyId, kek, rec), KeyRingAddStatus::Ok);

    // MAC-valid bytes under the ring's key, but the wire names a key_id the
    // ring does not hold -- load() must refuse at the pin step (KeyNotFound),
    // before ever reaching decode.
    auto v = make_sample_migration();
    v.v2_kek_key_id = 999;
    const PinResult pin = ring.pin_key(kTestKeyId);
    ASSERT_EQ(pin.status, PinStatus::Pinned);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.canned_bytes = to_vector(encode_migration(v, pin.handle->key_bytes()));

    SealExportStartedMigrationLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealExportStartedMigrationWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::KeyNotFound);
}

TEST(SealExportStartedMigrationLoader, WrongFormatVersionMapsToCorrupt) {
    const auto kek = make_kek(0x1B);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(kTestKeyId, kek, rec), KeyRingAddStatus::Ok);

    const auto v = make_sample_migration();
    const PinResult pin = ring.pin_key(kTestKeyId);
    ASSERT_EQ(pin.status, PinStatus::Pinned);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.canned_bytes = to_vector(encode_migration(v, pin.handle->key_bytes()));
    // format_version := 1 (not the active 2) -- decode answers
    // SealStartedWireDecodeStatus::UnknownVersion, which load() maps to
    // Corrupt exactly like every other decode failure (no separate status).
    mock.canned_bytes[0] = std::byte{1};
    mock.canned_bytes[1] = std::byte{0};
    mock.canned_bytes[2] = std::byte{0};
    mock.canned_bytes[3] = std::byte{0};

    SealExportStartedMigrationLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealExportStartedMigrationWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::Corrupt);
}

// ===========================================================================
// SealStartedCleanupTombstoneLoader
// ===========================================================================

TEST(SealStartedCleanupTombstoneLoader, RoundTripLoadsAllFields) {
    const auto kek = make_kek(0x20);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(kTestKeyId, kek, rec), KeyRingAddStatus::Ok);

    const auto v = make_sample_cleanup();
    const PinResult pin = ring.pin_key(kTestKeyId);
    ASSERT_EQ(pin.status, PinStatus::Pinned);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.canned_bytes = to_vector(encode_cleanup(v, pin.handle->key_bytes()));

    SealStartedCleanupTombstoneLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealStartedCleanupTombstoneWire out{};
    ASSERT_EQ(loader.load(ring, out), LoadStatus::Ok);
    expect_cleanup_equal(out, v);
}

TEST(SealStartedCleanupTombstoneLoader, WrongOwnerMapsToLeaseNotHeld) {
    const auto kek = make_kek(0x21);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_outcome = LeaseIoOutcome::WrongOwner;
    SealStartedCleanupTombstoneLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealStartedCleanupTombstoneWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::LeaseNotHeld);
}

TEST(SealStartedCleanupTombstoneLoader, NotHeldMapsToLeaseNotHeld) {
    const auto kek = make_kek(0x22);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_outcome = LeaseIoOutcome::NotHeld;
    SealStartedCleanupTombstoneLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealStartedCleanupTombstoneWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::LeaseNotHeld);
}

TEST(SealStartedCleanupTombstoneLoader, CandidateFencedMapsToCandidateFenced) {
    const auto kek = make_kek(0x23);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_outcome = LeaseIoOutcome::CandidateFenced;
    SealStartedCleanupTombstoneLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealStartedCleanupTombstoneWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::CandidateFenced);
}

TEST(SealStartedCleanupTombstoneLoader, DirectoryIdentityChangedMapsToDirectoryIdentityChanged) {
    const auto kek = make_kek(0x24);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_outcome = LeaseIoOutcome::DirectoryIdentityChanged;
    SealStartedCleanupTombstoneLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealStartedCleanupTombstoneWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::DirectoryIdentityChanged);
}

TEST(SealStartedCleanupTombstoneLoader, NotFoundMapsToNotFound) {
    const auto kek = make_kek(0x25);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_read_status = compaction_detail::ReadFixedStatus::NotFound;
    SealStartedCleanupTombstoneLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealStartedCleanupTombstoneWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::NotFound);
}

TEST(SealStartedCleanupTombstoneLoader, WrongSizeMapsToCorrupt) {
    const auto kek = make_kek(0x26);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_read_status = compaction_detail::ReadFixedStatus::WrongSize;
    SealStartedCleanupTombstoneLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealStartedCleanupTombstoneWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::Corrupt);
}

TEST(SealStartedCleanupTombstoneLoader, NotRegularFileMapsToCorrupt) {
    const auto kek = make_kek(0x27);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_read_status = compaction_detail::ReadFixedStatus::NotRegularFile;
    SealStartedCleanupTombstoneLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealStartedCleanupTombstoneWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::Corrupt);
}

TEST(SealStartedCleanupTombstoneLoader, IoErrorMapsToIoError) {
    const auto kek = make_kek(0x28);
    KeyRing ring(kek);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.next_read_status = compaction_detail::ReadFixedStatus::IoError;
    SealStartedCleanupTombstoneLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealStartedCleanupTombstoneWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::IoError);
}

TEST(SealStartedCleanupTombstoneLoader, TamperedMacMapsToCorrupt) {
    const auto kek = make_kek(0x29);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(kTestKeyId, kek, rec), KeyRingAddStatus::Ok);

    const auto v = make_sample_cleanup();
    const PinResult pin = ring.pin_key(kTestKeyId);
    ASSERT_EQ(pin.status, PinStatus::Pinned);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.canned_bytes = to_vector(encode_cleanup(v, pin.handle->key_bytes()));
    mock.canned_bytes.back() ^= std::byte{0x01};  // flip one MAC trailer byte

    SealStartedCleanupTombstoneLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealStartedCleanupTombstoneWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::Corrupt);
}

TEST(SealStartedCleanupTombstoneLoader, UnknownKekKeyIdMapsToKeyNotFound) {
    const auto kek = make_kek(0x2A);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(kTestKeyId, kek, rec), KeyRingAddStatus::Ok);

    auto v = make_sample_cleanup();
    v.kek_key_id = 999;
    const PinResult pin = ring.pin_key(kTestKeyId);
    ASSERT_EQ(pin.status, PinStatus::Pinned);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.canned_bytes = to_vector(encode_cleanup(v, pin.handle->key_bytes()));

    SealStartedCleanupTombstoneLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealStartedCleanupTombstoneWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::KeyNotFound);
}

TEST(SealStartedCleanupTombstoneLoader, WrongFormatVersionMapsToCorrupt) {
    const auto kek = make_kek(0x2B);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(kTestKeyId, kek, rec), KeyRingAddStatus::Ok);

    const auto v = make_sample_cleanup();
    const PinResult pin = ring.pin_key(kTestKeyId);
    ASSERT_EQ(pin.status, PinStatus::Pinned);

    MockCandidateLeaseForMigrationCleanup mock{};
    mock.canned_bytes = to_vector(encode_cleanup(v, pin.handle->key_bytes()));
    mock.canned_bytes[0] = std::byte{1};
    mock.canned_bytes[1] = std::byte{0};
    mock.canned_bytes[2] = std::byte{0};
    mock.canned_bytes[3] = std::byte{0};

    SealStartedCleanupTombstoneLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);
    SealStartedCleanupTombstoneWire out{};
    EXPECT_EQ(loader.load(ring, out), LoadStatus::Corrupt);
}

// ===========================================================================
// Property tests: 2000 random byte payloads per wire type, fed through the
// mock with a healthy read result. Must never crash and must always return
// one of LoadStatus's legal values (0..IoError).
// ===========================================================================

TEST(SealExportStartedMigrationLoaderProperty, RandomCannedBytesNeverCrashOrMisreport) {
    const auto kek = make_kek(0x30);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(kTestKeyId, kek, rec), KeyRingAddStatus::Ok);

    MockCandidateLeaseForMigrationCleanup mock{};
    SealExportStartedMigrationLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);

    std::mt19937 rng(0x5EEDC0DE);
    for (int i = 0; i < 2000; ++i) {
        mock.next_outcome = LeaseIoOutcome::Ok;
        mock.next_read_status = compaction_detail::ReadFixedStatus::Ok;
        const std::size_t n = kSealExportStartedMigrationWireBytes + (rng() % 64);
        mock.canned_bytes.resize(n);
        for (auto& b : mock.canned_bytes) {
            b = std::byte{static_cast<std::uint8_t>(rng() & 0xFF)};
        }
        SealExportStartedMigrationWire out{};
        const LoadStatus s = loader.load(ring, out);
        EXPECT_LE(static_cast<std::uint8_t>(s), static_cast<std::uint8_t>(LoadStatus::IoError));
    }
}

TEST(SealStartedCleanupTombstoneLoaderProperty, RandomCannedBytesNeverCrashOrMisreport) {
    const auto kek = make_kek(0x31);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(kTestKeyId, kek, rec), KeyRingAddStatus::Ok);

    MockCandidateLeaseForMigrationCleanup mock{};
    SealStartedCleanupTombstoneLoader<MockCandidateLeaseForMigrationCleanup> loader(mock);

    std::mt19937 rng(0xC0FFEE);
    for (int i = 0; i < 2000; ++i) {
        mock.next_outcome = LeaseIoOutcome::Ok;
        mock.next_read_status = compaction_detail::ReadFixedStatus::Ok;
        const std::size_t n = kSealStartedCleanupWireBytes + (rng() % 64);
        mock.canned_bytes.resize(n);
        for (auto& b : mock.canned_bytes) {
            b = std::byte{static_cast<std::uint8_t>(rng() & 0xFF)};
        }
        SealStartedCleanupTombstoneWire out{};
        const LoadStatus s = loader.load(ring, out);
        EXPECT_LE(static_cast<std::uint8_t>(s), static_cast<std::uint8_t>(LoadStatus::IoError));
    }
}
