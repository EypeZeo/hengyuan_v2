// Read-only CompactionIntentGcAuthorizedLoader tests (`.xgc`). Branch
// classification is driven through a mock lease into
// CompactionIntentGcAuthorizedLoader::load_from (the same function load()
// calls) -- same structure as test_seal_started_abandon_loader.cpp, adapted
// for the one real difference this loader has: `.xgc`'s filename carries
// build_nonce, so load()/load_from() take an extra parameter the other six
// breadcrumb loaders don't need. One additional test at the end drives the
// REAL CandidateLease + ValidatedArtifactName::for_xgc() path end-to-end
// (not just the mocked branch logic), proving the new read method and
// filename factory actually wire together correctly.

#include <gtest/gtest.h>
#include <hengyuan/compaction_intent_gc_authorized_loader.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

using hy::CandidateLease;
using hy::CompactionIntentGcAuthorizedLoader;
using hy::CompactionIntentGcAuthorizedWire;
using hy::KeyRing;
using hy::KeyRingAddStatus;
using hy::LeaseAcquireStatus;
using hy::LeaseIoOutcome;
using hy::LeaseReadResult;
using hy::LoadStatus;
using hy::ReleaseStatus;
using hy::WrappedKeyRecord;
using hy::encode_compaction_intent_gc_authorized_wire;
using hy::kCompactionCandidateIntentPhaseBuilding;
using hy::kCompactionIntentGcAuthorizedFormatVersion;
using hy::kCompactionIntentGcAuthorizedWireBytes;
using hy::kCompactionIntentGcDispositionPreSealAbandonClear;
using hy::kKeyBlockSize;
using hy::kKekSize;

namespace {

struct MockCandidateLeaseForGcAuthorized {
    LeaseIoOutcome next_outcome = LeaseIoOutcome::Ok;
    hy::compaction_detail::ReadFixedStatus next_read_status = hy::compaction_detail::ReadFixedStatus::Ok;
    std::vector<std::byte> canned_bytes;

    LeaseReadResult read_compaction_intent_gc_authorized(
        std::uint64_t /*build_nonce*/, std::span<std::byte, kCompactionIntentGcAuthorizedWireBytes> out) noexcept {
        LeaseReadResult r{};
        r.outcome = next_outcome;
        if (next_outcome != LeaseIoOutcome::Ok) return r;
        r.read.status = next_read_status;
        if (next_read_status == hy::compaction_detail::ReadFixedStatus::Ok) {
            const std::size_t n = std::min(out.size(), canned_bytes.size());
            // memcpy's second parameter is nonnull-annotated (glibc/GCC) --
            // an empty canned_bytes can make .data() return nullptr even
            // with n==0, which UBSan flags as UB despite the zero length.
            if (n > 0) std::memcpy(out.data(), canned_bytes.data(), n);
        }
        return r;
    }
};

LoadStatus load_via_mock(MockCandidateLeaseForGcAuthorized& lease, std::uint64_t build_nonce, KeyRing& key_ring,
                          CompactionIntentGcAuthorizedWire& out) noexcept {
    return CompactionIntentGcAuthorizedLoader::load_from(lease, build_nonce, key_ring, out);
}

std::array<std::byte, kKekSize> make_kek(std::uint8_t fill) {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(fill + static_cast<std::uint8_t>(i));
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

CompactionIntentGcAuthorizedWire make_sample_gc(std::uint32_t kek_key_id, std::uint64_t build_nonce) {
    CompactionIntentGcAuthorizedWire g{};
    g.format_version = kCompactionIntentGcAuthorizedFormatVersion;
    g.total_bytes = static_cast<std::uint32_t>(kCompactionIntentGcAuthorizedWireBytes);
    g.store_uuid_lo = 0x1111111111111111ull;
    g.store_uuid_hi = 0x2222222222222222ull;
    g.kek_key_id = kek_key_id;
    // PreSealAbandonClear, Building-only clear -- the one disposition whose
    // legal shape needs no started_kind/present_mask/proof_*/gate_trailer_mac
    // (all stay default-zero), keeping this sample minimal and focused on
    // the loader's own branch logic rather than re-testing shape rules the
    // codec's own test file already covers exhaustively.
    g.terminal_disposition = kCompactionIntentGcDispositionPreSealAbandonClear;
    g.intent_phase_at_auth = kCompactionCandidateIntentPhaseBuilding;
    g.source_generation = 5;
    g.target_generation = 6;
    g.baseline_tip_seq = 42;
    fill_bytes(g.baseline_tip_mac, 0x10);
    g.baseline_key_id = 3;
    g.build_nonce = build_nonce;
    g.candidate_id = 100;
    g.request_id = 200;
    fill_bytes(g.intent_mac, 0x20);
    zero_bytes(g.terminal_transition_mac);  // legal all-zero only for Building-only PreSeal clear
    g.cleanup_auth_flags = 0;
    g.started_kind = 0;  // 0 = PreSeal
    g.present_mask_at_auth = 0;
    return g;
}

struct Fixture {
    static constexpr std::uint32_t kKeyId = 7;

    Fixture() : kek_(make_kek(0xA1)), key_(make_plaintext_key(0x40)), ring_(kek_) {
        WrappedKeyRecord record{};
        EXPECT_EQ(ring_.add_key(kKeyId, key_, record), KeyRingAddStatus::Ok);
    }

    std::vector<std::byte> encode(CompactionIntentGcAuthorizedWire& v) const {
        std::array<std::byte, kCompactionIntentGcAuthorizedWireBytes> buf{};
        EXPECT_EQ(encode_compaction_intent_gc_authorized_wire(buf, v, key_), kCompactionIntentGcAuthorizedWireBytes);
        // encode writes the MAC into the buffer only -- copy it back so
        // round-trip EXPECT_EQ against the preimage can include mac[32].
        std::memcpy(v.mac, buf.data() + (kCompactionIntentGcAuthorizedWireBytes - 32), 32);
        return std::vector<std::byte>(buf.begin(), buf.end());
    }

    std::array<std::byte, kKekSize> kek_;
    std::array<std::byte, kKeyBlockSize> key_;
    KeyRing ring_;
};

CompactionIntentGcAuthorizedWire make_sentinel_out() {
    CompactionIntentGcAuthorizedWire out{};
    out.format_version = 0xA5A5A5A5u;
    out.total_bytes = 0x5A5A5A5Au;
    out.store_uuid_lo = 0xDEADBEEFDEADBEEFull;
    out.store_uuid_hi = 0xCAFEBABECAFEBABEull;
    out.kek_key_id = 0x9999AAAAu;
    out.terminal_disposition = 0xBBu;
    out.intent_phase_at_auth = 0xCCu;
    out.reserved0 = 0xDDDDu;
    out.source_generation = 0xF00F1234u;
    out.target_generation = 0x1234F00Fu;
    out.baseline_tip_seq = 0x0123456789ABCDEFull;
    out.baseline_key_id = 0x13579BDFu;
    out.build_nonce = 0x1111222233334444ull;
    out.candidate_id = 0x5555666677778888ull;
    out.request_id = 0x99998888AAAABBBBull;
    out.cleanup_auth_flags = 0xEEu;
    out.started_kind = 0xFFu;
    out.present_mask_at_auth = 0x11u;
    out.reserved1 = 0x22u;
    out.proof_new_final_seq = 0xFEDCBA9876543210ull;
    out.proof_new_key_id = 0x87654321u;
    fill_bytes(out.baseline_tip_mac, 0x10);
    fill_bytes(out.intent_mac, 0x40);
    fill_bytes(out.terminal_transition_mac, 0x50);
    fill_bytes(out.proof_new_final_tip_mac, 0x60);
    fill_bytes(out.proof_content_root, 0x70);
    fill_bytes(out.gate_trailer_mac, 0x80);
    fill_bytes(out.mac, 0xA0);
    return out;
}

bool wire_fields_equal(const CompactionIntentGcAuthorizedWire& a, const CompactionIntentGcAuthorizedWire& b) {
    return a.format_version == b.format_version && a.total_bytes == b.total_bytes &&
           a.store_uuid_lo == b.store_uuid_lo && a.store_uuid_hi == b.store_uuid_hi &&
           a.kek_key_id == b.kek_key_id && a.terminal_disposition == b.terminal_disposition &&
           a.intent_phase_at_auth == b.intent_phase_at_auth && a.reserved0 == b.reserved0 &&
           a.source_generation == b.source_generation && a.target_generation == b.target_generation &&
           a.baseline_tip_seq == b.baseline_tip_seq && a.baseline_key_id == b.baseline_key_id &&
           a.build_nonce == b.build_nonce && a.candidate_id == b.candidate_id && a.request_id == b.request_id &&
           a.cleanup_auth_flags == b.cleanup_auth_flags && a.started_kind == b.started_kind &&
           a.present_mask_at_auth == b.present_mask_at_auth && a.reserved1 == b.reserved1 &&
           a.proof_new_final_seq == b.proof_new_final_seq && a.proof_new_key_id == b.proof_new_key_id &&
           std::memcmp(a.baseline_tip_mac, b.baseline_tip_mac, 32) == 0 &&
           std::memcmp(a.intent_mac, b.intent_mac, 32) == 0 &&
           std::memcmp(a.terminal_transition_mac, b.terminal_transition_mac, 32) == 0 &&
           std::memcmp(a.proof_new_final_tip_mac, b.proof_new_final_tip_mac, 32) == 0 &&
           std::memcmp(a.proof_content_root, b.proof_content_root, 32) == 0 &&
           std::memcmp(a.gate_trailer_mac, b.gate_trailer_mac, 32) == 0 && std::memcmp(a.mac, b.mac, 32) == 0;
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

void expect_wire_fields_eq(const CompactionIntentGcAuthorizedWire& got, const CompactionIntentGcAuthorizedWire& want) {
    EXPECT_EQ(got.format_version, want.format_version);
    EXPECT_EQ(got.total_bytes, want.total_bytes);
    EXPECT_EQ(got.store_uuid_lo, want.store_uuid_lo);
    EXPECT_EQ(got.store_uuid_hi, want.store_uuid_hi);
    EXPECT_EQ(got.kek_key_id, want.kek_key_id);
    EXPECT_EQ(got.terminal_disposition, want.terminal_disposition);
    EXPECT_EQ(got.intent_phase_at_auth, want.intent_phase_at_auth);
    EXPECT_EQ(got.source_generation, want.source_generation);
    EXPECT_EQ(got.target_generation, want.target_generation);
    EXPECT_EQ(got.baseline_tip_seq, want.baseline_tip_seq);
    EXPECT_EQ(0, std::memcmp(got.baseline_tip_mac, want.baseline_tip_mac, 32));
    EXPECT_EQ(got.baseline_key_id, want.baseline_key_id);
    EXPECT_EQ(got.build_nonce, want.build_nonce);
    EXPECT_EQ(got.candidate_id, want.candidate_id);
    EXPECT_EQ(got.request_id, want.request_id);
    EXPECT_EQ(0, std::memcmp(got.intent_mac, want.intent_mac, 32));
    EXPECT_EQ(0, std::memcmp(got.terminal_transition_mac, want.terminal_transition_mac, 32));
    EXPECT_EQ(got.cleanup_auth_flags, want.cleanup_auth_flags);
    EXPECT_EQ(got.started_kind, want.started_kind);
    EXPECT_EQ(got.present_mask_at_auth, want.present_mask_at_auth);
    EXPECT_EQ(got.proof_new_final_seq, want.proof_new_final_seq);
    EXPECT_EQ(0, std::memcmp(got.proof_new_final_tip_mac, want.proof_new_final_tip_mac, 32));
    EXPECT_EQ(got.proof_new_key_id, want.proof_new_key_id);
    EXPECT_EQ(0, std::memcmp(got.proof_content_root, want.proof_content_root, 32));
    EXPECT_EQ(0, std::memcmp(got.gate_trailer_mac, want.gate_trailer_mac, 32));
    EXPECT_EQ(0, std::memcmp(got.mac, want.mac, 32));
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Round-trip: every field
// ---------------------------------------------------------------------------

TEST(CompactionIntentGcAuthorizedLoader, RoundTripAllFields) {
    Fixture fx;
    CompactionIntentGcAuthorizedWire original = make_sample_gc(Fixture::kKeyId, /*build_nonce=*/0xABCULL);

    MockCandidateLeaseForGcAuthorized lease;
    lease.canned_bytes = fx.encode(original);

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    ASSERT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::Ok);
    expect_wire_fields_eq(out, original);
}

// ---------------------------------------------------------------------------
// 2. CandidateFenced
// ---------------------------------------------------------------------------

TEST(CompactionIntentGcAuthorizedLoader, CandidateFencedMapsToCandidateFenced) {
    Fixture fx;
    MockCandidateLeaseForGcAuthorized lease;
    lease.next_outcome = LeaseIoOutcome::CandidateFenced;

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    const CompactionIntentGcAuthorizedWire before = out;
    EXPECT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::CandidateFenced);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 3. DirectoryIdentityChanged
// ---------------------------------------------------------------------------

TEST(CompactionIntentGcAuthorizedLoader, DirectoryIdentityChangedMapsToDirectoryIdentityChanged) {
    Fixture fx;
    MockCandidateLeaseForGcAuthorized lease;
    lease.next_outcome = LeaseIoOutcome::DirectoryIdentityChanged;

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    const CompactionIntentGcAuthorizedWire before = out;
    EXPECT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::DirectoryIdentityChanged);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 4. WrongOwner / NotHeld -- separate explicit TESTs, both -> LeaseNotHeld
// ---------------------------------------------------------------------------

TEST(CompactionIntentGcAuthorizedLoader, WrongOwnerMapsToLeaseNotHeld) {
    Fixture fx;
    MockCandidateLeaseForGcAuthorized lease;
    lease.next_outcome = LeaseIoOutcome::WrongOwner;

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    const CompactionIntentGcAuthorizedWire before = out;
    EXPECT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::LeaseNotHeld);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

TEST(CompactionIntentGcAuthorizedLoader, NotHeldMapsToLeaseNotHeld) {
    Fixture fx;
    MockCandidateLeaseForGcAuthorized lease;
    lease.next_outcome = LeaseIoOutcome::NotHeld;

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    const CompactionIntentGcAuthorizedWire before = out;
    EXPECT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::LeaseNotHeld);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 5. NotFound
// ---------------------------------------------------------------------------

TEST(CompactionIntentGcAuthorizedLoader, NotFoundMapsToNotFound) {
    Fixture fx;
    MockCandidateLeaseForGcAuthorized lease;
    lease.next_read_status = hy::compaction_detail::ReadFixedStatus::NotFound;

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    const CompactionIntentGcAuthorizedWire before = out;
    EXPECT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::NotFound);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 6. WrongSize: wire-1 and wire+1 both -> Corrupt
// ---------------------------------------------------------------------------

TEST(CompactionIntentGcAuthorizedLoader, WrongSizeSmallerByOneMapsToCorrupt) {
    Fixture fx;
    MockCandidateLeaseForGcAuthorized lease;
    lease.next_read_status = hy::compaction_detail::ReadFixedStatus::WrongSize;
    lease.canned_bytes.assign(kCompactionIntentGcAuthorizedWireBytes - 1, std::byte{0x11});

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    const CompactionIntentGcAuthorizedWire before = out;
    EXPECT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::Corrupt);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

TEST(CompactionIntentGcAuthorizedLoader, WrongSizeLargerByOneMapsToCorrupt) {
    Fixture fx;
    MockCandidateLeaseForGcAuthorized lease;
    lease.next_read_status = hy::compaction_detail::ReadFixedStatus::WrongSize;
    lease.canned_bytes.assign(kCompactionIntentGcAuthorizedWireBytes + 1, std::byte{0x22});

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    const CompactionIntentGcAuthorizedWire before = out;
    EXPECT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::Corrupt);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 7. IoError passes through unchanged (this loader's own Corrupt-not-IoError
//    distinction only applies to WrongSize/NotRegularFile, see the header
//    comment)
// ---------------------------------------------------------------------------

TEST(CompactionIntentGcAuthorizedLoader, IoErrorMapsToIoError) {
    Fixture fx;
    MockCandidateLeaseForGcAuthorized lease;
    lease.next_read_status = hy::compaction_detail::ReadFixedStatus::IoError;

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    const CompactionIntentGcAuthorizedWire before = out;
    EXPECT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::IoError);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 8. format_version -> UnknownVersion at decode -> Corrupt
// ---------------------------------------------------------------------------

TEST(CompactionIntentGcAuthorizedLoader, UnknownFormatVersionMapsToCorrupt) {
    Fixture fx;
    CompactionIntentGcAuthorizedWire v = make_sample_gc(Fixture::kKeyId, /*build_nonce=*/0xABCULL);
    v.format_version = 99;
    std::array<std::byte, kCompactionIntentGcAuthorizedWireBytes> buf{};
    ASSERT_EQ(encode_compaction_intent_gc_authorized_wire(buf, v, fx.key_), kCompactionIntentGcAuthorizedWireBytes);

    MockCandidateLeaseForGcAuthorized lease;
    lease.canned_bytes.assign(buf.begin(), buf.end());

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    const CompactionIntentGcAuthorizedWire before = out;
    ASSERT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::Corrupt);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 9. total_bytes -> TotalBytesInvalid at decode -> Corrupt
// ---------------------------------------------------------------------------

TEST(CompactionIntentGcAuthorizedLoader, InvalidTotalBytesMapsToCorrupt) {
    Fixture fx;
    CompactionIntentGcAuthorizedWire v = make_sample_gc(Fixture::kKeyId, /*build_nonce=*/0xABCULL);
    v.total_bytes = 999;
    std::array<std::byte, kCompactionIntentGcAuthorizedWireBytes> buf{};
    ASSERT_EQ(encode_compaction_intent_gc_authorized_wire(buf, v, fx.key_), kCompactionIntentGcAuthorizedWireBytes);

    MockCandidateLeaseForGcAuthorized lease;
    lease.canned_bytes.assign(buf.begin(), buf.end());

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    const CompactionIntentGcAuthorizedWire before = out;
    EXPECT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::Corrupt);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 10. MAC tamper: first / middle / last byte -- three separate TESTs
// ---------------------------------------------------------------------------

TEST(CompactionIntentGcAuthorizedLoader, MacTamperFirstByteMapsToCorrupt) {
    Fixture fx;
    CompactionIntentGcAuthorizedWire sample = make_sample_gc(Fixture::kKeyId, /*build_nonce=*/0xABCULL);
    MockCandidateLeaseForGcAuthorized lease;
    lease.canned_bytes = fx.encode(sample);
    lease.canned_bytes.front() ^= std::byte{0x01};

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    const CompactionIntentGcAuthorizedWire before = out;
    ASSERT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::Corrupt);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

TEST(CompactionIntentGcAuthorizedLoader, MacTamperMiddleByteMapsToCorrupt) {
    Fixture fx;
    CompactionIntentGcAuthorizedWire sample = make_sample_gc(Fixture::kKeyId, /*build_nonce=*/0xABCULL);
    MockCandidateLeaseForGcAuthorized lease;
    lease.canned_bytes = fx.encode(sample);
    lease.canned_bytes[kCompactionIntentGcAuthorizedWireBytes / 2] ^= std::byte{0x01};

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    const CompactionIntentGcAuthorizedWire before = out;
    ASSERT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::Corrupt);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

TEST(CompactionIntentGcAuthorizedLoader, MacTamperLastByteMapsToCorrupt) {
    Fixture fx;
    CompactionIntentGcAuthorizedWire sample = make_sample_gc(Fixture::kKeyId, /*build_nonce=*/0xABCULL);
    MockCandidateLeaseForGcAuthorized lease;
    lease.canned_bytes = fx.encode(sample);
    lease.canned_bytes.back() ^= std::byte{0x01};

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    const CompactionIntentGcAuthorizedWire before = out;
    ASSERT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::Corrupt);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 11. Unknown kek_key_id -> KeyNotFound
// ---------------------------------------------------------------------------

TEST(CompactionIntentGcAuthorizedLoader, UnknownKekKeyIdMapsToKeyNotFound) {
    Fixture fx;
    CompactionIntentGcAuthorizedWire v = make_sample_gc(/*kek_key_id=*/99, /*build_nonce=*/0xABCULL);
    std::array<std::byte, kCompactionIntentGcAuthorizedWireBytes> buf{};
    ASSERT_EQ(encode_compaction_intent_gc_authorized_wire(buf, v, fx.key_), kCompactionIntentGcAuthorizedWireBytes);

    MockCandidateLeaseForGcAuthorized lease;
    lease.canned_bytes.assign(buf.begin(), buf.end());

    CompactionIntentGcAuthorizedWire out = make_sentinel_out();
    const CompactionIntentGcAuthorizedWire before = out;
    EXPECT_EQ(load_via_mock(lease, 0xABCULL, fx.ring_, out), LoadStatus::KeyNotFound);
    EXPECT_TRUE(wire_fields_equal(out, before));
}

// ---------------------------------------------------------------------------
// 12. Property test: 4000 random-byte iterations
// ---------------------------------------------------------------------------

TEST(CompactionIntentGcAuthorizedLoader, PropertyRandomBytesNeverCrashAndPreserveOutOnFailure) {
    Fixture fx;
    std::mt19937 rng(0xC0FFEEU);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> len_dist(0, static_cast<int>(kCompactionIntentGcAuthorizedWireBytes * 2));
    std::uniform_int_distribution<int> outcome_dist(0, 4);
    std::uniform_int_distribution<int> read_status_dist(0, 4);

    for (int trial = 0; trial < 4000; ++trial) {
        MockCandidateLeaseForGcAuthorized lease;
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

        CompactionIntentGcAuthorizedWire out = make_sentinel_out();
        const CompactionIntentGcAuthorizedWire before = out;
        const LoadStatus status = load_via_mock(lease, 0xABCULL, fx.ring_, out);
        ASSERT_TRUE(is_legal_load_status(status)) << "trial=" << trial;
        if (status != LoadStatus::Ok) {
            EXPECT_TRUE(wire_fields_equal(out, before)) << "trial=" << trial;
        }
    }
}

TEST(CompactionIntentGcAuthorizedLoader, PublicSurfaceBindsCandidateLeaseReference) {
    static_assert(std::is_constructible_v<CompactionIntentGcAuthorizedLoader, hy::CandidateLease&>);
    static_assert(std::is_nothrow_constructible_v<CompactionIntentGcAuthorizedLoader, hy::CandidateLease&>);
}

// ---------------------------------------------------------------------------
// 13. Real CandidateLease + real filesystem: proves for_xgc(build_nonce) and
//     read_compaction_intent_gc_authorized() actually wire together, not
//     just the mocked branch logic above. Plants the file directly via
//     std::ofstream (bypassing CandidateLease's own write path -- this repo
//     has no `.xgc` writer, same precedent as plant_watermark()/plant_clr()/
//     plant_abd() in the other test files for this exact reason).
// ---------------------------------------------------------------------------

namespace {

std::filesystem::path make_temp_candidate_dir(std::string_view tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("hy_xgc_loader_" + std::string(tag) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST(CompactionIntentGcAuthorizedLoader, RealCandidateLeaseRoundTripViaForXgcFilename) {
    Fixture fx;
    constexpr std::uint64_t kBuildNonce = 0x1234ABCDEF56ULL;
    auto dir = make_temp_candidate_dir("real_roundtrip");

    CompactionIntentGcAuthorizedWire original = make_sample_gc(Fixture::kKeyId, kBuildNonce);
    std::array<std::byte, kCompactionIntentGcAuthorizedWireBytes> encoded{};
    ASSERT_EQ(encode_compaction_intent_gc_authorized_wire(encoded, original, fx.key_),
              kCompactionIntentGcAuthorizedWireBytes);

    const auto name = hy::compaction_detail::ValidatedArtifactName::for_xgc(kBuildNonce);
    {
        std::ofstream out(dir / name.relative_name(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    }

    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);

    CompactionIntentGcAuthorizedLoader loader(lease);
    CompactionIntentGcAuthorizedWire loaded{};
    ASSERT_EQ(loader.load(kBuildNonce, fx.ring_, loaded), LoadStatus::Ok);
    EXPECT_EQ(loaded.build_nonce, kBuildNonce);
    EXPECT_EQ(loaded.candidate_id, original.candidate_id);

    // A different build_nonce must not find this file -- `.xgc` identity is
    // build_nonce-scoped, confirming for_xgc() actually encodes build_nonce
    // into the filename rather than ignoring it.
    CompactionIntentGcAuthorizedWire not_found_out{};
    EXPECT_EQ(loader.load(kBuildNonce + 1, fx.ring_, not_found_out), LoadStatus::NotFound);

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}
