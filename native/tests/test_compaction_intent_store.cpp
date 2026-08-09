// Real-filesystem tests for compaction_intent_store.hpp's IntentStore --
// genesis create/match/uncertain-provenance, KeyNotFound/InvalidRequest
// refusal, and the CandidateLease friend relationship exercised end to end
// (not just CandidateLease's own lifecycle surface, which
// test_compaction_lease.cpp already covers). Governance: L2 (real file I/O).
#include <gtest/gtest.h>
#include <hengyuan/compaction_intent_store.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

using namespace hy;

namespace {

std::filesystem::path make_temp_candidate_dir(std::string_view tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("hy_round_d_intent_store_" + std::string(tag) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::array<std::byte, kKekSize> make_kek(std::uint8_t fill) {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(fill + i);
    return kek;
}

GenesisRequest make_request(std::uint32_t kek_key_id = 7) {
    GenesisRequest req{};
    req.store_uuid_lo = 0x1111111111111111ULL;
    req.store_uuid_hi = 0x2222222222222222ULL;
    req.kek_key_id = kek_key_id;
    req.source_generation = 3;
    req.target_generation = 4;
    req.baseline_tip_seq = 999;
    req.baseline_tip_mac.fill(std::uint8_t{0xAB});
    req.baseline_key_id = kek_key_id;
    req.build_nonce = 0xABCDEF0123ULL;
    return req;
}

}  // namespace

TEST(IntentStore, CreateBuildingIntentReturnsCreatedOnFirstCall) {
    auto dir = make_temp_candidate_dir("create_ok");
    const auto kek = make_kek(0x01);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(7, kek, rec), KeyRingAddStatus::Ok);

    IntentStore store(dir, ring);
    ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);
    EXPECT_EQ(store.create_building_intent(make_request()), GenesisStatus::Created);
    ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(IntentStore, SecondCreateSameRequestReturnsAlreadyExists) {
    auto dir = make_temp_candidate_dir("already_exists");
    const auto kek = make_kek(0x02);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(7, kek, rec), KeyRingAddStatus::Ok);

    IntentStore store(dir, ring);
    ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);
    const auto req = make_request();
    ASSERT_EQ(store.create_building_intent(req), GenesisStatus::Created);
    EXPECT_EQ(store.create_building_intent(req), GenesisStatus::AlreadyExists);
    ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(IntentStore, CreateBuildingIntentRefusesWithoutLease) {
    auto dir = make_temp_candidate_dir("no_lease");
    const auto kek = make_kek(0x03);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(7, kek, rec), KeyRingAddStatus::Ok);

    IntentStore store(dir, ring);
    // Deliberately never call acquire_lease().
    EXPECT_EQ(store.create_building_intent(make_request()), GenesisStatus::LeaseNotHeld);
    std::filesystem::remove_all(dir);
}

TEST(IntentStore, CreateBuildingIntentRefusesUnknownKeyId) {
    auto dir = make_temp_candidate_dir("unknown_key");
    const auto kek = make_kek(0x04);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(7, kek, rec), KeyRingAddStatus::Ok);

    IntentStore store(dir, ring);
    ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);
    EXPECT_EQ(store.create_building_intent(make_request(/*kek_key_id=*/999)), GenesisStatus::KeyNotFound);

    CompactionCandidateIntentWire loaded{};
    EXPECT_EQ(store.load_and_validate_intent(loaded), LoadStatus::NotFound)
        << "a refused genesis (KeyNotFound) must not have written anything to disk";

    ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(IntentStore, CreateBuildingIntentRefusesIllegalGenerationTransition) {
    auto dir = make_temp_candidate_dir("bad_gen");
    const auto kek = make_kek(0x05);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(7, kek, rec), KeyRingAddStatus::Ok);

    IntentStore store(dir, ring);
    ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);
    auto req = make_request();
    req.target_generation = req.source_generation;  // not source+1 -- illegal
    EXPECT_EQ(store.create_building_intent(req), GenesisStatus::InvalidRequest);

    CompactionCandidateIntentWire loaded{};
    EXPECT_EQ(store.load_and_validate_intent(loaded), LoadStatus::NotFound);

    ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(IntentStore, LoadAndValidateIntentRoundTripsAllFields) {
    auto dir = make_temp_candidate_dir("roundtrip");
    const auto kek = make_kek(0x06);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(7, kek, rec), KeyRingAddStatus::Ok);

    IntentStore store(dir, ring);
    ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);
    const auto req = make_request();
    ASSERT_EQ(store.create_building_intent(req), GenesisStatus::Created);

    CompactionCandidateIntentWire loaded{};
    ASSERT_EQ(store.load_and_validate_intent(loaded), LoadStatus::Ok);
    EXPECT_EQ(loaded.phase, kCompactionCandidateIntentPhaseBuilding);
    EXPECT_EQ(loaded.candidate_id, 0u);
    EXPECT_EQ(loaded.request_id, 0u);
    EXPECT_EQ(loaded.store_uuid_lo, req.store_uuid_lo);
    EXPECT_EQ(loaded.store_uuid_hi, req.store_uuid_hi);
    EXPECT_EQ(loaded.kek_key_id, req.kek_key_id);
    EXPECT_EQ(loaded.source_generation, req.source_generation);
    EXPECT_EQ(loaded.target_generation, req.target_generation);
    EXPECT_EQ(loaded.baseline_tip_seq, req.baseline_tip_seq);
    EXPECT_EQ(loaded.baseline_key_id, req.baseline_key_id);
    EXPECT_EQ(loaded.build_nonce, req.build_nonce);

    ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(IntentStore, LoadAndValidateIntentSurvivesFreshInstanceAgainstSameDirectory) {
    auto dir = make_temp_candidate_dir("fresh_instance");
    const auto kek = make_kek(0x07);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(7, kek, rec), KeyRingAddStatus::Ok);

    const auto req = make_request();
    {
        IntentStore writer(dir, ring);
        ASSERT_EQ(writer.acquire_lease(), LeaseAcquireStatus::Acquired);
        ASSERT_EQ(writer.create_building_intent(req), GenesisStatus::Created);
        ASSERT_EQ(writer.release_lease(), ReleaseStatus::Released);
    }
    {
        IntentStore reader(dir, ring);
        ASSERT_EQ(reader.acquire_lease(), LeaseAcquireStatus::Acquired);
        CompactionCandidateIntentWire loaded{};
        ASSERT_EQ(reader.load_and_validate_intent(loaded), LoadStatus::Ok);
        EXPECT_EQ(loaded.build_nonce, req.build_nonce);
        ASSERT_EQ(reader.release_lease(), ReleaseStatus::Released);
    }
    std::filesystem::remove_all(dir);
}

TEST(IntentStore, LoadAndMatchBuildingGenesisMatchesIdenticalRequest) {
    auto dir = make_temp_candidate_dir("match_ok");
    const auto kek = make_kek(0x08);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(7, kek, rec), KeyRingAddStatus::Ok);

    IntentStore store(dir, ring);
    ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);
    const auto req = make_request();
    ASSERT_EQ(store.create_building_intent(req), GenesisStatus::Created);

    CompactionCandidateIntentWire out{};
    EXPECT_EQ(store.load_and_match_building_genesis(req, out), GenesisMatchStatus::MatchesRequestedGenesis);

    ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(IntentStore, LoadAndMatchBuildingGenesisRejectsDifferentRequest) {
    auto dir = make_temp_candidate_dir("match_diff");
    const auto kek = make_kek(0x09);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(7, kek, rec), KeyRingAddStatus::Ok);

    IntentStore store(dir, ring);
    ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);
    const auto req = make_request();
    ASSERT_EQ(store.create_building_intent(req), GenesisStatus::Created);

    auto different = req;
    different.build_nonce = req.build_nonce + 1;
    CompactionCandidateIntentWire out{};
    EXPECT_EQ(store.load_and_match_building_genesis(different, out), GenesisMatchStatus::ExistingDifferent);

    ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(IntentStore, LoadAndMatchBuildingGenesisNotFoundWhenNothingWritten) {
    auto dir = make_temp_candidate_dir("match_notfound");
    const auto kek = make_kek(0x0A);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(7, kek, rec), KeyRingAddStatus::Ok);

    IntentStore store(dir, ring);
    ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);
    CompactionCandidateIntentWire out{};
    EXPECT_EQ(store.load_and_match_building_genesis(make_request(), out), GenesisMatchStatus::NotFound);
    ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(IntentStore, InspectX1ChainEmptyWhenNoTransitionsWritten) {
    auto dir = make_temp_candidate_dir("x1_empty");
    const auto kek = make_kek(0x0B);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(7, kek, rec), KeyRingAddStatus::Ok);

    IntentStore store(dir, ring);
    ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);
    const auto req = make_request();
    ASSERT_EQ(store.create_building_intent(req), GenesisStatus::Created);

    std::array<std::uint8_t, 32> terminal{};
    EXPECT_EQ(store.inspect_x1_chain(req.build_nonce, terminal), X1ChainStatus::Empty)
        << "Round D never writes a .x1 -- inspect_x1_chain() must honestly report Empty, "
           "not fabricate a chain";

    ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}

TEST(IntentStore, InspectX1ChainEmptyWhenNoIntentGenesisAtAll) {
    auto dir = make_temp_candidate_dir("x1_no_intent");
    const auto kek = make_kek(0x0C);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(7, kek, rec), KeyRingAddStatus::Ok);

    IntentStore store(dir, ring);
    ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);
    std::array<std::uint8_t, 32> terminal{};
    EXPECT_EQ(store.inspect_x1_chain(0xDEADBEEFULL, terminal), X1ChainStatus::Empty);
    ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);
    std::filesystem::remove_all(dir);
}
