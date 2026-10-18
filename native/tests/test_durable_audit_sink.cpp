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
#include "durable_log_store_test_hooks.hpp"

#include <gtest/gtest.h>
#include <hengyuan/durable_audit_sink.hpp>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
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
    // base_path_ minus its ".log"; use_fresh_base_path() derives its never-used names from it.
    std::string base_path_stem_;
    unsigned fresh_base_paths_used_{0};

    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        base_path_stem_ = std::string(tmp) + "hy_das_" + std::to_string(GetCurrentProcessId()) + "_" +
                          std::to_string(counter);
#else
        base_path_stem_ = "/tmp/hy_das_" + std::to_string(getpid()) + "_" + std::to_string(counter);
#endif
        base_path_ = base_path_stem_ + ".log";
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

    // For a loop that runs one scenario many times, each pass on a store of its own: moves the fixture to a base path
    // nothing has used yet and deletes the files of the one it leaves. Do not call remove_all() at the top of such a
    // loop and re-use the names: on a Windows host with real-time antivirus that races with the scanner. The sink
    // renames <tip>.tmp over <tip>, the loop deletes <tip> a few milliseconds later, and the next pass's
    // CreateFileA(<tip>.tmp, share=0) can fail with ERROR_SHARING_VIOLATION. DurableLogStore::write_tip_anchor reports
    // that as a failed append and the sink fences (fence_reason IoError, recovery_status Clean), which is how
    // EveryTornPrefixLengthIsCutAndTheLogKeepsGrowing failed in roughly one run in ten. A name nothing has used cannot
    // collide with a handle still held for an old one (a standalone probe with the same Win32 calls saw no failure with
    // fresh names, and none with a 30 ms gap before the delete; the commit message has the numbers). The sink itself
    // never deletes its tip, so this is the fixture's pattern, not the sink's.
    void use_fresh_base_path() {
        remove_all();
        base_path_ = base_path_stem_ + "_" + std::to_string(++fresh_base_paths_used_) + ".log";
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
    EXPECT_EQ(sink.recovered_intent_orphans(), 0u);
    EXPECT_EQ(sink.max_intent_orphans(), kMaxIntentOrphans);
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

// --- Intent-only orphans (SAFE-03 slice 2; SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md round 9) ---
//
// A client order id whose LAST durable frame is Intent was never sent: OrderSubmitPrepared is durable before the
// send, and every local refusal after the Intent frame (no operator confirmation, a port that is not valid, a
// rate-limit refusal at the real charge, a full in-flight registry) stops right there. Recovery counts such an id
// against its own budget (kMaxIntentOrphans) and hands NOBODY a checkpoint for it. As a checkpoint it was
// registered in flight and never resolved (nothing reconciles an Intent), one of them held startup recovery in
// Reconciling until the deadline, and kMaxInFlight + 1 of them made the whole log CapacityExceeded.

namespace {

// "HY-0000", "HY-0001", ... -- one distinct fixed-width client order id per index.
std::string indexed_coid(std::size_t i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "HY-%04u", static_cast<unsigned>(i));
    return std::string(buf);
}

// The comment on kMaxIntentOrphans sizes the budget at 1 MiB of Intent frames: pin the arithmetic it rests on.
static_assert(kOrderEventFrameSize == 256, "one order-event frame is 256 bytes");
static_assert(kMaxIntentOrphans == 4096, "owner decision 2026-10-13");
static_assert(kMaxIntentOrphans * kOrderEventFrameSize == 1024u * 1024u, "the orphan budget is 1 MiB of frames");

}  // namespace

TEST_F(DurableAuditSinkTest, AnIntentOnlyOrderIsAnOrphanNotACheckpoint) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        // The refusal that follows (no confirmation, ...) writes nothing more: the id stays at Intent.
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_FALSE(restarted.fenced());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Clean)
        << "an order that was never sent leaves nothing to recover";
    EXPECT_TRUE(restarted.recovered_checkpoints().empty())
        << "a checkpoint would be registered in flight and never resolved";
    EXPECT_EQ(restarted.recovered_intent_orphans(), 1u);
}

TEST_F(DurableAuditSinkTest, ARepeatedIntentFrameForTheSameOrderIsStillOneOrphan) {
    // A refused attempt retried under the SAME client order id: two Intent frames, one order.
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1001).acked());
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_TRUE(restarted.recovered_checkpoints().empty());
    EXPECT_EQ(restarted.recovered_intent_orphans(), 1u);
}

TEST_F(DurableAuditSinkTest, AnOrderThatMovedPastIntentIsNeverAnOrphan) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        // HY-A: a refused attempt and then a retry under the same id that went out -- two Intent frames, then
        // Submitting. It was sent, so it is an open order, whatever its first frames look like.
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1001).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1002).acked());
        // HY-B: accepted and still open.
        ASSERT_TRUE(sink.append_durable(make_intent("HY-B", 200, 20, 2), 1003).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-B", 200, 20, 2), 1004).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-B", 2, 777), 1005).acked());
        // HY-C: filled -- exchange-final, neither a checkpoint nor an orphan.
        ASSERT_TRUE(sink.append_durable(make_intent("HY-C", 300, 30, 3), 1006).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-C", 300, 30, 3), 1007).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-C", 3, 888), 1008).acked());
        ASSERT_TRUE(sink.append_durable(make_filled("HY-C", 3, 888, 30, 300), 1009).acked());
        // HY-D: refused before the send -- the only orphan.
        ASSERT_TRUE(sink.append_durable(make_intent("HY-D", 400, 40, 4), 1010).acked());
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 2u) << "only the two orders that were actually sent and are not final";
    EXPECT_STREQ(cps[0].client_order_id.id, "HY-A");
    EXPECT_EQ(cps[0].resulting_state, OrderState::Ambiguous);
    EXPECT_STREQ(cps[1].client_order_id.id, "HY-B");
    EXPECT_EQ(cps[1].resulting_state, OrderState::Accepted);
    EXPECT_EQ(restarted.recovered_intent_orphans(), 1u) << "HY-D alone";
}

TEST_F(DurableAuditSinkTest, IntentOrphansDoNotCountTowardTheInFlightCapacity) {
    // kMaxInFlight + 1 orphans. Before slice 2 each was an open-order checkpoint and this log was CapacityExceeded.
    constexpr std::size_t kOrphans = kMaxInFlight + 1;
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        for (std::size_t i = 0; i < kOrphans; ++i) {
            const std::string coid = indexed_coid(i);
            ASSERT_TRUE(
                sink.append_durable(make_intent(coid.c_str(), 100, 10, 1), 1000 + static_cast<std::int64_t>(i))
                    .acked());
        }
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_FALSE(restarted.fenced());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_TRUE(restarted.recovered_checkpoints().empty());
    EXPECT_EQ(restarted.recovered_intent_orphans(), kOrphans);
}

TEST_F(DurableAuditSinkTest, IntentOrphansBesideRealOpenOrdersLeaveOnlyThoseOrdersAsCheckpoints) {
    constexpr std::size_t kOrphans = kMaxInFlight + 1;
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        for (std::size_t i = 0; i < kOrphans; ++i) {
            const std::string coid = indexed_coid(i);
            ASSERT_TRUE(
                sink.append_durable(make_intent(coid.c_str(), 100, 10, 1), 1000 + static_cast<std::int64_t>(i))
                    .acked());
            if (i == 10) {  // a real order opens in the middle of the refusals
                ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 5000).acked());
                ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 5001).acked());
            }
        }
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_STREQ(cps[0].client_order_id.id, "HY-A");
    EXPECT_EQ(cps[0].resulting_state, OrderState::Ambiguous);
    EXPECT_EQ(restarted.recovered_intent_orphans(), kOrphans);
}

TEST_F(DurableAuditSinkTest, IntentOrphansAreStillCountedWhenTheCheckpointTableIsFull) {
    // The registry-full refusal is how most orphans come about: kMaxInFlight orders are open, the next attempt
    // writes its Intent frame (Gate 10b) and is refused at the in-flight registry (Gate 12b). The log then holds a
    // FULL checkpoint table with orphans after it -- those must neither be refused for want of a checkpoint slot
    // nor displace a real order.
    constexpr std::size_t kOrphans = 3;
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        for (std::size_t i = 0; i < kMaxInFlight; ++i) {
            const std::string coid = indexed_coid(i);
            const std::int64_t t = 1000 + 2 * static_cast<std::int64_t>(i);
            ASSERT_TRUE(sink.append_durable(make_intent(coid.c_str(), 100, 10, 1), t).acked());
            ASSERT_TRUE(sink.append_durable(make_submitted(coid.c_str(), 100, 10, 1), t + 1).acked());
        }
        for (std::size_t j = 0; j < kOrphans; ++j) {
            const std::string coid = indexed_coid(1000 + j);
            ASSERT_TRUE(
                sink.append_durable(make_intent(coid.c_str(), 100, 10, 1), 5000 + static_cast<std::int64_t>(j))
                    .acked());
        }
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_FALSE(restarted.fenced());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    EXPECT_EQ(restarted.recovered_checkpoints().size(), kMaxInFlight);
    EXPECT_EQ(restarted.recovered_intent_orphans(), kOrphans);
}

TEST_F(DurableAuditSinkTest, MoreOpenOrdersThanTheInFlightRegistryHoldsIsStillCapacityExceeded) {
    // The orphan rule must not loosen the other bound: orders that WERE sent and are still open do land in the
    // checkpoint table, and kMaxInFlight + 1 of them do not fit.
    constexpr std::size_t kOpen = kMaxInFlight + 1;
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        for (std::size_t i = 0; i < kOpen; ++i) {
            const std::string coid = indexed_coid(i);
            const std::int64_t t = 1000 + 2 * static_cast<std::int64_t>(i);
            ASSERT_TRUE(sink.append_durable(make_intent(coid.c_str(), 100, 10, 1), t).acked());
            ASSERT_TRUE(sink.append_durable(make_submitted(coid.c_str(), 100, 10, 1), t + 1).acked());
        }
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::CapacityExceeded);
    EXPECT_TRUE(restarted.fenced());
    EXPECT_EQ(restarted.recovered_checkpoints().size(), kMaxInFlight) << "the first kMaxInFlight, in scan order";
    EXPECT_EQ(restarted.recovered_intent_orphans(), 0u);
}

TEST_F(DurableAuditSinkTest, TheIntentOrphanBudgetIsAnExactBound) {
    // A budget of N holds N orphans and refuses the (N+1)th. One log grows across the boundary for N = 0..3; the
    // budget is tightened through the constructor so that 4096 frames need not be written.
    for (std::size_t n = 0; n <= 3; ++n) {
        {  // the log holds exactly n orphans: a budget of n takes them all
            DurableAuditSink at_budget(base_path_, *key_ring_, 1, n);
            ASSERT_TRUE(at_budget.is_open());
            EXPECT_EQ(at_budget.max_intent_orphans(), n);
            EXPECT_EQ(at_budget.recovery_status(), RecoveryScanStatus::Clean) << "n=" << n;
            EXPECT_FALSE(at_budget.fenced()) << "n=" << n;
            EXPECT_EQ(at_budget.recovered_intent_orphans(), n);
        }
        if (n > 0) {  // ...and a budget of n-1 does not
            DurableAuditSink below(base_path_, *key_ring_, 1, n - 1);
            ASSERT_TRUE(below.is_open());
            EXPECT_EQ(below.recovery_status(), RecoveryScanStatus::CapacityExceeded) << "n=" << n;
            EXPECT_TRUE(below.fenced()) << "a log that cannot be accounted for takes no appends, n=" << n;
            EXPECT_FALSE(below.append_durable(make_intent("HY-NO", 100, 10, 1), 9000).acked());
        }
        {  // one more orphan
            DurableAuditSink writer(base_path_, *key_ring_, 1);
            ASSERT_TRUE(writer.is_open());
            ASSERT_FALSE(writer.fenced()) << "n=" << n;
            const std::string coid = indexed_coid(n);
            ASSERT_TRUE(writer.append_durable(make_intent(coid.c_str(), 100, 10, 1), 1000).acked());
        }
    }
}

TEST_F(DurableAuditSinkTest, TheIntentOrphanBudgetCanOnlyBeTightened) {
    {
        DurableAuditSink by_default(base_path_, *key_ring_, 1);
        EXPECT_EQ(by_default.max_intent_orphans(), kMaxIntentOrphans);
    }
    {
        DurableAuditSink exact(base_path_, *key_ring_, 1, kMaxIntentOrphans);
        EXPECT_EQ(exact.max_intent_orphans(), kMaxIntentOrphans);
    }
    {
        DurableAuditSink above(base_path_, *key_ring_, 1, kMaxIntentOrphans + 1);
        EXPECT_EQ(above.max_intent_orphans(), kMaxIntentOrphans) << "a larger budget is clamped, never honoured";
    }
    {
        DurableAuditSink huge(base_path_, *key_ring_, 1, std::numeric_limits<std::size_t>::max());
        EXPECT_EQ(huge.max_intent_orphans(), kMaxIntentOrphans);
    }
    {
        DurableAuditSink none(base_path_, *key_ring_, 1, 0);
        EXPECT_EQ(none.max_intent_orphans(), 0u) << "a tighter budget is honoured, down to none";
    }
}

// --- Torn write vs corruption (the round-6-P0 distinction) ---

TEST_F(DurableAuditSinkTest, TornTailWriteDiscardsOnlyTheIncompleteRecord) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
        // Anchor now correctly points at sequence 1. Simulate a crash mid-way
        // through what would have been the NEXT append (Accepted) -- a
        // real partial frame prefix lands in the log, but nothing ever
        // touched the anchor for it, exactly like a real crash before that
        // append's own fsync.
    }
    append_prefix_of_a_real_frame(make_accepted("HY-A", 1, 555), /*sequence_number=*/2, kZeroMac,
                                   /*prefix_len=*/30);

    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered)
        << "two complete frames (Intent, Submitting) should remain and recover cleanly";
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].resulting_state, OrderState::Ambiguous)
        << "only the first two frames are real (Submitting, remapped to Ambiguous); the trailing garbage must be "
           "discarded as a torn tail -- Accepted here would mean the partial frame was applied";
}

TEST_F(DurableAuditSinkTest, TornTailAfterAnIntentOnlyOrderLeavesAnOrphanNotACheckpoint) {
    // The crash falls between the Intent frame and the Submitting frame: the partial Submitting frame is
    // discarded, so the order was never sent -- an orphan (SAFE-03 slice 2), nothing to recover.
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    append_prefix_of_a_real_frame(make_submitted("HY-A", 100, 10, 1), /*sequence_number=*/1, kZeroMac,
                                   /*prefix_len=*/30);

    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_FALSE(restarted.fenced());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Clean)
        << "one complete frame (Intent) remains, and an Intent-only order has nothing to recover";
    EXPECT_TRUE(restarted.recovered_checkpoints().empty());
    EXPECT_EQ(restarted.recovered_intent_orphans(), 1u);
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
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }
    append_prefix_of_a_real_frame(make_accepted("HY-A", 1, 555), /*sequence_number=*/2, kZeroMac,
                                   /*prefix_len=*/3);

    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered)
        << "a 3-byte torn tail (fewer than peek_frame_key_id's own 6-byte minimum) must still be Recovered, "
           "never misclassified as Corrupt from the key_id peek alone";
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].resulting_state, OrderState::Ambiguous);
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

// --- R-10 / L-30: the durable time an order first became uncertain ---
//
// The frame header's recorded_utc_ms is MAC-verified and already on disk, so recovery can hand the
// reconcile loop a wall anchor for an order left Ambiguous (or Submitting, which the scan remaps to
// Ambiguous) without any change to the on-disk format.

namespace {
constexpr std::int64_t kT0 = 1'790'000'000'000;  // a plausible wall time (2026-09)

AuditRecord make_ambiguous(const char* coid, std::uint32_t symbol_id) {
    AuditRecord rec{};
    rec.timestamp_ms = 1002;
    rec.event_type = AuditEventType::OrderAmbiguous;
    rec.mode = ExecutionMode::Live;
    rec.symbol_id = symbol_id;
    rec.set_client_order_id(coid);
    rec.resulting_state = OrderState::Ambiguous;
    return rec;
}
}  // namespace

TEST_F(DurableAuditSinkTest, RecoveredAmbiguousOrderCarriesTheTimeItFirstBecameUncertain) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), kT0).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), kT0 + 100).acked());
        ASSERT_TRUE(sink.append_durable(make_ambiguous("HY-A", 1), kT0 + 10'000).acked());  // the POST timed out
        // A repeated Ambiguous record (same state) must not move the anchor.
        ASSERT_TRUE(sink.append_durable(make_ambiguous("HY-A", 1), kT0 + 20'000).acked());
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].resulting_state, OrderState::Ambiguous);
    EXPECT_EQ(cps[0].unknown_since_utc_ms, kT0 + 10'000) << "the FIRST Ambiguous frame, not the later repeat";
}

TEST_F(DurableAuditSinkTest, ARecoveredSubmittingOrderUsesTheSubmittingFrameTime) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), kT0).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), kT0 + 1'500).acked());
    }  // the process died before any outcome was written
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].resulting_state, OrderState::Ambiguous) << "Submitting is remapped to Ambiguous";
    EXPECT_EQ(cps[0].unknown_since_utc_ms, kT0 + 1'500)
        << "the earliest moment its fate could have become unknown: errs toward quarantining sooner";
}

TEST_F(DurableAuditSinkTest, ARecoveredLiveOrderHasNoUnknownAnchor) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), kT0).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), kT0 + 100).acked());
        ASSERT_TRUE(sink.append_durable(make_ambiguous("HY-A", 1), kT0 + 10'000).acked());
        ASSERT_TRUE(sink.append_durable(make_accepted("HY-A", 1, 555), kT0 + 11'000).acked());
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].resulting_state, OrderState::Accepted);
    EXPECT_EQ(cps[0].unknown_since_utc_ms, 0) << "no longer uncertain: nothing to seed";
    OrderRecord rec = checkpoint_to_order_record(cps[0], kT0 + 60'000);
    EXPECT_EQ(rec.unknown_since_wall_ms, OrderRecord::kClockUnset);
    EXPECT_FALSE(rec.wall_anchor_recovered);
}

TEST_F(DurableAuditSinkTest, CheckpointToOrderRecordSeedsTheWallAnchorAndMarksItRecovered) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), kT0).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), kT0 + 1'500).acked());
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);

    OrderRecord rec = checkpoint_to_order_record(cps[0], kT0 + 60'000);
    EXPECT_EQ(rec.unknown_since_wall_ms, kT0 + 1'500);
    EXPECT_TRUE(rec.wall_anchor_recovered);
    EXPECT_EQ(rec.unknown_since_mono_ms, OrderRecord::kClockUnset)
        << "the monotonic clock starts over with the process: the tracker stamps that one";

    // No wall clock at the call site: only the lower bound is checked, the anchor is still seeded.
    OrderRecord no_clock = checkpoint_to_order_record(cps[0]);
    EXPECT_EQ(no_clock.unknown_since_wall_ms, kT0 + 1'500);
    EXPECT_TRUE(no_clock.wall_anchor_recovered);
}

TEST_F(DurableAuditSinkTest, FrameTimesThatAreNotWallTimesSeedNoAnchor) {
    {
        // The demo harnesses stamp their frames with a monotonic reading (small numbers).
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].unknown_since_utc_ms, 1001) << "the scan reports what the frame says ...";
    OrderRecord rec = checkpoint_to_order_record(cps[0], kT0);
    EXPECT_EQ(rec.unknown_since_wall_ms, OrderRecord::kClockUnset) << "... but it is not a wall time, so it seeds nothing";
    EXPECT_FALSE(rec.wall_anchor_recovered);
}

TEST_F(DurableAuditSinkTest, AFrameTimeFromTheFutureSeedsNoAnchor) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), kT0 + 3'600'000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), kT0 + 3'600'100).acked());
    }
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    auto cps = restarted.recovered_checkpoints();
    ASSERT_EQ(cps.size(), 1u);
    OrderRecord rec = checkpoint_to_order_record(cps[0], kT0);  // our wall clock says an hour earlier
    EXPECT_EQ(rec.unknown_since_wall_ms, OrderRecord::kClockUnset);
    EXPECT_FALSE(rec.wall_anchor_recovered);
}

// The recovery scan reads a frame's time through frame_wall_time_ms(). append_durable() only ever writes
// ServerCorrectedUtc frames, so the UnknownBootstrap branch cannot be reached end to end from here and is
// pinned on the function itself: a frame written while there was no trustworthy clock never seeds one.
TEST(FrameWallTimeTest, OnlyAServerCorrectedUtcFrameCarriesAWallTime) {
    EXPECT_EQ(frame_wall_time_ms(FrameTimeKind::ServerCorrectedUtc, kT0), kT0);
    EXPECT_EQ(frame_wall_time_ms(FrameTimeKind::UnknownBootstrap, kT0), 0)
        << "whatever an UnknownBootstrap frame's recorded time says, it is not a wall time";
    EXPECT_EQ(frame_wall_time_ms(FrameTimeKind::UnknownBootstrap, 0), 0);
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


// --- SAFE-03 crash-window slice (blueprint L-36): a torn tail is CUT, not skipped ---
//
// append_durable() appends at the PHYSICAL end of the log. Recovery used to skip a torn tail logically and leave
// the bytes on disk, so the first append after a crash-torn restart landed behind them, and the NEXT start read
// "torn prefix + the start of the new frame" as one frame: Corrupt, fenced. Recovery now cuts the tail -- once
// the anchor check has proven everything before it -- and discards the bytes (owner decision, 2026-10-14).

namespace {

std::string slurp_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void spit_file(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

const char* fence_reason_name(FenceReason r) {
    switch (r) {
        case FenceReason::None: return "None";
        case FenceReason::IoError: return "IoError";
        case FenceReason::DiskFull: return "DiskFull";
    }
    return "?";
}

const char* recovery_status_name(RecoveryScanStatus s) {
    switch (s) {
        case RecoveryScanStatus::Clean: return "Clean";
        case RecoveryScanStatus::Recovered: return "Recovered";
        case RecoveryScanStatus::Corrupt: return "Corrupt";
        case RecoveryScanStatus::CapacityExceeded: return "CapacityExceeded";
        case RecoveryScanStatus::IoError: return "IoError";
        case RecoveryScanStatus::ExternalAnchorUnavailable: return "ExternalAnchorUnavailable";
    }
    return "?";
}

// Why a sink refused an append. The bool alone cannot tell an I/O failure while appending (fenced, fence_reason
// IoError, recovery_status Clean -- on Windows typically a sharing violation from a scanner, see
// DurableAuditSinkTest::use_fresh_base_path()) from a failed recovery (recovery_status Corrupt / IoError): the two
// need different fixes.
std::string describe_sink(const DurableAuditSink& sink) {
    return std::string(" [is_open=") + (sink.is_open() ? "1" : "0") + " fenced=" + (sink.fenced() ? "1" : "0") +
           " fence_reason=" + fence_reason_name(sink.fence_reason()) +
           " recovery_status=" + recovery_status_name(sink.recovery_status()) + "]";
}

}  // namespace

TEST_F(DurableAuditSinkTest, EveryTornPrefixLengthIsCutAndTheLogKeepsGrowing) {
    // A crash in the middle of the second append leaves 1 .. kOrderEventFrameSize-1 bytes of it. Every one of
    // those lengths must be cut, the next append must land on the frame boundary, and the start after THAT must
    // still replay.
    for (std::size_t len = 1; len < kOrderEventFrameSize; ++len) {
        use_fresh_base_path();  // not remove_all(): see its comment (Windows: re-using names just deleted flakes)
        {
            DurableAuditSink sink(base_path_, *key_ring_, 1);
            ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked())
                << "len=" << len << describe_sink(sink);
        }
        append_prefix_of_a_real_frame(make_submitted("HY-A", 100, 10, 1), /*sequence_number=*/1, kZeroMac, len);
        ASSERT_EQ(log_file_size(), kOrderEventFrameSize + len) << "len=" << len;
        {
            DurableAuditSink restarted(base_path_, *key_ring_, 1);
            ASSERT_TRUE(restarted.is_open()) << "len=" << len;
            EXPECT_FALSE(restarted.fenced()) << "len=" << len;
            EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Clean) << "len=" << len;
            EXPECT_EQ(restarted.recovered_torn_tail_bytes(), len) << "len=" << len;
            EXPECT_EQ(restarted.recovered_intent_orphans(), 1u) << "len=" << len;
            // the continuation of the order: Submitting follows Intent, on the frame boundary
            ASSERT_TRUE(restarted.append_durable(make_submitted("HY-A", 100, 10, 1), 1002).acked())
                << "len=" << len << describe_sink(restarted);
        }
        EXPECT_EQ(log_file_size(), 2 * kOrderEventFrameSize) << "len=" << len << ": no debris between the frames";
        {
            DurableAuditSink again(base_path_, *key_ring_, 1);
            ASSERT_TRUE(again.is_open()) << "len=" << len;
            EXPECT_FALSE(again.fenced()) << "len=" << len << ": the log the cut tail left must replay";
            ASSERT_EQ(again.recovery_status(), RecoveryScanStatus::Recovered) << "len=" << len;
            EXPECT_EQ(again.recovered_torn_tail_bytes(), 0u) << "len=" << len;
            auto cps = again.recovered_checkpoints();
            ASSERT_EQ(cps.size(), 1u) << "len=" << len;
            EXPECT_EQ(cps[0].resulting_state, OrderState::Ambiguous) << "len=" << len;
        }
    }
}

TEST_F(DurableAuditSinkTest, AFirstFrameThatNeverCompletedIsCutAndTheLogStartsAfresh) {
    // The crash came during the very first append of the log's life: no complete frame, no anchor. The lengths
    // straddle every decoder threshold (under the 6 bytes of the key id, under the 27-byte header, mid-payload).
    for (const std::size_t len : {std::size_t{1}, std::size_t{5}, std::size_t{6}, std::size_t{26}, std::size_t{27},
                                  std::size_t{28}, std::size_t{100}, kOrderEventFrameSize - 1}) {
        use_fresh_base_path();
        append_prefix_of_a_real_frame(make_intent("HY-A", 100, 10, 1), /*sequence_number=*/0, kZeroMac, len);
        ASSERT_EQ(log_file_size(), len) << "len=" << len;
        {
            DurableAuditSink sink(base_path_, *key_ring_, 1);
            ASSERT_TRUE(sink.is_open()) << "len=" << len;
            EXPECT_FALSE(sink.fenced()) << "len=" << len;
            EXPECT_EQ(sink.recovery_status(), RecoveryScanStatus::Clean) << "len=" << len;
            EXPECT_EQ(sink.recovered_torn_tail_bytes(), len) << "len=" << len;
            ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked())
                << "len=" << len << describe_sink(sink);
        }
        EXPECT_EQ(log_file_size(), kOrderEventFrameSize) << "len=" << len << ": the first frame starts the file";
        DurableAuditSink again(base_path_, *key_ring_, 1);
        EXPECT_FALSE(again.fenced()) << "len=" << len;
        EXPECT_EQ(again.recovery_status(), RecoveryScanStatus::Clean) << "len=" << len;
        EXPECT_EQ(again.recovered_intent_orphans(), 1u) << "len=" << len;
    }
}

TEST_F(DurableAuditSinkTest, ATornFirstFrameBehindAnAnchorThatClaimsAFrameIsCorruptAndTheFileIsLeftAlone) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    // the log is replaced by a torn prefix of a first frame; the anchor still claims sequence 0
    std::remove(base_path_.c_str());
    append_prefix_of_a_real_frame(make_intent("HY-A", 100, 10, 1), 0, kZeroMac, 40);
    {
        DurableAuditSink restarted(base_path_, *key_ring_, 1);
        EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt);
        EXPECT_TRUE(restarted.fenced());
        EXPECT_EQ(restarted.recovered_torn_tail_bytes(), 0u);
    }
    EXPECT_EQ(log_file_size(), 40u) << "a log that fails the anchor check is evidence: not one byte is cut";
}

TEST_F(DurableAuditSinkTest, ACompleteButWrongFrameIsCorruptAndNothingBehindItIsCut) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }
    corrupt_log_byte(2 * kOrderEventFrameSize - 5);  // inside the second frame's MAC
    append_prefix_of_a_real_frame(make_accepted("HY-A", 1, 555), 2, kZeroMac, 30);
    ASSERT_EQ(log_file_size(), 2 * kOrderEventFrameSize + 30);
    {
        DurableAuditSink restarted(base_path_, *key_ring_, 1);
        EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt);
        EXPECT_TRUE(restarted.fenced());
        EXPECT_EQ(restarted.recovered_torn_tail_bytes(), 0u);
    }
    EXPECT_EQ(log_file_size(), 2 * kOrderEventFrameSize + 30) << "Corrupt: the file is left byte-for-byte alone";
}

TEST_F(DurableAuditSinkTest, ATruncateTheOsRefusesFencesTheSinkAndLeavesTheTailOnDisk) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
    }
    append_prefix_of_a_real_frame(make_submitted("HY-A", 100, 10, 1), 1, kZeroMac, 30);
    {
        TruncateFailureSeamGuard seam(1);
        DurableAuditSink refused(base_path_, *key_ring_, 1);
        ASSERT_TRUE(refused.is_open());
        EXPECT_EQ(refused.recovery_status(), RecoveryScanStatus::IoError);
        EXPECT_TRUE(refused.fenced()) << "the tail is still on disk: an append would land behind it";
        EXPECT_FALSE(refused.append_durable(make_submitted("HY-A", 100, 10, 1), 1002).acked());
        EXPECT_EQ(refused.recovered_torn_tail_bytes(), 0u);
        EXPECT_EQ(DurableLogStoreTestHooks::pending_truncate_failures(), 0u) << "the scan really tried to cut";
    }
    EXPECT_EQ(log_file_size(), kOrderEventFrameSize + 30);

    DurableAuditSink healed(base_path_, *key_ring_, 1);  // the seam is spent: a start with a working truncate cuts it
    EXPECT_FALSE(healed.fenced());
    EXPECT_EQ(healed.recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(healed.recovered_torn_tail_bytes(), 30u);
}

TEST_F(DurableAuditSinkTest, AnAnchorOneFrameBehindTheLogIsTheLateFsyncWindowAndHeals) {
    // append_durable() fsyncs the frame and THEN writes the anchor; a crash between the two leaves the anchor one
    // frame behind a perfectly good frame. This window was accepted but never tested.
    std::string anchor_after_0;
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        anchor_after_0 = slurp_file(base_path_ + ".tip");
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }
    spit_file(base_path_ + ".tip", anchor_after_0);
    {
        DurableAuditSink restarted(base_path_, *key_ring_, 1);
        EXPECT_FALSE(restarted.fenced());
        ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
        ASSERT_EQ(restarted.recovered_checkpoints().size(), 1u);
        EXPECT_EQ(restarted.recovered_checkpoints()[0].resulting_state, OrderState::Ambiguous);
        ASSERT_TRUE(restarted.append_durable(make_accepted("HY-A", 1, 555), 1002).acked()) << "the anchor catches up";
    }
    DurableAuditSink again(base_path_, *key_ring_, 1);
    EXPECT_FALSE(again.fenced());
    ASSERT_EQ(again.recovery_status(), RecoveryScanStatus::Recovered);
    ASSERT_EQ(again.recovered_checkpoints().size(), 1u);
    EXPECT_EQ(again.recovered_checkpoints()[0].resulting_state, OrderState::Accepted);
}

TEST_F(DurableAuditSinkTest, AnAnchorOneFrameBehindWhoseMacIsNotTheLogsIsStillCorrupt) {
    std::string foreign_anchor;
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-FOREIGN", 999, 99, 7), 1000).acked());
        foreign_anchor = slurp_file(base_path_ + ".tip");
    }
    remove_all();
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }
    spit_file(base_path_ + ".tip", foreign_anchor);  // sequence 0, but not THIS log's frame 0
    DurableAuditSink restarted(base_path_, *key_ring_, 1);
    EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt);
    EXPECT_TRUE(restarted.fenced());
}

TEST_F(DurableAuditSinkTest, AnAnchorOneFrameBehindTogetherWithATornTailIsBothHandled) {
    std::string anchor_after_0;
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        anchor_after_0 = slurp_file(base_path_ + ".tip");
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }
    spit_file(base_path_ + ".tip", anchor_after_0);
    append_prefix_of_a_real_frame(make_accepted("HY-A", 1, 555), 2, kZeroMac, 77);
    {
        DurableAuditSink restarted(base_path_, *key_ring_, 1);
        EXPECT_FALSE(restarted.fenced());
        ASSERT_EQ(restarted.recovery_status(), RecoveryScanStatus::Recovered);
        EXPECT_EQ(restarted.recovered_torn_tail_bytes(), 77u);
        ASSERT_TRUE(restarted.append_durable(make_accepted("HY-A", 1, 555), 1002).acked());
    }
    EXPECT_EQ(log_file_size(), 3 * kOrderEventFrameSize);
    DurableAuditSink again(base_path_, *key_ring_, 1);
    EXPECT_FALSE(again.fenced());
    EXPECT_EQ(again.recovery_status(), RecoveryScanStatus::Recovered);
}

TEST_F(DurableAuditSinkTest, ATornKeyRotatedFrameIsCutSoTheNextRotationStillWorks) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);
    WrappedKeyRecord rec3{};
    ASSERT_EQ(key_ring_->add_key(3, distinct_key(0x55), rec3), KeyRingAddStatus::Ok);
    WrappedKeyRecord rec4{};
    ASSERT_EQ(key_ring_->add_key(4, distinct_key(0x77), rec4), KeyRingAddStatus::Ok);

    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.rotate_active_key(2, 1500));
    }
    // a crash in the middle of the second rotation: 40 bytes of its KeyRotated frame reached the sidecar
    {
        std::array<std::byte, kKeyBlockSize> key3{};
        ASSERT_TRUE(key_ring_->active_key(3, key3));
        std::array<std::byte, kKeyRotatedFrameSize> frame{};
        const KeyRotationPayload payload{2, 3, 0};
        ASSERT_EQ(encode_key_rotated_frame(frame, 3, /*sequence=*/1, FrameTimeKind::ServerCorrectedUtc, 2000, payload,
                                           kZeroMac, std::span<const std::byte>(key3.data(), key3.size())),
                  kKeyRotatedFrameSize);
        std::ofstream f(base_path_ + ".keyrotations", std::ios::binary | std::ios::app);
        ASSERT_TRUE(f.is_open());
        f.write(reinterpret_cast<const char*>(frame.data()), 40);
    }
    {
        DurableAuditSink restarted(base_path_, *key_ring_, 2);
        ASSERT_TRUE(restarted.is_open());
        EXPECT_FALSE(restarted.rotation_fenced());
        EXPECT_EQ(restarted.recovered_rotation_torn_tail_bytes(), 40u);
        EXPECT_TRUE(restarted.rotate_active_key(3, 2000)) << "the next rotation lands on the frame boundary";
    }
    {
        DurableAuditSink third(base_path_, *key_ring_, 3);
        ASSERT_TRUE(third.is_open());
        EXPECT_FALSE(third.rotation_fenced()) << "the sidecar the cut tail left must replay";
        EXPECT_EQ(third.recovered_rotation_torn_tail_bytes(), 0u);
        ASSERT_TRUE(third.last_rotation().has_value());
        EXPECT_EQ(third.last_rotation()->new_key_id, 3u);
        EXPECT_TRUE(third.rotate_active_key(4, 3000));
    }
}

TEST_F(DurableAuditSinkTest, ARotationAnchorOneFrameBehindTheSidecarDoesNotFenceRotation) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);
    WrappedKeyRecord rec3{};
    ASSERT_EQ(key_ring_->add_key(3, distinct_key(0x55), rec3), KeyRingAddStatus::Ok);
    WrappedKeyRecord rec4{};
    ASSERT_EQ(key_ring_->add_key(4, distinct_key(0x77), rec4), KeyRingAddStatus::Ok);

    std::string sidecar_anchor_after_first_rotation;
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.rotate_active_key(2, 1500));
        sidecar_anchor_after_first_rotation = slurp_file(base_path_ + ".keyrotations.tip");
        ASSERT_TRUE(sink.rotate_active_key(3, 2000));
    }
    // rotate_active_key() fsyncs the sidecar frame and THEN writes the sidecar anchor: the crash window leaves it
    // one frame behind
    spit_file(base_path_ + ".keyrotations.tip", sidecar_anchor_after_first_rotation);
    {
        DurableAuditSink restarted(base_path_, *key_ring_, 3);
        ASSERT_TRUE(restarted.is_open());
        EXPECT_FALSE(restarted.rotation_fenced()) << "an anchor one frame behind is the window, not corruption";
        ASSERT_TRUE(restarted.last_rotation().has_value());
        EXPECT_EQ(restarted.last_rotation()->new_key_id, 3u);
        EXPECT_TRUE(restarted.rotate_active_key(4, 3000)) << "the chain continues from the frame the anchor missed";
    }
    DurableAuditSink again(base_path_, *key_ring_, 4);
    EXPECT_FALSE(again.rotation_fenced());
}

TEST_F(DurableAuditSinkTest, CutTornTailMapsEveryOutcomeToAScanStatus) {
    DurableLogStore store(base_path_, base_path_ + ".lock", base_path_ + ".tip");
    ASSERT_TRUE(store.acquire_lock());
    ASSERT_TRUE(store.open_log());
    std::vector<std::byte> bytes(13, std::byte{0x41});
    ASSERT_TRUE(store.append_and_fsync(bytes));  // 10 bytes of "frames" + 3 torn

    std::uint64_t dropped = 99;
    EXPECT_EQ(detail::cut_torn_tail(store, 13, 13, 5, dropped), RecoveryScanStatus::Clean) << "nothing to cut";
    EXPECT_EQ(dropped, 0u);
    EXPECT_EQ(detail::cut_torn_tail(store, 0, 13, 5, dropped), RecoveryScanStatus::Corrupt)
        << "13 unexplained bytes are more than any one torn append can leave: kept, and the log is Corrupt";
    EXPECT_EQ(store.log_size(), 13u);
    {
        TruncateFailureSeamGuard seam(1);
        EXPECT_EQ(detail::cut_torn_tail(store, 10, 13, 5, dropped), RecoveryScanStatus::IoError);
    }
    EXPECT_EQ(store.log_size(), 13u);
    EXPECT_EQ(detail::cut_torn_tail(store, 10, 13, 5, dropped), RecoveryScanStatus::Clean) << "cut";
    EXPECT_EQ(dropped, 3u);
    EXPECT_EQ(store.log_size(), 10u);
}

TEST_F(DurableAuditSinkTest, AnAnchorAheadOfTheLogTogetherWithATornTailIsCorruptAndNothingIsCut) {
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.append_durable(make_submitted("HY-A", 100, 10, 1), 1001).acked());
    }
    // the anchor claims sequence 1, but the second frame is gone and only a torn piece of it remains
    truncate_log_to(kOrderEventFrameSize + 30);
    {
        DurableAuditSink restarted(base_path_, *key_ring_, 1);
        EXPECT_EQ(restarted.recovery_status(), RecoveryScanStatus::Corrupt) << "tail deletion";
        EXPECT_TRUE(restarted.fenced());
        EXPECT_EQ(restarted.recovered_torn_tail_bytes(), 0u);
    }
    EXPECT_EQ(log_file_size(), kOrderEventFrameSize + 30) << "the anchor check failed first: the bytes are evidence";
}

TEST_F(DurableAuditSinkTest, ATornFirstKeyRotatedFrameIsCutToo) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);  // creates the (empty) sidecar
        ASSERT_TRUE(sink.is_open());
    }
    {
        std::array<std::byte, kKeyBlockSize> key2{};
        ASSERT_TRUE(key_ring_->active_key(2, key2));
        std::array<std::byte, kKeyRotatedFrameSize> frame{};
        const KeyRotationPayload payload{1, 2, 0};
        ASSERT_EQ(encode_key_rotated_frame(frame, 2, /*sequence=*/0, FrameTimeKind::ServerCorrectedUtc, 1500, payload,
                                           kZeroMac, std::span<const std::byte>(key2.data(), key2.size())),
                  kKeyRotatedFrameSize);
        std::ofstream f(base_path_ + ".keyrotations", std::ios::binary | std::ios::app);
        ASSERT_TRUE(f.is_open());
        f.write(reinterpret_cast<const char*>(frame.data()), 40);  // the very first rotation never completed
    }
    {
        DurableAuditSink restarted(base_path_, *key_ring_, 1);
        ASSERT_TRUE(restarted.is_open());
        EXPECT_FALSE(restarted.rotation_fenced());
        EXPECT_EQ(restarted.recovered_rotation_torn_tail_bytes(), 40u);
        EXPECT_TRUE(restarted.rotate_active_key(2, 2000)) << "the first rotation starts the sidecar file";
    }
    DurableAuditSink again(base_path_, *key_ring_, 2);
    EXPECT_FALSE(again.rotation_fenced());
    ASSERT_TRUE(again.last_rotation().has_value());
    EXPECT_EQ(again.last_rotation()->new_key_id, 2u);
}

TEST_F(DurableAuditSinkTest, ARotationAnchorOneFrameBehindWhoseMacIsNotTheSidecarsFencesRotation) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);
    WrappedKeyRecord rec3{};
    ASSERT_EQ(key_ring_->add_key(3, distinct_key(0x55), rec3), KeyRingAddStatus::Ok);
    WrappedKeyRecord rec4{};
    ASSERT_EQ(key_ring_->add_key(4, distinct_key(0x77), rec4), KeyRingAddStatus::Ok);

    std::string foreign_anchor;
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.rotate_active_key(2, 1500));  // history A: 1 -> 2
        foreign_anchor = slurp_file(base_path_ + ".keyrotations.tip");
    }
    remove_all();
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.rotate_active_key(3, 1500));  // history B: 1 -> 3 -> 4
        ASSERT_TRUE(sink.rotate_active_key(4, 2000));
    }
    spit_file(base_path_ + ".keyrotations.tip", foreign_anchor);  // sequence 0, but not THIS sidecar's frame 0
    DurableAuditSink restarted(base_path_, *key_ring_, 4);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_TRUE(restarted.rotation_fenced()) << "an anchor that is not the sidecar's is not the crash window";
}

namespace {

// A prefix of a real KeyRotated frame, appended to the key-rotation sidecar the way a crash in the middle of a
// rotation leaves it. The prefix is all that matters (the cut never looks inside it), so the chain MAC is zero.
void append_torn_key_rotated_frame(KeyRing& ring, const std::string& sidecar_path, std::uint32_t old_key_id,
                                   std::uint32_t new_key_id, std::uint64_t sequence, std::size_t len) {
    std::array<std::byte, kKeyBlockSize> key{};
    ASSERT_TRUE(ring.active_key(new_key_id, key));
    std::array<std::byte, kKeyRotatedFrameSize> frame{};
    const KeyRotationPayload payload{old_key_id, new_key_id, 0};
    ASSERT_EQ(encode_key_rotated_frame(frame, new_key_id, sequence, FrameTimeKind::ServerCorrectedUtc, 2000, payload,
                                       kZeroMac, std::span<const std::byte>(key.data(), key.size())),
              kKeyRotatedFrameSize);
    std::ofstream f(sidecar_path, std::ios::binary | std::ios::app);
    ASSERT_TRUE(f.is_open());
    f.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(len));
}

}  // namespace

TEST_F(DurableAuditSinkTest, AFirstFrameCutTheOsRefusesFencesTheSinkAndLeavesTheDebrisOnDisk) {
    append_prefix_of_a_real_frame(make_intent("HY-A", 100, 10, 1), /*sequence_number=*/0, kZeroMac, 40);
    {
        TruncateFailureSeamGuard seam(1);
        DurableAuditSink refused(base_path_, *key_ring_, 1);
        ASSERT_TRUE(refused.is_open());
        EXPECT_EQ(refused.recovery_status(), RecoveryScanStatus::IoError);
        EXPECT_TRUE(refused.fenced()) << "the debris is still on disk: the first append would land behind it";
        EXPECT_FALSE(refused.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        EXPECT_EQ(refused.recovered_torn_tail_bytes(), 0u);
        EXPECT_EQ(DurableLogStoreTestHooks::pending_truncate_failures(), 0u) << "the scan really tried to cut";
    }
    EXPECT_EQ(log_file_size(), 40u);

    DurableAuditSink healed(base_path_, *key_ring_, 1);  // the seam is spent: a start with a working truncate cuts it
    EXPECT_FALSE(healed.fenced());
    EXPECT_EQ(healed.recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(healed.recovered_torn_tail_bytes(), 40u);
}

TEST_F(DurableAuditSinkTest, ASidecarCutTheOsRefusesFencesRotationAndLeavesTheTailOnDisk) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);
    WrappedKeyRecord rec3{};
    ASSERT_EQ(key_ring_->add_key(3, distinct_key(0x55), rec3), KeyRingAddStatus::Ok);
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.append_durable(make_intent("HY-A", 100, 10, 1), 1000).acked());
        ASSERT_TRUE(sink.rotate_active_key(2, 1500));
    }
    ASSERT_NO_FATAL_FAILURE(append_torn_key_rotated_frame(*key_ring_, base_path_ + ".keyrotations", 2, 3, 1, 40));
    const std::size_t sidecar_size = slurp_file(base_path_ + ".keyrotations").size();
    {
        TruncateFailureSeamGuard seam(1);
        DurableAuditSink refused(base_path_, *key_ring_, 2);
        ASSERT_TRUE(refused.is_open());
        EXPECT_FALSE(refused.fenced()) << "the sidecar is its own durability domain: the main log is unaffected";
        EXPECT_TRUE(refused.rotation_fenced()) << "the tail is still on disk: a rotation would land behind it";
        EXPECT_FALSE(refused.rotate_active_key(3, 2000));
        EXPECT_EQ(refused.recovered_rotation_torn_tail_bytes(), 0u);
        EXPECT_EQ(DurableLogStoreTestHooks::pending_truncate_failures(), 0u) << "the scan really tried to cut";
    }
    EXPECT_EQ(slurp_file(base_path_ + ".keyrotations").size(), sidecar_size);

    DurableAuditSink healed(base_path_, *key_ring_, 2);  // the seam is spent: a start with a working truncate cuts it
    EXPECT_FALSE(healed.rotation_fenced());
    EXPECT_EQ(healed.recovered_rotation_torn_tail_bytes(), 40u);
    EXPECT_TRUE(healed.rotate_active_key(3, 2000));
}

TEST_F(DurableAuditSinkTest, AFirstSidecarFrameCutTheOsRefusesFencesRotationAndLeavesTheTailOnDisk) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);  // creates the (empty) sidecar
        ASSERT_TRUE(sink.is_open());
    }
    ASSERT_NO_FATAL_FAILURE(append_torn_key_rotated_frame(*key_ring_, base_path_ + ".keyrotations", 1, 2, 0, 40));
    {
        TruncateFailureSeamGuard seam(1);
        DurableAuditSink refused(base_path_, *key_ring_, 1);
        ASSERT_TRUE(refused.is_open());
        EXPECT_TRUE(refused.rotation_fenced());
        EXPECT_FALSE(refused.rotate_active_key(2, 2000));
        EXPECT_EQ(refused.recovered_rotation_torn_tail_bytes(), 0u);
        EXPECT_EQ(DurableLogStoreTestHooks::pending_truncate_failures(), 0u) << "the scan really tried to cut";
    }
    EXPECT_EQ(slurp_file(base_path_ + ".keyrotations").size(), 40u);

    DurableAuditSink healed(base_path_, *key_ring_, 1);
    EXPECT_FALSE(healed.rotation_fenced());
    EXPECT_EQ(healed.recovered_rotation_torn_tail_bytes(), 40u);
}

TEST_F(DurableAuditSinkTest, AForeignSidecarAnchorTogetherWithATornSidecarTailFencesRotationAndCutsNothing) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);
    WrappedKeyRecord rec3{};
    ASSERT_EQ(key_ring_->add_key(3, distinct_key(0x55), rec3), KeyRingAddStatus::Ok);
    WrappedKeyRecord rec4{};
    ASSERT_EQ(key_ring_->add_key(4, distinct_key(0x77), rec4), KeyRingAddStatus::Ok);

    std::string foreign_anchor;
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.rotate_active_key(2, 1500));  // history A: 1 -> 2
        foreign_anchor = slurp_file(base_path_ + ".keyrotations.tip");
    }
    remove_all();
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.rotate_active_key(3, 1500));  // history B: 1 -> 3 -> 4
        ASSERT_TRUE(sink.rotate_active_key(4, 2000));
    }
    spit_file(base_path_ + ".keyrotations.tip", foreign_anchor);  // sequence 0, but not THIS sidecar's frame 0
    ASSERT_NO_FATAL_FAILURE(append_torn_key_rotated_frame(*key_ring_, base_path_ + ".keyrotations", 4, 2, 2, 33));
    const std::size_t sidecar_size = slurp_file(base_path_ + ".keyrotations").size();
    {
        DurableAuditSink restarted(base_path_, *key_ring_, 4);
        ASSERT_TRUE(restarted.is_open());
        EXPECT_TRUE(restarted.rotation_fenced());
        EXPECT_EQ(restarted.recovered_rotation_torn_tail_bytes(), 0u);
    }
    EXPECT_EQ(slurp_file(base_path_ + ".keyrotations").size(), sidecar_size)
        << "a sidecar that fails the anchor check is evidence: not one byte is cut";
}

TEST_F(DurableAuditSinkTest, ASidecarAnchorAtTheTipWhoseMacIsNotTheSidecarsFencesRotation) {
    WrappedKeyRecord rec2{};
    ASSERT_EQ(key_ring_->add_key(2, distinct_key(0xFF), rec2), KeyRingAddStatus::Ok);
    WrappedKeyRecord rec3{};
    ASSERT_EQ(key_ring_->add_key(3, distinct_key(0x55), rec3), KeyRingAddStatus::Ok);

    std::string foreign_anchor;
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.rotate_active_key(2, 1500));  // history A: 1 -> 2
        foreign_anchor = slurp_file(base_path_ + ".keyrotations.tip");
    }
    remove_all();
    {
        DurableAuditSink sink(base_path_, *key_ring_, 1);
        ASSERT_TRUE(sink.rotate_active_key(3, 1500));  // history B: 1 -> 3
    }
    spit_file(base_path_ + ".keyrotations.tip", foreign_anchor);  // sequence 0 like B's tip, but not B's frame 0
    DurableAuditSink restarted(base_path_, *key_ring_, 3);
    ASSERT_TRUE(restarted.is_open());
    EXPECT_TRUE(restarted.rotation_fenced()) << "a genuine anchor of another history is not this sidecar's anchor";
}
