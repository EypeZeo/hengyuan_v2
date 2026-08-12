// Read-only SealStartedAbandonLoader tests (Round E breadcrumb L2).
// Branch classification is driven through MockCandidateLeaseForAbandon into
// SealStartedAbandonLoader::load_from (the same function load() calls).
// One compile-time check pins the production surface to CandidateLease&.

#include <gtest/gtest.h>
#include <hengyuan/seal_started_abandon_loader.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <type_traits>
#include <vector>

using hy::KeyRing;
using hy::KeyRingAddStatus;
using hy::LeaseIoOutcome;
using hy::LeaseReadResult;
using hy::LoadStatus;
using hy::SealStartedAbandonLoader;
using hy::SealStartedAbandonWire;
using hy::WrappedKeyRecord;
using hy::encode_seal_started_abandon_wire;
using hy::kKeyBlockSize;
using hy::kKekSize;
using hy::kSealStartedAbandonFormatVersion;
using hy::kSealStartedAbandonPhaseAbdPending;
using hy::kSealStartedAbandonPhaseAuthorized;
using hy::kSealStartedAbandonReasonNotFound;
using hy::kSealStartedAbandonWireBytes;
using hy::kSealStartedKindNativeV2;

namespace {

struct MockCandidateLeaseForAbandon {
    LeaseIoOutcome next_outcome = LeaseIoOutcome::Ok;
    hy::compaction_detail::ReadFixedStatus next_read_status =
        hy::compaction_detail::ReadFixedStatus::Ok;
    std::vector<std::byte> canned_bytes;

    LeaseReadResult read_seal_started_abandon(
        std::span<std::byte, kSealStartedAbandonWireBytes> out) noexcept {
        LeaseReadResult r{};
        r.outcome = next_outcome;
        if (next_outcome != LeaseIoOutcome::Ok) return r;
        r.read.status = next_read_status;
        if (next_read_status == hy::compaction_detail::ReadFixedStatus::Ok) {
            const std::size_t n = std::min(out.size(), canned_bytes.size());
            std::memcpy(out.data(), canned_bytes.data(), n);
        }
        return r;
    }
};

LoadStatus load_via_mock(MockCandidateLeaseForAbandon& lease, KeyRing& key_ring,
                         SealStartedAbandonWire& out) noexcept {
    return SealStartedAbandonLoader::load_from(lease, key_ring, out);
}

std::array<std::byte, kKekSize> make_kek(std::uint8_t fill) {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) {
        kek[i] = static_cast<std::byte>(fill + static_cast<std::uint8_t>(i));
    }
    return kek;
}

std::array<std::byte, kKeyBlockSize> make_plaintext_key(std::uint8_t fill) {
    std::array<std::byte, kKeyBlockSize> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::byte>(fill + static_cast<std::uint8_t>(i * 3));
    }
    return key;
}

void fill_bytes(std::uint8_t (&arr)[32], std::uint8_t seed) {
    for (int i = 0; i < 32; ++i) arr[i] = static_cast<std::uint8_t>(seed + i);
}

void zero_bytes(std::uint8_t (&arr)[32]) {
    for (int i = 0; i < 32; ++i) arr[i] = 0;
}

SealStartedAbandonWire make_sample_abandon(std::uint32_t kek_key_id) {
    SealStartedAbandonWire v{};
    v.format_version = kSealStartedAbandonFormatVersion;
    v.total_bytes = static_cast<std::uint32_t>(kSealStartedAbandonWireBytes);
    v.store_uuid_lo = 0x1111111111111111ull;
    v.store_uuid_hi = 0x2222222222222222ull;
    v.candidate_id = 100;
    v.request_id = 200;
    v.kek_key_id = kek_key_id;
    v.started_kind = kSealStartedKindNativeV2;
    v.abandon_reason = kSealStartedAbandonReasonNotFound;
    v.present_mask = 0b0001;
    v.phase = kSealStartedAbandonPhaseAuthorized;
    v.source_generation = 5;
    v.baseline_tip_seq = 42;
    fill_bytes(v.baseline_tip_mac, 0x10);
    v.baseline_key_id = 3;
    fill_bytes(v.content_root, 0x30);
    zero_bytes(v.digest_C);
    return v;
}

struct Fixture {
    static constexpr std::uint32_t kKeyId = 7;

    Fixture() : kek_(make_kek(0xA1)), key_(make_plaintext_key(0x40)), ring_(kek_) {
        WrappedKeyRecord record{};
        EXPECT_EQ(ring_.add_key(kKeyId, key_, record), KeyRingAddStatus::Ok);
    }

    std::vector<std::byte> encode(SealStartedAbandonWire& v) const {
        std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
        EXPECT_EQ(encode_seal_started_abandon_wire(buf, v, key_), kSealStartedAbandonWireBytes);
        // encode writes the MAC into the buffer only -- copy it back so
        // round-trip EXPECT_EQ against the preimage can include mac[32].
        std::memcpy(v.mac, buf.data() + (kSealStartedAbandonWireBytes - 32), 32);
        return std::vector<std::byte>(buf.begin(), buf.end());
    }

    std::array<std::byte, kKekSize> kek_;
    std::array<std::byte, kKeyBlockSize> key_;
    KeyRing ring_;
};

SealStartedAbandonWire make_sentinel_out() {
    SealStartedAbandonWire out{};
    out.format_version = 0xA5A5A5A5u;
    out.total_bytes = 0x5A5A5A5Au;
    out.store_uuid_lo = 0xDEADBEEFDEADBEEFull;
    out.store_uuid_hi = 0xCAFEBABECAFEBABEull;
    out.candidate_id = 0x1111222233334444ull;
    out.request_id = 0x5555666677778888ull;
    out.kek_key_id = 0x9999AAAAu;
    out.started_kind = 0xBBu;
    out.abandon_reason = 0xCCu;
    out.present_mask = 0xDDu;
    out.phase = 0xEEu;
    out.source_generation = 0xF00F1234u;
    out.baseline_tip_seq = 0x0123456789ABCDEFull;
    out.baseline_key_id = 0x13579BDFu;
    for (int i = 0; i < 32; ++i) {
        out.baseline_tip_mac[i] = static_cast<std::uint8_t>(0x10 + i);
        out.content_root[i] = static_cast<std::uint8_t>(0x40 + i);
        out.digest_C[i] = static_cast<std::uint8_t>(0x70 + i);
        out.mac[i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    return out;
}

bool wire_fields_equal(const SealStartedAbandonWire& a, const SealStartedAbandonWire& b) {
    return a.format_version == b.format_version && a.total_bytes == b.total_bytes &&
           a.store_uuid_lo == b.store_uuid_lo && a.store_uuid_hi == b.store_uuid_hi &&
           a.candidate_id == b.candidate_id && a.request_id == b.request_id &&
           a.kek_key_id == b.kek_key_id && a.started_kind == b.started_kind &&
           a.abandon_reason == b.abandon_reason && a.present_mask == b.present_mask &&
           a.phase == b.phase && a.source_generation == b.source_generation &&
           a.baseline_tip_seq == b.baseline_tip_seq && a.baseline_key_id == b.baseline_key_id &&
           std::memcmp(a.baseline_tip_mac, b.baseline_tip_mac, 32) == 0 &&
           std::memcmp(a.content_root, b.content_root, 32) == 0 &&
           std::memcmp(a.digest_C, b.digest_C, 32) == 0 && std::memcmp(a.mac, b.mac, 32) == 0;
}

bool is_legal_load_status(LoadStatus s) {
    switch (s) {
        case LoadStatus::Ok:
        case LoadStatus::NotFound:
        case LoadStatus::Corrupt:
        case LoadStatus::LeaseNotHeld:
        case LoadStatus::DirectoryIdentityChanged:
        case LoadStatus::CandidateFenced:
        case LoadStatus::KeyNotFound:
        case LoadStatus::IoError:
            return true;
    }
    return false;
}

void expect_wire_fields_eq(const SealStartedAbandonWire& got, const SealStartedAbandonWire& want) {
    EXPECT_EQ(got.format_version, want.format_version);
    EXPECT_EQ(got.total_bytes, want.total_bytes);
    EXPECT_EQ(got.store_uuid_lo, want.store_uuid_lo);
    EXPECT_EQ(got.store_uuid_hi, want.store_uuid_hi);
    EXPECT_EQ(got.candidate_id, want.candidate_id);
    EXPECT_EQ(got.request_id, want.request_id);
    EXPECT_EQ(got.kek_key_id, want.kek_key_id);
    EXPECT_EQ(got.started_kind, want.started_kind);
    EXPECT_EQ(got.abandon_reason, want.abandon_reason);
    EXPECT_EQ(got.present_mask, want.present_mask);
    EXPECT_EQ(got.phase, want.phase);
    EXPECT_EQ(got.source_generation, want.source_generation);
    EXPECT_EQ(got.baseline_tip_seq, want.baseline_tip_seq);
    EXPECT_EQ(0, std::memcmp(got.baseline_tip_mac, want.baseline_tip_mac, 32));
    EXPECT_EQ(got.baseline_key_id, want.baseline_key_id);
    EXPECT_EQ(0, std::memcmp(got.content_root, want.content_root, 32));
    EXPECT_EQ(0, std::memcmp(got.digest_C, want.digest_C, 32));
    EXPECT_EQ(0, std::memcmp(got.mac, want.mac, 32));
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Round-trip: every field, including abandon_reason / phase
// ---------------------------------------------------------------------------

TEST(SealStartedAbandonLoader, RoundTripAllFields) {
    Fixture fx;
    SealStartedAbandonWire original = make_sample_abandon(Fixture::kKeyId);

    MockCandidateLeaseForAbandon lease;
    lease.canned_bytes = fx.encode(original);

    SealStartedAbandonWire out = make_sentinel_out();
    ASSERT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::Ok);
    expect_wire_fields_eq(out, original);
}

// ---------------------------------------------------------------------------
// 2. CandidateFenced
// ---------------------------------------------------------------------------

TEST(SealStartedAbandonLoader, CandidateFencedMapsToCandidateFenced) {
    Fixture fx;
    MockCandidateLeaseForAbandon lease;
    lease.next_outcome = LeaseIoOutcome::CandidateFenced;

    SealStartedAbandonWire out = make_sentinel_out();
    const SealStartedAbandonWire before = out;
    EXPECT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::CandidateFenced);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 3. DirectoryIdentityChanged
// ---------------------------------------------------------------------------

TEST(SealStartedAbandonLoader, DirectoryIdentityChangedMapsToDirectoryIdentityChanged) {
    Fixture fx;
    MockCandidateLeaseForAbandon lease;
    lease.next_outcome = LeaseIoOutcome::DirectoryIdentityChanged;

    SealStartedAbandonWire out = make_sentinel_out();
    const SealStartedAbandonWire before = out;
    EXPECT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::DirectoryIdentityChanged);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 4. WrongOwner / NotHeld -- separate explicit TESTs, both -> LeaseNotHeld
// ---------------------------------------------------------------------------

TEST(SealStartedAbandonLoader, WrongOwnerMapsToLeaseNotHeld) {
    Fixture fx;
    MockCandidateLeaseForAbandon lease;
    lease.next_outcome = LeaseIoOutcome::WrongOwner;

    SealStartedAbandonWire out = make_sentinel_out();
    const SealStartedAbandonWire before = out;
    EXPECT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::LeaseNotHeld);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

TEST(SealStartedAbandonLoader, NotHeldMapsToLeaseNotHeld) {
    Fixture fx;
    MockCandidateLeaseForAbandon lease;
    lease.next_outcome = LeaseIoOutcome::NotHeld;

    SealStartedAbandonWire out = make_sentinel_out();
    const SealStartedAbandonWire before = out;
    EXPECT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::LeaseNotHeld);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 5. NotFound
// ---------------------------------------------------------------------------

TEST(SealStartedAbandonLoader, NotFoundMapsToNotFound) {
    Fixture fx;
    MockCandidateLeaseForAbandon lease;
    lease.next_read_status = hy::compaction_detail::ReadFixedStatus::NotFound;

    SealStartedAbandonWire out = make_sentinel_out();
    const SealStartedAbandonWire before = out;
    EXPECT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::NotFound);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 6. WrongSize: wire-1 and wire+1 both -> Corrupt
// ---------------------------------------------------------------------------

TEST(SealStartedAbandonLoader, WrongSizeSmallerByOneMapsToCorrupt) {
    Fixture fx;
    MockCandidateLeaseForAbandon lease;
    lease.next_read_status = hy::compaction_detail::ReadFixedStatus::WrongSize;
    lease.canned_bytes.assign(kSealStartedAbandonWireBytes - 1, std::byte{0x11});

    SealStartedAbandonWire out = make_sentinel_out();
    const SealStartedAbandonWire before = out;
    EXPECT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::Corrupt);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

TEST(SealStartedAbandonLoader, WrongSizeLargerByOneMapsToCorrupt) {
    Fixture fx;
    MockCandidateLeaseForAbandon lease;
    lease.next_read_status = hy::compaction_detail::ReadFixedStatus::WrongSize;
    lease.canned_bytes.assign(kSealStartedAbandonWireBytes + 1, std::byte{0x22});

    SealStartedAbandonWire out = make_sentinel_out();
    const SealStartedAbandonWire before = out;
    EXPECT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::Corrupt);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 7. format_version -> UnknownVersion at decode -> Corrupt
// ---------------------------------------------------------------------------

TEST(SealStartedAbandonLoader, UnknownFormatVersionMapsToCorrupt) {
    Fixture fx;
    SealStartedAbandonWire v = make_sample_abandon(Fixture::kKeyId);
    v.format_version = 99;
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    ASSERT_EQ(encode_seal_started_abandon_wire(buf, v, fx.key_), kSealStartedAbandonWireBytes);

    MockCandidateLeaseForAbandon lease;
    lease.canned_bytes.assign(buf.begin(), buf.end());

    SealStartedAbandonWire out = make_sentinel_out();
    const SealStartedAbandonWire before = out;
    ASSERT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::Corrupt);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 8. total_bytes -> TotalBytesInvalid at decode -> Corrupt
// ---------------------------------------------------------------------------

TEST(SealStartedAbandonLoader, InvalidTotalBytesMapsToCorrupt) {
    Fixture fx;
    SealStartedAbandonWire v = make_sample_abandon(Fixture::kKeyId);
    v.total_bytes = 999;
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    ASSERT_EQ(encode_seal_started_abandon_wire(buf, v, fx.key_), kSealStartedAbandonWireBytes);

    MockCandidateLeaseForAbandon lease;
    lease.canned_bytes.assign(buf.begin(), buf.end());

    SealStartedAbandonWire out = make_sentinel_out();
    const SealStartedAbandonWire before = out;
    EXPECT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::Corrupt);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 9. MAC tamper: first / middle / last byte -- three separate TESTs
// ---------------------------------------------------------------------------

TEST(SealStartedAbandonLoader, MacTamperFirstByteMapsToCorrupt) {
    Fixture fx;
    SealStartedAbandonWire sample = make_sample_abandon(Fixture::kKeyId);
    MockCandidateLeaseForAbandon lease;
    lease.canned_bytes = fx.encode(sample);
    lease.canned_bytes.front() ^= std::byte{0x01};

    SealStartedAbandonWire out = make_sentinel_out();
    const SealStartedAbandonWire before = out;
    ASSERT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::Corrupt);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

TEST(SealStartedAbandonLoader, MacTamperMiddleByteMapsToCorrupt) {
    Fixture fx;
    SealStartedAbandonWire sample = make_sample_abandon(Fixture::kKeyId);
    MockCandidateLeaseForAbandon lease;
    lease.canned_bytes = fx.encode(sample);
    lease.canned_bytes[kSealStartedAbandonWireBytes / 2] ^= std::byte{0x01};

    SealStartedAbandonWire out = make_sentinel_out();
    const SealStartedAbandonWire before = out;
    ASSERT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::Corrupt);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

TEST(SealStartedAbandonLoader, MacTamperLastByteMapsToCorrupt) {
    Fixture fx;
    SealStartedAbandonWire sample = make_sample_abandon(Fixture::kKeyId);
    MockCandidateLeaseForAbandon lease;
    lease.canned_bytes = fx.encode(sample);
    lease.canned_bytes.back() ^= std::byte{0x01};

    SealStartedAbandonWire out = make_sentinel_out();
    const SealStartedAbandonWire before = out;
    ASSERT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::Corrupt);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 10. Unknown kek_key_id -> KeyNotFound
// ---------------------------------------------------------------------------

TEST(SealStartedAbandonLoader, UnknownKekKeyIdMapsToKeyNotFound) {
    Fixture fx;
    SealStartedAbandonWire v = make_sample_abandon(/*kek_key_id=*/99);
    std::array<std::byte, kSealStartedAbandonWireBytes> buf{};
    ASSERT_EQ(encode_seal_started_abandon_wire(buf, v, fx.key_), kSealStartedAbandonWireBytes);

    MockCandidateLeaseForAbandon lease;
    lease.canned_bytes.assign(buf.begin(), buf.end());

    SealStartedAbandonWire out = make_sentinel_out();
    const SealStartedAbandonWire before = out;
    EXPECT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::KeyNotFound);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 11. phase / abandon_reason legal-range boundaries (min and max round-trip)
//
// abandon_reason legal set is only {kSealStartedAbandonReasonNotFound=1}
// (validate_seal_started_abandon_shape). phase legal set is
// [Authorized=0 .. AbdPending=7] per durable_control_plane.hpp.
// ---------------------------------------------------------------------------

TEST(SealStartedAbandonLoader, RoundTripPhaseMinAndAbandonReasonLegal) {
    Fixture fx;
    SealStartedAbandonWire original = make_sample_abandon(Fixture::kKeyId);
    original.phase = kSealStartedAbandonPhaseAuthorized;
    original.abandon_reason = kSealStartedAbandonReasonNotFound;

    MockCandidateLeaseForAbandon lease;
    lease.canned_bytes = fx.encode(original);  // also fills original.mac

    SealStartedAbandonWire out = make_sentinel_out();
    ASSERT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::Ok);
    EXPECT_EQ(out.phase, kSealStartedAbandonPhaseAuthorized);
    EXPECT_EQ(out.abandon_reason, kSealStartedAbandonReasonNotFound);
    expect_wire_fields_eq(out, original);
}

TEST(SealStartedAbandonLoader, RoundTripPhaseMaxAndAbandonReasonLegal) {
    Fixture fx;
    SealStartedAbandonWire original = make_sample_abandon(Fixture::kKeyId);
    original.phase = kSealStartedAbandonPhaseAbdPending;
    original.abandon_reason = kSealStartedAbandonReasonNotFound;

    MockCandidateLeaseForAbandon lease;
    lease.canned_bytes = fx.encode(original);

    SealStartedAbandonWire out = make_sentinel_out();
    ASSERT_EQ(load_via_mock(lease, fx.ring_, out), LoadStatus::Ok);
    EXPECT_EQ(out.phase, kSealStartedAbandonPhaseAbdPending);
    EXPECT_EQ(out.abandon_reason, kSealStartedAbandonReasonNotFound);
    expect_wire_fields_eq(out, original);
}

// ---------------------------------------------------------------------------
// 12. Property test: 4000 random-byte iterations
// ---------------------------------------------------------------------------

TEST(SealStartedAbandonLoader, PropertyRandomBytesNeverCrashAndPreserveOutOnFailure) {
    Fixture fx;
    std::mt19937 rng(0xC0FFEEU);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> len_dist(0, static_cast<int>(kSealStartedAbandonWireBytes * 2));
    std::uniform_int_distribution<int> outcome_dist(0, 4);
    std::uniform_int_distribution<int> read_status_dist(0, 4);

    for (int trial = 0; trial < 4000; ++trial) {
        MockCandidateLeaseForAbandon lease;
        switch (outcome_dist(rng)) {
            case 0:
                lease.next_outcome = LeaseIoOutcome::Ok;
                break;
            case 1:
                lease.next_outcome = LeaseIoOutcome::WrongOwner;
                break;
            case 2:
                lease.next_outcome = LeaseIoOutcome::NotHeld;
                break;
            case 3:
                lease.next_outcome = LeaseIoOutcome::CandidateFenced;
                break;
            default:
                lease.next_outcome = LeaseIoOutcome::DirectoryIdentityChanged;
                break;
        }
        switch (read_status_dist(rng)) {
            case 0:
                lease.next_read_status = hy::compaction_detail::ReadFixedStatus::Ok;
                break;
            case 1:
                lease.next_read_status = hy::compaction_detail::ReadFixedStatus::NotFound;
                break;
            case 2:
                lease.next_read_status = hy::compaction_detail::ReadFixedStatus::WrongSize;
                break;
            case 3:
                lease.next_read_status = hy::compaction_detail::ReadFixedStatus::NotRegularFile;
                break;
            default:
                lease.next_read_status = hy::compaction_detail::ReadFixedStatus::IoError;
                break;
        }

        const int n = len_dist(rng);
        lease.canned_bytes.resize(static_cast<std::size_t>(n));
        for (auto& b : lease.canned_bytes) {
            b = static_cast<std::byte>(byte_dist(rng));
        }

        SealStartedAbandonWire out = make_sentinel_out();
        const SealStartedAbandonWire before = out;
        const LoadStatus status = load_via_mock(lease, fx.ring_, out);
        ASSERT_TRUE(is_legal_load_status(status)) << "trial=" << trial;
        if (status != LoadStatus::Ok) {
            EXPECT_TRUE(wire_fields_equal(out, before)) << "trial=" << trial;
        }
    }
}

TEST(SealStartedAbandonLoader, PublicSurfaceBindsCandidateLeaseReference) {
    static_assert(std::is_constructible_v<SealStartedAbandonLoader, hy::CandidateLease&>);
    static_assert(std::is_nothrow_constructible_v<SealStartedAbandonLoader, hy::CandidateLease&>);
}
