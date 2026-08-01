// Real-file tests for durable_audit_sink.hpp's DurableAuditSink. Every test
// in this file touches an actual temp file on disk -- this is what actually
// exercises append_durable()'s fsync/tip-anchor/lock machinery and
// recovery_scan()'s real torn-write/corruption handling, neither of which
// test_durable_frame_codec.cpp's pure in-memory tests can reach.
//
// A "process restart" is simulated by destroying one DurableAuditSink
// instance and constructing a fresh one over the same path -- recovery
// happens automatically in the constructor, exactly like a real restart.
#include <gtest/gtest.h>
#include <hengyuan/durable_audit_sink.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

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
    }

    void TearDown() override { remove_all(); }

    void remove_all() {
        std::remove(base_path_.c_str());
        std::remove((base_path_ + ".lock").c_str());
        std::remove((base_path_ + ".tip").c_str());
        std::remove((base_path_ + ".tip.tmp").c_str());
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
        auto key = test_key();
        auto n = encode_order_event_frame(full_frame, sequence_number, FrameTimeKind::ServerCorrectedUtc,
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
    DurableAuditSink sink(base_path_, test_key());
    ASSERT_TRUE(sink.is_open());
    EXPECT_FALSE(sink.fenced());
    EXPECT_EQ(sink.recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_TRUE(sink.recovered_checkpoints().empty());
}

TEST_F(DurableAuditSinkTest, AppendSucceedsAndAcksIncrementingSequence) {
    DurableAuditSink sink(base_path_, test_key());
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
        DurableAuditSink sink(base_path_, test_key());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-A", 1, 555), 1002).acked());
        ASSERT_TRUE(sink.append_durable(make_filled("HY-A", 1, 555, 10, 100), 1003).acked());
    }
    DurableAuditSink restarted(base_path_, test_key());
    ASSERT_TRUE(restarted.is_open());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_TRUE(restarted.recovered_checkpoints().empty())
        << "Filled is exchange-final -- nothing should need recovering";
}

TEST_F(DurableAuditSinkTest, DanglingSubmittingRecoversAsAmbiguous) {
    {
        DurableAuditSink sink(base_path_, test_key());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        // No outcome ever lands -- simulates a crash right after the POST.
    }
    DurableAuditSink restarted(base_path_, test_key());
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
        DurableAuditSink sink(base_path_, test_key());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-A", 1, 555), 1002).acked());
    }
    DurableAuditSink restarted(base_path_, test_key());
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].resulting_state, OrderState::Accepted);
    EXPECT_EQ(cps[0].exchange_order_id, 555);
}

TEST_F(DurableAuditSinkTest, MultipleOrdersRecoveredIndependently) {
    {
        DurableAuditSink sink(base_path_, test_key());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-B", 200, 20, 2), 1002).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-B", 200, 20, 2), 1003).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-B", 2, 777), 1004).acked());
        ASSERT_TRUE(sink.append_durable(make_filled("HY-B", 2, 777, 20, 200), 1005).acked());
        // HY-A stays dangling Submitting; HY-B resolves to Filled.
    }
    DurableAuditSink restarted(base_path_, test_key());
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u) << "only HY-A should need recovery; HY-B is exchange-final";
    EXPECT_STREQ(cps[0].client_order_id.id, "HY-A");
    EXPECT_EQ(cps[0].resulting_state, OrderState::Ambiguous);
}

// --- Torn write vs corruption (the round-6-P0 distinction) ---

TEST_F(DurableAuditSinkTest, TornTailWriteDiscardsOnlyTheIncompleteRecord) {
    {
        DurableAuditSink sink(base_path_, test_key());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        // Anchor now correctly points at sequence 0. Simulate a crash mid-way
        // through what would have been the NEXT append (Submitting) -- a
        // real partial frame prefix lands in the log, but nothing ever
        // touched the anchor for it, exactly like a real crash before that
        // append's own fsync.
    }
    append_prefix_of_a_real_frame(make_submitted("HY-A", 100, 10, 1), /*sequence_number=*/1, kZeroMac,
                                   /*prefix_len=*/30);

    DurableAuditSink restarted(base_path_, test_key());
    ASSERT_TRUE(restarted.is_open());
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered)
        << "one complete frame (Intent) should remain and recover cleanly";
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].resulting_state, OrderState::Intent)
        << "only the first frame (Intent) is real; the trailing garbage must be discarded as a torn tail";
}

TEST_F(DurableAuditSinkTest, AnchorAheadOfTruncatedLogIsCorruptTailDeletion) {
    // Distinct from the torn-write case above: here BOTH appends genuinely
    // completed (anchor legitimately advanced to sequence 1), and the log's
    // bytes were removed AFTERWARD. This is what a tail-deletion attack (or
    // a corrupted filesystem) looks like, not a crash -- must be Corrupt,
    // never silently treated as "the second write just didn't happen."
    {
        DurableAuditSink sink(base_path_, test_key());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }
    const std::size_t full_size = log_file_size();
    truncate_log_to(full_size - 20);

    DurableAuditSink restarted(base_path_, test_key());
    ASSERT_TRUE(restarted.is_open());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt);
    EXPECT_TRUE(restarted.fenced());
}

TEST_F(DurableAuditSinkTest, ChecksumCorruptionMidLogIsCorruptNotSilentlySkipped) {
    {
        DurableAuditSink sink(base_path_, test_key());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-A", 1, 555), 1002).acked());
    }
    // Flip a byte inside the FIRST frame's payload (well before the physical
    // end of the file) -- this must be Corrupt, not treated as a torn tail.
    corrupt_log_byte(40);

    DurableAuditSink restarted(base_path_, test_key());
    ASSERT_TRUE(restarted.is_open());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt);
    EXPECT_TRUE(restarted.fenced()) << "a Corrupt scan must fail closed -- no appends allowed";
}

TEST_F(DurableAuditSinkTest, ChecksumCorruptionOnLastFrameIsCorruptNotTruncated) {
    {
        DurableAuditSink sink(base_path_, test_key());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    const std::size_t size = log_file_size();
    corrupt_log_byte(size - 1);  // last byte of the (complete) last frame's mac

    DurableAuditSink restarted(base_path_, test_key());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt)
        << "a complete-but-corrupt frame at the physical end must never be treated like a torn write";
}

TEST_F(DurableAuditSinkTest, CorruptSinkRefusesAppends) {
    {
        DurableAuditSink sink(base_path_, test_key());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    corrupt_log_byte(40);

    DurableAuditSink restarted(base_path_, test_key());
    ASSERT_TRUE(restarted.fenced());
    auto result = restarted.append_durable(make_intent("HY-B", 1, 1, 1), 2000);
    EXPECT_FALSE(result.acked());
}

// --- Single-writer lock ---

TEST_F(DurableAuditSinkTest, SecondSinkOverSamePathFailsToOpen) {
    DurableAuditSink first(base_path_, test_key());
    ASSERT_TRUE(first.is_open());

    DurableAuditSink second(base_path_, test_key());
    EXPECT_FALSE(second.is_open()) << "the lock must be exclusive while `first` is still alive";
}

TEST_F(DurableAuditSinkTest, LockIsReleasedOnDestruction) {
    {
        DurableAuditSink first(base_path_, test_key());
        ASSERT_TRUE(first.is_open());
    }
    DurableAuditSink second(base_path_, test_key());
    EXPECT_TRUE(second.is_open()) << "the lock must be released once `first` is destroyed";
}

// --- Wrong key ---

TEST_F(DurableAuditSinkTest, WrongKeyOnRestartIsCorrupt) {
    {
        DurableAuditSink sink(base_path_, test_key());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    std::vector<std::byte> wrong_key(4, std::byte{0xAB});
    DurableAuditSink restarted(base_path_, wrong_key);
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt);
}

// --- InFlightRegistry rebuild integration ---

TEST_F(DurableAuditSinkTest, RepopulateInFlightRegistryHelperRegistersEveryCheckpoint) {
    {
        DurableAuditSink sink(base_path_, test_key());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }
    DurableAuditSink restarted(base_path_, test_key());
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);

    InFlightRegistry registry;
    auto n = repopulate_in_flight_registry(registry, restarted.recovered_checkpoints());
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(registry.count(), 1u);
    EXPECT_TRUE(registry.is_in_flight("HY-A"));
}

TEST_F(DurableAuditSinkTest, CheckpointToOrderRecordPreservesAllFields) {
    {
        DurableAuditSink sink(base_path_, test_key());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-A", 1, 555), 1002).acked());
    }
    DurableAuditSink restarted(base_path_, test_key());
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
