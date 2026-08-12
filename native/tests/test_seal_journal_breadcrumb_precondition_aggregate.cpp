// Coordinator-only test for load_all_breadcrumb_preconditions() -- confirms
// the composition itself (all five reads attempted, each field's status
// independent, no early return on a partial failure) against a real temp
// directory. The individual loaders' own correctness (round-trip, MAC
// tamper, fencing, etc.) is already covered by each module's own test file;
// this file only tests the aggregation glue.
#include <gtest/gtest.h>
#include <hengyuan/seal_journal_breadcrumb_precondition_aggregate.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace hy;

namespace {

std::filesystem::path make_temp_dir(std::string_view tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("hy_breadcrumb_aggregate_" + std::string(tag) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::array<std::byte, kKekSize> make_kek(std::uint8_t seed) {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(seed + i);
    return kek;
}

}  // namespace

TEST(BreadcrumbPreconditionsAggregate, AllFiveNotFoundOnEmptyDirectory) {
    auto dir = make_temp_dir("all_notfound");
    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);

    const auto root_kek = make_kek(0x01);
    KeyRing key_ring(root_kek);
    const auto result = load_all_breadcrumb_preconditions(lease, key_ring, /*seal_id_watermark_kek_key_id=*/7);

    EXPECT_EQ(result.seal_id_watermark_status, LoadStatus::NotFound);
    EXPECT_EQ(result.seal_export_started_legacy_status, LoadStatus::NotFound);
    EXPECT_EQ(result.seal_export_started_migration_companion_status, LoadStatus::NotFound);
    EXPECT_EQ(result.seal_export_started_migration_status, LoadStatus::NotFound);
    EXPECT_EQ(result.seal_started_cleanup_tombstone_status, LoadStatus::NotFound);
    EXPECT_EQ(result.seal_started_abandon_status, LoadStatus::NotFound);

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

// The key property this test exists to prove: one file's presence/absence
// must not affect any other field's status -- there is no early return
// anywhere in load_all_breadcrumb_preconditions().
TEST(BreadcrumbPreconditionsAggregate, OnlyOneFilePresentDoesNotAffectOthers) {
    auto dir = make_temp_dir("one_present");
    const auto root_kek = make_kek(0x00);
    KeyRing key_ring(root_kek);
    const auto kek = make_kek(0x40);
    WrappedKeyRecord record{};
    ASSERT_EQ(key_ring.add_key(11, kek, record), KeyRingAddStatus::Ok);

    SealIdWatermark watermark{};
    watermark.store_uuid_lo = 0x1ull;
    watermark.store_uuid_hi = 0x2ull;
    watermark.next_candidate_id = 5;
    watermark.next_request_id = 6;
    std::array<std::byte, kSealIdWatermarkWireBytes> encoded{};
    ASSERT_EQ(encode_seal_id_watermark_wire(encoded, watermark, kek), kSealIdWatermarkWireBytes);
    {
        std::ofstream out(dir / "seal-id-watermark", std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    }

    CandidateLease lease(dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);

    const auto result = load_all_breadcrumb_preconditions(lease, key_ring, /*seal_id_watermark_kek_key_id=*/11);

    EXPECT_EQ(result.seal_id_watermark_status, LoadStatus::Ok);
    EXPECT_EQ(result.seal_id_watermark.next_candidate_id, 5u);
    EXPECT_EQ(result.seal_id_watermark.next_request_id, 6u);

    // Every other field: still cleanly NotFound, not aborted or corrupted by
    // the SealIdWatermark read that came before them.
    EXPECT_EQ(result.seal_export_started_legacy_status, LoadStatus::NotFound);
    EXPECT_EQ(result.seal_export_started_migration_companion_status, LoadStatus::NotFound);
    EXPECT_EQ(result.seal_export_started_migration_status, LoadStatus::NotFound);
    EXPECT_EQ(result.seal_started_cleanup_tombstone_status, LoadStatus::NotFound);
    EXPECT_EQ(result.seal_started_abandon_status, LoadStatus::NotFound);

    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}
