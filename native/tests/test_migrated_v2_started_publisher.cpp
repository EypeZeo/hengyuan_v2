// Real-filesystem tests for migrated_v2_started_publisher.hpp's
// MigratedV2StartedPublisher -- the V+M companion write path built on top of
// an already-durable "L" (written by a real raise_started_published() call).
// Governance: L2 (real file I/O). Test-only scope (see migrated_v2_started_
// publisher.hpp's own header comment) -- this class is not called from any
// production path.
//
// Two distinct KeyRing entries are used throughout: kLKeyId signs L (and is
// the key raise_started_published() itself used), kVKeyId signs V+M -- kept
// genuinely different byte material (not just a different id pointing at the
// same bytes) so that "M's MAC only verifies under v_kek_key_id, never
// legacy_kek_key_id" is an actual, meaningful assertion rather than a
// trivially-true one.
#include <gtest/gtest.h>
#include <hengyuan/compaction_intent_store.hpp>
#include <hengyuan/intent_phase_advancer.hpp>
#include <hengyuan/migrated_v2_started_publisher.hpp>
#include <hengyuan/seal_export_migration_cleanup_abandon_codec.hpp>
#include <hengyuan/seal_id_watermark_export_started_loader.hpp>
#include <hengyuan/seal_journal_precondition_codec.hpp>
#include <hengyuan/seal_journal_store_lease.hpp>
#include <hengyuan/seal_started_migration_cleanup_loader.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace hy;

namespace {

std::filesystem::path make_temp_dir(std::string_view tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("hy_migrated_v2_publisher_" + std::string(tag) + "_" +
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

// Same shape as test_intent_phase_advancer.cpp's own make_started() -- kept
// as a separate copy in this file rather than a shared header, matching the
// existing convention that each test file owns its fixtures.
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

std::array<SealHandoffRingId, kMaxSealHandoffProducers> make_v_topology_ring_id() {
    std::array<SealHandoffRingId, kMaxSealHandoffProducers> r{};
    r[0] = 201;
    return r;
}

struct Fixture {
    static constexpr std::uint32_t kLKeyId = 7;
    static constexpr std::uint32_t kVKeyId = 8;

    std::filesystem::path dir;
    std::filesystem::path seal_journal_dir;
    std::array<std::byte, kKekSize> root_kek;
    std::array<std::byte, kKekSize> l_key;
    std::array<std::byte, kKekSize> v_key;
    KeyRing ring;

    explicit Fixture(std::string_view tag)
        : dir(make_temp_dir(tag)),
          seal_journal_dir(make_temp_dir(std::string(tag) + "_sealjournal")),
          root_kek(make_kek(0x01)),
          l_key(make_kek(0x01)),
          v_key(make_kek(0x55)),
          ring(root_kek) {
        WrappedKeyRecord rec_l{};
        EXPECT_EQ(ring.add_key(kLKeyId, l_key, rec_l), KeyRingAddStatus::Ok);
        WrappedKeyRecord rec_v{};
        EXPECT_EQ(ring.add_key(kVKeyId, v_key, rec_v), KeyRingAddStatus::Ok);
    }

    ~Fixture() {
        std::filesystem::remove_all(dir);
        std::filesystem::remove_all(seal_journal_dir);
    }

    void create_building_genesis(std::uint64_t build_nonce) {
        IntentStore store(dir, ring);
        ASSERT_EQ(store.acquire_lease(), LeaseAcquireStatus::Acquired);
        ASSERT_EQ(store.create_building_intent(make_request(kLKeyId, build_nonce)), GenesisStatus::Created);
        ASSERT_EQ(store.release_lease(), ReleaseStatus::Released);
    }

    void plant_watermark_reserving(std::uint64_t candidate_id, std::uint64_t request_id) {
        SealIdWatermark v{};
        v.store_uuid_lo = 0x1111111111111111ULL;
        v.store_uuid_hi = 0x2222222222222222ULL;
        v.next_candidate_id = candidate_id;
        v.next_request_id = request_id;
        std::array<std::byte, kSealIdWatermarkWireBytes> encoded{};
        encode_seal_id_watermark_wire(encoded, v, l_key);
        const auto path = dir / compaction_detail::ValidatedArtifactName::for_seal_id_watermark().relative_name();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    }
};

// RAII pair for the two leases raise_intent_phase()/raise_started_published()
// need (same LOCK ORDER discipline as test_intent_phase_advancer.cpp) --
// MigratedV2StartedPublisher itself only needs the CandidateLease half.
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

// Drives an already-genesis'd, already-watermark-planted Intent through to
// StartedPublished (L durable) -- every test below needs this as its setup.
// Caller must call fx.create_building_genesis()/fx.plant_watermark_reserving()
// BEFORE constructing TwoLeases (same lease-acquisition-order discipline as
// test_intent_phase_advancer.cpp).
void advance_to_started_published(IntentPhaseAdvancer& advancer, std::uint64_t build_nonce,
                                   std::uint64_t candidate_id, std::uint64_t request_id) {
    ASSERT_EQ(advancer.raise_intent_phase(build_nonce), PhaseAdvanceStatus::PhaseRaised);
    ASSERT_EQ(advancer.raise_started_published(build_nonce, make_started(Fixture::kLKeyId, candidate_id, request_id)),
              PhaseAdvanceStatus::PhaseRaised);
}

}  // namespace

TEST(MigratedV2StartedPublisher, PublishesVWithClosedFieldsCopiedFromL) {
    Fixture fx("v_closed_fields");
    constexpr std::uint64_t kBuildNonce = 0xA001ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(11, 11);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(advancer, kBuildNonce, 11, 11);

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    ASSERT_EQ(publisher.publish(kBuildNonce, Fixture::kVKeyId, /*registered_producer_mask=*/0b00000001,
                                 /*producer_count=*/1, topology_ring),
              MigratedV2StartedPublishStatus::Published);
    leases.release();

    CandidateLease verify_lease(fx.dir);
    ASSERT_EQ(verify_lease.acquire(), LeaseAcquireStatus::Acquired);

    SealExportStartedLoader l_reader(verify_lease);
    SealExportStartedWire l{};
    ASSERT_EQ(l_reader.load(/*legacy_or_greenfield=*/true, fx.ring, l), LoadStatus::Ok);

    SealExportStartedLoader v_reader(verify_lease);
    SealExportStartedWire v{};
    ASSERT_EQ(v_reader.load(/*legacy_or_greenfield=*/false, fx.ring, v), LoadStatus::Ok);

    ASSERT_EQ(verify_lease.release(), ReleaseStatus::Released);

    // Closed fields must be byte-identical to L.
    EXPECT_EQ(v.store_uuid_lo, l.store_uuid_lo);
    EXPECT_EQ(v.store_uuid_hi, l.store_uuid_hi);
    EXPECT_EQ(v.candidate_id, l.candidate_id);
    EXPECT_EQ(v.source_generation, l.source_generation);
    EXPECT_EQ(v.baseline_tip_seq, l.baseline_tip_seq);
    EXPECT_EQ(0, std::memcmp(v.baseline_tip_mac, l.baseline_tip_mac, 32));
    EXPECT_EQ(v.baseline_key_id, l.baseline_key_id);
    EXPECT_EQ(v.new_generation, l.new_generation);
    EXPECT_EQ(v.new_final_seq, l.new_final_seq);
    EXPECT_EQ(0, std::memcmp(v.new_final_tip_mac, l.new_final_tip_mac, 32));
    EXPECT_EQ(v.new_key_id, l.new_key_id);  // the field a per-field-copy implementation is most
                                             // likely to accidentally drop -- explicit regression.
    EXPECT_EQ(v.request_id, l.request_id);
    EXPECT_EQ(0, std::memcmp(v.content_root, l.content_root, 32));

    // Mutable fields must differ as instructed, not be copied from L.
    EXPECT_EQ(v.kek_key_id, Fixture::kVKeyId);
    EXPECT_NE(v.kek_key_id, l.kek_key_id);
    EXPECT_EQ(v.registered_producer_mask, 0b00000001);
    EXPECT_EQ(v.producer_count, 1u);
    EXPECT_EQ(v.ring_id[0], 201u);
}

TEST(MigratedV2StartedPublisher, PublishesMWithCorrectDigestsAndMacs) {
    Fixture fx("m_digests");
    constexpr std::uint64_t kBuildNonce = 0xA002ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(12, 12);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(advancer, kBuildNonce, 12, 12);

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    ASSERT_EQ(publisher.publish(kBuildNonce, Fixture::kVKeyId, 0b00000001, 1, topology_ring),
              MigratedV2StartedPublishStatus::Published);
    leases.release();

    CandidateLease verify_lease(fx.dir);
    ASSERT_EQ(verify_lease.acquire(), LeaseAcquireStatus::Acquired);

    SealExportStartedLoader l_reader(verify_lease);
    SealExportStartedWire l{};
    ASSERT_EQ(l_reader.load(/*legacy_or_greenfield=*/true, fx.ring, l), LoadStatus::Ok);
    SealExportStartedLoader v_reader(verify_lease);
    SealExportStartedWire v{};
    ASSERT_EQ(v_reader.load(/*legacy_or_greenfield=*/false, fx.ring, v), LoadStatus::Ok);
    SealExportStartedMigrationLoader<CandidateLease> m_reader(verify_lease);
    SealExportStartedMigrationWire m{};
    ASSERT_EQ(m_reader.load(fx.ring, m), LoadStatus::Ok);

    ASSERT_EQ(verify_lease.release(), ReleaseStatus::Released);

    EXPECT_EQ(m.store_uuid_lo, l.store_uuid_lo);
    EXPECT_EQ(m.candidate_id, l.candidate_id);
    EXPECT_EQ(m.request_id, l.request_id);
    EXPECT_EQ(m.legacy_kek_key_id, Fixture::kLKeyId);
    EXPECT_EQ(m.v2_kek_key_id, Fixture::kVKeyId);

    // Re-encode the already-decoded L/V structs with their own keys --
    // deterministic, so this reconstructs the exact bytes each file holds
    // on disk without needing raw-byte lease access from this test file.
    std::array<std::byte, kSealExportStartedWireBytes> l_reencoded{};
    encode_seal_export_started_wire(l_reencoded, l, fx.l_key);
    std::array<std::byte, kSealExportStartedWireBytes> v_reencoded{};
    encode_seal_export_started_wire(v_reencoded, v, fx.v_key);

    const auto expected_legacy_digest = crypto::sha256(l_reencoded);
    const auto expected_v2_digest = crypto::sha256(v_reencoded);
    EXPECT_EQ(0, std::memcmp(m.legacy_file_digest, expected_legacy_digest.bytes.data(), 32));
    EXPECT_EQ(0, std::memcmp(m.v2_file_digest, expected_v2_digest.bytes.data(), 32));

    // legacy_mac/v2_mac are each L's/V's own trailer -- the last 32 bytes of
    // the re-encoded buffers.
    EXPECT_EQ(0, std::memcmp(m.legacy_mac, l_reencoded.data() + (kSealExportStartedWireBytes - 32), 32));
    EXPECT_EQ(0, std::memcmp(m.v2_mac, v_reencoded.data() + (kSealExportStartedWireBytes - 32), 32));
}

TEST(MigratedV2StartedPublisher, MacOnlyVerifiesUnderVKeyNotLegacyKey) {
    Fixture fx("m_wrong_key");
    constexpr std::uint64_t kBuildNonce = 0xA003ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(13, 13);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(advancer, kBuildNonce, 13, 13);

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    ASSERT_EQ(publisher.publish(kBuildNonce, Fixture::kVKeyId, 0b00000001, 1, topology_ring),
              MigratedV2StartedPublishStatus::Published);
    leases.release();

    CandidateLease verify_lease(fx.dir);
    ASSERT_EQ(verify_lease.acquire(), LeaseAcquireStatus::Acquired);
    SealExportStartedMigrationLoader<CandidateLease> m_reader(verify_lease);
    SealExportStartedMigrationWire m{};
    ASSERT_EQ(m_reader.load(fx.ring, m), LoadStatus::Ok);
    ASSERT_EQ(verify_lease.release(), ReleaseStatus::Released);

    // Reconstruct M's exact on-disk bytes (deterministic re-encode under its
    // real key), then attempt to decode those SAME bytes under L's key --
    // must fail. Confirms M's HMAC domain is genuinely bound to v_kek_key_id,
    // not merely labeled that way in the plaintext fields.
    std::array<std::byte, kSealExportStartedMigrationWireBytes> m_reencoded{};
    ASSERT_EQ(encode_seal_export_started_migration_wire(m_reencoded, m, fx.v_key), kSealExportStartedMigrationWireBytes);

    std::optional<VerifiedSealExportStartedMigration> decoded_with_wrong_key;
    EXPECT_EQ(decode_seal_export_started_migration_wire(m_reencoded, fx.l_key, decoded_with_wrong_key),
              SealStartedWireDecodeStatus::ChecksumMismatch);

    std::optional<VerifiedSealExportStartedMigration> decoded_with_right_key;
    EXPECT_EQ(decode_seal_export_started_migration_wire(m_reencoded, fx.v_key, decoded_with_right_key),
              SealStartedWireDecodeStatus::Ok);
}

TEST(MigratedV2StartedPublisher, IdempotentRetrySameInputsReturnsPublished) {
    Fixture fx("idempotent_retry");
    constexpr std::uint64_t kBuildNonce = 0xA004ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(14, 14);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(advancer, kBuildNonce, 14, 14);

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    ASSERT_EQ(publisher.publish(kBuildNonce, Fixture::kVKeyId, 0b00000001, 1, topology_ring),
              MigratedV2StartedPublishStatus::Published);
    // Second call, same instance, identical inputs -- V and M's CREATE_NEW
    // both hit byte-equal collisions and self-heal; must not error.
    EXPECT_EQ(publisher.publish(kBuildNonce, Fixture::kVKeyId, 0b00000001, 1, topology_ring),
              MigratedV2StartedPublishStatus::Published);
    leases.release();
}

TEST(MigratedV2StartedPublisher, RefusesWhileReserved) {
    Fixture fx("while_reserved");
    constexpr std::uint64_t kBuildNonce = 0xA005ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(15, 15);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    ASSERT_EQ(advancer.raise_intent_phase(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);
    // Deliberately never calls raise_started_published() -- L never written.

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    EXPECT_EQ(publisher.publish(kBuildNonce, Fixture::kVKeyId, 0b00000001, 1, topology_ring),
              MigratedV2StartedPublishStatus::IntentNotAtStartedPublished);
    leases.release();
}

TEST(MigratedV2StartedPublisher, RefusesWhilePostSealFinalizing) {
    // Reaches a phase PAST StartedPublished via a real raise_post_seal_
    // finalizing() call, proving the precondition is exact equality, not
    // ">=" -- StartedPublished(2) < PostSealFinalizing(3).
    Fixture fx("while_postseal");
    constexpr std::uint64_t kBuildNonce = 0xA006ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(16, 16);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(advancer, kBuildNonce, 16, 16);

    SealStartedCleanupTombstoneWire clr{};
    clr.store_uuid_lo = 0x1111111111111111ULL;
    clr.store_uuid_hi = 0x2222222222222222ULL;
    clr.candidate_id = 16;
    clr.request_id = 16;
    clr.kek_key_id = Fixture::kLKeyId;
    clr.started_kind = kSealStartedKindNativeV2;
    clr.present_mask = seal_started_wire_codec_detail::kNativeV2Mask;
    clr.phase = kSealStartedCleanupPhaseAuthorized;
    clr.source_generation = 3;
    clr.baseline_tip_seq = 999;
    for (auto& b : clr.baseline_tip_mac) b = std::uint8_t{0xAB};
    clr.baseline_key_id = Fixture::kLKeyId;
    clr.new_generation = 4;
    clr.new_final_seq = 555;
    for (auto& b : clr.new_final_tip_mac) b = std::uint8_t{0xCD};
    clr.new_key_id = Fixture::kLKeyId;
    for (auto& b : clr.content_root) b = std::uint8_t{0xEF};
    std::array<std::byte, kSealStartedCleanupWireBytes> encoded_clr{};
    ASSERT_EQ(encode_seal_started_cleanup_tombstone_wire(encoded_clr, clr, fx.l_key), kSealStartedCleanupWireBytes);
    {
        const auto name = compaction_detail::ValidatedArtifactName::for_seal_started_cleanup_tombstone();
        std::ofstream out(fx.dir / name.relative_name(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(encoded_clr.data()), static_cast<std::streamsize>(encoded_clr.size()));
    }
    ASSERT_EQ(advancer.raise_post_seal_finalizing(kBuildNonce), PhaseAdvanceStatus::PhaseRaised);

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    EXPECT_EQ(publisher.publish(kBuildNonce, Fixture::kVKeyId, 0b00000001, 1, topology_ring),
              MigratedV2StartedPublishStatus::IntentNotAtStartedPublished);
    leases.release();
}

TEST(MigratedV2StartedPublisher, RefusesForeignBuildNonce) {
    Fixture fx("foreign_nonce");
    constexpr std::uint64_t kBuildNonce = 0xA007ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(17, 17);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(advancer, kBuildNonce, 17, 17);

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    EXPECT_EQ(publisher.publish(/*build_nonce=*/0x9999ULL, Fixture::kVKeyId, 0b00000001, 1, topology_ring),
              MigratedV2StartedPublishStatus::ForeignBuildNonce);
    leases.release();
}

TEST(MigratedV2StartedPublisher, RefusesWhenLMissing) {
    Fixture fx("l_missing");
    constexpr std::uint64_t kBuildNonce = 0xA008ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(18, 18);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(advancer, kBuildNonce, 18, 18);

    const auto l_name = compaction_detail::ValidatedArtifactName::for_seal_export_started(/*legacy_or_greenfield=*/true);
    std::filesystem::remove(fx.dir / l_name.relative_name());

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    EXPECT_EQ(publisher.publish(kBuildNonce, Fixture::kVKeyId, 0b00000001, 1, topology_ring),
              MigratedV2StartedPublishStatus::LNotFound);
    leases.release();
}

TEST(MigratedV2StartedPublisher, RefusesWhenLMacTampered) {
    Fixture fx("l_mac_tampered");
    constexpr std::uint64_t kBuildNonce = 0xA009ULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(19, 19);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(advancer, kBuildNonce, 19, 19);

    const auto l_name = compaction_detail::ValidatedArtifactName::for_seal_export_started(/*legacy_or_greenfield=*/true);
    const auto l_path = fx.dir / l_name.relative_name();
    {
        std::fstream f(l_path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(0, std::ios::end);
        const auto size = f.tellp();
        f.seekp(static_cast<std::streamoff>(size) - 1);
        char last_byte = 0;
        f.seekg(static_cast<std::streamoff>(size) - 1);
        f.read(&last_byte, 1);
        last_byte = static_cast<char>(last_byte ^ 0x01);
        f.seekp(static_cast<std::streamoff>(size) - 1);
        f.write(&last_byte, 1);
    }

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    EXPECT_EQ(publisher.publish(kBuildNonce, Fixture::kVKeyId, 0b00000001, 1, topology_ring),
              MigratedV2StartedPublishStatus::LCorrupt);
    leases.release();
}

TEST(MigratedV2StartedPublisher, RefusesWhenLIsLegacy192ByteSized) {
    // Explicit regression: this class's decode path (decode_seal_export_
    // started_wire(), the same 238-byte-only function every other caller in
    // this repo uses) must not accidentally treat a legacy-sized buffer as
    // valid input -- this repo has no legacy-192B codec, see the header
    // comment's point 2.
    Fixture fx("l_legacy_sized");
    constexpr std::uint64_t kBuildNonce = 0xA00AULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(20, 20);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(advancer, kBuildNonce, 20, 20);

    const auto l_name = compaction_detail::ValidatedArtifactName::for_seal_export_started(/*legacy_or_greenfield=*/true);
    const auto l_path = fx.dir / l_name.relative_name();
    std::vector<char> legacy_sized_bytes(kSealExportStartedLegacyV1Bytes, 'Z');
    {
        std::ofstream out(l_path, std::ios::binary | std::ios::trunc);
        out.write(legacy_sized_bytes.data(), static_cast<std::streamsize>(legacy_sized_bytes.size()));
    }

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    EXPECT_EQ(publisher.publish(kBuildNonce, Fixture::kVKeyId, 0b00000001, 1, topology_ring),
              MigratedV2StartedPublishStatus::LCorrupt);
    leases.release();
}

TEST(MigratedV2StartedPublisher, RefusesWhenLForeignBinding) {
    Fixture fx("l_foreign_binding");
    constexpr std::uint64_t kBuildNonce = 0xA00BULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(21, 21);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(advancer, kBuildNonce, 21, 21);

    // Overwrite L directly (bypassing raise_started_published()) with a
    // record whose candidate_id doesn't match this Intent's own.
    SealExportStartedWire foreign_l = make_started(Fixture::kLKeyId, /*candidate_id=*/999, /*request_id=*/21);
    std::array<std::byte, kSealExportStartedWireBytes> encoded_foreign{};
    encode_seal_export_started_wire(encoded_foreign, foreign_l, fx.l_key);
    const auto l_name = compaction_detail::ValidatedArtifactName::for_seal_export_started(/*legacy_or_greenfield=*/true);
    {
        std::ofstream out(fx.dir / l_name.relative_name(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(encoded_foreign.data()),
                   static_cast<std::streamsize>(encoded_foreign.size()));
    }

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    EXPECT_EQ(publisher.publish(kBuildNonce, Fixture::kVKeyId, 0b00000001, 1, topology_ring),
              MigratedV2StartedPublishStatus::LForeignBinding);
    leases.release();
}

TEST(MigratedV2StartedPublisher, RefusesIllegalTopologyAndWritesNeitherVNorM) {
    Fixture fx("illegal_topology");
    constexpr std::uint64_t kBuildNonce = 0xA00CULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(22, 22);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(advancer, kBuildNonce, 22, 22);

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    // registered_producer_mask == 0 is illegal per validate_seal_export_
    // started_shape() (seal_journal_precondition_codec.hpp).
    EXPECT_EQ(publisher.publish(kBuildNonce, Fixture::kVKeyId, /*registered_producer_mask=*/0,
                                 /*producer_count=*/1, topology_ring),
              MigratedV2StartedPublishStatus::VShapeInvalid);
    leases.release();

    CandidateLease verify_lease(fx.dir);
    ASSERT_EQ(verify_lease.acquire(), LeaseAcquireStatus::Acquired);
    SealExportStartedLoader v_reader(verify_lease);
    SealExportStartedWire v{};
    EXPECT_EQ(v_reader.load(/*legacy_or_greenfield=*/false, fx.ring, v), LoadStatus::NotFound);
    SealExportStartedMigrationLoader<CandidateLease> m_reader(verify_lease);
    SealExportStartedMigrationWire m{};
    EXPECT_EQ(m_reader.load(fx.ring, m), LoadStatus::NotFound);
    ASSERT_EQ(verify_lease.release(), ReleaseStatus::Released);
}

TEST(MigratedV2StartedPublisher, RefusesWhenVKeyNotFound) {
    Fixture fx("v_key_not_found");
    constexpr std::uint64_t kBuildNonce = 0xA00DULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(23, 23);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(advancer, kBuildNonce, 23, 23);

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    EXPECT_EQ(publisher.publish(kBuildNonce, /*v_kek_key_id=*/9999u, 0b00000001, 1, topology_ring),
              MigratedV2StartedPublishStatus::VKeyNotFound);
    leases.release();
}

TEST(MigratedV2StartedPublisher, VWriteConflictReturnsVWriteFailed) {
    Fixture fx("v_write_conflict");
    constexpr std::uint64_t kBuildNonce = 0xA00EULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(24, 24);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(advancer, kBuildNonce, 24, 24);

    // Pre-plant a colliding V file with different content.
    const auto v_name = compaction_detail::ValidatedArtifactName::for_seal_export_started(/*legacy_or_greenfield=*/false);
    std::vector<char> wrong_bytes(kSealExportStartedWireBytes, 'X');
    {
        std::ofstream out(fx.dir / v_name.relative_name(), std::ios::binary | std::ios::trunc);
        out.write(wrong_bytes.data(), static_cast<std::streamsize>(wrong_bytes.size()));
    }

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    EXPECT_EQ(publisher.publish(kBuildNonce, Fixture::kVKeyId, 0b00000001, 1, topology_ring),
              MigratedV2StartedPublishStatus::VWriteFailed);
    leases.release();
}

TEST(MigratedV2StartedPublisher, MWriteConflictReturnsMWriteFailed) {
    Fixture fx("m_write_conflict");
    constexpr std::uint64_t kBuildNonce = 0xA00FULL;
    fx.create_building_genesis(kBuildNonce);
    fx.plant_watermark_reserving(25, 25);
    TwoLeases leases(fx);
    IntentPhaseAdvancer advancer(leases.candidate_lease, leases.seal_journal_lease, fx.ring);
    advance_to_started_published(advancer, kBuildNonce, 25, 25);

    // Pre-plant a colliding M file with different content -- V itself must
    // still succeed (it's written first), only M's write should fail.
    const auto m_name = compaction_detail::ValidatedArtifactName::for_seal_export_started_migration();
    std::vector<char> wrong_bytes(kSealExportStartedMigrationWireBytes, 'X');
    {
        std::ofstream out(fx.dir / m_name.relative_name(), std::ios::binary | std::ios::trunc);
        out.write(wrong_bytes.data(), static_cast<std::streamsize>(wrong_bytes.size()));
    }

    MigratedV2StartedPublisher publisher(leases.candidate_lease, fx.ring);
    const auto topology_ring = make_v_topology_ring_id();
    EXPECT_EQ(publisher.publish(kBuildNonce, Fixture::kVKeyId, 0b00000001, 1, topology_ring),
              MigratedV2StartedPublishStatus::MWriteFailed);
    leases.release();

    CandidateLease verify_lease(fx.dir);
    ASSERT_EQ(verify_lease.acquire(), LeaseAcquireStatus::Acquired);
    SealExportStartedLoader v_reader(verify_lease);
    SealExportStartedWire v{};
    EXPECT_EQ(v_reader.load(/*legacy_or_greenfield=*/false, fx.ring, v), LoadStatus::Ok);
    ASSERT_EQ(verify_lease.release(), ReleaseStatus::Released);
}
