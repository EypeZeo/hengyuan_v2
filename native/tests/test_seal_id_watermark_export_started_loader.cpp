// L2 read-only loader coverage. The production interface intentionally accepts
// CandidateLease&, not an injectable filesystem handle: exercising it through
// a real temporary directory verifies the fixed-name, handle-relative path.
#include <gtest/gtest.h>
#include <hengyuan/seal_id_watermark_export_started_loader.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>

using namespace hy;

namespace {

std::filesystem::path make_temp_candidate_dir(std::string_view tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("hy_seal_loader_" + std::string(tag) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::array<std::byte, kKekSize> make_kek(std::uint8_t seed) {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(seed + i);
    return kek;
}

void add_key(KeyRing& key_ring, std::uint32_t key_id, std::span<const std::byte> key) {
    WrappedKeyRecord record{};
    ASSERT_EQ(key_ring.add_key(key_id, key, record), KeyRingAddStatus::Ok);
}

void write_bytes(const std::filesystem::path& file, std::span<const std::byte> bytes) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(stream.good());
}

SealIdWatermark make_watermark() {
    SealIdWatermark value{};
    value.store_uuid_lo = 0x1111111111111111ULL;
    value.store_uuid_hi = 0x2222222222222222ULL;
    value.next_candidate_id = 42;
    value.next_request_id = 43;
    return value;
}

void fill_bytes(std::uint8_t (&out)[32], std::uint8_t seed) {
    for (std::size_t i = 0; i < std::size(out); ++i) out[i] = static_cast<std::uint8_t>(seed + i);
}

SealExportStartedWire make_started(std::uint32_t key_id) {
    SealExportStartedWire value{};
    value.format_version = kSealExportStartedFormatVersion;
    value.total_bytes = kSealExportStartedWireBytes;
    value.store_uuid_lo = 0x1111111111111111ULL;
    value.store_uuid_hi = 0x2222222222222222ULL;
    value.candidate_id = 100;
    value.source_generation = 5;
    value.baseline_tip_seq = 42;
    fill_bytes(value.baseline_tip_mac, 0x10);
    value.baseline_key_id = 3;
    value.new_generation = 6;
    value.new_final_seq = 99;
    fill_bytes(value.new_final_tip_mac, 0x20);
    value.new_key_id = 4;
    value.request_id = 200;
    fill_bytes(value.content_root, 0x30);
    value.kek_key_id = key_id;
    value.registered_producer_mask = 0b0010'0101;
    value.producer_count = 3;
    value.ring_id[0] = 101;
    value.ring_id[2] = 102;
    value.ring_id[5] = 103;
    return value;
}

void expect_started_eq(const SealExportStartedWire& actual, const SealExportStartedWire& expected) {
    EXPECT_EQ(actual.format_version, expected.format_version);
    EXPECT_EQ(actual.total_bytes, expected.total_bytes);
    EXPECT_EQ(actual.store_uuid_lo, expected.store_uuid_lo);
    EXPECT_EQ(actual.store_uuid_hi, expected.store_uuid_hi);
    EXPECT_EQ(actual.candidate_id, expected.candidate_id);
    EXPECT_EQ(actual.source_generation, expected.source_generation);
    EXPECT_EQ(actual.baseline_tip_seq, expected.baseline_tip_seq);
    // EXPECT_EQ on a raw C array decays both sides to pointers and compares
    // addresses, not contents -- these three fields are std::uint8_t[32]
    // members, so they need an explicit byte-wise comparison instead (same
    // memcmp idiom this codebase already uses elsewhere for mac[32]-style
    // fields, e.g. the codec round-trip tests).
    EXPECT_EQ(0, std::memcmp(actual.baseline_tip_mac, expected.baseline_tip_mac, sizeof(actual.baseline_tip_mac)));
    EXPECT_EQ(actual.baseline_key_id, expected.baseline_key_id);
    EXPECT_EQ(actual.new_generation, expected.new_generation);
    EXPECT_EQ(actual.new_final_seq, expected.new_final_seq);
    EXPECT_EQ(0,
              std::memcmp(actual.new_final_tip_mac, expected.new_final_tip_mac, sizeof(actual.new_final_tip_mac)));
    EXPECT_EQ(actual.new_key_id, expected.new_key_id);
    EXPECT_EQ(actual.request_id, expected.request_id);
    EXPECT_EQ(0, std::memcmp(actual.content_root, expected.content_root, sizeof(actual.content_root)));
    EXPECT_EQ(actual.kek_key_id, expected.kek_key_id);
    EXPECT_EQ(actual.registered_producer_mask, expected.registered_producer_mask);
    EXPECT_EQ(actual.producer_count, expected.producer_count);
    for (std::size_t i = 0; i < std::size(actual.ring_id); ++i) EXPECT_EQ(actual.ring_id[i], expected.ring_id[i]);
}

bool is_load_status(LoadStatus status) {
    switch (status) {
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

}  // namespace

TEST(SealIdWatermarkLoader, RoundTripCopiesEveryField) {
    const auto dir = make_temp_candidate_dir("watermark_round_trip");
    const auto key = make_kek(0x11);
    KeyRing key_ring(key);
    add_key(key_ring, 7, key);
    const SealIdWatermark expected = make_watermark();
    std::array<std::byte, kSealIdWatermarkWireBytes> encoded{};
    ASSERT_EQ(encode_seal_id_watermark_wire(encoded, expected, key), encoded.size());
    write_bytes(dir / "seal-id-watermark", encoded);

    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    SealIdWatermarkLoader loader(lease);
    SealIdWatermark actual{};
    ASSERT_EQ(loader.load(7, key_ring, actual), LoadStatus::Ok);
    EXPECT_EQ(actual.store_uuid_lo, expected.store_uuid_lo);
    EXPECT_EQ(actual.store_uuid_hi, expected.store_uuid_hi);
    EXPECT_EQ(actual.next_candidate_id, expected.next_candidate_id);
    EXPECT_EQ(actual.next_request_id, expected.next_request_id);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealExportStartedLoader, LegacyAndMigrationCompanionRoundTripIndependently) {
    const auto dir = make_temp_candidate_dir("started_round_trip");
    const auto key = make_kek(0x21);
    KeyRing key_ring(key);
    add_key(key_ring, 7, key);
    const SealExportStartedWire legacy = make_started(7);
    SealExportStartedWire companion = make_started(7);
    companion.candidate_id = 101;
    std::array<std::byte, kSealExportStartedWireBytes> legacy_encoded{};
    std::array<std::byte, kSealExportStartedWireBytes> companion_encoded{};
    ASSERT_EQ(encode_seal_export_started_wire(legacy_encoded, legacy, key), legacy_encoded.size());
    ASSERT_EQ(encode_seal_export_started_wire(companion_encoded, companion, key), companion_encoded.size());
    write_bytes(dir / "seal-export-started", legacy_encoded);
    write_bytes(dir / "seal-export-started.v2", companion_encoded);

    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    SealExportStartedLoader loader(lease);
    SealExportStartedWire actual{};
    ASSERT_EQ(loader.load(true, key_ring, actual), LoadStatus::Ok);
    expect_started_eq(actual, legacy);
    ASSERT_EQ(loader.load(false, key_ring, actual), LoadStatus::Ok);
    expect_started_eq(actual, companion);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealIdWatermarkLoader, NotHeldAndWrongOwnerReturnLeaseNotHeld) {
    const auto dir = make_temp_candidate_dir("ownership");
    const auto key = make_kek(0x31);
    KeyRing key_ring(key);
    CandidateLease lease(dir);
    SealIdWatermarkLoader loader(lease);
    SealIdWatermark out{};
    EXPECT_EQ(loader.load(7, key_ring, out), LoadStatus::LeaseNotHeld);

    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    LoadStatus other_thread_status = LoadStatus::Ok;
    std::thread non_owner([&] { other_thread_status = loader.load(7, key_ring, out); });
    non_owner.join();
    EXPECT_EQ(other_thread_status, LoadStatus::LeaseNotHeld);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealIdWatermarkLoader, MissingAndWrongSizeAreClassifiedPrecisely) {
    const auto dir = make_temp_candidate_dir("missing_wrong_size");
    const auto key = make_kek(0x41);
    KeyRing key_ring(key);
    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    SealIdWatermarkLoader loader(lease);
    SealIdWatermark out{};
    EXPECT_EQ(loader.load(7, key_ring, out), LoadStatus::NotFound);
    const std::array<std::byte, 1> short_file{std::byte{0x01}};
    write_bytes(dir / "seal-id-watermark", short_file);
    EXPECT_EQ(loader.load(7, key_ring, out), LoadStatus::Corrupt);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealIdWatermarkLoader, DirectoryIdentityChangeThenStickyFenceAreDistinct) {
#ifdef _WIN32
    GTEST_SKIP() << "Windows intentionally denies candidate-directory rename while leased";
#else
    const auto dir = make_temp_candidate_dir("identity_change");
    const auto moved_dir = dir.string() + "_moved";
    const auto key = make_kek(0x49);
    KeyRing key_ring(key);
    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    SealIdWatermarkLoader loader(lease);
    SealIdWatermark out{};

    std::error_code error;
    std::filesystem::rename(dir, moved_dir, error);
    ASSERT_FALSE(error) << error.message();
    std::filesystem::create_directories(dir, error);
    ASSERT_FALSE(error) << error.message();
    EXPECT_EQ(loader.load(7, key_ring, out), LoadStatus::DirectoryIdentityChanged);
    EXPECT_EQ(loader.load(7, key_ring, out), LoadStatus::CandidateFenced);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(moved_dir);
#endif
}

TEST(SealIdWatermarkLoader, TamperedMacAndUnknownKeyFailClosed) {
    const auto dir = make_temp_candidate_dir("tampered_unknown_key");
    const auto key = make_kek(0x51);
    KeyRing key_ring(key);
    add_key(key_ring, 7, key);
    std::array<std::byte, kSealIdWatermarkWireBytes> encoded{};
    ASSERT_EQ(encode_seal_id_watermark_wire(encoded, make_watermark(), key), encoded.size());
    encoded[0] ^= std::byte{0x01};
    write_bytes(dir / "seal-id-watermark", encoded);

    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    SealIdWatermarkLoader loader(lease);
    SealIdWatermark out{};
    EXPECT_EQ(loader.load(7, key_ring, out), LoadStatus::Corrupt);
    EXPECT_EQ(loader.load(999, key_ring, out), LoadStatus::KeyNotFound);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(SealIdWatermarkLoader, RandomBytesNeverProduceAnInvalidStatus) {
    const auto dir = make_temp_candidate_dir("random");
    const auto key = make_kek(0x61);
    KeyRing key_ring(key);
    add_key(key_ring, 7, key);
    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    SealIdWatermarkLoader loader(lease);
    std::mt19937_64 random(0xD1CEB00CULL);
    std::array<std::byte, kSealIdWatermarkWireBytes> bytes{};
    SealIdWatermark out{};
    for (std::size_t iteration = 0; iteration < 2000; ++iteration) {
        for (auto& byte : bytes) byte = static_cast<std::byte>(random());
        write_bytes(dir / "seal-id-watermark", bytes);
        EXPECT_TRUE(is_load_status(loader.load(7, key_ring, out))) << "iteration=" << iteration;
    }
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}
