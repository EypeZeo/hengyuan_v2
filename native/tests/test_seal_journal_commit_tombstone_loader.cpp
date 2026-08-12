// Read-only SealJournalCommitWatermarkLoader / SealJournalTombstoneLoader
// coverage. Branch classification is driven through a deterministic lease
// fake; one real-filesystem test exercises the production friend-only path.
#include <gtest/gtest.h>
#include <hengyuan/compaction_intent_store.hpp>
#include <hengyuan/seal_journal_commit_tombstone_loader.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <random>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace allocation_probe {

std::atomic<std::size_t> allocation_count{0};
thread_local bool enabled = false;
thread_local bool fail = false;

void record() noexcept {
    if (enabled) allocation_count.fetch_add(1, std::memory_order_relaxed);
}

void* aligned_allocate(std::size_t size, std::size_t alignment) noexcept {
#ifdef _WIN32
    return ::_aligned_malloc(size == 0 ? 1 : size, alignment);
#else
    void* result = nullptr;
    if (::posix_memalign(&result, alignment, size == 0 ? 1 : size) != 0) {
        return nullptr;
    }
    return result;
#endif
}

void aligned_deallocate(void* ptr) noexcept {
#ifdef _WIN32
    ::_aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

}  // namespace allocation_probe

#if defined(_MSC_VER)
#define HY_ALLOCATION_PROBE_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define HY_ALLOCATION_PROBE_NOINLINE __attribute__((noinline))
#else
#define HY_ALLOCATION_PROBE_NOINLINE
#endif

HY_ALLOCATION_PROBE_NOINLINE void* operator new(std::size_t size) {
    allocation_probe::record();
    if (allocation_probe::fail) throw std::bad_alloc{};
    if (void* const ptr = std::malloc(size == 0 ? 1 : size); ptr != nullptr) {
        return ptr;
    }
    throw std::bad_alloc{};
}

HY_ALLOCATION_PROBE_NOINLINE void* operator new[](std::size_t size) {
    return ::operator new(size);
}

HY_ALLOCATION_PROBE_NOINLINE void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

HY_ALLOCATION_PROBE_NOINLINE void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

HY_ALLOCATION_PROBE_NOINLINE void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

HY_ALLOCATION_PROBE_NOINLINE void operator delete[](void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

HY_ALLOCATION_PROBE_NOINLINE void* operator new(std::size_t size, std::align_val_t alignment) {
    allocation_probe::record();
    if (allocation_probe::fail) throw std::bad_alloc{};
    if (void* const ptr =
            allocation_probe::aligned_allocate(size, static_cast<std::size_t>(alignment));
        ptr != nullptr) {
        return ptr;
    }
    throw std::bad_alloc{};
}

HY_ALLOCATION_PROBE_NOINLINE void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

HY_ALLOCATION_PROBE_NOINLINE void operator delete(void* ptr, std::align_val_t) noexcept {
    allocation_probe::aligned_deallocate(ptr);
}

HY_ALLOCATION_PROBE_NOINLINE void operator delete[](void* ptr, std::align_val_t) noexcept {
    allocation_probe::aligned_deallocate(ptr);
}

HY_ALLOCATION_PROBE_NOINLINE void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
    allocation_probe::aligned_deallocate(ptr);
}

HY_ALLOCATION_PROBE_NOINLINE void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
    allocation_probe::aligned_deallocate(ptr);
}

HY_ALLOCATION_PROBE_NOINLINE void* operator new(
    std::size_t size, const std::nothrow_t&) noexcept {
    allocation_probe::record();
    if (allocation_probe::fail) return nullptr;
    return std::malloc(size == 0 ? 1 : size);
}

HY_ALLOCATION_PROBE_NOINLINE void* operator new[](
    std::size_t size, const std::nothrow_t&) noexcept {
    return ::operator new(size, std::nothrow);
}

HY_ALLOCATION_PROBE_NOINLINE void operator delete(
    void* ptr, const std::nothrow_t&) noexcept {
    std::free(ptr);
}

HY_ALLOCATION_PROBE_NOINLINE void operator delete[](
    void* ptr, const std::nothrow_t&) noexcept {
    std::free(ptr);
}

HY_ALLOCATION_PROBE_NOINLINE void* operator new(
    std::size_t size, std::align_val_t alignment,
    const std::nothrow_t&) noexcept {
    allocation_probe::record();
    if (allocation_probe::fail) return nullptr;
    return allocation_probe::aligned_allocate(
        size, static_cast<std::size_t>(alignment));
}

HY_ALLOCATION_PROBE_NOINLINE void* operator new[](
    std::size_t size, std::align_val_t alignment,
    const std::nothrow_t&) noexcept {
    return ::operator new(size, alignment, std::nothrow);
}

HY_ALLOCATION_PROBE_NOINLINE void operator delete(
    void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
    allocation_probe::aligned_deallocate(ptr);
}

HY_ALLOCATION_PROBE_NOINLINE void operator delete[](
    void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
    allocation_probe::aligned_deallocate(ptr);
}

#undef HY_ALLOCATION_PROBE_NOINLINE

using namespace hy;

namespace {

class ScopedAllocationCounter {
public:
    ScopedAllocationCounter() noexcept {
        allocation_probe::allocation_count.store(0, std::memory_order_relaxed);
        allocation_probe::enabled = true;
    }

    ~ScopedAllocationCounter() {
        allocation_probe::enabled = false;
    }

    ScopedAllocationCounter(const ScopedAllocationCounter&) = delete;
    ScopedAllocationCounter& operator=(const ScopedAllocationCounter&) = delete;

    std::size_t stop() noexcept {
        allocation_probe::enabled = false;
        return allocation_probe::allocation_count.load(std::memory_order_relaxed);
    }
};

class ScopedAllocationFailure {
public:
    ScopedAllocationFailure() noexcept {
        allocation_probe::fail = true;
    }

    ~ScopedAllocationFailure() {
        allocation_probe::fail = false;
    }

    ScopedAllocationFailure(const ScopedAllocationFailure&) = delete;
    ScopedAllocationFailure& operator=(const ScopedAllocationFailure&) = delete;
};

constexpr std::uint64_t kCandidateId = 42;
constexpr std::uint32_t kKeyId = 7;

struct MockSealJournalStoreLease {
    SealJournalLeaseIoOutcome next_outcome{SealJournalLeaseIoOutcome::Ok};
    seal_journal_store_detail::ReadFixedStatus next_read_status{
        seal_journal_store_detail::ReadFixedStatus::Ok};
    std::map<std::uint64_t, std::vector<std::byte>> canned_files;

    SealJournalLeaseReadResult read_seal_journal_commit_watermark(
        std::uint64_t candidate_id,
        std::span<std::byte, kSealJournalCommitWatermarkWireBytes> out) noexcept {
        return fill(candidate_id, out);
    }

    SealJournalLeaseReadResult read_seal_journal_tombstone(
        std::uint64_t candidate_id, std::uint64_t journal_seq,
        std::span<std::byte, kSealJournalTombstoneBytes> out) noexcept {
        return fill(candidate_id * 1'000'000ULL + journal_seq, out);
    }

private:
    template <std::size_t N>
    SealJournalLeaseReadResult fill(std::uint64_t key, std::span<std::byte, N> out) noexcept {
        SealJournalLeaseReadResult result{};
        result.outcome = next_outcome;
        if (next_outcome != SealJournalLeaseIoOutcome::Ok) return result;

        const auto it = canned_files.find(key);
        if (it == canned_files.end()) {
            result.read.status = seal_journal_store_detail::ReadFixedStatus::NotFound;
            return result;
        }

        result.read.status = next_read_status;
        if (next_read_status == seal_journal_store_detail::ReadFixedStatus::Ok) {
            const std::size_t n = std::min(out.size(), it->second.size());
            std::memcpy(out.data(), it->second.data(), n);
        }
        return result;
    }
};

std::array<std::byte, kKekSize> make_kek(std::uint8_t seed) {
    std::array<std::byte, kKekSize> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
    }
    return key;
}

std::array<std::byte, kKeyBlockSize> make_signing_key(std::uint8_t seed) {
    std::array<std::byte, kKeyBlockSize> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i * 3U));
    }
    return key;
}

void fill_bytes(std::uint8_t (&out)[32], std::uint8_t seed) {
    for (std::size_t i = 0; i < std::size(out); ++i) {
        out[i] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(i));
    }
}

SealJournalCommitWatermark make_watermark(std::uint32_t key_id = kKeyId) {
    SealJournalCommitWatermark value{};
    value.store_uuid_lo = 0x1111111111111111ULL;
    value.store_uuid_hi = 0x2222222222222222ULL;
    value.candidate_id = kCandidateId;
    value.highest_committed_journal_seq = 5;
    value.kek_key_id = key_id;
    return value;
}

SealJournalTombstoneWire make_tombstone(std::uint64_t journal_seq,
                                        std::uint32_t key_id = kKeyId) {
    SealJournalTombstoneWire value{};
    value.format_version = kSealJournalTombstoneFormatVersion;
    value.total_bytes = static_cast<std::uint32_t>(kSealJournalTombstoneBytes);
    value.store_uuid_lo = 0x1111111111111111ULL;
    value.store_uuid_hi = 0x2222222222222222ULL;
    value.kek_key_id = key_id;
    value.candidate_id = kCandidateId;
    value.journal_seq = journal_seq;
    fill_bytes(value.entry_mac, static_cast<std::uint8_t>(0x20U + journal_seq));
    return value;
}

SealJournalCommitWatermark make_watermark_sentinel() {
    SealJournalCommitWatermark value{};
    value.store_uuid_lo = 0xDEADBEEFDEADBEEFULL;
    value.store_uuid_hi = 0xCAFEBABECAFEBABEULL;
    value.candidate_id = 0x1111222233334444ULL;
    value.highest_committed_journal_seq = 0x5555666677778888ULL;
    value.kek_key_id = 0xA5A5A5A5U;
    fill_bytes(value.mac, 0x80);
    return value;
}

SealJournalTombstoneWire make_tombstone_sentinel() {
    SealJournalTombstoneWire value{};
    value.format_version = 0xA5A5A5A5U;
    value.total_bytes = 0x5A5A5A5AU;
    value.store_uuid_lo = 0xDEADBEEFDEADBEEFULL;
    value.store_uuid_hi = 0xCAFEBABECAFEBABEULL;
    value.kek_key_id = 0x11223344U;
    value.candidate_id = 0x1111222233334444ULL;
    value.journal_seq = 0x5555666677778888ULL;
    fill_bytes(value.entry_mac, 0x40);
    fill_bytes(value.mac, 0x80);
    return value;
}

bool watermark_equal(const SealJournalCommitWatermark& lhs,
                     const SealJournalCommitWatermark& rhs) {
    return lhs.store_uuid_lo == rhs.store_uuid_lo &&
           lhs.store_uuid_hi == rhs.store_uuid_hi &&
           lhs.candidate_id == rhs.candidate_id &&
           lhs.highest_committed_journal_seq == rhs.highest_committed_journal_seq &&
           lhs.kek_key_id == rhs.kek_key_id &&
           std::memcmp(lhs.mac, rhs.mac, sizeof(lhs.mac)) == 0;
}

bool tombstone_equal(const SealJournalTombstoneWire& lhs,
                     const SealJournalTombstoneWire& rhs) {
    return lhs.format_version == rhs.format_version &&
           lhs.total_bytes == rhs.total_bytes &&
           lhs.store_uuid_lo == rhs.store_uuid_lo &&
           lhs.store_uuid_hi == rhs.store_uuid_hi &&
           lhs.kek_key_id == rhs.kek_key_id &&
           lhs.candidate_id == rhs.candidate_id &&
           lhs.journal_seq == rhs.journal_seq &&
           std::memcmp(lhs.entry_mac, rhs.entry_mac, sizeof(lhs.entry_mac)) == 0 &&
           std::memcmp(lhs.mac, rhs.mac, sizeof(lhs.mac)) == 0;
}

bool is_load_status(SealJournalStoreLoadStatus status) noexcept {
    switch (status) {
        case SealJournalStoreLoadStatus::Ok:
        case SealJournalStoreLoadStatus::NotFound:
        case SealJournalStoreLoadStatus::Corrupt:
        case SealJournalStoreLoadStatus::LeaseNotHeld:
        case SealJournalStoreLoadStatus::StoreDirFenced:
        case SealJournalStoreLoadStatus::DirectoryIdentityChanged:
        case SealJournalStoreLoadStatus::KeyNotFound:
        case SealJournalStoreLoadStatus::IoError:
            return true;
    }
    return false;
}

struct Fixture {
    Fixture() : kek(make_kek(0xA0)), signing_key(make_signing_key(0x31)), key_ring(kek) {
        WrappedKeyRecord record{};
        EXPECT_EQ(key_ring.add_key(kKeyId, signing_key, record), KeyRingAddStatus::Ok);
    }

    std::vector<std::byte> encode(const SealJournalCommitWatermark& value) const {
        std::array<std::byte, kSealJournalCommitWatermarkWireBytes> bytes{};
        EXPECT_EQ(encode_seal_journal_commit_watermark_wire(bytes, value, signing_key),
                  bytes.size());
        return {bytes.begin(), bytes.end()};
    }

    std::vector<std::byte> encode(const SealJournalTombstoneWire& value) const {
        std::array<std::byte, kSealJournalTombstoneBytes> bytes{};
        EXPECT_EQ(encode_seal_journal_tombstone_wire(bytes, value, signing_key),
                  bytes.size());
        return {bytes.begin(), bytes.end()};
    }

    std::array<std::byte, kKekSize> kek;
    std::array<std::byte, kKeyBlockSize> signing_key;
    KeyRing key_ring;
};

SealJournalStoreLoadStatus load_watermark(MockSealJournalStoreLease& lease, Fixture& fixture,
                          SealJournalCommitWatermark& out) {
    return seal_journal_commit_tombstone_loader_detail::load_commit_watermark(
        lease, kCandidateId, fixture.key_ring, out);
}

SealJournalStoreLoadStatus load_tombstone(MockSealJournalStoreLease& lease, Fixture& fixture,
                          std::uint64_t journal_seq, SealJournalTombstoneWire& out) {
    return seal_journal_commit_tombstone_loader_detail::load_tombstone(
        lease, kCandidateId, journal_seq, fixture.key_ring, out);
}

std::filesystem::path make_temp_store_dir() {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("hy_seal_journal_loader_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    return path;
}

void write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(stream.good());
}

void write_u32_le(std::vector<std::byte>& bytes, std::size_t offset,
                  std::uint32_t value) {
    ASSERT_GE(bytes.size(), offset + 4U);
    for (std::size_t i = 0; i < 4U; ++i) {
        bytes[offset + i] =
            static_cast<std::byte>((value >> static_cast<unsigned int>(i * 8U)) & 0xFFU);
    }
}

}  // namespace

TEST(SealJournalCommitWatermarkLoader, RoundTripCopiesVerifiedValue) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    const auto expected = make_watermark();
    const auto encoded = fixture.encode(expected);
    lease.canned_files[kCandidateId] = encoded;

    auto out = make_watermark_sentinel();
    ASSERT_EQ(load_watermark(lease, fixture, out), SealJournalStoreLoadStatus::Ok);
    EXPECT_EQ(out.store_uuid_lo, expected.store_uuid_lo);
    EXPECT_EQ(out.store_uuid_hi, expected.store_uuid_hi);
    EXPECT_EQ(out.candidate_id, expected.candidate_id);
    EXPECT_EQ(out.highest_committed_journal_seq,
              expected.highest_committed_journal_seq);
    EXPECT_EQ(out.kek_key_id, expected.kek_key_id);
    EXPECT_EQ(std::memcmp(out.mac, encoded.data() + 36, sizeof(out.mac)), 0);
}

TEST(SealJournalCommitWatermarkLoader, StoreDirFencedMapsExactly) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    lease.next_outcome = SealJournalLeaseIoOutcome::StoreDirFenced;
    auto out = make_watermark_sentinel();
    const auto sentinel = out;
    EXPECT_EQ(load_watermark(lease, fixture, out), SealJournalStoreLoadStatus::StoreDirFenced);
    EXPECT_TRUE(watermark_equal(out, sentinel));
}

TEST(SealJournalCommitWatermarkLoader, DirectoryIdentityChangedMapsExactly) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    lease.next_outcome = SealJournalLeaseIoOutcome::DirectoryIdentityChanged;
    auto out = make_watermark_sentinel();
    const auto sentinel = out;
    EXPECT_EQ(load_watermark(lease, fixture, out),
              SealJournalStoreLoadStatus::DirectoryIdentityChanged);
    EXPECT_TRUE(watermark_equal(out, sentinel));
}

TEST(SealJournalCommitWatermarkLoader, WrongOwnerAndNotHeldMapToLeaseNotHeld) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    auto out = make_watermark_sentinel();
    const auto sentinel = out;

    lease.next_outcome = SealJournalLeaseIoOutcome::WrongOwner;
    EXPECT_EQ(load_watermark(lease, fixture, out), SealJournalStoreLoadStatus::LeaseNotHeld);
    EXPECT_TRUE(watermark_equal(out, sentinel));
    lease.next_outcome = SealJournalLeaseIoOutcome::NotHeld;
    EXPECT_EQ(load_watermark(lease, fixture, out), SealJournalStoreLoadStatus::LeaseNotHeld);
    EXPECT_TRUE(watermark_equal(out, sentinel));
}

TEST(SealJournalCommitWatermarkLoader, NotFoundMapsExactly) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    auto out = make_watermark_sentinel();
    const auto sentinel = out;
    EXPECT_EQ(load_watermark(lease, fixture, out), SealJournalStoreLoadStatus::NotFound);
    EXPECT_TRUE(watermark_equal(out, sentinel));
}

TEST(SealJournalCommitWatermarkLoader, WrongSizeAndNotRegularMapToCorrupt) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    lease.canned_files[kCandidateId] = {std::byte{0x01}};
    auto out = make_watermark_sentinel();
    const auto sentinel = out;

    lease.next_read_status = seal_journal_store_detail::ReadFixedStatus::WrongSize;
    EXPECT_EQ(load_watermark(lease, fixture, out), SealJournalStoreLoadStatus::Corrupt);
    EXPECT_TRUE(watermark_equal(out, sentinel));
    lease.next_read_status =
        seal_journal_store_detail::ReadFixedStatus::NotRegularFile;
    EXPECT_EQ(load_watermark(lease, fixture, out), SealJournalStoreLoadStatus::Corrupt);
    EXPECT_TRUE(watermark_equal(out, sentinel));
}

TEST(SealJournalCommitWatermarkLoader, IoErrorMapsExactly) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    lease.canned_files[kCandidateId] = {std::byte{0x01}};
    lease.next_read_status = seal_journal_store_detail::ReadFixedStatus::IoError;
    auto out = make_watermark_sentinel();
    const auto sentinel = out;
    EXPECT_EQ(load_watermark(lease, fixture, out), SealJournalStoreLoadStatus::IoError);
    EXPECT_TRUE(watermark_equal(out, sentinel));
}

TEST(SealJournalCommitWatermarkLoader, MacTamperMapsToCorrupt) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    lease.canned_files[kCandidateId] = fixture.encode(make_watermark());
    lease.canned_files[kCandidateId].back() ^= std::byte{0x01};
    auto out = make_watermark_sentinel();
    const auto sentinel = out;
    EXPECT_EQ(load_watermark(lease, fixture, out), SealJournalStoreLoadStatus::Corrupt);
    EXPECT_TRUE(watermark_equal(out, sentinel));
}

TEST(SealJournalCommitWatermarkLoader, UnknownKeyIdMapsToKeyNotFound) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    lease.canned_files[kCandidateId] = fixture.encode(make_watermark(999));
    auto out = make_watermark_sentinel();
    const auto sentinel = out;
    EXPECT_EQ(load_watermark(lease, fixture, out), SealJournalStoreLoadStatus::KeyNotFound);
    EXPECT_TRUE(watermark_equal(out, sentinel));
}

TEST(SealJournalTombstoneLoader, RoundTripCopiesVerifiedValue) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    const auto expected = make_tombstone(5);
    const auto encoded = fixture.encode(expected);
    lease.canned_files[kCandidateId * 1'000'000ULL + 5] = encoded;

    auto out = make_tombstone_sentinel();
    ASSERT_EQ(load_tombstone(lease, fixture, 5, out), SealJournalStoreLoadStatus::Ok);
    EXPECT_EQ(out.format_version, expected.format_version);
    EXPECT_EQ(out.total_bytes, expected.total_bytes);
    EXPECT_EQ(out.store_uuid_lo, expected.store_uuid_lo);
    EXPECT_EQ(out.store_uuid_hi, expected.store_uuid_hi);
    EXPECT_EQ(out.kek_key_id, expected.kek_key_id);
    EXPECT_EQ(out.candidate_id, expected.candidate_id);
    EXPECT_EQ(out.journal_seq, expected.journal_seq);
    EXPECT_EQ(std::memcmp(out.entry_mac, expected.entry_mac,
                          sizeof(out.entry_mac)),
              0);
    EXPECT_EQ(std::memcmp(out.mac, encoded.data() + 76, sizeof(out.mac)), 0);
}

TEST(SealJournalTombstoneLoader, StoreDirFencedMapsExactly) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    lease.next_outcome = SealJournalLeaseIoOutcome::StoreDirFenced;
    auto out = make_tombstone_sentinel();
    const auto sentinel = out;
    EXPECT_EQ(load_tombstone(lease, fixture, 5, out),
              SealJournalStoreLoadStatus::StoreDirFenced);
    EXPECT_TRUE(tombstone_equal(out, sentinel));
}

TEST(SealJournalTombstoneLoader, DirectoryIdentityChangedMapsExactly) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    lease.next_outcome = SealJournalLeaseIoOutcome::DirectoryIdentityChanged;
    auto out = make_tombstone_sentinel();
    const auto sentinel = out;
    EXPECT_EQ(load_tombstone(lease, fixture, 5, out),
              SealJournalStoreLoadStatus::DirectoryIdentityChanged);
    EXPECT_TRUE(tombstone_equal(out, sentinel));
}

TEST(SealJournalTombstoneLoader, WrongOwnerAndNotHeldMapToLeaseNotHeld) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    auto out = make_tombstone_sentinel();
    const auto sentinel = out;

    lease.next_outcome = SealJournalLeaseIoOutcome::WrongOwner;
    EXPECT_EQ(load_tombstone(lease, fixture, 5, out),
              SealJournalStoreLoadStatus::LeaseNotHeld);
    EXPECT_TRUE(tombstone_equal(out, sentinel));
    lease.next_outcome = SealJournalLeaseIoOutcome::NotHeld;
    EXPECT_EQ(load_tombstone(lease, fixture, 5, out),
              SealJournalStoreLoadStatus::LeaseNotHeld);
    EXPECT_TRUE(tombstone_equal(out, sentinel));
}

TEST(SealJournalTombstoneLoader, NotFoundMapsExactly) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    auto out = make_tombstone_sentinel();
    const auto sentinel = out;
    EXPECT_EQ(load_tombstone(lease, fixture, 5, out), SealJournalStoreLoadStatus::NotFound);
    EXPECT_TRUE(tombstone_equal(out, sentinel));
}

TEST(SealJournalTombstoneLoader, WrongSizeAndNotRegularMapToCorrupt) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    lease.canned_files[kCandidateId * 1'000'000ULL + 5] = {
        std::byte{0x01}};
    auto out = make_tombstone_sentinel();
    const auto sentinel = out;

    lease.next_read_status = seal_journal_store_detail::ReadFixedStatus::WrongSize;
    EXPECT_EQ(load_tombstone(lease, fixture, 5, out), SealJournalStoreLoadStatus::Corrupt);
    EXPECT_TRUE(tombstone_equal(out, sentinel));
    lease.next_read_status =
        seal_journal_store_detail::ReadFixedStatus::NotRegularFile;
    EXPECT_EQ(load_tombstone(lease, fixture, 5, out), SealJournalStoreLoadStatus::Corrupt);
    EXPECT_TRUE(tombstone_equal(out, sentinel));
}

TEST(SealJournalTombstoneLoader, IoErrorMapsExactly) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    lease.canned_files[kCandidateId * 1'000'000ULL + 5] = {
        std::byte{0x01}};
    lease.next_read_status = seal_journal_store_detail::ReadFixedStatus::IoError;
    auto out = make_tombstone_sentinel();
    const auto sentinel = out;
    EXPECT_EQ(load_tombstone(lease, fixture, 5, out), SealJournalStoreLoadStatus::IoError);
    EXPECT_TRUE(tombstone_equal(out, sentinel));
}

TEST(SealJournalTombstoneLoader, MacTamperMapsToCorrupt) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    const std::uint64_t key = kCandidateId * 1'000'000ULL + 5;
    lease.canned_files[key] = fixture.encode(make_tombstone(5));
    lease.canned_files[key].back() ^= std::byte{0x01};
    auto out = make_tombstone_sentinel();
    const auto sentinel = out;
    EXPECT_EQ(load_tombstone(lease, fixture, 5, out), SealJournalStoreLoadStatus::Corrupt);
    EXPECT_TRUE(tombstone_equal(out, sentinel));
}

TEST(SealJournalTombstoneLoader, UnknownKeyIdMapsToKeyNotFound) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    lease.canned_files[kCandidateId * 1'000'000ULL + 5] =
        fixture.encode(make_tombstone(5, 999));
    auto out = make_tombstone_sentinel();
    const auto sentinel = out;
    EXPECT_EQ(load_tombstone(lease, fixture, 5, out),
              SealJournalStoreLoadStatus::KeyNotFound);
    EXPECT_TRUE(tombstone_equal(out, sentinel));
}

TEST(SealJournalTombstoneLoaderScan, ZeroWatermarkReturnsEmptyOk) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    const auto result =
        seal_journal_commit_tombstone_loader_detail::scan_up_to_watermark(
            lease, kCandidateId, 0, fixture.key_ring);
    EXPECT_EQ(result.status, SealJournalStoreLoadStatus::Ok);
    EXPECT_TRUE(result.found.empty());
}

TEST(SealJournalTombstoneLoaderScan, SparseFilesSkipNotFoundAndRemainOrdered) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    lease.canned_files[kCandidateId * 1'000'000ULL + 2] =
        fixture.encode(make_tombstone(2));
    lease.canned_files[kCandidateId * 1'000'000ULL + 5] =
        fixture.encode(make_tombstone(5));

    const auto result =
        seal_journal_commit_tombstone_loader_detail::scan_up_to_watermark(
            lease, kCandidateId, 5, fixture.key_ring);
    ASSERT_EQ(result.status, SealJournalStoreLoadStatus::Ok);
    ASSERT_EQ(result.found.size(), 2U);
    EXPECT_EQ(result.found[0].journal_seq, 2U);
    EXPECT_EQ(result.found[1].journal_seq, 5U);
}

TEST(SealJournalTombstoneLoaderScan, CorruptStopsAndReturnsPartialPrefix) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
        lease.canned_files[kCandidateId * 1'000'000ULL + seq] =
            fixture.encode(make_tombstone(seq));
    }
    lease.canned_files[kCandidateId * 1'000'000ULL + 3].back() ^=
        std::byte{0x01};

    const auto result =
        seal_journal_commit_tombstone_loader_detail::scan_up_to_watermark(
            lease, kCandidateId, 5, fixture.key_ring);
    ASSERT_EQ(result.status, SealJournalStoreLoadStatus::Corrupt);
    ASSERT_EQ(result.found.size(), 2U);
    EXPECT_EQ(result.found[0].journal_seq, 1U);
    EXPECT_EQ(result.found[1].journal_seq, 2U);
}

TEST(SealJournalCommitTombstoneLoaderProperty,
     RandomBytesNeverCrashAndAlwaysReturnClosedStatus) {
    Fixture fixture;
    MockSealJournalStoreLease lease;
    std::mt19937 random(0x5EA1C0DEU);
    std::uniform_int_distribution<int> byte_distribution(0, 255);

    for (std::size_t iteration = 0; iteration < 2000U; ++iteration) {
        std::vector<std::byte> watermark_bytes(
            kSealJournalCommitWatermarkWireBytes);
        std::vector<std::byte> tombstone_bytes(kSealJournalTombstoneBytes);
        for (auto& byte : watermark_bytes) {
            byte = static_cast<std::byte>(byte_distribution(random));
        }
        for (auto& byte : tombstone_bytes) {
            byte = static_cast<std::byte>(byte_distribution(random));
        }
        // Force the unauthenticated key selector to a present key so the
        // random payload reaches the decoder instead of stopping at pin_key.
        write_u32_le(watermark_bytes, 32, kKeyId);
        write_u32_le(tombstone_bytes, 24, kKeyId);
        lease.canned_files[kCandidateId] = std::move(watermark_bytes);
        lease.canned_files[kCandidateId * 1'000'000ULL + 5] =
            std::move(tombstone_bytes);

        auto watermark_out = make_watermark_sentinel();
        const auto watermark_sentinel = watermark_out;
        const SealJournalStoreLoadStatus watermark_status =
            load_watermark(lease, fixture, watermark_out);
        ASSERT_TRUE(is_load_status(watermark_status))
            << "watermark iteration=" << iteration;
        if (watermark_status != SealJournalStoreLoadStatus::Ok) {
            EXPECT_TRUE(watermark_equal(watermark_out, watermark_sentinel))
                << "watermark iteration=" << iteration;
        }

        auto tombstone_out = make_tombstone_sentinel();
        const auto tombstone_sentinel = tombstone_out;
        const SealJournalStoreLoadStatus tombstone_status =
            load_tombstone(lease, fixture, 5, tombstone_out);
        ASSERT_TRUE(is_load_status(tombstone_status))
            << "tombstone iteration=" << iteration;
        if (tombstone_status != SealJournalStoreLoadStatus::Ok) {
            EXPECT_TRUE(tombstone_equal(tombstone_out, tombstone_sentinel))
                << "tombstone iteration=" << iteration;
        }
    }
}

TEST(SealJournalCommitTombstoneLoaderIntegration,
     ProductionLeaseReadsTypedNamesWithoutHeapAllocation) {
    Fixture fixture;
    const auto store_dir = make_temp_store_dir();
    const auto watermark_bytes = fixture.encode(make_watermark());
    const auto tombstone_bytes = fixture.encode(make_tombstone(5));
    write_bytes(store_dir / "000000000000002a.jhw", watermark_bytes);
    write_bytes(store_dir / "000000000000002a-0000000000000005.jts",
                tombstone_bytes);

    SealJournalStoreLease lease(store_dir);
    ASSERT_EQ(lease.acquire(), SealJournalLeaseAcquireStatus::Acquired);
    SealJournalCommitWatermarkLoader watermark_loader(lease);
    SealJournalTombstoneLoader tombstone_loader(lease);
    SealJournalCommitWatermark watermark_out{};
    SealJournalTombstoneWire tombstone_out{};
    SealJournalStoreLoadStatus watermark_status{};
    std::size_t watermark_allocations = 0;
    {
        ScopedAllocationCounter counter;
        watermark_status =
            watermark_loader.load(kCandidateId, fixture.key_ring, watermark_out);
        watermark_allocations = counter.stop();
    }
    EXPECT_EQ(watermark_status, SealJournalStoreLoadStatus::Ok);
    EXPECT_EQ(watermark_allocations, 0U);

    SealJournalStoreLoadStatus tombstone_status{};
    std::size_t tombstone_allocations = 0;
    {
        ScopedAllocationCounter counter;
        tombstone_status =
            tombstone_loader.load(kCandidateId, 5, fixture.key_ring, tombstone_out);
        tombstone_allocations = counter.stop();
    }
    EXPECT_EQ(tombstone_status, SealJournalStoreLoadStatus::Ok);
    EXPECT_EQ(tombstone_allocations, 0U);
    EXPECT_EQ(watermark_out.candidate_id, kCandidateId);
    EXPECT_EQ(tombstone_out.candidate_id, kCandidateId);
    EXPECT_EQ(tombstone_out.journal_seq, 5U);

    SealJournalTombstoneLoader::ScanResult allocation_failure_scan;
    {
        ScopedAllocationFailure fail_allocations;
        allocation_failure_scan = tombstone_loader.scan_up_to_watermark(
            kCandidateId, watermark_out.highest_committed_journal_seq,
            fixture.key_ring);
    }
    EXPECT_EQ(allocation_failure_scan.status,
              SealJournalStoreLoadStatus::IoError);
    EXPECT_TRUE(allocation_failure_scan.found.empty());

    const auto scan = tombstone_loader.scan_up_to_watermark(
        kCandidateId, watermark_out.highest_committed_journal_seq,
        fixture.key_ring);
    ASSERT_EQ(scan.status, SealJournalStoreLoadStatus::Ok);
    ASSERT_EQ(scan.found.size(), 1U);
    EXPECT_EQ(scan.found[0].journal_seq, 5U);
    EXPECT_EQ(fixture.key_ring.retire(kKeyId), RetireStatus::Retired)
        << "no PinnedKeyHandle may escape a load/scan call";
    ASSERT_EQ(lease.release(), SealJournalLeaseReleaseStatus::Released);
    std::filesystem::remove_all(store_dir);
}

TEST(SealJournalCommitTombstoneLoaderSurface, ConstructorsAreExactAndNoexcept) {
    static_assert(std::is_constructible_v<SealJournalCommitWatermarkLoader,
                                          SealJournalStoreLease&>);
    static_assert(std::is_nothrow_constructible_v<
                  SealJournalCommitWatermarkLoader, SealJournalStoreLease&>);
    static_assert(std::is_constructible_v<SealJournalTombstoneLoader,
                                          SealJournalStoreLease&>);
    static_assert(std::is_nothrow_constructible_v<SealJournalTombstoneLoader,
                                                  SealJournalStoreLease&>);
    static_assert(noexcept(std::declval<SealJournalCommitWatermarkLoader&>().load(
        std::declval<std::uint64_t>(), std::declval<KeyRing&>(),
        std::declval<SealJournalCommitWatermark&>())));
    static_assert(noexcept(std::declval<SealJournalTombstoneLoader&>().load(
        std::declval<std::uint64_t>(), std::declval<std::uint64_t>(),
        std::declval<KeyRing&>(), std::declval<SealJournalTombstoneWire&>())));
    static_assert(noexcept(
        std::declval<SealJournalTombstoneLoader&>().scan_up_to_watermark(
            std::declval<std::uint64_t>(), std::declval<std::uint64_t>(),
            std::declval<KeyRing&>())));
}
