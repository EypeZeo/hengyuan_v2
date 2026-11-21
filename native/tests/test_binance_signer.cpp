// P2-EXEC-LIVE-01: BinanceSigner HMAC-SHA256 tests.
// Verifies signing against known Binance API test vectors.
// SIMULATION ONLY — no real API keys, no network.
#include <gtest/gtest.h>
#include <hengyuan/binance_signer.hpp>
#include <hengyuan/owner_cancel_approval.hpp>
#include <hengyuan/owner_reset_approval.hpp>
#include <hengyuan/production_reset_key.hpp>
#include <hengyuan/spot_cancel_control.hpp>
#include <hengyuan/store_binding_writer.hpp>
#include "binance_clock_sync_test_hooks.hpp"
#include "binance_signer_test_hooks.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using hy::BinanceSigner;
using hy::BinanceSignerTestHooks;

namespace {

// Counting stub MemoryLockBehavior — deterministic, not tied to real system
// lock quota. Each test gets its own pair of counters via a small fixture
// struct rather than shared globals (tests may run in parallel).
struct LockStub {
    int lock_calls = 0;
    int unlock_calls = 0;
    bool lock_result = true;
    // AUDIT L4-LOCK-ORDER-003 regression: true iff every buffer handed to
    // stub_lock() across the whole test was still all-zero at the moment
    // lock() was called — i.e. the secret had not been memcpy'd in yet.
    bool saw_all_zero_at_lock = true;
};

LockStub* g_active_stub = nullptr;  // one at a time — tests are sequential
                                     // within this TU (gtest default runner).

bool stub_lock(void* ptr, std::size_t size) noexcept {
    ++g_active_stub->lock_calls;
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != 0) {
            g_active_stub->saw_all_zero_at_lock = false;
            break;
        }
    }
    return g_active_stub->lock_result;
}
void stub_unlock(void*, std::size_t) noexcept {
    ++g_active_stub->unlock_calls;
}

}  // namespace

namespace {

struct P256Fixture {
    EVP_PKEY* key{nullptr};
    std::vector<unsigned char> public_der;

    P256Fixture() {
        key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1");
        if (!key) return;
        const int size = i2d_PUBKEY(key, nullptr);
        if (size <= 0) return;
        public_der.resize(static_cast<std::size_t>(size));
        unsigned char* cursor = public_der.data();
        if (i2d_PUBKEY(key, &cursor) != size) public_der.clear();
    }

    ~P256Fixture() { EVP_PKEY_free(key); }
    P256Fixture(const P256Fixture&) = delete;
    P256Fixture& operator=(const P256Fixture&) = delete;

    std::vector<unsigned char> sign(std::span<const std::byte> payload) const {
        std::vector<unsigned char> out;
        if (!key) return out;
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        if (!context) return out;
        if (EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key) != 1 ||
            EVP_DigestSignUpdate(context, payload.data(), payload.size()) != 1) {
            EVP_MD_CTX_free(context);
            return out;
        }
        std::size_t size = 0;
        if (EVP_DigestSignFinal(context, nullptr, &size) != 1) {
            EVP_MD_CTX_free(context);
            return out;
        }
        out.resize(size);
        if (EVP_DigestSignFinal(context, out.data(), &size) != 1) out.clear();
        else out.resize(size);
        EVP_MD_CTX_free(context);
        return out;
    }
};

void fresh_clock(hy::ClockOffsetPublisher& publisher) {
    hy::ClockOffsetSnapshot snapshot{};
    snapshot.system_at_fetch_ms = 1'000'000;
    snapshot.steady_at_fetch_ms = 2'000'000;
    snapshot.error_bound_ms = 0;
    EXPECT_TRUE(publisher.publish(snapshot));
}

hy::ClockPairSample clock_now() {
    return hy::ClockPairSampleTestHooks::make(1'000'000, 2'000'000);
}

hy::CancelApproval approval_for(const hy::CancelSubject& subject, std::uint64_t serial,
                                std::int64_t not_after) {
    hy::CancelApproval approval{};
    approval.subject = subject;
    std::strncpy(approval.symbol, "BTCUSDT", sizeof(approval.symbol) - 1);
    approval.serial = serial;
    approval.not_after_ms = not_after;
    return approval;
}

hy::AutomaticCancelRequest request_for(const hy::CancelApproval& approval) {
    hy::AutomaticCancelRequest request{};
    request.subject = approval.subject;
    std::memcpy(request.symbol, approval.symbol, sizeof(request.symbol));
    request.approval_serial = approval.serial;
    request.approval_not_after_ms = approval.not_after_ms;
    return request;
}

std::span<const std::byte> grant_bytes(const hy::OwnerCancelGrantBytes& grant) {
    return std::span<const std::byte>(grant.data(), grant.size());
}

std::span<const std::byte> reset_bytes(const hy::OwnerResetChallengeBytes& challenge) {
    return std::span<const std::byte>(challenge.data(), challenge.size());
}

hy::OperatorResetTarget reset_target(std::uint64_t revision = 9) {
    return {{hy::StoreEnvironment::Testnet, 42, 0x1122334455667788ULL, 0x8877665544332211ULL}, revision};
}

bool write_binary_file(const std::string& path, std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

void remove_enrollment_store_files(const std::string& log_path, const std::string& kek_path,
                                   const std::string& wrapped_keys_path) {
    for (const auto& path : {
             log_path,
             log_path + ".lock",
             log_path + ".tip",
             log_path + ".tip.tmp",
             log_path + ".keyrotations",
             log_path + ".keyrotations.lock",
             log_path + ".keyrotations.tip",
             log_path + ".keyrotations.tip.tmp",
             log_path + ".storeid",
             log_path + ".storeid.lock",
             log_path + ".storeid.tip",
             log_path + ".storeid.tip.tmp",
             log_path + ".binding.lock",
             log_path + ".binding.tip",
             log_path + ".binding.tip.tmp",
             log_path + ".owner-cancel.pub",
             hy::ProductionResetKey::enrollment_path(log_path),
             kek_path,
             wrapped_keys_path,
         }) {
        std::remove(path.c_str());
    }
}

struct EnrollmentStoreFixture {
    std::string log_path;
    std::string kek_path;
    std::string wrapped_keys_path;
    std::array<std::byte, hy::kKekSize> kek{};
    std::uint32_t audit_key_id{1};

    static std::string new_prefix() {
        static std::uint64_t sequence = 0;
        const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        return (std::filesystem::temp_directory_path() /
                ("hy_owner_cancel_enrollment_" + std::to_string(now) + "_" +
                 std::to_string(++sequence)))
            .string();
    }

    explicit EnrollmentStoreFixture(std::string label) {
        const std::string prefix = new_prefix() + "_" + std::move(label);
        log_path = prefix + ".log";
        kek_path = prefix + ".kek";
        wrapped_keys_path = prefix + ".keys";
        remove_enrollment_store_files(log_path, kek_path, wrapped_keys_path);
    }

    ~EnrollmentStoreFixture() {
        remove_enrollment_store_files(log_path, kek_path, wrapped_keys_path);
    }

    EnrollmentStoreFixture(const EnrollmentStoreFixture&) = delete;
    EnrollmentStoreFixture& operator=(const EnrollmentStoreFixture&) = delete;

    hy::spot_cancel_control::StoreFiles files() const {
        return {log_path, audit_key_id, kek_path, wrapped_keys_path};
    }

    bool create_bound_store(hy::StoreEnvironment environment, std::uint64_t account_uid) {
        for (std::size_t i = 0; i < kek.size(); ++i) {
            kek[i] = static_cast<std::byte>(0x70 + i);
        }
        hy::KeyRing writer_ring(std::span<const std::byte, hy::kKekSize>(kek.data(), kek.size()));
        std::array<std::byte, hy::kKeyBlockSize> plaintext{};
        for (std::size_t i = 0; i < plaintext.size(); ++i) {
            plaintext[i] = static_cast<std::byte>(0x20 + i);
        }
        hy::WrappedKeyRecord wrapped{};
        if (writer_ring.add_key(audit_key_id, plaintext, wrapped) != hy::KeyRingAddStatus::Ok) return false;
        const auto wrapped_file = hy::encode_audit_key_file({&wrapped, 1});
        if (!write_binary_file(kek_path, std::span<const std::byte>(kek.data(), kek.size())) ||
            !write_binary_file(wrapped_keys_path, wrapped_file)) return false;
#ifndef _WIN32
        std::error_code permission_error;
        std::filesystem::permissions(kek_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, permission_error);
        if (permission_error) return false;
#endif

        hy::DurableAuditSink sink(log_path, writer_ring, audit_key_id);
        if (!sink.is_open() || sink.fenced()) return false;
        return hy::ensure_store_binding(sink, log_path, writer_ring, environment, account_uid, 1'000) ==
               hy::EnsureBindingStatus::BoundNew;
    }

};

}  // namespace

TEST(OwnerCancelApproval, InstallsSignedCanonicalGrantAndPermitsMatchingRequest) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    ASSERT_FALSE(fixture.public_der.empty());
    const hy::StoreBinding binding{hy::StoreEnvironment::Testnet, 42, 100};
    auto authority = hy::OwnerCancelAuthority::create(binding, 7, 8, 11, fixture.public_der, 60'000);
    ASSERT_NE(authority, nullptr);
    const auto approval = approval_for(authority->subject(), 1, 1'030'000);
    const auto grant = authority->signing_payload("BTCUSDT", approval.serial, approval.not_after_ms);
    ASSERT_TRUE(grant.has_value());
    const auto signature = fixture.sign(grant_bytes(*grant));
    ASSERT_FALSE(signature.empty());
    hy::ClockOffsetPublisher publisher;
    fresh_clock(publisher);
    EXPECT_EQ(authority->install(grant_bytes(*grant), signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::Installed);
    EXPECT_TRUE(authority->permits(request_for(approval), publisher, clock_now()));
    auto config = authority->config();
    const auto installed = config.approvals.read("BTCUSDT");
    ASSERT_TRUE(installed.has_value());
    EXPECT_EQ(installed->serial, approval.serial);
}

TEST(OwnerCancelApproval, RejectsPayloadTamperingAndWrongKeyId) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    const hy::StoreBinding binding{hy::StoreEnvironment::Testnet, 42, 100};
    auto authority = hy::OwnerCancelAuthority::create(binding, 7, 8, 11, fixture.public_der, 60'000);
    ASSERT_NE(authority, nullptr);
    const auto approval = approval_for(authority->subject(), 1, 1'030'000);
    auto grant = hy::encode_owner_cancel_grant(approval, 11);
    ASSERT_TRUE(grant.has_value());
    const auto signature = fixture.sign(grant_bytes(*grant));
    ASSERT_FALSE(signature.empty());
    hy::ClockOffsetPublisher publisher;
    fresh_clock(publisher);
    auto tampered = *grant;
    tampered[0] ^= std::byte{1};
    EXPECT_EQ(authority->install(grant_bytes(tampered), signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::MalformedGrant);
    const auto modified_grant = authority->signing_payload("ETHUSDT", approval.serial, approval.not_after_ms);
    ASSERT_TRUE(modified_grant.has_value());
    EXPECT_EQ(authority->install(*modified_grant, signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::InvalidSignature);
    P256Fixture other_key;
    ASSERT_NE(other_key.key, nullptr);
    const auto wrong_key_signature = other_key.sign(*grant);
    ASSERT_FALSE(wrong_key_signature.empty());
    EXPECT_EQ(authority->install(*grant, wrong_key_signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::InvalidSignature);
    hy::ClockOffsetPublisher unsynchronized;
    EXPECT_EQ(authority->install(*grant, signature, unsynchronized, clock_now()),
              hy::OwnerCancelInstallResult::ClockUnavailable);
    ASSERT_FALSE(authority->config().approvals.read("BTCUSDT").has_value());
    auto wrong_key = hy::encode_owner_cancel_grant(approval, 12);
    ASSERT_TRUE(wrong_key.has_value());
    const auto wrong_signature = fixture.sign(grant_bytes(*wrong_key));
    EXPECT_EQ(authority->install(grant_bytes(*wrong_key), wrong_signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::WrongScope);
}

TEST(OwnerCancelApproval, EnforcesSerialWatermarkRevocationAndExpiration) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    const hy::StoreBinding binding{hy::StoreEnvironment::Testnet, 42, 100};
    auto authority = hy::OwnerCancelAuthority::create(binding, 7, 8, 11, fixture.public_der, 60'000);
    ASSERT_NE(authority, nullptr);
    hy::ClockOffsetPublisher publisher;
    fresh_clock(publisher);
    const auto first = approval_for(authority->subject(), 2, 1'030'000);
    auto first_grant = hy::encode_owner_cancel_grant(first, 11);
    ASSERT_TRUE(first_grant.has_value());
    const auto first_signature = fixture.sign(grant_bytes(*first_grant));
    EXPECT_EQ(authority->install(grant_bytes(*first_grant), first_signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::Installed);
    authority->revoke(2);
    EXPECT_FALSE(authority->permits(request_for(first), publisher, clock_now()));
    EXPECT_EQ(authority->install(grant_bytes(*first_grant), first_signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::StaleSerial);
    const auto expired = approval_for(authority->subject(), 3, 999'999);
    auto expired_grant = hy::encode_owner_cancel_grant(expired, 11);
    ASSERT_TRUE(expired_grant.has_value());
    const auto expired_signature = fixture.sign(grant_bytes(*expired_grant));
    EXPECT_EQ(authority->install(grant_bytes(*expired_grant), expired_signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::InvalidDeadline);
}

TEST(OwnerCancelApproval, FreshAuthorityUsesDifferentActivationScope) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    const hy::StoreBinding binding{hy::StoreEnvironment::Testnet, 42, 100};
    auto first = hy::OwnerCancelAuthority::create(binding, 7, 8, 11, fixture.public_der, 60'000);
    auto second = hy::OwnerCancelAuthority::create(binding, 7, 8, 11, fixture.public_der, 60'000);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first->subject().activation_id, second->subject().activation_id);
    const auto approval = approval_for(first->subject(), 1, 1'030'000);
    auto grant = hy::encode_owner_cancel_grant(approval, 11);
    ASSERT_TRUE(grant.has_value());
    const auto signature = fixture.sign(grant_bytes(*grant));
    hy::ClockOffsetPublisher publisher;
    fresh_clock(publisher);
    EXPECT_EQ(second->install(grant_bytes(*grant), signature, publisher, clock_now()),
              hy::OwnerCancelInstallResult::WrongScope);
}

TEST(OwnerResetApproval, AcceptsOnlyPreparedCanonicalChallenge) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    ASSERT_FALSE(fixture.public_der.empty());

    auto authority = hy::OwnerResetAuthority::create(fixture.public_der);
    ASSERT_NE(authority, nullptr);
    const auto target = reset_target(9);
    constexpr std::int64_t server_upper_ms = 1'000'000;
    const auto challenge = authority->prepare(target, server_upper_ms);
    ASSERT_TRUE(challenge.has_value());

    const auto signature = fixture.sign(reset_bytes(*challenge));
    ASSERT_FALSE(signature.empty());
    const auto port = authority->approval_port();
    const auto approval = port.verify(
        port.context, std::span<const unsigned char>(signature.data(), signature.size()));
    ASSERT_TRUE(approval.has_value());
    EXPECT_EQ(approval->target.scope, target.scope);
    EXPECT_EQ(approval->target.revision, target.revision);
    EXPECT_EQ(approval->not_after_ms, server_upper_ms + hy::kOwnerResetValidityMs);

    EXPECT_FALSE(authority->prepare(target, server_upper_ms).has_value());

    auto tampered_domain = *challenge;
    tampered_domain[0] ^= std::byte{1};
    const auto domain_signature = fixture.sign(reset_bytes(tampered_domain));
    ASSERT_FALSE(domain_signature.empty());
    EXPECT_FALSE(port.verify(
        port.context, std::span<const unsigned char>(domain_signature.data(), domain_signature.size())));

    auto tampered_revision = *challenge;
    constexpr std::size_t revision_offset = sizeof(hy::kOwnerResetDomain) + 2 + 3 * sizeof(std::uint64_t);
    tampered_revision[revision_offset] ^= std::byte{1};
    const auto revision_signature = fixture.sign(reset_bytes(tampered_revision));
    ASSERT_FALSE(revision_signature.empty());
    EXPECT_FALSE(port.verify(
        port.context, std::span<const unsigned char>(revision_signature.data(), revision_signature.size())));

    auto tampered_expiry = *challenge;
    constexpr std::size_t expiry_offset = sizeof(hy::kOwnerResetDomain) + 2 + 4 * sizeof(std::uint64_t);
    tampered_expiry[expiry_offset] ^= std::byte{1};
    const auto expiry_signature = fixture.sign(reset_bytes(tampered_expiry));
    ASSERT_FALSE(expiry_signature.empty());
    EXPECT_FALSE(port.verify(
        port.context, std::span<const unsigned char>(expiry_signature.data(), expiry_signature.size())));
}

TEST(OwnerResetApproval, RejectsWrongKeyAndPreviousProcessChallenge) {
    P256Fixture fixture;
    P256Fixture wrong_key;
    ASSERT_NE(fixture.key, nullptr);
    ASSERT_NE(wrong_key.key, nullptr);
    ASSERT_FALSE(fixture.public_der.empty());

    auto first = hy::OwnerResetAuthority::create(fixture.public_der);
    ASSERT_NE(first, nullptr);
    const auto target = reset_target(17);
    const auto first_challenge = first->prepare(target, 2'000'000);
    ASSERT_TRUE(first_challenge.has_value());
    const auto first_signature = fixture.sign(reset_bytes(*first_challenge));
    ASSERT_FALSE(first_signature.empty());

    const auto first_port = first->approval_port();
    const auto wrong_key_signature = wrong_key.sign(reset_bytes(*first_challenge));
    ASSERT_FALSE(wrong_key_signature.empty());
    EXPECT_FALSE(first_port.verify(
        first_port.context,
        std::span<const unsigned char>(wrong_key_signature.data(), wrong_key_signature.size())));

    auto second = hy::OwnerResetAuthority::create(fixture.public_der);
    ASSERT_NE(second, nullptr);
    const auto second_challenge = second->prepare(target, 2'000'000);
    ASSERT_TRUE(second_challenge.has_value());
    const auto second_port = second->approval_port();
    EXPECT_FALSE(second_port.verify(
        second_port.context, std::span<const unsigned char>(first_signature.data(), first_signature.size())));

    const auto second_signature = fixture.sign(reset_bytes(*second_challenge));
    ASSERT_FALSE(second_signature.empty());
    const auto second_approval = second_port.verify(
        second_port.context, std::span<const unsigned char>(second_signature.data(), second_signature.size()));
    ASSERT_TRUE(second_approval.has_value());
    EXPECT_EQ(second_approval->target.scope, target.scope);
    EXPECT_EQ(second_approval->target.revision, target.revision);
}

TEST(OwnerResetApproval, RejectsCancellationSignature) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    ASSERT_FALSE(fixture.public_der.empty());

    const hy::StoreBinding binding{hy::StoreEnvironment::Testnet, 42, 100};
    auto cancellation = hy::OwnerCancelAuthority::create(binding, 7, 8, 11, fixture.public_der, 60'000);
    ASSERT_NE(cancellation, nullptr);
    const auto cancel_approval = approval_for(cancellation->subject(), 1, 1'030'000);
    const auto cancel_grant = cancellation->signing_payload(
        "BTCUSDT", cancel_approval.serial, cancel_approval.not_after_ms);
    ASSERT_TRUE(cancel_grant.has_value());
    const auto cancellation_signature = fixture.sign(grant_bytes(*cancel_grant));
    ASSERT_FALSE(cancellation_signature.empty());

    auto reset = hy::OwnerResetAuthority::create(fixture.public_der);
    ASSERT_NE(reset, nullptr);
    ASSERT_TRUE(reset->prepare(reset_target(), 3'000'000).has_value());
    const auto port = reset->approval_port();
    EXPECT_FALSE(port.verify(
        port.context,
        std::span<const unsigned char>(cancellation_signature.data(), cancellation_signature.size())));
}

TEST(OwnerResetApproval, RefusesInvalidKeyAndExpiryOverflow) {
    const std::array<unsigned char, 1> invalid_der{0};
    EXPECT_EQ(hy::OwnerResetAuthority::create(invalid_der), nullptr);

    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    ASSERT_FALSE(fixture.public_der.empty());
    auto authority = hy::OwnerResetAuthority::create(fixture.public_der);
    ASSERT_NE(authority, nullptr);
    EXPECT_FALSE(authority->prepare(reset_target(), std::numeric_limits<std::int64_t>::max()).has_value());
}

TEST(OwnerCancelEnrollment, BindsGeneratedP256KeyToOneProductionStoreExactlyOnce) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    ASSERT_FALSE(fixture.public_der.empty());

    const std::string der_path = EnrollmentStoreFixture::new_prefix() + ".owner.der";
    ASSERT_TRUE(write_binary_file(
        der_path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(fixture.public_der.data()),
                                             fixture.public_der.size())));

    EnrollmentStoreFixture first("first");
    EnrollmentStoreFixture second("second");
    constexpr auto environment = hy::StoreEnvironment::Production;
    constexpr std::uint64_t account_uid = 8'765'432'100;
    constexpr std::uint64_t owner_key_id = 23;
    ASSERT_TRUE(first.create_bound_store(environment, account_uid));
    ASSERT_TRUE(second.create_bound_store(environment, account_uid));

    const auto first_files = first.files();
    ASSERT_EQ(hy::spot_cancel_control::enroll(first_files, environment, owner_key_id, der_path),
              hy::ExportStatus::Ok);
    const std::string first_enrollment = hy::spot_cancel_control::enrollment_path(first.log_path);
    ASSERT_TRUE(hy::spot_cancel_control::is_regular_file(first_enrollment));

    {
        auto opened = hy::spot_cancel_control::open_existing_store(first_files, environment);
        ASSERT_NE(opened.sink, nullptr);
        auto authority = hy::spot_cancel_control::load_authority(first.log_path, *opened.sink);
        ASSERT_NE(authority, nullptr);
        EXPECT_EQ(authority->subject().environment, environment);
        EXPECT_EQ(authority->subject().account_uid, account_uid);
        EXPECT_TRUE(authority->subject().store_uuid_lo != 0 || authority->subject().store_uuid_hi != 0);
    }

    EXPECT_EQ(hy::spot_cancel_control::enroll(first_files, environment, owner_key_id, der_path),
              hy::ExportStatus::OutputExists);

    const std::string second_enrollment = hy::spot_cancel_control::enrollment_path(second.log_path);
    ASSERT_TRUE(std::filesystem::copy_file(first_enrollment, second_enrollment,
                                           std::filesystem::copy_options::overwrite_existing));
    {
        const auto second_files = second.files();
        auto opened = hy::spot_cancel_control::open_existing_store(second_files, environment);
        ASSERT_NE(opened.sink, nullptr);
        EXPECT_EQ(hy::spot_cancel_control::load_authority(second.log_path, *opened.sink), nullptr);
    }

    std::remove(der_path.c_str());
}

TEST(ProductionResetKey, EnrollsOfflineP256AndVerifiesOnlyTheEnrolledProductionScope) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    ASSERT_FALSE(fixture.public_der.empty());

    const std::string der_path = EnrollmentStoreFixture::new_prefix() + ".owner-reset.der";
    ASSERT_TRUE(write_binary_file(
        der_path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(fixture.public_der.data()),
                                             fixture.public_der.size())));

    EnrollmentStoreFixture store("production-reset");
    constexpr auto environment = hy::StoreEnvironment::Production;
    constexpr std::uint64_t account_uid = 8'765'432'100;
    constexpr std::uint64_t owner_key_id = 23;
    ASSERT_TRUE(store.create_bound_store(environment, account_uid));

    const auto files = store.files();
    ASSERT_EQ(hy::ProductionResetKey::enroll(files, owner_key_id, der_path), hy::ExportStatus::Ok);
    const std::string reset_enrollment = hy::ProductionResetKey::enrollment_path(store.log_path);
    ASSERT_TRUE(hy::spot_cancel_control::is_regular_file(reset_enrollment));

    auto opened = hy::spot_cancel_control::open_existing_store(files, environment);
    ASSERT_NE(opened.sink, nullptr);
    const auto binding = opened.sink->read_binding();
    ASSERT_EQ(binding.status, hy::StoreBindingStatus::Bound);
    ASSERT_EQ(binding.binding.environment, environment);
    ASSERT_EQ(binding.binding.account_uid, account_uid);
    ASSERT_TRUE(opened.sink->store_uuid_lo() != 0 || opened.sink->store_uuid_hi() != 0);

    auto authority = hy::ProductionResetKey::load(store.log_path, *opened.sink);
    ASSERT_NE(authority, nullptr);

    const hy::OperatorResetTarget target{
        {environment, account_uid, opened.sink->store_uuid_lo(), opened.sink->store_uuid_hi()}, 17};
    const hy::OperatorResetTarget other_store{
        {environment, account_uid, opened.sink->store_uuid_lo() ^ 1ULL, opened.sink->store_uuid_hi()}, 17};
    auto wrong_account = target;
    ++wrong_account.scope.account_uid;
    EXPECT_FALSE(authority->prepare(other_store, 2'000'000).has_value());
    EXPECT_FALSE(authority->prepare(wrong_account, 2'000'000).has_value());
    EXPECT_FALSE(authority->prepare(reset_target(17), 2'000'000).has_value());

    const auto drill_authority = hy::OwnerResetAuthority::create(fixture.public_der);
    ASSERT_NE(drill_authority, nullptr);
    EXPECT_FALSE(drill_authority->prepare(target, 2'000'000).has_value());

    const auto challenge = authority->prepare(target, 2'000'000);
    ASSERT_TRUE(challenge.has_value());
    const auto signature = fixture.sign(reset_bytes(*challenge));
    ASSERT_FALSE(signature.empty());
    const auto port = authority->approval_port();
    const auto approval = port.verify(
        port.context, std::span<const unsigned char>(signature.data(), signature.size()));
    ASSERT_TRUE(approval.has_value());
    EXPECT_EQ(approval->target.scope, target.scope);
    EXPECT_EQ(approval->target.revision, target.revision);
    EXPECT_EQ(approval->not_after_ms, 2'000'000 + hy::kOwnerResetValidityMs);

    std::remove(der_path.c_str());
}

TEST(ProductionResetKey, RefusesDuplicateUnboundTestnetAndCopiedEnrollment) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    ASSERT_FALSE(fixture.public_der.empty());

    const std::string der_path = EnrollmentStoreFixture::new_prefix() + ".owner-reset.der";
    ASSERT_TRUE(write_binary_file(
        der_path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(fixture.public_der.data()),
                                             fixture.public_der.size())));

    EnrollmentStoreFixture unbound("unbound");
    EnrollmentStoreFixture testnet("testnet");
    EnrollmentStoreFixture first("first-production");
    EnrollmentStoreFixture second("second-production");
    constexpr std::uint64_t owner_key_id = 23;
    ASSERT_TRUE(testnet.create_bound_store(hy::StoreEnvironment::Testnet, 8'765'432'100));
    ASSERT_TRUE(first.create_bound_store(hy::StoreEnvironment::Production, 8'765'432'100));
    ASSERT_TRUE(second.create_bound_store(hy::StoreEnvironment::Production, 8'765'432'100));

    EXPECT_EQ(hy::ProductionResetKey::enroll(unbound.files(), owner_key_id, der_path),
              hy::ExportStatus::OutputError);
    EXPECT_EQ(hy::ProductionResetKey::enroll(testnet.files(), owner_key_id, der_path),
              hy::ExportStatus::OutputError);

    const auto first_files = first.files();
    ASSERT_EQ(hy::ProductionResetKey::enroll(first_files, owner_key_id, der_path), hy::ExportStatus::Ok);
    EXPECT_EQ(hy::ProductionResetKey::enroll(first_files, owner_key_id, der_path),
              hy::ExportStatus::OutputExists);

    const auto first_enrollment = hy::ProductionResetKey::enrollment_path(first.log_path);
    const auto second_enrollment = hy::ProductionResetKey::enrollment_path(second.log_path);
    ASSERT_TRUE(std::filesystem::copy_file(first_enrollment, second_enrollment,
                                           std::filesystem::copy_options::overwrite_existing));
    auto opened = hy::spot_cancel_control::open_existing_store(second.files(), hy::StoreEnvironment::Production);
    ASSERT_NE(opened.sink, nullptr);
    EXPECT_EQ(hy::ProductionResetKey::load(second.log_path, *opened.sink), nullptr);

    std::remove(der_path.c_str());
}

TEST(ProductionResetKey, KeepsCancellationEnrollmentSeparateFromResetEnrollment) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    ASSERT_FALSE(fixture.public_der.empty());

    const std::string der_path = EnrollmentStoreFixture::new_prefix() + ".owner-reset.der";
    ASSERT_TRUE(write_binary_file(
        der_path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(fixture.public_der.data()),
                                             fixture.public_der.size())));

    EnrollmentStoreFixture store("purpose-separation");
    constexpr auto environment = hy::StoreEnvironment::Production;
    constexpr std::uint64_t account_uid = 8'765'432'100;
    constexpr std::uint64_t owner_key_id = 23;
    ASSERT_TRUE(store.create_bound_store(environment, account_uid));
    const auto files = store.files();

    ASSERT_EQ(hy::spot_cancel_control::enroll(files, environment, owner_key_id, der_path),
              hy::ExportStatus::Ok);
    const auto cancel_path = hy::spot_cancel_control::enrollment_path(store.log_path);
    const auto reset_path = hy::ProductionResetKey::enrollment_path(store.log_path);
    EXPECT_NE(cancel_path, reset_path);
    ASSERT_TRUE(hy::spot_cancel_control::is_regular_file(cancel_path));
    ASSERT_TRUE(std::filesystem::copy_file(cancel_path, reset_path));
    {
        auto opened = hy::spot_cancel_control::open_existing_store(files, environment);
        ASSERT_NE(opened.sink, nullptr);
        EXPECT_EQ(hy::ProductionResetKey::load(store.log_path, *opened.sink), nullptr);
    }
    ASSERT_TRUE(std::filesystem::remove(reset_path));

    ASSERT_EQ(hy::ProductionResetKey::enroll(files, owner_key_id, der_path), hy::ExportStatus::Ok);
    ASSERT_TRUE(hy::spot_cancel_control::is_regular_file(reset_path));
    auto reopened = hy::spot_cancel_control::open_existing_store(files, environment);
    ASSERT_NE(reopened.sink, nullptr);
    EXPECT_NE(hy::ProductionResetKey::load(store.log_path, *reopened.sink), nullptr);

    std::remove(der_path.c_str());
}

TEST(ProductionResetKey, RefusesLiveStoreWriterAndInvalidPublicKey) {
    P256Fixture fixture;
    ASSERT_NE(fixture.key, nullptr);
    ASSERT_FALSE(fixture.public_der.empty());

    const std::string der_path = EnrollmentStoreFixture::new_prefix() + ".owner-reset.der";
    ASSERT_TRUE(write_binary_file(
        der_path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(fixture.public_der.data()),
                                             fixture.public_der.size())));

    EnrollmentStoreFixture store("lock-refusal");
    ASSERT_TRUE(store.create_bound_store(hy::StoreEnvironment::Production, 8'765'432'100));
    const auto files = store.files();
    auto held = hy::spot_cancel_control::open_existing_store(files, hy::StoreEnvironment::Production);
    ASSERT_NE(held.sink, nullptr);
    EXPECT_EQ(hy::ProductionResetKey::enroll(files, 23, der_path), hy::ExportStatus::OutputError);
    held.sink.reset();

    const std::array<std::byte, 1> invalid_der{std::byte{0}};
    ASSERT_TRUE(write_binary_file(der_path, invalid_der));
    EXPECT_EQ(hy::ProductionResetKey::enroll(files, 23, der_path), hy::ExportStatus::OutputError);

    std::remove(der_path.c_str());
}

TEST(BinanceSigner, UninitializedRefusesSigning) {
    BinanceSigner signer;
    auto sig = signer.sign("test");
    EXPECT_TRUE(sig.empty());
    EXPECT_EQ(signer.stats().failures, 1u);
}

TEST(BinanceSigner, EmptySecretFails) {
    BinanceSigner signer;
    EXPECT_FALSE(signer.init(""));
    EXPECT_FALSE(signer.is_initialized());
}

TEST(BinanceSigner, InitSucceeds) {
    BinanceSigner signer;
    EXPECT_TRUE(signer.init("NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j"));
    EXPECT_TRUE(signer.is_initialized());
}

TEST(BinanceSigner, KnownTestVector) {
    // Binance API docs test vector:
    // secret = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j"
    // payload = "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559"
    // expected HMAC = "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71"
    BinanceSigner signer;
    EXPECT_TRUE(signer.init("NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j"));

    auto sig = signer.sign(
        "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC"
        "&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559");

    ASSERT_EQ(sig.size(), 64u);
    std::string hex(sig.data(), sig.size());
    EXPECT_EQ(hex, "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71");
}

TEST(BinanceSigner, MultipleSignsConsistent) {
    BinanceSigner signer;
    signer.init("testsecret123");

    auto sig1 = signer.sign("payload1");
    std::string hex1(sig1.data(), sig1.size());

    auto sig2 = signer.sign("payload1");
    std::string hex2(sig2.data(), sig2.size());

    EXPECT_EQ(hex1, hex2);
    EXPECT_EQ(signer.stats().signed_ok, 2u);
}

TEST(BinanceSigner, DifferentPayloadsDifferentSigs) {
    BinanceSigner signer;
    signer.init("testsecret123");

    auto sig1 = signer.sign("payload_a");
    std::string hex1(sig1.data(), sig1.size());

    auto sig2 = signer.sign("payload_b");
    std::string hex2(sig2.data(), sig2.size());

    EXPECT_NE(hex1, hex2);
}

TEST(BinanceSigner, EmptyPayloadRefused) {
    BinanceSigner signer;
    signer.init("secret");
    auto sig = signer.sign("");
    EXPECT_TRUE(sig.empty());
    EXPECT_EQ(signer.stats().failures, 1u);
}

TEST(BinanceSigner, ZeroAllocSpanOutput) {
    BinanceSigner signer;
    signer.init("key");
    auto sig = signer.sign("data");
    ASSERT_EQ(sig.size(), 64u);
    // Verify all chars are valid hex
    for (char c : sig) {
        bool valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        EXPECT_TRUE(valid) << "Invalid hex char: " << c;
    }
}

// --- Plan v17 §"BinanceSigner 锁注入" regressions ---

TEST(BinanceSignerLockInjection, StubPairingFailurePath) {
    LockStub stub;
    stub.lock_result = false;
    g_active_stub = &stub;

    BinanceSigner signer;
    bool ok = BinanceSignerTestHooks::init_with_lock_behavior(
        signer, "secretvalue", {&stub_lock, &stub_unlock});

    EXPECT_FALSE(ok);
    EXPECT_EQ(stub.lock_calls, 1);
    EXPECT_EQ(stub.unlock_calls, 0);   // never locked, so destroy() must not
                                        // call unlock — precise pairing, not
                                        // "unlocking unlocked memory is
                                        // harmless" reasoning.
    EXPECT_FALSE(BinanceSignerTestHooks::is_locked(signer));
    EXPECT_EQ(BinanceSignerTestHooks::last_failure(signer),
              BinanceSigner::SignFailure::LockFailed);
    EXPECT_FALSE(signer.is_initialized());
}

TEST(BinanceSignerLockInjection, StubPairingSuccessPath) {
    LockStub stub;
    stub.lock_result = true;
    g_active_stub = &stub;

    {
        BinanceSigner signer;
        bool ok = BinanceSignerTestHooks::init_with_lock_behavior(
            signer, "secretvalue", {&stub_lock, &stub_unlock});
        EXPECT_TRUE(ok);
        EXPECT_EQ(stub.lock_calls, 1);
        EXPECT_EQ(stub.unlock_calls, 0);   // not unlocked yet — destructor
                                             // hasn't run.
        EXPECT_TRUE(BinanceSignerTestHooks::is_locked(signer));
    }  // ~BinanceSigner() -> destroy() runs here

    EXPECT_EQ(stub.unlock_calls, 1);   // exactly one paired unlock call
}

TEST(BinanceSignerLockInjection, NullBehaviorRefusedNotCalled) {
    BinanceSigner signer;
    bool ok = BinanceSignerTestHooks::init_with_lock_behavior(
        signer, "secretvalue", {nullptr, nullptr});

    EXPECT_FALSE(ok);
    EXPECT_EQ(BinanceSignerTestHooks::last_failure(signer),
              BinanceSigner::SignFailure::LockFailed);
    EXPECT_FALSE(signer.is_initialized());
    // No crash: the null function pointers were never invoked.
}

TEST(BinanceSignerLockInjection, RealSyscallSmoke) {
    // Independent of the stub tests above — exercises the actual default
    // MemoryLockBehavior{} (hy::try_lock_memory/unlock_memory). Does not
    // assert the lock necessarily succeeds (depends on the machine's
    // RLIMIT_MEMLOCK/working-set quota) — only that internal state stays
    // consistent and nothing crashes.
    BinanceSigner signer;
    bool ok = signer.init("secretvalue");
    if (ok) {
        EXPECT_TRUE(BinanceSignerTestHooks::is_locked(signer));
    } else {
        EXPECT_FALSE(BinanceSignerTestHooks::is_locked(signer));
    }
}

TEST(BinanceSignerLockInjection, LockSeesEmptyBufferBeforeSecretIsWritten) {
    LockStub stub;
    stub.lock_result = true;
    g_active_stub = &stub;

    BinanceSigner signer;
    bool ok = BinanceSignerTestHooks::init_with_lock_behavior(
        signer, "secretvalue", {&stub_lock, &stub_unlock});

    EXPECT_TRUE(ok);
    EXPECT_EQ(stub.lock_calls, 1);
    // Proves the ordering directly (not just the final signing result):
    // secret_buf_ was still all-zero at the instant lock() ran, i.e. the
    // memcpy of the real secret happened strictly after locking succeeded.
    EXPECT_TRUE(stub.saw_all_zero_at_lock);
}

TEST(BinanceSignerLockInjection, InvalidSecretSetsFailureAndResetsOnRetry) {
    LockStub stub;
    g_active_stub = &stub;
    BinanceSigner signer;

    bool empty_ok = BinanceSignerTestHooks::init_with_lock_behavior(
        signer, "", {&stub_lock, &stub_unlock});
    EXPECT_FALSE(empty_ok);
    EXPECT_EQ(BinanceSignerTestHooks::last_failure(signer),
              BinanceSigner::SignFailure::InvalidSecret);

    std::string too_long(257, 'x');
    bool long_ok = BinanceSignerTestHooks::init_with_lock_behavior(
        signer, too_long, {&stub_lock, &stub_unlock});
    EXPECT_FALSE(long_ok);
    EXPECT_EQ(BinanceSignerTestHooks::last_failure(signer),
              BinanceSigner::SignFailure::InvalidSecret);

    // Reset timing lives at init_impl() entry, not inside destroy() — a
    // subsequent successful attempt must clear the stale diagnostic.
    bool retry_ok = BinanceSignerTestHooks::init_with_lock_behavior(
        signer, "validsecret", {&stub_lock, &stub_unlock});
    EXPECT_TRUE(retry_ok);
    EXPECT_EQ(BinanceSignerTestHooks::last_failure(signer),
              BinanceSigner::SignFailure::None);
}
