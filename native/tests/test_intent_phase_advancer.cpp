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
#include <hengyuan/seal_export_migration_cleanup_abandon_codec.hpp>
#include <hengyuan/seal_id_watermark_export_started_loader.hpp>
#include <hengyuan/seal_journal_commit_tombstone_loader.hpp>
#include <hengyuan/seal_journal_precondition_codec.hpp>
#include <hengyuan/seal_journal_store_lease.hpp>
#include <hengyuan/seal_started_abandon_loader.hpp>
#include <hengyuan/seal_started_migration_cleanup_loader.hpp>

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

// A SealExportStartedWire whose bound fields match make_request()'s fixed
// constants (store_uuid/source_generation=3/target_generation=4/
// baseline_tip_seq=999/baseline_tip_mac=0xAB fill/baseline_key_id=kek_key_id)
// -- the caller-supplied fields raise_started_published() checks (not
// derives) at its Step 2. candidate_id/request_id are the caller's, since
// those are the ids raise_intent_phase() already bound at Reserved.
SealExportStartedWire make_started(std::uint32_t kek_key_id, std::uint64_t candidate_id, std::uint64_t request_id) {
    SealExportStartedWire v{};
    v.store_uuid_lo = 0x1111111111111111ULL;
    v.store_uuid_hi = 0x2222222222222222ULL;
    v.candidate_id = candidate_id;
    v.source_generation = 3;
    v.baseline_tip_seq = 999;
    for (auto& b : v.baseline_tip_mac) b = std::uint8_t{0xAB};
    v.baseline_key_id = kek_key_id;
    v.new_generation = 4;  // == make_request()'s target_generation
    v.new_final_seq = 555;
    for (auto& b : v.new_final_tip_mac) b = std::uint8_t{0xCD};
    v.new_key_id = kek_key_id;
    v.request_id = request_id;
    for (auto& b : v.content_root) b = std::uint8_t{0xEF};
    v.kek_key_id = kek_key_id;
    v.registered_producer_mask = 0b00000011;
    v.producer_count = 2;
    v.ring_id[0] = 101;
    v.ring_id[1] = 102;
    return v;
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

// Plants a durable `.clr` directly (bypassing CandidateLease -- same
// precedent as plant_watermark() above; raise_post_seal_finalizing() only
// ever reads this file, never writes it -- see intent_phase_advancer.hpp's
// header comment's point 6). `v` must already be a fully legal, shape-valid
// record: encode_seal_started_cleanup_tombstone_wire() silently returns 0
// bytes on a shape violation rather than asserting, so the ASSERT_EQ below
// is load-bearing -- without it, a shape bug in a test's `v` would silently
// write an empty/truncated file instead of failing loudly at the call site
// that actually has the wrong value.
void plant_clr(const std::filesystem::path& candidate_dir, const SealStartedCleanupTombstoneWire& v,
               const std::array<std::byte, kKekSize>& kek) {
    std::array<std::byte, kSealStartedCleanupWireBytes> encoded{};
    ASSERT_EQ(encode_seal_started_cleanup_tombstone_wire(encoded, v, kek), kSealStartedCleanupWireBytes);
    const auto path =
        candidate_dir / compaction_detail::ValidatedArtifactName::for_seal_started_cleanup_tombstone().relative_name();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
}

// Same precedent, for `.abd`.
void plant_abd(const std::filesystem::path& candidate_dir, const SealStartedAbandonWire& v,
               const std::array<std::byte, kKekSize>& kek) {
    std::array<std::byte, kSealStartedAbandonWireBytes> encoded{};
    ASSERT_EQ(encode_seal_started_abandon_wire(encoded, v, kek), kSealStartedAbandonWireBytes);
    const auto path = candidate_dir / compaction_detail::ValidatedArtifactName::for_seal_started_abandon().relative_name();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
}

// A SealStartedCleanupTombstoneWire whose bound fields match make_request()/
// make_started()'s fixed constants, at the given phase (default Authorized --
// raise_post_seal_finalizing()'s only legal precondition phase). new_generation
// is forced to source_generation+1 by validate_seal_started_cleanup_shape()'s
// own internal algebra (encode_* silently returns 0 otherwise) -- given
// source_generation is ALSO one of this round's checked-must-match-Intent
// fields, `.clr`'s new_generation therefore can never independently diverge
// from Intent's target_generation while source_generation still matches; the
// `clr.new_generation == before.target_generation` check in
// intent_phase_advancer.hpp is real defense-in-depth (protects against either
// upstream invariant being loosened later), not something a standalone test
// can exercise as a distinct failure mode from a source_generation mismatch.
SealStartedCleanupTombstoneWire make_clr(std::uint32_t kek_key_id, std::uint64_t candidate_id,
                                          std::uint64_t request_id,
                                          std::uint8_t phase = kSealStartedCleanupPhaseAuthorized) {
    SealStartedCleanupTombstoneWire v{};
    v.store_uuid_lo = 0x1111111111111111ULL;
    v.store_uuid_hi = 0x2222222222222222ULL;
    v.candidate_id = candidate_id;
    v.request_id = request_id;
    v.kek_key_id = kek_key_id;
    v.started_kind = kSealStartedKindNativeV2;
    v.present_mask = seal_started_wire_codec_detail::kNativeV2Mask;
    v.phase = phase;
    v.source_generation = 3;
    v.baseline_tip_seq = 999;
    for (auto& b : v.baseline_tip_mac) b = std::uint8_t{0xAB};
    v.baseline_key_id = kek_key_id;
    v.new_generation = 4;  // == make_request()'s target_generation
    v.new_final_seq = 555;
    for (auto& b : v.new_final_tip_mac) b = std::uint8_t{0xCD};
    v.new_key_id = kek_key_id;
    for (auto& b : v.content_root) b = std::uint8_t{0xEF};
    // digest_L/digest_V/digest_M stay default-zero -- required for NativeV2
    // (validate_seal_started_cleanup_shape()'s canonical-zero rule).
    return v;
}

// A SealStartedAbandonWire whose bound fields match make_request()'s fixed
// constants, at the given phase (default ResumeAuthorized(6) --
// raise_abandon_finalizing()'s only legal precondition phase). `.abd` has no
// new_generation/new_final_* fields at all, unlike `.clr` above.
SealStartedAbandonWire make_abd(std::uint32_t kek_key_id, std::uint64_t candidate_id, std::uint64_t request_id,
                                 std::uint8_t phase = kSealStartedAbandonPhaseResumeAuthorized) {
    SealStartedAbandonWire v{};
    v.store_uuid_lo = 0x1111111111111111ULL;
    v.store_uuid_hi = 0x2222222222222222ULL;
    v.candidate_id = candidate_id;
    v.request_id = request_id;
    v.kek_key_id = kek_key_id;
    v.started_kind = kSealStartedKindNativeV2;
    v.abandon_reason = kSealStartedAbandonReasonNotFound;
    v.present_mask = seal_started_wire_codec_detail::kNativeV2Mask;  // bit3(C) left 0 -> digest_C must be zero
    v.phase = phase;
    v.source_generation = 3;
    v.baseline_tip_seq = 999;
    for (auto& b : v.baseline_tip_mac) b = std::uint8_t{0xAB};
    v.baseline_key_id = kek_key_id;
    // content_root/digest_C stay default-zero -- required since bit3(C) is unset.
    return v;
}

// A seq=1 `.x1` frame whose bound fields (store_uuid/kek/generations/
// baseline/build_nonce) all match make_request()'s constants -- i.e. it
// passes walk_x1_chain_raw's ForeignBinding check -- but whose candidate_id/
// request_id are wrong. Used to prove the explicit id check this round adds
// (intent_phase_advancer.hpp's header comment's point 8) actually rejects
// what ForeignBinding alone would silently accept.
CompactionIntentTransitionWire make_seq1_with_foreign_ids(std::uint32_t kek_key_id, std::uint64_t build_nonce,
                                                           std::uint64_t wrong_candidate_id,
                                                           std::uint64_t wrong_request_id) {
    CompactionIntentTransitionWire t{};
    t.format_version = kCompactionIntentTransitionFormatVersion;
    t.total_bytes = kCompactionIntentTransitionWireBytes;
    t.store_uuid_lo = 0x1111111111111111ULL;
    t.store_uuid_hi = 0x2222222222222222ULL;
    t.kek_key_id = kek_key_id;
    t.from_phase = kCompactionCandidateIntentPhaseBuilding;
    t.to_phase = kCompactionCandidateIntentPhaseReserved;
    t.transition_seq = 1;
    t.source_generation = 3;
    t.target_generation = 4;
    t.baseline_tip_seq = 999;
    for (auto& b : t.baseline_tip_mac) b = std::uint8_t{0xAB};
    t.baseline_key_id = kek_key_id;
    t.build_nonce = build_nonce;
    t.candidate_id = wrong_candidate_id;
    t.request_id = wrong_request_id;
    // prev_transition_mac stays all-zero (default-constructed) -- required for transition_seq==1.
    return t;
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

// Drives an already-genesis'd, already-watermark-planted Intent from
// Building through StartedPublished (Building->Reserved->StartedPublished)
// via the given advancer, asserting each step succeeds -- shared setup every
// PostSeal/Abandon test below needs, since both new terminal edges only ever
// fire from StartedPublished. Callers must call fx.create_building_genesis()/
// fx.plant_watermark_reserving() BEFORE constructing TwoLeases/calling this
// (same discipline every other test in this file follows) --
// create_building_genesis()'s own IntentStore acquires and releases its own
// lease internally, which would collide with an already-held CandidateLease.
void advance_to_started_published(Fixture& fx, IntentPhaseAdvancer& advancer, std::uint64_t build_nonce,
                                   std::uint64_t candidate_id, std::uint64_t request_id) {
    ASSERT_EQ(advancer.raise_intent_phase(build_nonce), PhaseAdvanceStatus::PhaseRaised);
    ASSERT_EQ(advancer.raise_started_published(build_nonce, make_started(fx.kek_key_id, candidate_id, request_id)),
              PhaseAdvanceStatus::PhaseRaised);
}

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

// ---------------------------------------------------------------------------
// raise_started_published(): Reserved->StartedPublished (NativeV2Started).
// ---------------------------------------------------------------------------

TEST(IntentPhaseAdvancer, RaisesReservedToStartedPublishedWithValidStarted) {
    Fixture fx("started_ok");
    constexpr std::uint64_t kBuildNonce = 0x424242ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(/*candidate_id=*/3, /*request_id=*/6);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);

    const SealExportStartedWire started = make_started(fx.kek_key_id, /*candidate_id=*/3, /*request_id=*/6);
    EXPECT_EQ(advancer.raise_started_published(kBuildNonce, started), PhaseAdvanceStatus::PhaseRaised);

    const auto x1_seq2_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/2);
    EXPECT_TRUE(std::filesystem::exists(fx.dir / x1_seq2_name.relative_name()))
        << "the `.x1` seq=2 evidence must be durable once raise_started_published() reports success";

    SealExportStartedLoader started_loader(leases.candidate_lease);
    SealExportStartedWire loaded_started{};
    ASSERT_EQ(started_loader.load(/*legacy_or_greenfield=*/true, fx.ring, loaded_started), LoadStatus::Ok);
    EXPECT_EQ(loaded_started.candidate_id, 3u);
    EXPECT_EQ(loaded_started.request_id, 6u);
    EXPECT_EQ(loaded_started.new_generation, 4u);

    leases.release();

    IntentStore verify_store(fx.dir, fx.ring);
    ASSERT_EQ(verify_store.acquire_lease(), LeaseAcquireStatus::Acquired);
    CompactionCandidateIntentWire loaded{};
    ASSERT_EQ(verify_store.load_and_validate_intent(loaded), LoadStatus::Ok);
    EXPECT_EQ(loaded.phase, kCompactionCandidateIntentPhaseStartedPublished);
    EXPECT_EQ(loaded.candidate_id, 3u);
    EXPECT_EQ(loaded.request_id, 6u);
    ASSERT_EQ(verify_store.release_lease(), ReleaseStatus::Released);
}

TEST(IntentPhaseAdvancer, RetryAfterStartedPublishedSuccessReturnsAlreadyAtOrPastTargetPhase) {
    Fixture fx("started_retry_already_past");
    constexpr std::uint64_t kBuildNonce = 0x5151ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(1, 1);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);

    const SealExportStartedWire started = make_started(fx.kek_key_id, 1, 1);
    ASSERT_EQ(advancer.raise_started_published(kBuildNonce, started), PhaseAdvanceStatus::PhaseRaised);
    // Second call, same instance -- must not attempt a second `.x1` seq=2
    // write (that would collide) and must not error.
    EXPECT_EQ(advancer.raise_started_published(kBuildNonce, started), PhaseAdvanceStatus::AlreadyAtOrPastTargetPhase);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesStartedPublishedWhileStillBuilding) {
    Fixture fx("started_still_building");
    constexpr std::uint64_t kBuildNonce = 0x62ULL;
    fx.create_building_genesis(kBuildNonce);
    // Deliberately never calls raise_intent_phase() -- Intent is still Building.

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    const SealExportStartedWire started = make_started(fx.kek_key_id, /*candidate_id=*/1, /*request_id=*/1);
    EXPECT_EQ(advancer.raise_started_published(kBuildNonce, started), PhaseAdvanceStatus::IllegalTransition);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesStartedForeignBinding) {
    Fixture fx("started_foreign_binding");
    constexpr std::uint64_t kBuildNonce = 0x73ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(/*candidate_id=*/9, /*request_id=*/10);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);

    // candidate_id doesn't match the Intent's own bound candidate_id (9).
    SealExportStartedWire started = make_started(fx.kek_key_id, /*candidate_id=*/999, /*request_id=*/10);
    EXPECT_EQ(advancer.raise_started_published(kBuildNonce, started), PhaseAdvanceStatus::StartedForeignBinding);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesStartedShapeInvalid) {
    Fixture fx("started_shape_invalid");
    constexpr std::uint64_t kBuildNonce = 0x84ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(/*candidate_id=*/2, /*request_id=*/4);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);

    SealExportStartedWire started = make_started(fx.kek_key_id, /*candidate_id=*/2, /*request_id=*/4);
    started.registered_producer_mask = 0;  // topology self-consistency violation.
    started.producer_count = 0;
    started.ring_id[0] = 0;
    started.ring_id[1] = 0;
    EXPECT_EQ(advancer.raise_started_published(kBuildNonce, started), PhaseAdvanceStatus::StartedShapeInvalid);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesStartedPublishedForeignBuildNonce) {
    Fixture fx("started_foreign_nonce");
    constexpr std::uint64_t kBuildNonce = 0x95ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(1, 1);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);

    const SealExportStartedWire started = make_started(fx.kek_key_id, 1, 1);
    EXPECT_EQ(advancer.raise_started_published(/*build_nonce=*/0x9999ULL, started), PhaseAdvanceStatus::ForeignBuildNonce);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesStartedPublishedWhenSeqOneX1Missing) {
    Fixture fx("started_seq1_missing");
    constexpr std::uint64_t kBuildNonce = 0xA6ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(1, 1);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);

    // Out-of-band tampering: seq=1 `.x1` removed even though the Intent is
    // durably Reserved -- SeqOneX1NotFound, not a silent skip.
    const auto x1_seq1_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/1);
    std::filesystem::remove(fx.dir / x1_seq1_name.relative_name());

    const SealExportStartedWire started = make_started(fx.kek_key_id, 1, 1);
    EXPECT_EQ(advancer.raise_started_published(kBuildNonce, started), PhaseAdvanceStatus::SeqOneX1NotFound);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesStartedPublishedWhenSeqOneX1Corrupt) {
    Fixture fx("started_seq1_corrupt");
    constexpr std::uint64_t kBuildNonce = 0xB7ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(1, 1);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);

    // Out-of-band tampering: overwrite the durable seq=1 `.x1` frame with
    // bytes that fail decode (wrong format_version) -- MAC/decode failure,
    // not a WrongSize/NotFound case.
    const auto x1_seq1_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/1);
    {
        std::vector<char> corrupt_bytes(kCompactionIntentTransitionWireBytes, 'Z');
        std::ofstream corrupt(fx.dir / x1_seq1_name.relative_name(), std::ios::binary | std::ios::trunc);
        corrupt.write(corrupt_bytes.data(), static_cast<std::streamsize>(corrupt_bytes.size()));
    }

    const SealExportStartedWire started = make_started(fx.kek_key_id, 1, 1);
    EXPECT_EQ(advancer.raise_started_published(kBuildNonce, started), PhaseAdvanceStatus::SeqOneX1Corrupt);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesStartedPublishedWhenSeqOneX1IdsMismatch) {
    // Regression test for this round's retrofit fix: walk_x1_chain_raw's
    // ForeignBinding check never compares candidate_id/request_id (header
    // comment's point 8) -- a MAC-valid seq=1 frame with the WRONG ids for
    // this Intent must still be rejected, not silently accepted.
    Fixture fx("started_seq1_ids_mismatch");
    constexpr std::uint64_t kBuildNonce = 0xB8ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(1, 1);

    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);

    const auto tampered = make_seq1_with_foreign_ids(fx.kek_key_id, kBuildNonce, /*wrong_candidate_id=*/999,
                                                       /*wrong_request_id=*/999);
    std::array<std::byte, kCompactionIntentTransitionWireBytes> encoded{};
    encode_compaction_intent_transition_wire(encoded, tampered, fx.kek);
    const auto x1_seq1_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/1);
    {
        std::ofstream out(fx.dir / x1_seq1_name.relative_name(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    }

    const SealExportStartedWire started = make_started(fx.kek_key_id, 1, 1);
    EXPECT_EQ(advancer.raise_started_published(kBuildNonce, started), PhaseAdvanceStatus::SeqOneX1Corrupt);
    leases.release();
}

// ===========================================================================
// StartedPublished->PostSealFinalizing (raise_post_seal_finalizing())
// ===========================================================================

TEST(IntentPhaseAdvancer, RaisesPostSealFinalizingWithValidClr) {
    Fixture fx("postseal_ok");
    constexpr std::uint64_t kBuildNonce = 0x8801ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(8, 8);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, /*candidate_id=*/8, /*request_id=*/8);
    plant_clr(fx.dir, make_clr(fx.kek_key_id, 8, 8), fx.kek);

    EXPECT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);
    leases.release();

    IntentStore verify_store(fx.dir, fx.ring);
    ASSERT_EQ(verify_store.acquire_lease(), LeaseAcquireStatus::Acquired);
    CompactionCandidateIntentWire loaded{};
    ASSERT_EQ(verify_store.load_and_validate_intent(loaded), LoadStatus::Ok);
    EXPECT_EQ(loaded.phase, kCompactionCandidateIntentPhasePostSealFinalizing);
    EXPECT_EQ(loaded.candidate_id, 8u);
    EXPECT_EQ(loaded.request_id, 8u);
    ASSERT_EQ(verify_store.release_lease(), ReleaseStatus::Released);

    const auto x1_seq3_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/3);
    EXPECT_TRUE(std::filesystem::exists(fx.dir / x1_seq3_name.relative_name()))
        << "the `.x1` seq=3 evidence must be durable once raise_post_seal_finalizing() reports success";

    // `.clr` must still exist, untouched -- this function never writes/unlinks it.
    CandidateLease verify_lease(fx.dir);
    ASSERT_EQ(verify_lease.acquire(), LeaseAcquireStatus::Acquired);
    SealStartedCleanupTombstoneLoader<CandidateLease> clr_loader(verify_lease);
    SealStartedCleanupTombstoneWire clr_loaded{};
    ASSERT_EQ(clr_loader.load(fx.ring, clr_loaded), LoadStatus::Ok);
    EXPECT_EQ(clr_loaded.phase, kSealStartedCleanupPhaseAuthorized);
    ASSERT_EQ(verify_lease.release(), ReleaseStatus::Released);
}

TEST(IntentPhaseAdvancer, RetryAfterPostSealFinalizingSuccessReturnsAlreadyAtOrPastTargetPhase) {
    Fixture fx("postseal_retry");
    constexpr std::uint64_t kBuildNonce = 0x8802ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(8, 8);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 8, 8);
    plant_clr(fx.dir, make_clr(fx.kek_key_id, 8, 8), fx.kek);

    ASSERT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);
    // Second call, same instance -- must not attempt a second `.x1` seq=3
    // write (that would collide) and must not error.
    EXPECT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::AlreadyAtOrPastTargetPhase);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesPostSealFinalizingWhileReserved) {
    Fixture fx("postseal_while_reserved");
    constexpr std::uint64_t kBuildNonce = 0x8803ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(8, 8);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);
    // Deliberately never calls raise_started_published().
    EXPECT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::IllegalTransition);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesPostSealFinalizingWhileBuilding) {
    Fixture fx("postseal_while_building");
    constexpr std::uint64_t kBuildNonce = 0x8804ULL;
    fx.create_building_genesis(kBuildNonce);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    EXPECT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::IllegalTransition);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesPostSealFinalizingWhenAlreadyAbandonFinalizing) {
    Fixture fx("postseal_cross_branch");
    constexpr std::uint64_t kBuildNonce = 0x8805ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(8, 8);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 8, 8);
    plant_abd(fx.dir, make_abd(fx.kek_key_id, 8, 8), fx.kek);
    ASSERT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);

    // Spec forbids a PostSealFinalizing<->AbandonFinalizing cross
    // unconditionally -- permanent refusal, not a safe idempotent retry.
    EXPECT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::AlreadyAtOppositeTerminalPhase);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesPostSealFinalizingForeignBuildNonce) {
    Fixture fx("postseal_foreign_nonce");
    constexpr std::uint64_t kBuildNonce = 0x8806ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(8, 8);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 8, 8);
    plant_clr(fx.dir, make_clr(fx.kek_key_id, 8, 8), fx.kek);

    EXPECT_EQ(advancer.raise_post_seal_finalizing(/*build_nonce=*/0x9999ULL), PhaseAdvanceStatus::ForeignBuildNonce);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesPostSealFinalizingWhenClrMissing) {
    Fixture fx("postseal_clr_missing");
    constexpr std::uint64_t kBuildNonce = 0x8807ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(8, 8);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 8, 8);
    // Deliberately never plants `.clr`.
    EXPECT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::ClrNotFound);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesPostSealFinalizingWhenClrCorrupt) {
    Fixture fx("postseal_clr_corrupt");
    constexpr std::uint64_t kBuildNonce = 0x8808ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(8, 8);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 8, 8);

    std::array<std::byte, kSealStartedCleanupWireBytes> encoded{};
    ASSERT_EQ(encode_seal_started_cleanup_tombstone_wire(encoded, make_clr(fx.kek_key_id, 8, 8), fx.kek),
              kSealStartedCleanupWireBytes);
    // Break the trailer MAC without changing size.
    encoded.back() = static_cast<std::byte>(static_cast<unsigned char>(encoded.back()) ^ 0xFFu);
    const auto path =
        fx.dir / compaction_detail::ValidatedArtifactName::for_seal_started_cleanup_tombstone().relative_name();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    out.close();

    EXPECT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::ClrCorrupt);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesPostSealFinalizingWhenClrNotAuthorized) {
    Fixture fx("postseal_clr_not_authorized");
    constexpr std::uint64_t kBuildNonce = 0x8809ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(8, 8);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 8, 8);
    // MGone(1) -- not Authorized(0), and specifically NOT caught by a
    // "phase >= Authorized" style check either (round-1 review bug #2's
    // exact failure mode, since Authorized==0 makes such a check always true).
    plant_clr(fx.dir, make_clr(fx.kek_key_id, 8, 8, kSealStartedCleanupPhaseMGone), fx.kek);

    EXPECT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::ClrNotAuthorized);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesPostSealFinalizingWhenClrCandidateIdMismatch) {
    Fixture fx("postseal_clr_candidate_mismatch");
    constexpr std::uint64_t kBuildNonce = 0x880AULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(8, 8);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 8, 8);
    plant_clr(fx.dir, make_clr(fx.kek_key_id, /*candidate_id=*/999, /*request_id=*/8), fx.kek);

    EXPECT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::ClrForeignBinding);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesPostSealFinalizingWhenClrMigratedKind) {
    Fixture fx("postseal_clr_migrated_kind");
    constexpr std::uint64_t kBuildNonce = 0x880BULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(8, 8);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 8, 8);

    SealStartedCleanupTombstoneWire clr = make_clr(fx.kek_key_id, 8, 8);
    clr.started_kind = kSealStartedKindMigratedV2;
    clr.present_mask = seal_started_wire_codec_detail::kMigratedV2Mask;  // shape-legal for MigratedV2
    plant_clr(fx.dir, clr, fx.kek);

    // This round is NativeV2-only (see intent_phase_advancer.hpp's header
    // comment) -- a shape-valid MigratedV2 `.clr` is still a foreign binding.
    EXPECT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::ClrForeignBinding);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesPostSealFinalizingWhenSeqOneX1Missing) {
    Fixture fx("postseal_seq1_missing");
    constexpr std::uint64_t kBuildNonce = 0x880CULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(8, 8);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 8, 8);
    plant_clr(fx.dir, make_clr(fx.kek_key_id, 8, 8), fx.kek);

    const auto x1_seq1_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/1);
    std::filesystem::remove(fx.dir / x1_seq1_name.relative_name());

    EXPECT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::SeqOneX1NotFound);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesPostSealFinalizingWhenSeqOneX1Corrupt) {
    Fixture fx("postseal_seq1_corrupt");
    constexpr std::uint64_t kBuildNonce = 0x880DULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(8, 8);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 8, 8);
    plant_clr(fx.dir, make_clr(fx.kek_key_id, 8, 8), fx.kek);

    const auto x1_seq1_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/1);
    {
        std::vector<char> corrupt_bytes(kCompactionIntentTransitionWireBytes, 'Z');
        std::ofstream corrupt(fx.dir / x1_seq1_name.relative_name(), std::ios::binary | std::ios::trunc);
        corrupt.write(corrupt_bytes.data(), static_cast<std::streamsize>(corrupt_bytes.size()));
    }

    EXPECT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::SeqOneX1Corrupt);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesPostSealFinalizingWhenSeqTwoX1Missing) {
    Fixture fx("postseal_seq2_missing");
    constexpr std::uint64_t kBuildNonce = 0x880EULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(8, 8);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 8, 8);
    plant_clr(fx.dir, make_clr(fx.kek_key_id, 8, 8), fx.kek);

    const auto x1_seq2_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/2);
    std::filesystem::remove(fx.dir / x1_seq2_name.relative_name());

    EXPECT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::SeqTwoX1NotFound);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesPostSealFinalizingWhenSeqOneX1IdsMismatch) {
    Fixture fx("postseal_seq1_ids_mismatch");
    constexpr std::uint64_t kBuildNonce = 0x880FULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 9, 9);

    const auto tampered = make_seq1_with_foreign_ids(fx.kek_key_id, kBuildNonce, /*wrong_candidate_id=*/777,
                                                       /*wrong_request_id=*/777);
    std::array<std::byte, kCompactionIntentTransitionWireBytes> encoded{};
    encode_compaction_intent_transition_wire(encoded, tampered, fx.kek);
    const auto x1_seq1_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/1);
    {
        std::ofstream out(fx.dir / x1_seq1_name.relative_name(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    }
    // No `.clr` needed -- Step 1 (the seq1/seq2 read) must reject this before Step 2 ever runs.

    EXPECT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::SeqOneX1Corrupt);
    leases.release();
}

// ===========================================================================
// StartedPublished->AbandonFinalizing (raise_abandon_finalizing())
// ===========================================================================

TEST(IntentPhaseAdvancer, RaisesAbandonFinalizingWithValidAbd) {
    Fixture fx("abandon_ok");
    constexpr std::uint64_t kBuildNonce = 0x9901ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, /*candidate_id=*/9, /*request_id=*/9);
    plant_abd(fx.dir, make_abd(fx.kek_key_id, 9, 9), fx.kek);

    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);
    leases.release();

    IntentStore verify_store(fx.dir, fx.ring);
    ASSERT_EQ(verify_store.acquire_lease(), LeaseAcquireStatus::Acquired);
    CompactionCandidateIntentWire loaded{};
    ASSERT_EQ(verify_store.load_and_validate_intent(loaded), LoadStatus::Ok);
    EXPECT_EQ(loaded.phase, kCompactionCandidateIntentPhaseAbandonFinalizing);
    EXPECT_EQ(loaded.candidate_id, 9u);
    EXPECT_EQ(loaded.request_id, 9u);
    ASSERT_EQ(verify_store.release_lease(), ReleaseStatus::Released);

    const auto x1_seq3_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/3);
    EXPECT_TRUE(std::filesystem::exists(fx.dir / x1_seq3_name.relative_name()));

    // `.abd` must still exist, untouched -- "while A is still on disk" (spec).
    CandidateLease verify_lease(fx.dir);
    ASSERT_EQ(verify_lease.acquire(), LeaseAcquireStatus::Acquired);
    SealStartedAbandonLoader abd_loader(verify_lease);
    SealStartedAbandonWire abd_loaded{};
    ASSERT_EQ(abd_loader.load(fx.ring, abd_loaded), LoadStatus::Ok);
    EXPECT_EQ(abd_loaded.phase, kSealStartedAbandonPhaseResumeAuthorized);
    ASSERT_EQ(verify_lease.release(), ReleaseStatus::Released);
}

TEST(IntentPhaseAdvancer, RetryAfterAbandonFinalizingSuccessReturnsAlreadyAtOrPastTargetPhase) {
    Fixture fx("abandon_retry");
    constexpr std::uint64_t kBuildNonce = 0x9902ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 9, 9);
    plant_abd(fx.dir, make_abd(fx.kek_key_id, 9, 9), fx.kek);

    ASSERT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);
    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::AlreadyAtOrPastTargetPhase);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesAbandonFinalizingWhileReserved) {
    Fixture fx("abandon_while_reserved");
    constexpr std::uint64_t kBuildNonce = 0x9903ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);
    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::IllegalTransition);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesAbandonFinalizingWhileBuilding) {
    Fixture fx("abandon_while_building");
    constexpr std::uint64_t kBuildNonce = 0x9904ULL;
    fx.create_building_genesis(kBuildNonce);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::IllegalTransition);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesAbandonFinalizingWhenAlreadyPostSealFinalizing) {
    Fixture fx("abandon_cross_branch");
    constexpr std::uint64_t kBuildNonce = 0x9905ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 9, 9);
    plant_clr(fx.dir, make_clr(fx.kek_key_id, 9, 9), fx.kek);
    ASSERT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);

    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::AlreadyAtOppositeTerminalPhase);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesAbandonFinalizingForeignBuildNonce) {
    Fixture fx("abandon_foreign_nonce");
    constexpr std::uint64_t kBuildNonce = 0x9906ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 9, 9);
    plant_abd(fx.dir, make_abd(fx.kek_key_id, 9, 9), fx.kek);

    EXPECT_EQ(advancer.raise_abandon_finalizing(/*build_nonce=*/0x9999ULL), PhaseAdvanceStatus::ForeignBuildNonce);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesAbandonFinalizingWhenAbdMissing) {
    Fixture fx("abandon_abd_missing");
    constexpr std::uint64_t kBuildNonce = 0x9907ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 9, 9);
    // Deliberately never plants `.abd`.
    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::AbdNotFound);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesAbandonFinalizingWhenAbdCorrupt) {
    Fixture fx("abandon_abd_corrupt");
    constexpr std::uint64_t kBuildNonce = 0x9908ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 9, 9);

    std::array<std::byte, kSealStartedAbandonWireBytes> encoded{};
    ASSERT_EQ(encode_seal_started_abandon_wire(encoded, make_abd(fx.kek_key_id, 9, 9), fx.kek),
              kSealStartedAbandonWireBytes);
    encoded.back() = static_cast<std::byte>(static_cast<unsigned char>(encoded.back()) ^ 0xFFu);
    const auto path = fx.dir / compaction_detail::ValidatedArtifactName::for_seal_started_abandon().relative_name();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    out.close();

    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::AbdCorrupt);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesAbandonAtGenGoneNotResumeAuthorized) {
    // The single most important regression test in this round -- round-1
    // review bug #3: GenGone(5) alone does not prove the tip was
    // re-verified against the abandon baseline; only ResumeAuthorized(6)
    // does. A `>= GenGone` check would wrongly accept this.
    Fixture fx("abandon_gengone_not_resume_authorized");
    constexpr std::uint64_t kBuildNonce = 0x9909ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 9, 9);
    plant_abd(fx.dir, make_abd(fx.kek_key_id, 9, 9, kSealStartedAbandonPhaseGenGone), fx.kek);

    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::AbdNotResumeAuthorized);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesAbandonFinalizingWhenAbdAtEarlierPhase) {
    Fixture fx("abandon_abd_earlier_phase");
    constexpr std::uint64_t kBuildNonce = 0x990AULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 9, 9);
    plant_abd(fx.dir, make_abd(fx.kek_key_id, 9, 9, kSealStartedAbandonPhaseAuthorized), fx.kek);

    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::AbdNotResumeAuthorized);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesAbandonFinalizingWhenAbdAtAbdPending) {
    // Proves the gate is exact equality, not `>=` -- AbdPending(7) is PAST
    // ResumeAuthorized(6) and must still be refused.
    Fixture fx("abandon_abd_pending");
    constexpr std::uint64_t kBuildNonce = 0x990BULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 9, 9);
    plant_abd(fx.dir, make_abd(fx.kek_key_id, 9, 9, kSealStartedAbandonPhaseAbdPending), fx.kek);

    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::AbdNotResumeAuthorized);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesAbandonFinalizingWhenAbdCandidateIdMismatch) {
    Fixture fx("abandon_abd_candidate_mismatch");
    constexpr std::uint64_t kBuildNonce = 0x990CULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 9, 9);
    plant_abd(fx.dir, make_abd(fx.kek_key_id, /*candidate_id=*/999, /*request_id=*/9), fx.kek);

    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::AbdForeignBinding);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesAbandonFinalizingWhenAbdMigratedKind) {
    Fixture fx("abandon_abd_migrated_kind");
    constexpr std::uint64_t kBuildNonce = 0x990DULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 9, 9);

    SealStartedAbandonWire abd = make_abd(fx.kek_key_id, 9, 9);
    abd.started_kind = kSealStartedKindMigratedV2;
    abd.present_mask = seal_started_wire_codec_detail::kMigratedV2Mask;  // bit3(C) still 0 -> digest_C stays zero
    plant_abd(fx.dir, abd, fx.kek);

    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::AbdForeignBinding);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesAbandonFinalizingWhenSeqOneX1Missing) {
    Fixture fx("abandon_seq1_missing");
    constexpr std::uint64_t kBuildNonce = 0x990EULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 9, 9);
    plant_abd(fx.dir, make_abd(fx.kek_key_id, 9, 9), fx.kek);

    const auto x1_seq1_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/1);
    std::filesystem::remove(fx.dir / x1_seq1_name.relative_name());

    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::SeqOneX1NotFound);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesAbandonFinalizingWhenSeqTwoX1Missing) {
    Fixture fx("abandon_seq2_missing");
    constexpr std::uint64_t kBuildNonce = 0x990FULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 9, 9);
    plant_abd(fx.dir, make_abd(fx.kek_key_id, 9, 9), fx.kek);

    const auto x1_seq2_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/2);
    std::filesystem::remove(fx.dir / x1_seq2_name.relative_name());

    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::SeqTwoX1NotFound);
    leases.release();
}

TEST(IntentPhaseAdvancer, RefusesAbandonFinalizingWhenSeqTwoX1Corrupt) {
    Fixture fx("abandon_seq2_corrupt");
    constexpr std::uint64_t kBuildNonce = 0x9910ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(9, 9);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(fx, advancer, kBuildNonce, 9, 9);
    plant_abd(fx.dir, make_abd(fx.kek_key_id, 9, 9), fx.kek);

    const auto x1_seq2_name = compaction_detail::ValidatedArtifactName::for_x1(kBuildNonce, /*transition_seq=*/2);
    {
        std::vector<char> corrupt_bytes(kCompactionIntentTransitionWireBytes, 'Z');
        std::ofstream corrupt(fx.dir / x1_seq2_name.relative_name(), std::ios::binary | std::ios::trunc);
        corrupt.write(corrupt_bytes.data(), static_cast<std::streamsize>(corrupt_bytes.size()));
    }

    EXPECT_EQ(advancer.raise_abandon_finalizing(kBuildNonce), PhaseAdvanceStatus::SeqTwoX1Corrupt);
    leases.release();
}
