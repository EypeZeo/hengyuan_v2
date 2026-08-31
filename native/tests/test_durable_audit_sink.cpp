// Real-file tests for durable_audit_sink.hpp's DurableAuditSink. Every test
// in this file touches an actual temp file on disk -- this is what actually
// exercises append_durable()'s fsync/tip-anchor/lock machinery and
// recovery_scan()'s real torn-write/corruption handling, neither of which
// test_durable_frame_codec.cpp's pure in-memory tests can reach.
//
// A "process restart" is simulated by destroying one DurableAuditSink
// instance and constructing a fresh one over the same path -- recovery
// happens automatically in the constructor, exactly like a real restart.
//
// Phase 2 (docs/SPEC_INVARIANTS.md): DurableAuditSink now takes a KeyRing&
// + active_key_id instead of a raw key -- the fixture builds a KeyRing with
// key_id=1 loaded, mirroring test_control_plane_log_sink.cpp's own fixture.
#include <gtest/gtest.h>
#include <hengyuan/durable_audit_sink.hpp>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace hy;

namespace {

std::vector<std::byte> test_key() {
    static const char kKey[] = "durable-audit-sink-test-key";
    std::vector<std::byte> k(sizeof(kKey) - 1);
    std::memcpy(k.data(), kKey, k.size());
    return k;
}

// A key distinct from test_key(), for Phase 4 rotation tests that need a
// genuinely second key loaded into the same KeyRing.
std::vector<std::byte> distinct_key(std::uint8_t seed) {
    static const char kKey[] = "durable-audit-sink-test-key";
    std::vector<std::byte> k(sizeof(kKey) - 1);
    for (std::size_t i = 0; i < k.size(); ++i) k[i] = static_cast<std::byte>(static_cast<unsigned char>(kKey[i]) ^ seed);
    return k;
}

std::array<std::byte, kKeyBlockSize> make_plaintext_key(std::uint8_t fill) {
    std::array<std::byte, kKeyBlockSize> key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::byte>(fill + i);
    return key;
}

AuditRecord make_intent(const char* coid, std::int64_t price, std::int64_t qty, std::uint32_t symbol_id) {
    AuditRecord rec{};
    rec.timestamp_ms = 1000;
    rec.event_type = AuditEventType::OrderIntentCreated;
    rec.mode = ExecutionMode::Live;
    rec.symbol_id = symbol_id;
    rec.set_client_order_id(coid);
    rec.price_ticks = price;
    rec.qty_ticks = qty;
    rec.resulting_state = OrderState::Intent;
    return rec;
}

AuditRecord make_submitted(const char* coid, std::int64_t price, std::int64_t qty, std::uint32_t symbol_id) {
    AuditRecord rec{};
    rec.timestamp_ms = 1001;
    rec.event_type = AuditEventType::OrderSubmitted;
    rec.mode = ExecutionMode::Live;
    rec.symbol_id = symbol_id;
    rec.set_client_order_id(coid);
    rec.price_ticks = price;
    rec.qty_ticks = qty;
    rec.resulting_state = OrderState::Submitting;
    return rec;
}

AuditRecord make_accepted(const char* coid, std::uint32_t symbol_id, std::int64_t exch_id) {
    AuditRecord rec{};
    rec.timestamp_ms = 1002;
    rec.event_type = AuditEventType::OrderAccepted;
    rec.mode = ExecutionMode::Live;
    rec.symbol_id = symbol_id;
    rec.set_client_order_id(coid);
    rec.exchange_order_id = exch_id;
    rec.resulting_state = OrderState::Accepted;
    return rec;
}

AuditRecord make_filled(const char* coid, std::uint32_t symbol_id, std::int64_t exch_id, std::int64_t filled_qty,
                         std::int64_t avg_price) {
    AuditRecord rec{};
    rec.timestamp_ms = 1003;
    rec.event_type = AuditEventType::OrderReconciled;
    rec.mode = ExecutionMode::Live;
    rec.symbol_id = symbol_id;
    rec.set_client_order_id(coid);
    rec.exchange_order_id = exch_id;
    rec.resulting_state = OrderState::Filled;
    rec.filled_qty_ticks = filled_qty;
    rec.avg_fill_price_ticks = avg_price;
    return rec;
}

constexpr std::array<std::byte, kMacLen> kZeroMac{};

}  // namespace

class DurableAuditSinkTest : public ::testing::Test {
protected:
    std::string base_path_;
    std::unique_ptr<KeyRing> key_ring_;

    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        base_path_ = std::string(tmp) + "hy_das_" + std::to_string(GetCurrentProcessId()) + "_" +
                     std::to_string(counter) + ".log";
#else
        base_path_ =
            "/tmp/hy_das_" + std::to_string(getpid()) + "_" + std::to_string(counter) + ".log";
#endif
        remove_all();

        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x20 + i);
        key_ring_ = std::make_unique<KeyRing>(kek);

        WrappedKeyRecord rec{};
        ASSERT_EQ(key_ring_->add_key(1, test_key(), rec), KeyRingAddStatus::Ok);
    }

    void TearDown() override { remove_all(); }

    void remove_all() {
        std::remove(base_path_.c_str());
        std::remove((base_path_ + ".lock").c_str());
        std::remove((base_path_ + ".tip").c_str());
        std::remove((base_path_ + ".tip.tmp").c_str());
        // Phase 4: rotate_active_key()'s KeyRotated sidecar.
        std::remove((base_path_ + ".keyrotations").c_str());
        std::remove((base_path_ + ".keyrotations.lock").c_str());
        std::remove((base_path_ + ".keyrotations.tip").c_str());
        std::remove((base_path_ + ".keyrotations.tip.tmp").c_str());
        // Phase 5: store-identity sidecar.
        std::remove((base_path_ + ".storeid").c_str());
        std::remove((base_path_ + ".storeid.lock").c_str());
        std::remove((base_path_ + ".storeid.tip").c_str());
        std::remove((base_path_ + ".storeid.tip.tmp").c_str());
    }

    // Overwrites `n` bytes at `offset` in the log file with garbage, for
    // corruption tests -- flips each targeted byte's low bit.
    void corrupt_log_byte(std::size_t offset) {
        std::fstream f(base_path_, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(f.is_open());
        f.seekg(static_cast<std::streamoff>(offset));
        char b = 0;
        f.read(&b, 1);
        b = static_cast<char>(b ^ 0x01);
        f.seekp(static_cast<std::streamoff>(offset));
        f.write(&b, 1);
    }

    // Simulates a real crash mid-append: a PREFIX of a genuine, correctly-
    // encoded frame lands in the log (as if a second append_durable() call's
    // single write() started laying down real bytes but the process/power
    // was cut before it completed), WITHOUT going through append_durable()
    // itself -- so the tip anchor is never touched, exactly matching what a
    // real crash leaves behind (the anchor only ever advances AFTER a log
    // fsync + its own fsync both succeed). This is deliberately NOT random
    // garbage bytes: a real interrupted write's surviving bytes are a
    // coherent prefix of the real frame (same version/record_type/sequence
    // bytes, since those are written first), which is what actually lets
    // recovery_scan() recognize it as "too short" (Truncated) rather than
    // "doesn't even parse as a frame" (Corrupt) -- those are different, both
    // real, failure modes; this test is specifically about the former.
    // Appending fully-valid frames and truncating afterward is NOT
    // equivalent either -- that leaves the anchor pointing past what's
    // readable, indistinguishable from a deliberate tail-deletion attack,
    // correctly Corrupt (see AnchorAheadOfTruncatedLogIsCorruptTailDeletion).
    void append_prefix_of_a_real_frame(const AuditRecord& rec, std::uint64_t sequence_number,
                                        std::array<std::byte, kMacLen> prev_mac, std::size_t prefix_len) {
        std::array<std::byte, kOrderEventFrameSize> full_frame{};
        std::array<std::byte, kKeyBlockSize> key_block{};
        ASSERT_TRUE(key_ring_->active_key(1, key_block));
        std::span<const std::byte> key(key_block.data(), key_block.size());
        auto n = encode_order_event_frame(full_frame, /*key_id=*/1u, sequence_number,
                                           FrameTimeKind::ServerCorrectedUtc,
                                           2000, rec, prev_mac, key);
        ASSERT_EQ(n, kOrderEventFrameSize);
        ASSERT_LE(prefix_len, kOrderEventFrameSize);

        std::ofstream f(base_path_, std::ios::binary | std::ios::app);
        ASSERT_TRUE(f.is_open());
        f.write(reinterpret_cast<const char*>(full_frame.data()), static_cast<std::streamsize>(prefix_len));
    }

    void truncate_log_to(std::size_t new_size) {
#ifdef _WIN32
        // std::filesystem::resize_file avoided (project convention here
        // favors raw handles for file ops it actually needs, see
        // env_loader.hpp) -- but truncation to a SMALLER size via a fresh
        // read+rewrite of the first new_size bytes is simplest/portable.
#endif
        std::vector<char> content;
        {
            std::ifstream in(base_path_, std::ios::binary);
            content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        ASSERT_LE(new_size, content.size());
        std::ofstream out(base_path_, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(new_size));
    }

    std::size_t log_file_size() const {
        std::ifstream f(base_path_, std::ios::binary | std::ios::ate);
        return static_cast<std::size_t>(f.tellg());
    }
};

// --- Fresh store / basic lifecycle ---

TEST_F(DurableAuditSinkTest, FreshStoreIsClean) {
    DurableAuditSink sink(base_path_, *key_ring_, 1);
    ASSERT_TRUE(sink.is_open());
    EXPECT_FALSE(sink.fenced());
    EXPECT_EQ(sink.recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_TRUE(sink.recovered_checkpoints().empty());
}

TEST_F(DurableAuditSinkTest, AppendSucceedsAndAcksIncrementingSequence) {
    DurableAuditSink sink(base_path_, *key_ring_, 1);
    ASSERT_TRUE(sink.is_open());

    auto r1 = sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000);
    EXPECT_TRUE(r1.acked());
    EXPECT_EQ(r1.sequence, 0u);

    auto r2 = sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001);
    EXPECT_TRUE(r2.acked());
    EXPECT_EQ(r2.sequence, 1u);

    EXPECT_GT(log_file_size(), 0u);
}

TEST_F(DurableAuditSinkTest, ExchangeFinalOrderNeedsNoRecoveryAfterRestart) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-A", 1, 555), 1002).acked());
        ASSERT_TRUE(sink.append_durable(make_filled("HY-A", 1, 555, 10, 100), 1003).acked());
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_TRUE(restarted.recovered_checkpoints().empty())
        << "Filled is exchange-final -- nothing should need recovering";
}

TEST_F(DurableAuditSinkTest, DanglingSubmittingRecoversAsAmbiguous) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        // No outcome ever lands -- simulates a crash right after the POST.
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_STREQ(cps[0].client_order_id.id, "HY-A");
    EXPECT_EQ(cps[0].resulting_state, OrderState::Ambiguous)
        << "Submitting with no later outcome must remap to Ambiguous, never stay Submitting";
    EXPECT_EQ(cps[0].intended_price_ticks, 100);
    EXPECT_EQ(cps[0].intended_qty_ticks, 10);
}

TEST_F(DurableAuditSinkTest, AcceptedButUnresolvedRecoversAsAccepted) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-A", 1, 555), 1002).acked());
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].resulting_state, OrderState::Accepted);
    EXPECT_EQ(cps[0].exchange_order_id, 555);
}

TEST_F(DurableAuditSinkTest, MultipleOrdersRecoveredIndependently) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-B", 200, 20, 2), 1002).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-B", 200, 20, 2), 1003).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-B", 2, 777), 1004).acked());
        ASSERT_TRUE(sink.append_durable(make_filled("HY-B", 2, 777, 20, 200), 1005).acked());
        // HY-A stays dangling Submitting; HY-B resolves to Filled.
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u) << "only HY-A should need recovery; HY-B is exchange-final";
    EXPECT_STREQ(cps[0].client_order_id.id, "HY-A");
    EXPECT_EQ(cps[0].resulting_state, OrderState::Ambiguous);
}

// --- Torn write vs corruption (the round-6-P0 distinction) ---

TEST_F(DurableAuditSinkTest, TornTailWriteDiscardsOnlyTheIncompleteRecord) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        // Anchor now correctly points at sequence 0. Simulate a crash mid-way
        // through what would have been the NEXT append (Submitting) -- a
        // real partial frame prefix lands in the log, but nothing ever
        // touched the anchor for it, exactly like a real crash before that
        // append's own fsync.
    }
    append_prefix_of_a_real_frame(make_submitted("HY-A", 100, 10, 1), /*sequence_number=*/1, kZeroMac,
                                   /*prefix_len=*/30);

    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered)
        << "one complete frame (Intent) should remain and recover cleanly";
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].resulting_state, OrderState::Intent)
        << "only the first frame (Intent) is real; the trailing garbage must be discarded as a torn tail";
}

TEST_F(DurableAuditSinkTest, TornTailUnderSixBytesIsRecoveredNotCorrupt) {
    // Phase 2 (docs/SPEC_INVARIANTS.md): peek_frame_key_id() needs 6 bytes;
    // decode_order_event_frame()'s own Truncated threshold is 27 bytes. A
    // torn tail leaving fewer than 6 bytes must NOT be misclassified as
    // Corrupt just because the key_id peek itself failed -- it must fall
    // through to decode_order_event_frame's own length check and come back
    // Truncated, same as any other torn tail.
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    append_prefix_of_a_real_frame(make_submitted("HY-A", 100, 10, 1), /*sequence_number=*/1, kZeroMac,
                                   /*prefix_len=*/3);

    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered)
        << "a 3-byte torn tail (fewer than peek_frame_key_id's own 6-byte minimum) must still be Recovered, "
           "never misclassified as Corrupt from the key_id peek alone";
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].resulting_state, OrderState::Intent);
}

TEST_F(DurableAuditSinkTest, AnchorAheadOfTruncatedLogIsCorruptTailDeletion) {
    // Distinct from the torn-write case above: here BOTH appends genuinely
    // completed (anchor legitimately advanced to sequence 1), and the log's
    // bytes were removed AFTERWARD. This is what a tail-deletion attack (or
    // a corrupted filesystem) looks like, not a crash -- must be Corrupt,
    // never silently treated as "the second write just didn't happen."
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }
    const std::size_t full_size = log_file_size();
    truncate_log_to(full_size - 20);

    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt);
    EXPECT_TRUE(restarted.fenced());
}

TEST_F(DurableAuditSinkTest, ChecksumCorruptionMidLogIsCorruptNotSilentlySkipped) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-A", 1, 555), 1002).acked());
    }
    // Flip a byte inside the FIRST frame's payload (well before the physical
    // end of the file) -- this must be Corrupt, not treated as a torn tail.
    corrupt_log_byte(40);

    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt);
    EXPECT_TRUE(restarted.fenced()) << "a Corrupt scan must fail closed -- no appends allowed";
}

TEST_F(DurableAuditSinkTest, ChecksumCorruptionOnLastFrameIsCorruptNotTruncated) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    const std::size_t size = log_file_size();
    corrupt_log_byte(size - 1);  // last byte of the (complete) last frame's mac

    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt)
        << "a complete-but-corrupt frame at the physical end must never be treated like a torn write";
}

TEST_F(DurableAuditSinkTest, CorruptSinkRefusesAppends) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    corrupt_log_byte(40);

    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.fenced());
    auto result = restarted.append_durable(make_intent("HY-B", 1, 1, 1), 2000);
    EXPECT_FALSE(result.acked());
}

// --- Single-writer lock ---

TEST_F(DurableAuditSinkTest, SecondSinkOverSamePathFailsToOpen) {
    DurableAuditSink first(base_path_, *key_ring_, 1);
    ASSERT_TRUE(first.is_open());

    DurableAuditSink second(base_path_, *key_ring_, 1);
    EXPECT_FALSE(second.is_open()) << "the lock must be exclusive while `first` is still alive";
}

TEST_F(DurableAuditSinkTest, LockIsReleasedOnDestruction) {
    {
        DurableAuditSink first(base_path_, *key_ring_, 1);
        ASSERT_TRUE(first.is_open());
    }
    DurableAuditSink second(base_path_, *key_ring_, 1);
    EXPECT_TRUE(second.is_open()) << "the lock must be released once `first` is destroyed";
}

// --- Wrong / mismatched key ---

TEST_F(DurableAuditSinkTest, WrongKeyOnRestartIsCorrupt) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    // A fresh KeyRing that never loaded key_id 1 -- simulates an environment
    // mismatch (the key this log was signed under isn't available), same
    // idiom as ControlPlaneLogSinkTest::RecoveryWithUnresolvableFrameKeyIsCorrupt.
    std::array<std::byte, kKekSize> other_kek{};
    for (std::size_t i = 0; i < other_kek.size(); ++i) other_kek[i] = static_cast<std::byte>(0x99 + i);
    KeyRing empty_ring(other_kek);

    DurableAuditSink restarted(base_path_, empty_ring, 1);
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt);
    EXPECT_TRUE(restarted.fenced());
}

TEST_F(DurableAuditSinkTest, ActiveKeyIdMismatchWithSignedAnchorIsCorrupt) {
    // Phase 2 (docs/SPEC_INVARIANTS.md): the anchor's own key_id (1, MAC-
    // verified successfully) must equal the constructor's active_key_id_.
    // This is a narrower, more precise scenario than WrongKeyOnRestartIsCorrupt
    // above -- here the MAC verification itself SUCCEEDS (key 1 is loaded and
    // correctly resolves the anchor), but the configured identity (key 2)
    // doesn't match what actually signed the durable state, and must still
    // be rejected.
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, make_plaintext_key(0x55), rec2), KeyRingAddStatus::Ok);

    DurableAuditSink restarted(base_path_, *key_ring_, /*active_key_id=*/2);
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt);
    EXPECT_TRUE(restarted.fenced());
}

TEST_F(DurableAuditSinkTest, RestartWithGenuinelyFreshKeyRingViaPersistedWrappedRecord) {
    // Unlike every other test in this file (which reuses the same in-memory
    // key_ring_ across "before"/"after restart" blocks), this test verifies
    // the path closest to a REAL process restart: the key is recovered from
    // a persisted WrappedKeyRecord on a brand-new KeyRing instance, not a
    // reused in-memory object.
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x77 + i);

    WrappedKeyRecord persisted{};
    {
        KeyRing writer_ring(kek);
        ASSERT_EQ(writer_ring.add_key(1, test_key(), persisted), KeyRingAddStatus::Ok);

        DurableAuditSink sink(base_path_, writer_ring, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }

    // Brand-new KeyRing object, same KEK, key loaded ONLY via the persisted
    // WrappedKeyRecord -- not the same in-memory KeyRing that wrote the log.
    KeyRing reader_ring(kek);
    ASSERT_EQ(reader_ring.load_wrapped_key(persisted), KeyRingLoadStatus::Ok);

    DurableAuditSink restarted(base_path_, reader_ring, 1);
    ASSERT_TRUE(restarted.is_open());
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_STREQ(cps[0].client_order_id.id, "HY-A");
    EXPECT_EQ(cps[0].resulting_state, OrderState::Ambiguous);
}

// --- InFlightRegistry rebuild integration ---

TEST_F(DurableAuditSinkTest, RepopulateInFlightRegistryHelperRegistersEveryCheckpoint) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);

    InFlightRegistry registry;
    auto n = repopulate_in_flight_registry(registry, restarted.recovered_checkpoints());
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(registry.count(), 1u);
    EXPECT_TRUE(registry.is_in_flight("HY-A"));
}

TEST_F(DurableAuditSinkTest, CheckpointToOrderRecordPreservesAllFields) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-A", 1, 555), 1002).acked());
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);

    OrderRecord rec = checkpoint_to_order_record(cps[0]);
    EXPECT_STREQ(rec.client_order_id.id, "HY-A");
    EXPECT_EQ(rec.state, OrderState::Accepted);
    EXPECT_EQ(rec.exchange_order_id, 555);
    EXPECT_EQ(rec.symbol_id, 1u);
    EXPECT_EQ(rec.intended_price_ticks, 100);
    EXPECT_EQ(rec.intended_qty_ticks, 10);
}

// TODO 1A.3 follow-up: regression test for the "silently defaults to Buy"
// bug -- OrderRecoveryCheckpoint/AuditRecord never carried side before this
// batch, so every recovered order (regardless of its real side) silently
// came back as OrderSide::Buy. This constructs a genuine Sell order's
// OrderIntentCreated frame (the ONLY frame run_recovery_scan_impl() reads
// .side from, on a fresh COID's first appearance) and confirms it survives
// the full append -> restart -> recover -> checkpoint_to_order_record() path.
TEST_F(DurableAuditSinkTest, CheckpointToOrderRecordPreservesSellSide) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        AuditRecord intent = make_intent("HY-SELL", 100, 10, 1);
        intent.side = OrderSide::Sell;
        ASSERT_TRUE(sink.append_durable(intent, 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-SELL", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-SELL", 1, 555), 1002).acked());
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].side, OrderSide::Sell);

    OrderRecord rec = checkpoint_to_order_record(cps[0]);
    EXPECT_EQ(rec.side, OrderSide::Sell);
}

TEST_F(DurableAuditSinkTest, CheckpointToOrderRecordDefaultsBuySideWhenNeverSet) {
    // Sanity control for the test above: a genuinely-Buy order (make_intent()'s
    // own default) round-trips as Buy, not by coincidence of an unset field.
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-BUY", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-BUY", 100, 10, 1), 1001).acked());
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].side, OrderSide::Buy);
}

// --- seed_position_truth() ---

// PartialFill, not Filled -- Filled is exchange-final, and
// run_recovery_scan_impl()'s own recovery loop explicitly skips
// exchange-final states ("nothing to recover", is_exchange_final(rs.state)),
// so a fully-Filled order never produces a checkpoint at all. seed_position_
// truth()'s real audience is exactly this "still resting/uncertain, needs
// PositionTruth applied at boot before ordinary reconciliation resumes"
// case, not the fully-resolved case (which .side's OWN companion fix,
// applied at the moment a real order's terminal AuditRecord is written, or
// ReconcileEvent's fold-in path -- see order_tracker.hpp -- already covers
// for the live-process case).
AuditRecord make_partial_fill(const char* coid, std::uint32_t symbol_id, std::int64_t exch_id,
                               std::int64_t filled_qty, std::int64_t avg_price) {
    AuditRecord rec{};
    rec.timestamp_ms = 1002;
    rec.event_type = AuditEventType::OrderPartialFill;
    rec.mode = ExecutionMode::Live;
    rec.symbol_id = symbol_id;
    rec.set_client_order_id(coid);
    rec.exchange_order_id = exch_id;
    rec.resulting_state = OrderState::PartialFill;
    rec.filled_qty_ticks = filled_qty;
    rec.avg_fill_price_ticks = avg_price;
    return rec;
}

TEST_F(DurableAuditSinkTest, SeedPositionTruthFoldsEachRecoveredCheckpointsFilledAmount) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        AuditRecord buy_intent = make_intent("HY-BUY", 100, 10, 1);
        ASSERT_TRUE(sink.append_durable(buy_intent, 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-BUY", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_partial_fill("HY-BUY", 1, 555, 6, 100), 1002).acked());

        AuditRecord sell_intent = make_intent("HY-SELL", 200, 5, 2);
        sell_intent.side = OrderSide::Sell;
        ASSERT_TRUE(sink.append_durable(sell_intent, 1003).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-SELL", 200, 5, 2), 1004).acked());
        ASSERT_TRUE(sink.append_durable(make_partial_fill("HY-SELL", 2, 556, 3, 200), 1005).acked());
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 2u);

    PositionTruth truth;
    seed_position_truth(truth, cps);
    EXPECT_EQ(truth.net_qty_ticks(1), 6);    // buy partial fill
    EXPECT_EQ(truth.net_qty_ticks(2), -3);   // sell partial fill
}

TEST_F(DurableAuditSinkTest, SeedPositionTruthSkipsCheckpointsWithNoFill) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        // Never filled -- still Submitting/Ambiguous when the process "crashed".
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    ASSERT_EQ(cps[0].filled_qty_ticks, 0);

    PositionTruth truth;
    seed_position_truth(truth, cps);
    EXPECT_EQ(truth.net_qty_ticks(1), 0);
    EXPECT_EQ(truth.tracked_symbol_count(), 0u);  // no slot allocated for a zero-fill checkpoint
}

// --- Phase 4: rotate_active_key() ---

TEST_F(DurableAuditSinkTest, RotateActiveKeyRequiresNewKeyAlreadyLoaded) {
    DurableAuditSink sink(base_path_, *key_ring_, 1);
    ASSERT_TRUE(sink.is_open());
    EXPECT_FALSE(sink.rotate_active_key(2, 2000));  // key 2 never loaded into key_ring_
    EXPECT_EQ(sink.active_key_id(), 1u);
}

TEST_F(DurableAuditSinkTest, RotateActiveKeySucceedsAndSubsequentAppendUsesNewKey) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);

    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.rotate_active_key(2, 1500));
        EXPECT_EQ(sink.active_key_id(), 2u);
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1501).acked());
    }

    // Restart with active_key_id=2 (what a real operator would configure
    // after a completed rotation) -- both key 1 (for the old frame) and
    // key 2 (for the new frame + tip anchor) must still be loaded.
    DurableAuditSink restarted(base_path_, *key_ring_, 2);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_STREQ(cps[0].client_order_id.id, "HY-A");
}

TEST_F(DurableAuditSinkTest, RotateActiveKeyOnEmptyLogSkipsMainAnchorReanchor) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);

    DurableAuditSink sink(base_path_, *key_ring_, 1);
    ASSERT_TRUE(sink.is_open());
    ASSERT_TRUE(sink.rotate_active_key(2, 1000));
    EXPECT_EQ(sink.active_key_id(), 2u);
    // Main log was still empty at rotation time -- the first real append now
    // correctly uses the rotated key.
    EXPECT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1001).acked());
}

TEST_F(DurableAuditSinkTest, CrashBetweenRotateAndFirstAppendStillRecoversCleanly) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);

    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.rotate_active_key(2, 1500));
        // Simulate a crash right here -- no further appends after rotation.
    }

    // Restart configured with the NEW key (matching a real operator who
    // updated config after the rotation reported success) must NOT see this
    // as Corrupt, even though the crash landed immediately after rotation.
    DurableAuditSink restarted(base_path_, *key_ring_, 2);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_NE(restarted.recovery_status(), RecoveryScanStatus::Corrupt);
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
}

TEST_F(DurableAuditSinkTest, RotationSidecarSurvivesRestart) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);
    WrappedKeyRecord rec3{};
    ASSERT_EQ(key_ring_->add_key(3, distinct_key(0x55), rec3), KeyRingAddStatus::Ok);

    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.rotate_active_key(2, 1500));
    }

    DurableAuditSink restarted(base_path_, *key_ring_, 2);
    ASSERT_TRUE(restarted.is_open());
    ASSERT_FALSE(restarted.rotation_fenced());
    ASSERT_TRUE(restarted.rotation_log_open());
    // A second rotation after restart proves the sidecar's own sequence/mac
    // chain correctly continued from what run_rotation_recovery_scan()
    // recovered, not from a reset-to-zero state.
    EXPECT_TRUE(restarted.rotate_active_key(3, 2000));
    EXPECT_EQ(restarted.active_key_id(), 3u);
}

TEST_F(DurableAuditSinkTest, RotateActiveKeyFailsClosedWhenFenced) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);

    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    corrupt_log_byte(40);

    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.fenced());
    EXPECT_FALSE(restarted.rotate_active_key(2, 2000));
    EXPECT_EQ(restarted.active_key_id(), 1u);
}

// --- Phase 5 (docs/SPEC_INVARIANTS.md): store identity ---

TEST_F(DurableAuditSinkTest, StoreIdentityGeneratedOnFirstConstructionAndNonZero) {
    DurableAuditSink sink(base_path_, *key_ring_, 1);
    ASSERT_TRUE(sink.is_open());
    EXPECT_FALSE(sink.store_identity_degraded());
    EXPECT_TRUE(sink.store_uuid_lo() != 0 || sink.store_uuid_hi() != 0);
}

TEST_F(DurableAuditSinkTest, StoreIdentitySurvivesRestart) {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_FALSE(sink.store_identity_degraded());
        lo = sink.store_uuid_lo();
        hi = sink.store_uuid_hi();
    }

    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    EXPECT_FALSE(restarted.store_identity_degraded());
    EXPECT_EQ(restarted.store_uuid_lo(), lo);
    EXPECT_EQ(restarted.store_uuid_hi(), hi);
}

TEST_F(DurableAuditSinkTest, AppendDurableExportTuplePopulatesRealStoreUuid) {
    ExportOutboxRing ring;
    DurableAuditSink sink(base_path_, *key_ring_, 1);
    ASSERT_FALSE(sink.store_identity_degraded());
    sink.set_export_outbox(&ring);

    ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());

    ExportTuple t{};
    ASSERT_TRUE(ring.peek_oldest(t));
    EXPECT_EQ(t.store_uuid_lo, sink.store_uuid_lo());
    EXPECT_EQ(t.store_uuid_hi, sink.store_uuid_hi());
    EXPECT_TRUE(t.store_uuid_lo != 0 || t.store_uuid_hi != 0);
}

TEST_F(DurableAuditSinkTest, TamperedStoreIdentityFileDisablesExportWithoutFencingMainLog) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_FALSE(sink.store_identity_degraded());
    }

    // Flip a byte inside the store-identity sidecar's tip file -- same
    // corruption technique corrupt_log_byte() uses for the main log, applied
    // to the .storeid.tip file instead.
    {
        std::fstream f(base_path_ + ".storeid.tip", std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(f.is_open());
        char b = 0;
        f.read(&b, 1);
        b = static_cast<char>(b ^ 0x01);
        f.seekp(0);
        f.write(&b, 1);
    }

    ExportOutboxRing ring;
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_FALSE(restarted.fenced());  // a store-identity problem never fences the main log
    EXPECT_TRUE(restarted.store_identity_degraded());
    EXPECT_EQ(restarted.store_uuid_lo(), 0u);
    EXPECT_EQ(restarted.store_uuid_hi(), 0u);

    restarted.set_export_outbox(&ring);
    // Core assertion of this round's fix: main append path keeps working...
    ASSERT_TRUE(restarted.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    // ...but the degraded identity means NOTHING gets pushed to the export
    // outbox at all -- not a tuple with a zero/placeholder identity.
    ExportTuple t{};
    EXPECT_FALSE(ring.peek_oldest(t));
}

TEST_F(DurableAuditSinkTest, StoreIdentityWriteFailureDisablesExportEvenWithInMemoryUuid) {
    // Block ONLY the durable write of a first-time-generated identity: create
    // a directory at the exact path write_tip_anchor() needs for its
    // temp-file-then-rename step, while leaving the real ".storeid.tip" path
    // itself genuinely absent -- so establish_store_identity() reaches
    // "generate a fresh random identity", then fails specifically at the
    // write, not at the earlier read/exists probe.
    const std::string blocked_tmp_path = base_path_ + ".storeid.tip.tmp";
#ifdef _WIN32
    ASSERT_TRUE(CreateDirectoryA(blocked_tmp_path.c_str(), nullptr));
#else
    ASSERT_EQ(::mkdir(blocked_tmp_path.c_str(), 0700), 0);
#endif

    ExportOutboxRing ring;
    DurableAuditSink sink(base_path_, *key_ring_, 1);
    ASSERT_TRUE(sink.is_open());
    EXPECT_FALSE(sink.fenced());  // still never fences the main log
    EXPECT_TRUE(sink.store_identity_degraded());
    // Never uses the freshly-generated-but-unpersisted value -- 0, not some
    // in-memory-only UUID that would silently change again on the next
    // restart.
    EXPECT_EQ(sink.store_uuid_lo(), 0u);
    EXPECT_EQ(sink.store_uuid_hi(), 0u);

    sink.set_export_outbox(&ring);
    ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    ExportTuple t{};
    EXPECT_FALSE(ring.peek_oldest(t));

#ifdef _WIN32
    RemoveDirectoryA(blocked_tmp_path.c_str());
#else
    ::rmdir(blocked_tmp_path.c_str());
#endif
}

TEST_F(DurableAuditSinkTest, TwoDifferentSinksGetDifferentStoreUuids) {
    std::string other_path = base_path_ + "_other";
    auto remove_other = [&] {
        std::remove(other_path.c_str());
        std::remove((other_path + ".lock").c_str());
        std::remove((other_path + ".tip").c_str());
        std::remove((other_path + ".keyrotations").c_str());
        std::remove((other_path + ".keyrotations.lock").c_str());
        std::remove((other_path + ".keyrotations.tip").c_str());
        std::remove((other_path + ".storeid").c_str());
        std::remove((other_path + ".storeid.lock").c_str());
        std::remove((other_path + ".storeid.tip").c_str());
    };
    remove_other();

    DurableAuditSink a(base_path_, *key_ring_, 1);
    DurableAuditSink b(other_path, *key_ring_, 1);
    ASSERT_FALSE(a.store_identity_degraded());
    ASSERT_FALSE(b.store_identity_degraded());
    EXPECT_TRUE(a.store_uuid_lo() != b.store_uuid_lo() || a.store_uuid_hi() != b.store_uuid_hi());

    remove_other();
}

// --- Deterministic checkpoint order (audit REC-DETERM-007) ---
//
// recovered_checkpoints() is documented as "in scan order", and
// CapacityExceeded's contract promises "this array's FIRST kMaxInFlight entries".
// Both were false while the loop iterated a std::unordered_map: its order is
// unspecified and varies with hash seed, insertion history and standard-library
// version, so two processes recovering the SAME log could disagree on order and,
// at capacity, on which subset survived.

TEST_F(DurableAuditSinkTest, RecoveredCheckpointsFollowLogScanOrder) {
    constexpr int kOrders = 12;
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        for (int i = 0; i < kOrders; ++i) {
            char coid[16];
            std::snprintf(coid, sizeof(coid), "HY-%02d", i);
            ASSERT_TRUE(sink.append_durable(make_intent(coid, 100 + i, 10, 1), 1000 + i).acked());
            ASSERT_TRUE(sink.append_durable(make_submitted(coid, 100 + i, 10, 1), 2000 + i).acked());
        }
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), static_cast<std::size_t>(kOrders));
    for (int i = 0; i < kOrders; ++i) {
        char expected[16];
        std::snprintf(expected, sizeof(expected), "HY-%02d", i);
        EXPECT_STREQ(cps[static_cast<std::size_t>(i)].client_order_id.id, expected)
            << "checkpoint " << i << " is out of scan order";
    }
}

TEST_F(DurableAuditSinkTest, CheckpointOrderIsStableAcrossRepeatedRecovery) {
    // Same log, recovered three times in the same process. An unordered_map can
    // legitimately hand back a different order per container instance; scan order
    // cannot.
    constexpr int kOrders = 8;
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        for (int i = 0; i < kOrders; ++i) {
            char coid[16];
            std::snprintf(coid, sizeof(coid), "HY-%02d", i);
            ASSERT_TRUE(sink.append_durable(make_intent(coid, 100 + i, 10, 1), 1000 + i).acked());
            ASSERT_TRUE(sink.append_durable(make_submitted(coid, 100 + i, 10, 1), 2000 + i).acked());
        }
    }

    std::vector<std::string> first_order;
    for (int attempt = 0; attempt < 3; ++attempt) {
        DurableAuditSink restarted(base_path_, *key_ring_, 1);
        ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
        std::vector<std::string> seen;
        for (const auto& cp : restarted.recovered_checkpoints()) {
            seen.emplace_back(cp.client_order_id.id);
        }
        ASSERT_EQ(seen.size(), static_cast<std::size_t>(kOrders));
        if (attempt == 0) {
            first_order = seen;
        } else {
            EXPECT_EQ(seen, first_order) << "recovery order changed between runs (attempt " << attempt << ")";
        }
    }
}

// --- Oversized log fails closed instead of terminating (audit REC-NOEXCEPT-006) ---

TEST_F(DurableAuditSinkTest, LogLargerThanTheReadCapIsIoErrorNotAbort) {
    // The real trigger is std::bad_alloc escaping a noexcept recovery path, which a
    // unit test cannot provoke portably. kMaxDurableLogBytes is the tripwire that
    // makes the same condition reachable deterministically: exceed it and recovery
    // must report IoError and fence, NOT abort the process and not silently proceed.
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    // Sparse file: cheap to create, and read_whole_log() sizes its buffer from the
    // reported length, which is exactly what is under test.
    {
        std::FILE* f = std::fopen(base_path_.c_str(), "r+b");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(std::fseek(f, static_cast<long>(hy::kMaxDurableLogBytes) + 1024, SEEK_SET), 0);
        ASSERT_EQ(std::fputc(0, f), 0);
        std::fclose(f);
    }

    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::IoError);
    EXPECT_TRUE(restarted.fenced()) << "a log we cannot account for must fence, never append";

    AuditRecord rec = make_intent("HY-B", 100, 10, 1);
    EXPECT_FALSE(restarted.append_durable(rec, 5000).acked());
}

// --- Interrupted rotation is recoverable (audit KEY-ROTATE-008) ---
//
// rotate_active_key() is a two-step durable protocol: (1) append a KeyRotated
// record to the sidecar under the new key, (2) re-anchor the main log tip under
// the new key. A crash BETWEEN them leaves the sidecar saying "rotated to N" while
// the main tip anchor is still signed under the old key. Nothing is lost, but the
// anchor.key_id == active_key_id_ consistency check used to call that Corrupt and
// fence the sink permanently -- and run_rotation_recovery_scan() decoded the very
// payload that could have told the two apart, then dropped it.
//
// The crash is simulated by performing a REAL rotation and then restoring the
// pre-rotation main tip anchor, which is byte-for-byte the state step 2 would have
// left behind had it never run.

namespace {
std::vector<char> read_file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void write_file_bytes(const std::string& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
}  // namespace

TEST_F(DurableAuditSinkTest, InterruptedRotationRecoversAndCompletesStepTwo) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);

    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }

    // Snapshot the tip anchor as it stands under key 1 -- exactly what a crash
    // before step 2 would leave on disk.
    const auto tip_under_old_key = read_file_bytes(base_path_ + ".tip");
    ASSERT_FALSE(tip_under_old_key.empty());

    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.rotate_active_key(2, 1500));  // both steps run...
    }
    write_file_bytes(base_path_ + ".tip", tip_under_old_key);  // ...then undo step 2

    // Restart configured with the NEW key, which is what an operator reading the
    // sidecar would do. This used to be an unrecoverable Corrupt + permanent fence.
    {
        DurableAuditSink restarted(base_path_, *key_ring_, 2);
        ASSERT_TRUE(restarted.is_open());
        EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered)
            << "an interrupted rotation lost nothing and must not be treated as corruption";
        EXPECT_FALSE(restarted.fenced());
        EXPECT_TRUE(restarted.completed_interrupted_rotation());
        ASSERT_TRUE(restarted.last_rotation().has_value());
        EXPECT_EQ(restarted.last_rotation()->old_key_id, 1u);
        EXPECT_EQ(restarted.last_rotation()->new_key_id, 2u);
        EXPECT_TRUE(restarted.append_durable(make_accepted("HY-A", 1, 555), 2000).acked());
    }

    // Step 2 was completed on disk, so the next restart takes the ordinary path.
    DurableAuditSink again(base_path_, *key_ring_, 2);
    ASSERT_TRUE(again.is_open());
    EXPECT_FALSE(again.fenced());
    EXPECT_FALSE(again.completed_interrupted_rotation())
        << "the anchor is consistent now; this must not look like a fresh interruption";
}

TEST_F(DurableAuditSinkTest, AnchorKeyMismatchNotProvenBySidecarIsStillCorrupt) {
    // Negative control for the branch above. Without it, "accept a mismatch the
    // sidecar vouches for" could quietly degrade into "accept any mismatch", which
    // is exactly the unauthorized-identity-swap case the check exists to catch.
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);
    WrappedKeyRecord rec3{};
    ASSERT_EQ(key_ring_->add_key(3, distinct_key(0xAB), rec3), KeyRingAddStatus::Ok);

    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    const auto tip_under_old_key = read_file_bytes(base_path_ + ".tip");
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.rotate_active_key(2, 1500));  // sidecar proves 1 -> 2 only
    }
    write_file_bytes(base_path_ + ".tip", tip_under_old_key);

    // Configured with key 3, which no rotation record vouches for.
    DurableAuditSink restarted(base_path_, *key_ring_, 3);
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt);
    EXPECT_TRUE(restarted.fenced());
}

TEST_F(DurableAuditSinkTest, NoRotationMeansAnchorKeyMismatchIsCorrupt) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    // Never rotated: the sidecar is empty, so nothing can vouch for a key change.
    DurableAuditSink restarted(base_path_, *key_ring_, 2);
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt);
    EXPECT_FALSE(restarted.last_rotation().has_value());
}

// --- Key-retirement precondition is checkable (audit KEY-RETIRE-009) ---

TEST_F(DurableAuditSinkTest, ObservedKeyIdsReportsEveryKeyThatSignedTheLog) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.rotate_active_key(2, 1500));
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1501).acked());
    }

    DurableAuditSink restarted(base_path_, *key_ring_, 2);
    ASSERT_TRUE(restarted.is_open());
    auto ids = restarted.observed_key_ids();
    ASSERT_EQ(ids.size(), 2u) << "both the pre- and post-rotation signing keys are still needed";
    EXPECT_EQ(ids[0], 1u) << "scan order: the older frame first";
    EXPECT_EQ(ids[1], 2u);
}

TEST_F(DurableAuditSinkTest, RetiringAnObservedKeyIsWhatBreaksRecovery) {
    // Documents the limit rather than pretending it is gone: retiring a key that
    // observed_key_ids() reports IS destructive, which is precisely why that
    // accessor exists. Compaction is what would actually lift this.
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    {
        DurableAuditSink probe(base_path_, *key_ring_, 1);
        auto ids = probe.observed_key_ids();
        ASSERT_EQ(ids.size(), 1u);
        EXPECT_EQ(ids[0], 1u);
    }

    EXPECT_EQ(key_ring_->retire(1), RetireStatus::Retired)
        << "retire() must report whether it removed anything";
    EXPECT_EQ(key_ring_->retire(1), RetireStatus::NotFound) << "second retire of the same id removes nothing";

    DurableAuditSink after(base_path_, *key_ring_, 1);
    EXPECT_EQ(after.recovery_status(), RecoveryScanStatus::Corrupt)
        << "retiring a key the log still needs makes the log unrecoverable -- check "
           "observed_key_ids() first";
}

TEST_F(DurableAuditSinkTest, KeyRingExposesItsRotationCeiling) {
    EXPECT_EQ(KeyRing::capacity(), hy::kMaxLiveKeys);
    EXPECT_EQ(key_ring_->live_key_count(), 1u);
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);
    EXPECT_EQ(key_ring_->live_key_count(), 2u);
    EXPECT_EQ(key_ring_->retire(2), RetireStatus::Retired);
    EXPECT_EQ(key_ring_->live_key_count(), 1u);
}
