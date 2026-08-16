// Real-filesystem tests for intent_phase_advancer.hpp's IntentPhaseAdvancer
// -- Building->Reserved raise, receipt-derived id binding, idempotent-retry
// (scenario B: Intent already advanced) and refusal paths. Governance: L2
// (real file I/O). Test-only scope (see intent_phase_advancer.hpp's own
// header comment) -- this class is not called from any production path.
//
// raise_intent_phase() now performs BOTH the SealIdWatermark advance AND
// the SealJournalCommitWatermark (`.jhw`) CREATE_NEW itself (design v5 +
// the two-lock-coordination round -- see intent_phase_advancer.hpp's SCOPE
// section), including bootstrapping the watermark file when absent. Some
// tests below still plant a watermark file directly via plain file I/O (no
// CandidateLease/friend access needed for a bare write -- only reads are
// friend-gated), to set up a specific pre-existing state (a non-default
// starting point, a foreign store_uuid, a corrupt/zero watermark, an
// exhausted watermark) rather than relying on this store's first-ever
// bootstrap. IntentPhaseAdvancer now requires TWO leases -- CandidateLease
// (fx.dir) and SealJournalStoreLease (fx.seal_journal_dir), a genuinely
// separate directory -- both acquired by the test before constructing the
// advancer, released in the order seal_journal_store_lease.hpp's header
// comment's LOCK ORDER section documents (SealJournalStoreLease first).
#include <gtest/gtest.h>
#include <hengyuan/compaction_intent_store.hpp>
#include <hengyuan/intent_phase_advancer.hpp>
#include <hengyuan/seal_id_watermark_export_started_loader.hpp>
#include <hengyuan/seal_journal_commit_tombstone_loader.hpp>
#include <hengyuan/seal_journal_precondition_codec.hpp>
#include <hengyuan/seal_journal_store_lease.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace hy;

namespace {

std::filesystem::path make_temp_dir(std::string_view tag) {
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
// used to set up a specific pre-existing state; raise_intent_phase() itself
// is the only production-shaped writer of this file, see this file's header
// comment).
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
    std::filesystem::path dir;               // CandidateLease's breadcrumb directory
    std::filesystem::path seal_journal_dir;  // SealJournalStoreLease's directory -- genuinely
                                              // separate, not a subdirectory of `dir`.
    std::array<std::byte, kKekSize> kek;
    KeyRing ring;
    std::uint32_t kek_key_id;

    explicit Fixture(std::string_view tag, std::uint8_t kek_fill = 0x01, std::uint32_t key_id = 7)
        : dir(make_temp_dir(tag)),
          seal_journal_dir(make_temp_dir(std::string(tag) + "_sealjournal")),
          kek(make_kek(kek_fill)),
          ring(kek),
          kek_key_id(key_id) {
        WrappedKeyRecord rec{};
        EXPECT_EQ(ring.add_key(kek_key_id, kek, rec), KeyRingAddStatus::Ok);
    }

    ~Fixture() {
        std::filesystem::remove_all(dir);
        std::filesystem::remove_all(seal_journal_dir);
    }

    // Creates a Building genesis via IntentStore (acquire+create+release),
    // exactly the way a real caller would -- IntentPhaseAdvancer never
    // creates genesis itself.
    void create_building_genesis(std::uint64_t build_nonce) {
        IntentStore store(dir, ring);
        ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);
        ASSERT_EQ(store.create_building_intent(make_request(kek_key_id, build_nonce)), GenesisStatus::Created);
        ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);
    }

    // Plants a watermark such that the NEXT raise_intent_phase() call derives
    // exactly (candidate_id, request_id) -- since raise_intent_phase() now
    // binds candidate_id = next_candidate_id directly (design v5; no longer
    // "next - 1"), that means storing (candidate_id, request_id) as-is.
    void plant_watermark_reserving(std::uint64_t candidate_id, std::uint64_t request_id) {
        plant_watermark(dir, 0x1111111111111111ULL, 0x2222222222222222ULL, candidate_id, request_id, kek);
    }
};

// RAII pair for the two leases IntentPhaseAdvancer now requires, enforcing
// this file's own acquire/release order to match seal_journal_store_lease.hpp's
// LOCK ORDER rule (CandidateLease acquired first, released last).
struct TwoLeases {
    CandidateLease candidate_lease;
    SealJournalStoreLease seal_journal_lease;

    explicit TwoLeases(Fixture& fx) : candidate_lease(fx.dir), seal_journal_lease(fx.seal_journal_dir) {
        EXPECT_EQ(candidate_lease.acquire(), LeaseAcquireStatus::Acquired);
        EXPECT_EQ(seal_journal_lease.acquire(), SealJournalLeaseAcquireStatus::Acquired);
    }

    void release() {
        EXPECT_EQ(seal_journal_lease.release(), SealJournalLeaseReleaseStatus::Released);
        EXPECT_EQ(candidate_lease.release(), ReleaseStatus::Released);
    }
};

}  // namespace

TEST(IntentPhaseAdvancer, RaisesBuildingToReservedWithWatermarkDerivedIds) {
    Fixture fx("raise_ok");
    constexpr std::uint64_t kBuildNonce = 0xABCDEF0123ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(/*candidate_id=*/41, /*request_id=*/77);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);
    leases.release();

    // Verify via a fresh IntentStore/CandidateLease pair over the same
    // directory -- must acquire only after `leases` above has released
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

TEST(IntentPhaseAdvancer, WritesDurableJhwAndX1BeforeReplacingIntent) {
    Fixture fx("jhw_x1_before_replace");
    constexpr std::uint64_t kBuildNonce = 0x77ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(5, 9);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);

    const auto x1_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/1);
    EXPECT_TRUE(std::filesystem::exists(fx.dir / x1_name.relative_name()))
        << "the `.x1` seq=1 evidence must be durable once raise_intent_phase() reports success";

    // `.jhw` must exist for candidate_id=5, with highest_committed_journal_seq=0.
    SealJournalCommitWatermarkLoader jhw_loader(leases.seal_journal_lease);
    SealJournalCommitWatermark jhw{};
    ASSERT_EQ(jhw_loader.load(/*candidate_id=*/5, fx.ring, jhw), SealJournalStoreLoadStatus::Ok);
    EXPECT_EQ(jhw.candidate_id, 5u);
    EXPECT_EQ(jhw.highest_committed_journal_seq, 0u);
    EXPECT_EQ(jhw.store_uuid_lo, 0x1111111111111111ULL);
    EXPECT_EQ(jhw.store_uuid_hi, 0x2222222222222222ULL);

    leases.release();
}

TEST(IntentPhaseAdvancer, RetryAfterSuccessReturnsAlreadyAtOrPastTargetPhase) {
    Fixture fx("retry_already_past");
    constexpr std::uint64_t kBuildNonce = 0x99ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(1, 1);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);
    // Second call, same instance -- the Intent is now Reserved; must not
    // attempt a second `.x1` seq=1 write (that would collide) and must not
    // error, since this exact build already durably completed the raise.
    EXPECT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::AlreadyAtOrPastTargetPhase);
    leases.release();
}

TEST(IntentPhaseAdvancer, ForeignBuildNonceIsRefused) {
    Fixture fx("foreign_nonce");
    fx.create_building_genesis(/*build_nonce=*/0x1234ULL);
    fx.plant_watermark_reserving(1, 1);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(/*build_nonce=*/0x9999ULL), PhaseAdvanceStatus::ForeignBuildNonce);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesWithoutIntentGenesis) {
    Fixture fx("no_genesis");
    // Deliberately never creates a Building genesis.
    fx.plant_watermark_reserving(1, 1);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(0xABCULL), PhaseAdvanceStatus::IntentNotFound);
    leases.release();
}

TEST(IntentPhaseAdvancer, BootstrapsWatermarkWhenAbsentAndBindsOneOne) {
    Fixture fx("no_watermark");
    constexpr std::uint64_t kBuildNonce = 0xABCULL;
    fx.create_building_genesis(kBuildNonce);
    // Deliberately never plants a watermark -- this store's first-ever
    // candidate. raise_intent_phase() must bootstrap it (design v5) rather
    // than refuse.

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);
    leases.release();

    IntentStore verify_store(fx.dir, fx.ring);
    ASSERT_EQ(verify_store.acquire_lease(), LeaseAcquireStatus::Acquired);
    CompactionCandidateIntentWire loaded{};
    ASSERT_EQ(verify_store.load_and_validate_intent(loaded), LoadStatus::Ok);
    EXPECT_EQ(loaded.candidate_id, 1u);
    EXPECT_EQ(loaded.request_id, 1u);
    ASSERT_EQ(verify_store.release_lease(), ReleaseStatus::Released);

    // The watermark itself must now read next_*=2 -- bootstrap consumed 1,1.
    CandidateLease verify_lease(fx.dir);
    ASSERT_EQ(verify_lease.acquire(), LeaseAcquireStatus::Acquired);
    SealIdWatermark wm{};
    SealIdWatermarkLoader loader(verify_lease);
    ASSERT_EQ(loader.load(fx.kek_key_id, fx.ring, wm), LoadStatus::Ok);
    EXPECT_EQ(wm.next_candidate_id, 2u);
    EXPECT_EQ(wm.next_request_id, 2u);
    ASSERT_EQ(verify_lease.release(), ReleaseStatus::Released);
}

TEST(IntentPhaseAdvancer, RefusesWhenWatermarkAtUint64Max) {
    Fixture fx("watermark_exhausted");
    fx.create_building_genesis(0xABCULL);
    plant_watermark(fx.dir, 0x1111111111111111ULL, 0x2222222222222222ULL,
                     std::numeric_limits<std::uint64_t>::max(), 5, fx.kek);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(0xABCULL), PhaseAdvanceStatus::WatermarkExhausted);
    leases.release();
}

TEST(IntentPhaseAdvancer, RetryAfterX1FailureReusesSameWatermarkBoundIdsInsteadOfBurningAnother) {
    Fixture fx("retry_x1_failure");
    constexpr std::uint64_t kBuildNonce = 0x55ULL;
    fx.create_building_genesis(kBuildNonce);
    // No watermark planted -- bootstraps to 1,1 on the first call.

    // Pre-create a colliding, WRONG-content `.x1` seq=1 file so the first
    // call's `.x1` write hits a byte-mismatch collision (real conflict,
    // never overwritten -- see compaction_breadcrumb_io.hpp's
    // write_validated_no_replace) and the call fails downstream of both the
    // watermark advance AND the `.jhw` CREATE_NEW (which now runs before
    // `.x1`, so it succeeds on this first, otherwise-doomed call).
    const auto x1_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*seq=*/1);
    {
        std::vector<char> wrong_bytes(kCompactionIntentTransitionWireBytes, 'X');
        std::ofstream bogus(fx.dir / x1_name.relative_name(), std::ios::binary | std::ios::trunc);
        bogus.write(wrong_bytes.data(), static_cast<std::streamsize>(wrong_bytes.size()));
    }

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::X1WriteFailed);

    // The watermark must already show the advance (spent, per
    // WatermarkAdvanceProvenanceMemory's contract) even though the overall
    // call failed downstream.
    {
        SealIdWatermarkLoader loader(leases.candidate_lease);
        SealIdWatermark wm{};
        ASSERT_EQ(loader.load(fx.kek_key_id, fx.ring, wm), LoadStatus::Ok);
        EXPECT_EQ(wm.next_candidate_id, 2u);
        EXPECT_EQ(wm.next_request_id, 2u);
    }
    // `.jhw` for candidate_id=1 must already be durable too -- it ran before
    // the `.x1` write that failed.
    {
        SealJournalCommitWatermarkLoader jhw_loader(leases.seal_journal_lease);
        SealJournalCommitWatermark jhw{};
        ASSERT_EQ(jhw_loader.load(/*candidate_id=*/1, fx.ring, jhw), SealJournalStoreLoadStatus::Ok);
        EXPECT_EQ(jhw.candidate_id, 1u);
    }

    // Remove the colliding stub and retry with the SAME advancer instance --
    // must bind the SAME ids (1,1) the first attempt already spent, not
    // derive a new pair from the now-advanced watermark. The `.jhw`
    // CREATE_NEW retry hits its own byte-equal collision (same deterministic
    // content) and self-heals safely -- no provenance memory needed for it.
    std::filesystem::remove(fx.dir / x1_name.relative_name());
    EXPECT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);
    leases.release();

    IntentStore verify_store(fx.dir, fx.ring);
    ASSERT_EQ(verify_store.acquire_lease(), LeaseAcquireStatus::Acquired);
    CompactionCandidateIntentWire loaded{};
    ASSERT_EQ(verify_store.load_and_validate_intent(loaded), LoadStatus::Ok);
    EXPECT_EQ(loaded.candidate_id, 1u);
    EXPECT_EQ(loaded.request_id, 1u);
    ASSERT_EQ(verify_store.release_lease(), ReleaseStatus::Released);
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

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(0xABCULL), PhaseAdvanceStatus::WatermarkCorrupt);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesForeignStoreWatermark) {
    Fixture fx("foreign_store");
    fx.create_building_genesis(0xABCULL);
    // A watermark for a different store_uuid than the Intent's.
    plant_watermark(fx.dir, /*store_uuid_lo=*/0xDEADBEEFULL, /*store_uuid_hi=*/0xFEEDFACEULL,
                     /*next_candidate_id=*/2, /*next_request_id=*/2, fx.kek);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(0xABCULL), PhaseAdvanceStatus::WatermarkForeignStore);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesWithoutCandidateLease) {
    Fixture fx("no_candidate_lease");
    fx.create_building_genesis(0xABCULL);
    fx.plant_watermark_reserving(1, 1);

    CandidateLease lease(fx.dir);
    // Deliberately never acquires.
    SealJournalStoreLease seal_journal_lease(fx.seal_journal_dir);
    ASSERT_EQ(seal_journal_lease.acquire(), SealJournalLeaseAcquireStatus::Acquired);
    IntentPhaseAdvancer advancer(lease, seal_journal_lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(0xABCULL), PhaseAdvanceStatus::LeaseNotHeld);
    ASSERT_EQ(seal_journal_lease.release(), SealJournalLeaseReleaseStatus::Released);
}

TEST(IntentPhaseAdvancer, RefusesWithoutSealJournalStoreLease) {
    Fixture fx("no_seal_journal_lease");
    constexpr std::uint64_t kBuildNonce = 0xABCULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(1, 1);

    CandidateLease lease(fx.dir);
    ASSERT_EQ(lease.acquire(), LeaseAcquireStatus::Acquired);
    SealJournalStoreLease seal_journal_lease(fx.seal_journal_dir);
    // Deliberately never acquires -- watermark advance (CandidateLease-guarded)
    // must still succeed and be memoized before the `.jhw` step discovers the
    // second lease isn't held.
    IntentPhaseAdvancer advancer(lease, seal_journal_lease, fx.ring);
    EXPECT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::SealJournalStoreLeaseNotHeld);
    ASSERT_EQ(lease.release(), ReleaseStatus::Released);
}
