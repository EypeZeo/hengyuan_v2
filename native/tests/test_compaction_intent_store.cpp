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

// ===========================================================================
// Directory identity fencing (Round D review v3/v4's original TOCTOU
// finding): CandidateLease's private I/O methods -- the only way to reach
// check_can_operate() -- are friend-only to IntentStore, so this coverage
// necessarily lives here, not in test_compaction_lease.cpp (see that file's
// own trailing comment on this exact boundary).
//
// POSIX-only, and that is a real platform finding, not a test-portability
// shortcut. Confirmed empirically (a throwaway probe program, before writing
// this down) that on Windows, as long as CandidateLease holds ANY handle
// without FILE_SHARE_DELETE anywhere inside a directory subtree, the OS
// refuses to rename OR delete anything in that subtree -- not just the
// exact held object, but every ancestor up to and including the top-level
// parent -- from ANY process, this one included. Both
// std::filesystem::remove_all(leaf) and std::filesystem::rename(parent)
// failed outright (ERROR_SHARING_VIOLATION / ERROR_ACCESS_DENIED) while a
// lease was held. That means the exact attack this fencing exists for --
// an external actor renaming/recreating the directory out from under an
// acquire()'d lease -- is already structurally unreachable on Windows
// given CandidateLease's current (deliberately share-delete-less) open
// flags; there is no same-process way to test it there without granting
// FILE_SHARE_DELETE, which would be a real regression in the exact
// property this test exists to prove, not a test-only relaxation. POSIX
// rename()/unlink() never care about open file descriptors either way, so
// the scenario is genuinely reachable there, and IS exercised for real by
// this repo's WSL2/GCC-14 verification leg (tools/wsl_verify.sh).
#ifndef _WIN32

TEST(IntentStoreDirectoryFencing, RenameAwayThenRestoreProvesStickyFence) {
    auto parent = make_temp_candidate_dir("fence_rename_parent");
    auto dir = parent / "candidate";
    std::filesystem::create_directories(dir);
    const auto kek = make_kek(0x0D);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(7, kek, rec), KeyRingAddStatus::Ok);

    IntentStore store(dir, ring);
    ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);

    // Rename the PARENT out from under the still-open leaf handle -- the
    // original `dir` path now resolves to nothing.
    auto renamed_parent = parent;
    renamed_parent += "_renamed_away";
    std::filesystem::rename(parent, renamed_parent);

    const auto req = make_request();
    const auto first = store.create_building_intent(req);
    EXPECT_EQ(first, GenesisStatus::DirectoryIdentityChanged);
    EXPECT_TRUE(store.lease_fenced());
    const auto diag = store.lease_identity_diagnostic();
    ASSERT_TRUE(diag.has_value());
    EXPECT_TRUE(diag->observed_path_missing);

    // Restore the SAME parent object back to the original path -- if the
    // fence were re-evaluated per call, identity would now match again and
    // this second call would succeed. A sticky fence must refuse anyway,
    // without re-touching the filesystem to find out.
    std::filesystem::rename(renamed_parent, parent);
    const auto second = store.create_building_intent(req);
    EXPECT_EQ(second, GenesisStatus::CandidateFenced);

    ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);

    // Neither call should have written anything, anywhere.
    IntentStore verifier(dir, ring);
    ASSERT_EQ(verifier.acquire_lease(), LeaseAcquireStatus::Acquired);
    CompactionCandidateIntentWire loaded{};
    EXPECT_EQ(verifier.load_and_validate_intent(loaded), LoadStatus::NotFound);
    ASSERT_EQ(verifier.release_lease(), ReleaseStatus::Released);

    std::filesystem::remove_all(parent);
}

TEST(IntentStoreDirectoryFencing, RecreatedDirectoryWithDifferentIdentityIsDetected) {
    auto parent = make_temp_candidate_dir("fence_recreate_parent");
    auto dir = parent / "candidate";
    std::filesystem::create_directories(dir);
    const auto kek = make_kek(0x0E);
    KeyRing ring(kek);
    WrappedKeyRecord rec{};
    ASSERT_EQ(ring.add_key(7, kek, rec), KeyRingAddStatus::Ok);

    IntentStore store(dir, ring);
    ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);

    // Move the original parent aside, then create a BRAND NEW parent (with
    // its own new "candidate" subdirectory) at the exact same parent path
    // -- same leaf path string, genuinely different underlying filesystem
    // object (different dev/inode or volume-serial/file-index).
    auto orphaned_parent = parent;
    orphaned_parent += "_orphaned";
    std::filesystem::rename(parent, orphaned_parent);
    std::filesystem::create_directories(dir);

    const auto req = make_request();
    const auto status = store.create_building_intent(req);
    EXPECT_EQ(status, GenesisStatus::DirectoryIdentityChanged);
    EXPECT_TRUE(store.lease_fenced());
    const auto diag = store.lease_identity_diagnostic();
    ASSERT_TRUE(diag.has_value());
    EXPECT_FALSE(diag->observed_path_missing);
    EXPECT_TRUE(diag->expected_dev_or_volume_serial != diag->observed_dev_or_volume_serial ||
                diag->expected_inode_or_file_index != diag->observed_inode_or_file_index);

    ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);

    // The new leaf directory must be untouched (empty); the orphaned
    // original must also carry no genesis -- this call refused before any
    // I/O, on either object.
    EXPECT_TRUE(std::filesystem::is_empty(dir));
    IntentStore orphan_verifier(orphaned_parent / "candidate", ring);
    ASSERT_EQ(orphan_verifier.acquire_lease(), LeaseAcquireStatus::Acquired);
    CompactionCandidateIntentWire loaded{};
    EXPECT_EQ(orphan_verifier.load_and_validate_intent(loaded), LoadStatus::NotFound);
    ASSERT_EQ(orphan_verifier.release_lease(), ReleaseStatus::Released);

    std::filesystem::remove_all(parent);
    std::filesystem::remove_all(orphaned_parent);
}

#endif  // !_WIN32
