// Real-filesystem tests for intent_phase_advancer.hpp's IntentPhaseAdvancer
// -- Building->Reserved raise, receipt-derived id binding, idempotent-retry
// (scenario B: Intent already advanced) and refusal paths. Governance: L2
// (real file I/O). Test-only scope (see intent_phase_advancer.hpp's own
// header comment) -- this class is not called from any production path.
//
// SealIdWatermark has no production writer anywhere in this codebase yet
// (this round's explicit boundary -- see intent_phase_advancer.hpp point 5),
// so these tests plant a watermark file directly via plain file I/O (no
// CandidateLease/friend access needed for a bare write -- only reads are
// friend-gated), standing in for whatever external actor would durably
// advance it in a real deployment.
#include <gtest/gtest.h>
#include <hengyuan/compaction_intent_store.hpp>
#include <hengyuan/intent_phase_advancer.hpp>
#include <hengyuan/seal_journal_precondition_codec.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace hy;

namespace {

std::filesystem::path make_temp_candidate_dir(std::string_view tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("hy_round_e_phase_advancer_" + std::string(tag) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::array<std::byte, kKekSize> make_kek(std::uint8_t fill) {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(fill + i);
    return kek;
}

GenesisRequest make_request(std::uint32_t kek_key_id, std::uint64_t build_nonce) {
    GenesisRequest req{};
    req.store_uuid_lo = 0x1111111111111111ULL;
    req.store_uuid_hi = 0x2222222222222222ULL;
    req.kek_key_id = kek_key_id;
    req.source_generation = 3;
    req.target_generation = 4;
    req.baseline_tip_seq = 999;
    req.baseline_tip_mac.fill(std::uint8_t{0xAB});
    req.baseline_key_id = kek_key_id;
    req.build_nonce = build_nonce;
    return req;
}

// Plants a durable SealIdWatermark file directly (bypassing CandidateLease --
// no production writer for this file exists yet, see this file's header
// comment). next_candidate_id/next_request_id are "the next value to
// allocate," so a watermark that has "reserved" candidate_id/request_id N
// stores N+1 in these fields (matches intent_phase_advancer.hpp's "next - 1"
// derivation).
void plant_watermark(const std::filesystem::path& candidate_dir, std::uint64_t store_uuid_lo,
                       std::uint64_t store_uuid_hi, std::uint64_t next_candidate_id, std::uint64_t next_request_id,
                       const std::array<std::byte, kKekSize>& kek) {
    SealIdWatermark v{};
    v.store_uuid_lo = store_uuid_lo;
    v.store_uuid_hi = store_uuid_hi;
    v.next_candidate_id = next_candidate_id;
    v.next_request_id = next_request_id;

    std::array<std::byte, kSealIdWatermarkWireBytes> encoded{};
    encode_seal_id_watermark_wire(encoded, v, kek);

    const auto path = candidate_dir / compaction_detail::ValidatedArtifactName::for_seal_id_watermark().relative_name();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
}

struct Fixture {
    std::filesystem::path dir;
    std::array<std::byte, kKekSize> kek;
    KeyRing ring;
    std::uint32_t kek_key_id;

    explicit Fixture(std::string_view tag, std::uint8_t kek_fill = 0x01, std::uint32_t key_id = 7)
        : dir(make_temp_candidate_dir(tag)), kek(make_kek(kek_fill)), ring(kek), kek_key_id(key_id) {
        WrappedKeyRecord rec{};
        EXPECT_EQ(ring.add_key(kek_key_id, kek, rec), KeyRingAddStatus::Ok);
    }

    ~Fixture() { std::filesystem::remove_all(dir); }

    // Creates a Building genesis via IntentStore (acquire+create+release),
    // exactly the way a real caller would -- IntentPhaseAdvancer never
    // creates genesis itself.
    void create_building_genesis(std::uint64_t build_nonce) {
        IntentStore store(dir, ring);
        ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);
        ASSERT_EQ(store.create_building_intent(make_request(kek_key_id, build_nonce)), GenesisStatus::Created);
        ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);
    }

    void plant_watermark_reserving(std::uint64_t candidate_id, std::uint64_t request_id) {
        plant_watermark(dir, 0x1111111111111111ULL, 0x2222222222222222ULL, candidate_id + 1, request_id + 1, kek);
    }
};

}  // namespace

TEST(IntentPhaseAdvancer, RaisesBuildingToReservedWithWatermarkDerivedIds) {
    Fixture fx("raise_ok");
    constexpr std::uint64_t kBuildNonce = 0xABCDEF0123ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(/*candidate_id=*/41, /*request_id=*/77);

    CandidateLease lease(fx.dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    IntentPhaseAdvancer advancer(lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);

    // Verify via a fresh IntentStore/CandidateLease pair over the same
    // directory -- must acquire only after `lease` above has released
    // (the directory lock is exclusive).
    IntentStore verify_store(fx.dir, fx.ring);
    ASSERT_EQ(verify_store.acquire_lease(), LeaseAcquireStatus::Acquired);
    CompactionCandidateIntentWire loaded{};
    ASSERT_EQ(verify_store.load_and_validate_intent(loaded), LoadStatus::Ok);
    EXPECT_EQ(loaded.phase, kCompactionCandidateIntentPhaseReserved);
    EXPECT_EQ(loaded.candidate_id, 41u);
    EXPECT_EQ(loaded.request_id, 77u);
    ASSERT_EQ(verify_store.release_lease(), ReleaseStatus::Released);
}

TEST(IntentPhaseAdvancer, WritesDurableX1BeforeReplacingIntent) {
    Fixture fx("x1_before_replace");
    constexpr std::uint64_t kBuildNonce = 0x77ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(5, 9);

    CandidateLease lease(fx.dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    IntentPhaseAdvancer advancer(lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);

    const auto x1_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/1);
    EXPECT_TRUE(std::filesystem::exists(fx.dir / x1_name.relative_name()))
        << "the `.x1` seq=1 evidence must be durable once raise_intent_phase() reports success";
}

TEST(IntentPhaseAdvancer, RetryAfterSuccessReturnsAlreadyAtOrPastTargetPhase) {
    Fixture fx("retry_already_past");
    constexpr std::uint64_t kBuildNonce = 0x99ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(1, 1);

    CandidateLease lease(fx.dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    IntentPhaseAdvancer advancer(lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);
    // Second call, same instance -- the Intent is now Reserved; must not
    // attempt a second `.x1` seq=1 write (that would collide) and must not
    // error, since this exact build already durably completed the raise.
    EXPECT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::AlreadyAtOrPastTargetPhase);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
}

TEST(IntentPhaseAdvancer, ForeignBuildNonceIsRefused) {
    Fixture fx("foreign_nonce");
    fx.create_building_genesis(/*build_nonce=*/0x1234ULL);
    fx.plant_watermark_reserving(1, 1);

    CandidateLease lease(fx.dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    IntentPhaseAdvancer advancer(lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(/*build_nonce=*/0x9999ULL), PhaseAdvanceStatus::ForeignBuildNonce);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
}

TEST(IntentPhaseAdvancer, RefusesWithoutIntentGenesis) {
    Fixture fx("no_genesis");
    // Deliberately never creates a Building genesis.
    fx.plant_watermark_reserving(1, 1);

    CandidateLease lease(fx.dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    IntentPhaseAdvancer advancer(lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(0xABCULL), PhaseAdvanceStatus::IntentNotFound);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
}

TEST(IntentPhaseAdvancer, RefusesWithoutWatermark) {
    Fixture fx("no_watermark");
    fx.create_building_genesis(0xABCULL);
    // Deliberately never plants a watermark.

    CandidateLease lease(fx.dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    IntentPhaseAdvancer advancer(lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(0xABCULL), PhaseAdvanceStatus::WatermarkNotFound);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
}

TEST(IntentPhaseAdvancer, RefusesZeroWatermarkAsCorrupt) {
    Fixture fx("watermark_zero");
    fx.create_building_genesis(0xABCULL);
    // next_candidate_id/next_request_id both 0 -- nothing allocated yet. 0 is
    // never a legal next-allocatable id; decode_seal_id_watermark_wire()
    // itself refuses to decode this (seal_journal_precondition_codec.hpp's
    // allocator-safety check), surfacing as WatermarkCorrupt, not a distinct
    // status -- see intent_phase_advancer.hpp's WatermarkCorrupt comment.
    plant_watermark(fx.dir, 0x1111111111111111ULL, 0x2222222222222222ULL, /*next_candidate_id=*/0,
                     /*next_request_id=*/0, fx.kek);

    CandidateLease lease(fx.dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    IntentPhaseAdvancer advancer(lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(0xABCULL), PhaseAdvanceStatus::WatermarkCorrupt);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
}

TEST(IntentPhaseAdvancer, RefusesForeignStoreWatermark) {
    Fixture fx("foreign_store");
    fx.create_building_genesis(0xABCULL);
    // A watermark for a different store_uuid than the Intent's.
    plant_watermark(fx.dir, /*store_uuid_lo=*/0xDEADBEEFULL, /*store_uuid_hi=*/0xFEEDFACEULL,
                     /*next_candidate_id=*/2, /*next_request_id=*/2, fx.kek);

    CandidateLease lease(fx.dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    IntentPhaseAdvancer advancer(lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(0xABCULL), PhaseAdvanceStatus::WatermarkForeignStore);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
}

TEST(IntentPhaseAdvancer, RefusesWithoutLease) {
    Fixture fx("no_lease");
    fx.create_building_genesis(0xABCULL);
    fx.plant_watermark_reserving(1, 1);

    CandidateLease lease(fx.dir);
    // Deliberately never acquires.
    IntentPhaseAdvancer advancer(lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(0xABCULL), PhaseAdvanceStatus::LeaseNotHeld);
}
