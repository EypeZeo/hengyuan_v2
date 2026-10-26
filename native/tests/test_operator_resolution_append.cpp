// SPDX-License-Identifier: proprietary
// test_operator_resolution_append.cpp -- D5 slice R3-0: the WRITER side of the operator's way back
// (docs/OPERATOR_RESOLUTION_DESIGN.md 6 step 7 and 10).
//
// R1 taught the recovery scan to read an OrderOperatorResolved frame; R3-0 adds the one call that WRITES it
// (DurableAuditSink::append_operator_resolution()), the two checkpoint facts the offline tool needs (mode, send start
// time) and the shared rule both sides apply (operator_resolution_rules.hpp). Unlike R1's tests, which had to CRAFT logs
// because nothing could write a v6 frame, these use the real writer end to end: a trading sink lays down an order's
// history, an operator sink resolves it, a trading sink reads it back -- and a refusal is only a pass if it changed
// nothing on disk, did not fence the sink and left the write version alone.
//
// The places that still craft frames are the ones the real writer cannot (or should not have to) produce: frames with no
// wall time (UnknownBootstrap), the differential test, which lays the very same record down as a frame of a crafted log to
// see what the READER makes of it, and the capacity stores with a thousand resolved orders.
//
// A scenario that reopens the same store many times in one test uses a fresh file name each time (see the fixture): deleting
// a log and creating a file of the same name a moment later races a real-time antivirus scanner on a Windows host
// (docs/SPEC_INVARIANTS.md; DurableAuditSinkTest::use_fresh_base_path).

#include <gtest/gtest.h>
#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/operator_resolution_frame.hpp>
#include <hengyuan/operator_resolution_rules.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __linux__
#include <unistd.h>
#endif

using namespace hy;

namespace {

constexpr std::uint32_t kKeyId = 1;

std::vector<std::byte> plaintext_key() {
    static const char kKey[] = "operator-resolution-append-test-key";
    std::vector<std::byte> k(sizeof(kKey) - 1);
    std::memcpy(k.data(), kKey, k.size());
    return k;
}

// --- orders and their frames ----------------------------------------------------------------------------------------

struct OrderSpec {
    std::string coid;
    std::uint32_t symbol_id{7};
    OrderSide side{OrderSide::Sell};
    std::int64_t price{500'000};
    std::int64_t qty{100};
    ExecutionMode mode{ExecutionMode::Live};
};

AuditRecord make_record(const OrderSpec& o, AuditEventType ev, OrderState st) {
    AuditRecord r{};
    r.timestamp_ms = 1000;
    r.event_type = ev;
    r.mode = o.mode;
    r.symbol_id = o.symbol_id;
    r.set_client_order_id(o.coid);
    r.price_ticks = o.price;
    r.qty_ticks = o.qty;
    r.resulting_state = st;
    r.side = o.side;
    return r;
}

AuditRecord intent(const OrderSpec& o) { return make_record(o, AuditEventType::OrderIntentCreated, OrderState::Intent); }
AuditRecord prepared(const OrderSpec& o) {
    return make_record(o, AuditEventType::OrderSubmitPrepared, OrderState::Submitting);
}
AuditRecord ambiguous(const OrderSpec& o) { return make_record(o, AuditEventType::OrderAmbiguous, OrderState::Ambiguous); }
AuditRecord escalated(const OrderSpec& o) {
    return make_record(o, AuditEventType::OrderEscalated, OrderState::EscalatedToOperator);
}
AuditRecord accepted(const OrderSpec& o, std::int64_t exchange_id = 77) {
    AuditRecord r = make_record(o, AuditEventType::OrderAccepted, OrderState::Accepted);
    r.exchange_order_id = exchange_id;
    return r;
}
AuditRecord rejected_by_exchange(const OrderSpec& o) {
    return make_record(o, AuditEventType::OrderRejected, OrderState::Rejected);
}

// The machine field of a well-formed ABSENT conclusion, with the two informational fields set to something.
constexpr std::int64_t good_detail_code(std::uint64_t waited_s = 270, std::uint64_t tool_version = 1) {
    return static_cast<std::int64_t>(operator_resolution::kKindAbsent | operator_resolution::kAbsentRequiredBits |
                                     (waited_s << operator_resolution::kWaitedSecondsShift) |
                                     (tool_version << operator_resolution::kToolVersionShift));
}

AuditRecord operator_resolved(const OrderSpec& o, std::int64_t code = good_detail_code()) {
    AuditRecord r = make_record(o, AuditEventType::OrderOperatorResolved, OrderState::Rejected);
    r.detail_code = code;
    r.set_detail("op=test;ev=00000000000000000000000000000000");
    return r;
}

struct Timed {
    AuditRecord rec;
    std::int64_t at_ms;
};

// Intent + Prepared (the order is Submitting), then Ambiguous, then EscalatedToOperator: the three states the tool resolves.
std::vector<Timed> submitting_history(const OrderSpec& o, std::int64_t t0 = 1000) {
    return {{intent(o), t0}, {prepared(o), t0 + 1000}};
}
std::vector<Timed> ambiguous_history(const OrderSpec& o, std::int64_t t0 = 1000) {
    std::vector<Timed> v = submitting_history(o, t0);
    v.push_back({ambiguous(o), t0 + 2000});
    return v;
}
std::vector<Timed> escalated_history(const OrderSpec& o, std::int64_t t0 = 1000) {
    std::vector<Timed> v = ambiguous_history(o, t0);
    v.push_back({escalated(o), t0 + 3000});
    return v;
}

// --- crafted logs (only where the real writer cannot make the frame) -------------------------------------------------------

struct CraftedFrame {
    std::uint8_t version;
    AuditRecord rec;
    FrameTimeKind kind;
    std::int64_t recorded_ms;
};

struct Crafted {
    std::vector<std::byte> log;
    std::array<std::byte, kTipAnchorSize> anchor{};
    bool ok{false};
};

Crafted craft(const KeyRing& ring, const std::vector<CraftedFrame>& frames) {
    Crafted c;
    std::array<std::byte, kKeyBlockSize> key_block{};
    if (!ring.active_key(kKeyId, key_block)) return c;
    const std::span<const std::byte> key(key_block.data(), key_block.size());
    std::array<std::byte, kMacLen> prev{};
    std::uint64_t seq = 0;
    for (const CraftedFrame& f : frames) {
        std::array<std::byte, kOrderEventFrameSize> buf{};
        const auto n = encode_order_event_frame(buf, kKeyId, seq, f.kind, f.recorded_ms, f.rec, prev, key, f.version);
        if (n != kOrderEventFrameSize) return c;
        std::memcpy(prev.data(), buf.data() + kOrderEventFrameSize - kMacLen, kMacLen);
        c.log.insert(c.log.end(), buf.begin(), buf.end());
        ++seq;
    }
    if (!frames.empty()) detail::encode_tip_anchor(c.anchor, seq - 1, prev, kKeyId, key);
    c.ok = true;
    return c;
}

CraftedFrame v5(const AuditRecord& r, std::int64_t at_ms) {
    return {kFrameFormatVersion, r, FrameTimeKind::ServerCorrectedUtc, at_ms};
}
CraftedFrame v5_unknown_time(const AuditRecord& r) {
    return {kFrameFormatVersion, r, FrameTimeKind::UnknownBootstrap, 0};
}
CraftedFrame v6(const AuditRecord& r, std::int64_t at_ms) {
    return {kFrameFormatVersionV6, r, FrameTimeKind::ServerCorrectedUtc, at_ms};
}

// The names of the orders of store_with(): HY-OPCAP-00000, HY-OPCAP-00001, ...
OrderSpec capacity_order(std::size_t i) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "HY-OPCAP-%05zu", i);
    return OrderSpec{std::string(buf)};
}

// A crafted store with `resolved` Live orders already closed by an operator frame and `unresolved` Ambiguous ones after them
// (so unresolved order k is capacity_order(resolved + k), and it is checkpoint k). Every v5 frame comes before the first v6
// one: the version of a store never goes back.
std::vector<CraftedFrame> store_with(std::size_t resolved, std::size_t unresolved) {
    std::vector<CraftedFrame> frames;
    std::int64_t t = 1000;
    for (std::size_t i = 0; i < resolved + unresolved; ++i) {
        const OrderSpec o = capacity_order(i);
        frames.push_back(v5(intent(o), t += 10));
        frames.push_back(v5(prepared(o), t += 10));
        frames.push_back(v5(ambiguous(o), t += 10));
    }
    for (std::size_t i = 0; i < resolved; ++i) frames.push_back(v6(operator_resolved(capacity_order(i)), t += 10));
    return frames;
}

}  // namespace

class OperatorAppendTest : public ::testing::Test {
protected:
    std::string stem_;
    std::string base_path_;
    unsigned used_{0};
    std::unique_ptr<KeyRing> ring_;

    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        stem_ = std::string(tmp) + "hy_opapp_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter);
#else
        stem_ = "/tmp/hy_opapp_" + std::to_string(getpid()) + "_" + std::to_string(counter);
#endif
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x40 + i);
        ring_ = std::make_unique<KeyRing>(kek);
        WrappedKeyRecord rec{};
        ASSERT_EQ(ring_->add_key(kKeyId, plaintext_key(), rec), KeyRingAddStatus::Ok);
    }

    void TearDown() override { remove_all(); }

    void remove_all() {
        if (base_path_.empty()) return;
        for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".keyrotations", ".keyrotations.lock",
                                   ".keyrotations.tip", ".keyrotations.tip.tmp", ".storeid", ".storeid.lock",
                                   ".storeid.tip", ".storeid.tip.tmp"}) {
            std::remove((base_path_ + suffix).c_str());
        }
    }

    // A store of its own for every scenario (see the file header): the previous one is deleted, the name never reused.
    void fresh_path() {
        remove_all();
        base_path_ = stem_ + "_" + std::to_string(++used_) + ".log";
    }

    std::unique_ptr<DurableAuditSink> open(AuditSinkOpenMode mode) {
        return std::make_unique<DurableAuditSink>(base_path_, *ring_, kKeyId, kMaxIntentOrphans, mode);
    }

    // Lays an order history down on a fresh path with the real writer (a trading sink), then closes it.
    void write_history(const std::vector<Timed>& frames) {
        fresh_path();
        DurableAuditSink sink(base_path_, *ring_, kKeyId);
        ASSERT_TRUE(sink.is_open());
        ASSERT_FALSE(sink.fenced());
        for (const Timed& f : frames) {
            ASSERT_TRUE(sink.append_durable(f.rec, f.at_ms).acked()) << f.rec.client_order_id;
        }
    }

    // The same, for frames the real writer cannot make or for a log that must carry a v6 frame the test did not append.
    void write_crafted(const std::vector<CraftedFrame>& frames) {
        const Crafted c = craft(*ring_, frames);
        ASSERT_TRUE(c.ok) << "the crafted log could not be built";
        fresh_path();
        DurableLogStore store(base_path_, base_path_ + ".lock", base_path_ + ".tip");
        ASSERT_TRUE(store.acquire_lock());
        ASSERT_TRUE(store.open_log());
        ASSERT_TRUE(store.append_and_fsync(std::span<const std::byte>(c.log.data(), c.log.size())));
        ASSERT_TRUE(store.write_tip_anchor(std::span<const std::byte>(c.anchor.data(), c.anchor.size())));
        store.close_log();
        store.release_lock();
    }

    static std::vector<std::byte> file_bytes(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::byte> out(raw.size());
        if (!raw.empty()) std::memcpy(out.data(), raw.data(), raw.size());
        return out;
    }

    static const OrderRecoveryCheckpoint* find_checkpoint(const DurableAuditSink& sink, std::string_view coid) {
        for (const OrderRecoveryCheckpoint& cp : sink.recovered_checkpoints()) {
            if (cp.client_order_id.view() == coid) return &cp;
        }
        return nullptr;
    }

    // What one append_operator_resolution() attempt on a freshly opened sink did, and whether it left the disk alone.
    struct Attempt {
        OperatorResolutionAppend result{};
        bool fenced{false};
        std::uint8_t write_version{0};
        bool resolved_flag{false};
        bool unchanged_on_disk{false};
    };

    Attempt attempt(const AuditRecord& rec, AuditSinkOpenMode mode = AuditSinkOpenMode::OperatorOpen,
                    std::int64_t now_ms = 9000) {
        Attempt a;
        const std::vector<std::byte> log_before = file_bytes(base_path_);
        const std::vector<std::byte> tip_before = file_bytes(base_path_ + ".tip");
        {
            const auto sink = open(mode);
            a.result = sink->append_operator_resolution(rec, now_ms);
            a.fenced = sink->fenced();
            a.write_version = sink->write_format_version();
            a.resolved_flag = sink->operator_resolved_this_session(0);
        }
        a.unchanged_on_disk = file_bytes(base_path_) == log_before && file_bytes(base_path_ + ".tip") == tip_before;
        return a;
    }

    // A refusal made before anything touched the log: the status, no fence, the version the store had (5 unless it already
    // held a v6 frame), no flag, not a byte changed.
    void expect_refused_untouched(const Attempt& a, OperatorAppendStatus status, const char* why,
                                  std::uint8_t store_version = kFrameFormatVersion) {
        EXPECT_EQ(a.result.status, status) << why;
        EXPECT_FALSE(a.result.append.acked()) << why;
        EXPECT_FALSE(a.fenced) << why << ": a refusal is the caller's mistake, not damage to the store";
        EXPECT_EQ(a.write_version, store_version) << why;
        EXPECT_FALSE(a.resolved_flag) << why;
        EXPECT_TRUE(a.unchanged_on_disk) << why << ": a refused call must not write a byte";
    }
};

namespace {
const OrderSpec kA{"HY-OPAPP-A"};
const OrderSpec kB{"HY-OPAPP-B"};
const OrderSpec kC{"HY-OPAPP-C"};
}  // namespace

// --- the two facts the checkpoint now carries ---------------------------------------------------------------------------------

TEST_F(OperatorAppendTest, TheCheckpointCarriesTheModeAndTheTimeTheSendStarted) {
    OrderSpec dry = kC;
    dry.mode = ExecutionMode::DryRun;
    // interleaved on purpose: every fact must be attributed to its own client order id
    write_history({{intent(kA), 1000},
                   {intent(kB), 1100},
                   {intent(dry), 1150},
                   {prepared(kA), 2000},
                   {prepared(kB), 2100},
                   {prepared(dry), 2150},
                   {ambiguous(kA), 3000},
                   {ambiguous(dry), 3100}});
    const auto sink = open(AuditSinkOpenMode::OperatorOpen);
    ASSERT_EQ(sink->recovered_checkpoints().size(), 3u);

    const OrderRecoveryCheckpoint* a = find_checkpoint(*sink, kA.coid);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->mode, ExecutionMode::Live);
    EXPECT_EQ(a->send_started_utc_ms, 2000) << "the Prepared frame, not the Ambiguous one that came after the send";
    EXPECT_EQ(a->unknown_since_utc_ms, 3000) << "unchanged: the first uncertain moment is the Ambiguous frame";
    EXPECT_EQ(a->resulting_state, OrderState::Ambiguous);

    const OrderRecoveryCheckpoint* b = find_checkpoint(*sink, kB.coid);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->mode, ExecutionMode::Live);
    EXPECT_EQ(b->send_started_utc_ms, 2100);
    EXPECT_EQ(b->unknown_since_utc_ms, 2100) << "an order whose last frame is Submitting is recovered as Ambiguous since then";
    EXPECT_EQ(b->resulting_state, OrderState::Ambiguous);

    const OrderRecoveryCheckpoint* c = find_checkpoint(*sink, dry.coid);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->mode, ExecutionMode::DryRun);
    EXPECT_EQ(c->send_started_utc_ms, 2150);
}

TEST_F(OperatorAppendTest, ARepeatedSubmittingFrameDoesNotMoveTheSendStart) {
    write_history({{intent(kA), 1000}, {prepared(kA), 2000}, {prepared(kA), 2500}, {ambiguous(kA), 3000}});
    const auto sink = open(AuditSinkOpenMode::OperatorOpen);
    const OrderRecoveryCheckpoint* a = find_checkpoint(*sink, kA.coid);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->send_started_utc_ms, 2000) << "the FIRST frame that put the order in Submitting";
}

TEST_F(OperatorAppendTest, ASendStartWithoutAServerCorrectedTimeIsZeroAndStaysZero) {
    // The first Submitting frame of A carries no wall time (UnknownBootstrap); a later server-corrected repeat must not
    // supply one. B's first Submitting frame is fine and only its Ambiguous one has no time.
    write_crafted({v5(intent(kA), 1000), v5_unknown_time(prepared(kA)), v5(prepared(kA), 2500), v5(ambiguous(kA), 3000),
                   v5(intent(kB), 1100), v5(prepared(kB), 2100), v5_unknown_time(ambiguous(kB))});
    const auto sink = open(AuditSinkOpenMode::OperatorOpen);
    ASSERT_EQ(sink->recovered_checkpoints().size(), 2u);
    const OrderRecoveryCheckpoint* a = find_checkpoint(*sink, kA.coid);
    const OrderRecoveryCheckpoint* b = find_checkpoint(*sink, kB.coid);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->send_started_utc_ms, 0) << "no trustworthy time for the send: none, never a guess";
    EXPECT_EQ(b->send_started_utc_ms, 2100);
}

TEST_F(OperatorAppendTest, TheDefaultCheckpointIsADryRunWithNoSendTime) {
    const OrderRecoveryCheckpoint cp{};
    EXPECT_EQ(cp.mode, ExecutionMode::DryRun) << "a checkpoint nobody filled in is refused, not resolved";
    EXPECT_EQ(cp.send_started_utc_ms, 0);
}

// --- what a successful append does -----------------------------------------------------------------------------------------

namespace {
struct HistoryCase {
    const char* name;
    std::vector<Timed> (*make)(const OrderSpec&, std::int64_t);
};
const HistoryCase kHistories[] = {
    {"Submitting after Prepared", submitting_history},
    {"Ambiguous", ambiguous_history},
    {"EscalatedToOperator", escalated_history},
};
}  // namespace

TEST_F(OperatorAppendTest, ResolvesAnOrderInEachOfTheThreeStatesAndTheNextStartAcceptsIt) {
    for (const HistoryCase& h : kHistories) {
        SCOPED_TRACE(h.name);
        const std::vector<Timed> history = h.make(kA, 1000);
        write_history(history);
        const std::vector<std::byte> before = file_bytes(base_path_);
        ASSERT_EQ(before.size(), history.size() * kOrderEventFrameSize);

        {
            const auto sink = open(AuditSinkOpenMode::OperatorOpen);
            ASSERT_EQ(sink->recovered_checkpoints().size(), 1u);
            EXPECT_EQ(sink->write_format_version(), kFrameFormatVersion);
            const OperatorResolutionAppend r = sink->append_operator_resolution(operator_resolved(kA), 5000);
            ASSERT_EQ(r.status, OperatorAppendStatus::Appended);
            EXPECT_TRUE(r.append.acked());
            EXPECT_EQ(r.append.sequence, history.size());
            EXPECT_EQ(sink->write_format_version(), kFrameFormatVersionV6) << "the store's tip is v6 now";
            EXPECT_FALSE(sink->fenced());
            EXPECT_TRUE(sink->operator_resolved_this_session(0));
            EXPECT_EQ(sink->recovered_checkpoints().size(), 1u) << "the recovered list is never edited under a caller";
        }

        // on disk: exactly one more frame, the old bytes untouched, and the frame says what it should
        const std::vector<std::byte> after = file_bytes(base_path_);
        ASSERT_EQ(after.size(), before.size() + kOrderEventFrameSize);
        EXPECT_TRUE(std::equal(before.begin(), before.end(), after.begin())) << "append-only";
        std::array<std::byte, kKeyBlockSize> key_block{};
        ASSERT_TRUE(ring_->active_key(kKeyId, key_block));
        DecodedOrderFrame frame{};
        std::size_t frame_size = 0;
        ASSERT_EQ(decode_order_event_frame(std::span<const std::byte>(after).subspan(before.size()),
                                           std::span<const std::byte>(key_block.data(), key_block.size()), frame, frame_size),
                  FrameDecodeStatus::Ok);
        EXPECT_EQ(frame.format_version, kFrameFormatVersionV6);
        EXPECT_EQ(frame.record.event_type, AuditEventType::OrderOperatorResolved);
        EXPECT_EQ(frame.record.resulting_state, OrderState::Rejected);
        EXPECT_EQ(frame.record.detail_code, good_detail_code());
        EXPECT_EQ(frame.recorded_utc_ms, 5000);
        EXPECT_EQ(frame.time_kind, FrameTimeKind::ServerCorrectedUtc);

        // the next start (a TRADING open) accepts the store, sees the order as final, and keeps writing v6
        {
            const auto sink = open(AuditSinkOpenMode::Trading);
            EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
            EXPECT_FALSE(sink->fenced());
            EXPECT_TRUE(sink->recovered_checkpoints().empty()) << "a resolved order is final";
            ASSERT_EQ(sink->recovered_operator_resolved().size(), 1u);
            EXPECT_STREQ(sink->recovered_operator_resolved()[0].client_order_id.id, kA.coid.c_str());
            EXPECT_EQ(sink->recovered_operator_resolved()[0].frame_sequence, history.size());
            EXPECT_EQ(sink->write_format_version(), kFrameFormatVersionV6);
        }
    }
}

TEST_F(OperatorAppendTest, AfterTheOperatorFrameTheTradingProcessKeepsWritingAndTheStoreStaysReadable) {
    // The point of "the writer follows the tip": the operator's first v6 frame, then the trading process's own appends.
    // If those were v5 the NEXT start would read a v5 frame after a v6 one and call the store Corrupt.
    write_history(ambiguous_history(kA));
    {
        const auto sink = open(AuditSinkOpenMode::OperatorOpen);
        ASSERT_EQ(sink->append_operator_resolution(operator_resolved(kA), 5000).status, OperatorAppendStatus::Appended);
    }
    {
        const auto sink = open(AuditSinkOpenMode::Trading);
        ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
        ASSERT_EQ(sink->write_format_version(), kFrameFormatVersionV6);
        ASSERT_TRUE(sink->append_durable(intent(kB), 6000).acked());
        ASSERT_TRUE(sink->append_durable(prepared(kB), 6100).acked());
        ASSERT_TRUE(sink->append_durable(ambiguous(kB), 6200).acked());
    }
    {
        const auto sink = open(AuditSinkOpenMode::Trading);
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered) << "B is unresolved, A is not";
        EXPECT_FALSE(sink->fenced());
        ASSERT_EQ(sink->recovered_checkpoints().size(), 1u);
        EXPECT_STREQ(sink->recovered_checkpoints()[0].client_order_id.id, kB.coid.c_str());
        EXPECT_EQ(sink->recovered_operator_resolved().size(), 1u);
    }
    // ...and the operator can come back for B in a later session
    {
        const auto sink = open(AuditSinkOpenMode::OperatorOpen);
        EXPECT_EQ(sink->append_operator_resolution(operator_resolved(kB), 7000).status, OperatorAppendStatus::Appended);
    }
    {
        const auto sink = open(AuditSinkOpenMode::Trading);
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
        EXPECT_EQ(sink->recovered_operator_resolved().size(), 2u);
        EXPECT_TRUE(sink->recovered_checkpoints().empty());
    }
}

TEST_F(OperatorAppendTest, SeveralOrdersCanBeResolvedInOneSession) {
    std::vector<Timed> frames = ambiguous_history(kA, 1000);
    for (const Timed& t : escalated_history(kB, 1100)) frames.push_back(t);
    for (const Timed& t : submitting_history(kC, 1200)) frames.push_back(t);
    write_history(frames);
    {
        const auto sink = open(AuditSinkOpenMode::OperatorOpen);
        ASSERT_EQ(sink->recovered_checkpoints().size(), 3u);
        const OperatorResolutionAppend r1 = sink->append_operator_resolution(operator_resolved(kB), 5000);
        const OperatorResolutionAppend r2 = sink->append_operator_resolution(operator_resolved(kA), 5100);
        const OperatorResolutionAppend r3 = sink->append_operator_resolution(operator_resolved(kC), 5200);
        ASSERT_EQ(r1.status, OperatorAppendStatus::Appended);
        ASSERT_EQ(r2.status, OperatorAppendStatus::Appended);
        ASSERT_EQ(r3.status, OperatorAppendStatus::Appended);
        EXPECT_EQ(r2.append.sequence, r1.append.sequence + 1);
        EXPECT_EQ(r3.append.sequence, r2.append.sequence + 1);
        for (std::size_t i = 0; i < 3; ++i) EXPECT_TRUE(sink->operator_resolved_this_session(i)) << i;
        EXPECT_FALSE(sink->operator_resolved_this_session(3)) << "an index past the list is not resolved";
        EXPECT_FALSE(sink->operator_resolved_this_session(kMaxInFlight));
    }
    const auto sink = open(AuditSinkOpenMode::Trading);
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
    ASSERT_EQ(sink->recovered_operator_resolved().size(), 3u);
    EXPECT_STREQ(sink->recovered_operator_resolved()[0].client_order_id.id, kB.coid.c_str()) << "log order";
    EXPECT_STREQ(sink->recovered_operator_resolved()[1].client_order_id.id, kA.coid.c_str());
    EXPECT_STREQ(sink->recovered_operator_resolved()[2].client_order_id.id, kC.coid.c_str());
    EXPECT_TRUE(sink->recovered_checkpoints().empty());
}

TEST_F(OperatorAppendTest, ASecondResolutionOfTheSameOrderInOneSessionIsRefused) {
    write_history(ambiguous_history(kA));
    const auto sink = open(AuditSinkOpenMode::OperatorOpen);
    ASSERT_EQ(sink->append_operator_resolution(operator_resolved(kA), 5000).status, OperatorAppendStatus::Appended);
    const OperatorResolutionAppend again = sink->append_operator_resolution(operator_resolved(kA), 5100);
    EXPECT_EQ(again.status, OperatorAppendStatus::AlreadyResolvedThisSession);
    EXPECT_FALSE(again.append.acked());
    EXPECT_FALSE(sink->fenced());
}

// --- every refusal: decided before the log is touched ------------------------------------------------------------------------

TEST_F(OperatorAppendTest, ATradingOpenRefusesTheOperatorAppend) {
    write_history(ambiguous_history(kA));
    expect_refused_untouched(attempt(operator_resolved(kA), AuditSinkOpenMode::Trading), OperatorAppendStatus::NotOperatorOpen,
                             "a trading open");
}

TEST_F(OperatorAppendTest, OnlyTheOperatorEventIsAccepted) {
    write_history(ambiguous_history(kA));
    for (const AuditEventType other :
         {AuditEventType::OrderRejected, AuditEventType::OrderReconciled, AuditEventType::OrderEscalated,
          AuditEventType::OrderAmbiguous, AuditEventType::OrderSubmitPrepared, AuditEventType::OrderIntentCreated}) {
        AuditRecord r = operator_resolved(kA);
        r.event_type = other;
        expect_refused_untouched(attempt(r), OperatorAppendStatus::NotTheOperatorEvent, "another event type");
    }
}

TEST_F(OperatorAppendTest, AnOrderThatIsNotUnresolvedIsNotThere) {
    write_history(ambiguous_history(kA));
    expect_refused_untouched(attempt(operator_resolved(OrderSpec{"HY-OPAPP-NOPE"})), OperatorAppendStatus::NoSuchUnresolvedOrder,
                             "an id the log has never seen");
    AuditRecord empty = operator_resolved(kA);
    std::memset(empty.client_order_id, 0, sizeof(empty.client_order_id));
    expect_refused_untouched(attempt(empty), OperatorAppendStatus::NoSuchUnresolvedOrder, "an empty id");
    AuditRecord unterminated = operator_resolved(kA);
    std::memset(unterminated.client_order_id, 'A', sizeof(unterminated.client_order_id));
    expect_refused_untouched(attempt(unterminated), OperatorAppendStatus::NoSuchUnresolvedOrder, "an id with no terminator");
    AuditRecord prefix = operator_resolved(kA);
    prefix.set_client_order_id("HY-OPAPP-");
    expect_refused_untouched(attempt(prefix), OperatorAppendStatus::NoSuchUnresolvedOrder, "a prefix of an id");
    AuditRecord longer = operator_resolved(kA);
    longer.set_client_order_id("HY-OPAPP-AA");
    expect_refused_untouched(attempt(longer), OperatorAppendStatus::NoSuchUnresolvedOrder, "an extension of an id");

    write_history({{intent(kA), 1000}});
    expect_refused_untouched(attempt(operator_resolved(kA)), OperatorAppendStatus::NoSuchUnresolvedOrder,
                             "an Intent-only orphan: nothing was ever prepared, let alone sent");
    write_history({{intent(kA), 1000}, {prepared(kA), 2000}, {rejected_by_exchange(kA), 3000}});
    expect_refused_untouched(attempt(operator_resolved(kA)), OperatorAppendStatus::NoSuchUnresolvedOrder,
                             "an order that is already final");
}

TEST_F(OperatorAppendTest, ADryRunOrderIsNotResolved) {
    OrderSpec dry = kA;
    dry.mode = ExecutionMode::DryRun;
    write_history(ambiguous_history(dry));
    expect_refused_untouched(attempt(operator_resolved(dry)), OperatorAppendStatus::DryRunOrder,
                             "a dry-run order never reached an exchange");
}

TEST_F(OperatorAppendTest, AnOrderThatIsNotInOneOfTheThreeStatesIsRefusedByTheRule) {
    write_history({{intent(kA), 1000}, {prepared(kA), 2000}, {accepted(kA), 3000}});
    const Attempt a = attempt(operator_resolved(kA));
    expect_refused_untouched(a, OperatorAppendStatus::Refused, "an Accepted order: the exchange has it");
    EXPECT_EQ(a.result.check, operator_resolution::ResolutionCheck::NotInAResolvableState);
}

TEST_F(OperatorAppendTest, AnOrderThatEverShowedAnExchangeIdOrAFillIsRefusedByTheRule) {
    {
        AuditRecord amb = ambiguous(kA);
        amb.exchange_order_id = 77;
        write_history({{intent(kA), 1000}, {prepared(kA), 2000}, {amb, 3000}});
        const Attempt a = attempt(operator_resolved(kA));
        expect_refused_untouched(a, OperatorAppendStatus::Refused, "an exchange order id was recorded before");
        EXPECT_EQ(a.result.check, operator_resolution::ResolutionCheck::OrderShowsExistence);
    }
    {
        AuditRecord amb = ambiguous(kA);
        amb.filled_qty_ticks = 5;
        amb.avg_fill_price_ticks = 500'000;
        write_history({{intent(kA), 1000}, {prepared(kA), 2000}, {amb, 3000}});
        const Attempt a = attempt(operator_resolved(kA));
        expect_refused_untouched(a, OperatorAppendStatus::Refused, "a fill was recorded before");
        EXPECT_EQ(a.result.check, operator_resolution::ResolutionCheck::OrderShowsExistence);
    }
}

TEST_F(OperatorAppendTest, AFrameThatBreaksARuleIsRefusedWithThatRule) {
    using operator_resolution::ResolutionCheck;
    write_history(ambiguous_history(kA));
    struct Variant {
        const char* why;
        void (*mutate)(AuditRecord&);
        ResolutionCheck expected;
    };
    const Variant variants[] = {
        {"says Accepted", [](AuditRecord& r) { r.resulting_state = OrderState::Accepted; }, ResolutionCheck::FrameNotRejected},
        {"says Cancelled", [](AuditRecord& r) { r.resulting_state = OrderState::Cancelled; }, ResolutionCheck::FrameNotRejected},
        {"says Ambiguous", [](AuditRecord& r) { r.resulting_state = OrderState::Ambiguous; }, ResolutionCheck::FrameNotRejected},
        {"says Intent", [](AuditRecord& r) { r.resulting_state = OrderState::Intent; }, ResolutionCheck::FrameNotRejected},
        {"carries an exchange id", [](AuditRecord& r) { r.exchange_order_id = 5; }, ResolutionCheck::FrameClaimsExistence},
        {"carries a fill", [](AuditRecord& r) { r.filled_qty_ticks = 1; }, ResolutionCheck::FrameClaimsExistence},
        {"carries an average price", [](AuditRecord& r) { r.avg_fill_price_ticks = 1; }, ResolutionCheck::FrameClaimsExistence},
        {"another symbol", [](AuditRecord& r) { r.symbol_id += 1; }, ResolutionCheck::NotTheSameOrder},
        {"another side", [](AuditRecord& r) { r.side = OrderSide::Buy; }, ResolutionCheck::NotTheSameOrder},
        {"another price", [](AuditRecord& r) { r.price_ticks += 1; }, ResolutionCheck::NotTheSameOrder},
        {"another quantity", [](AuditRecord& r) { r.qty_ticks += 1; }, ResolutionCheck::NotTheSameOrder},
        {"another mode", [](AuditRecord& r) { r.mode = ExecutionMode::DryRun; }, ResolutionCheck::NotTheSameOrder},
        {"no detail code", [](AuditRecord& r) { r.detail_code = 0; }, ResolutionCheck::BadDetailCode},
        {"a negative detail code", [](AuditRecord& r) { r.detail_code = -1; }, ResolutionCheck::BadDetailCode},
        {"a missing evidence bit",
         [](AuditRecord& r) { r.detail_code = static_cast<std::int64_t>(static_cast<std::uint64_t>(r.detail_code) & ~operator_resolution::kLockHeld); },
         ResolutionCheck::BadDetailCode},
        {"a reserved bit",
         [](AuditRecord& r) { r.detail_code = static_cast<std::int64_t>(static_cast<std::uint64_t>(r.detail_code) | (std::uint64_t{1} << 12)); },
         ResolutionCheck::BadDetailCode},
    };
    for (const Variant& v : variants) {
        AuditRecord r = operator_resolved(kA);
        v.mutate(r);
        const Attempt a = attempt(r);
        expect_refused_untouched(a, OperatorAppendStatus::Refused, v.why);
        EXPECT_EQ(a.result.check, v.expected) << v.why;
    }
}

TEST_F(OperatorAppendTest, AFencedSinkRefusesAndWritesNothing) {
    write_history(ambiguous_history(kA));
    // damage one byte inside the first frame: the MAC no longer verifies, recovery says Corrupt and the sink fences itself
    std::vector<std::byte> bytes = file_bytes(base_path_);
    ASSERT_GT(bytes.size(), 100u);
    bytes[100] = static_cast<std::byte>(~static_cast<unsigned>(bytes[100]));
    {
        std::ofstream out(base_path_, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const Attempt a = attempt(operator_resolved(kA));
    EXPECT_EQ(a.result.status, OperatorAppendStatus::SinkFenced);
    EXPECT_TRUE(a.fenced);
    EXPECT_EQ(a.write_version, kFrameFormatVersion);
    EXPECT_TRUE(a.unchanged_on_disk);
}

TEST_F(OperatorAppendTest, AFailedAppendLeavesTheVersionAloneAndTheSinkUnfenced) {
    // The only failure that is not an I/O one: the active key is gone. Nothing was written, so nothing is fenced and the
    // version must not have moved -- a sink that believed it was at v6 would write v6 frames behind a log that has none.
    write_history(ambiguous_history(kA));
    const std::vector<std::byte> before = file_bytes(base_path_);
    {
        const auto sink = open(AuditSinkOpenMode::OperatorOpen);
        ASSERT_EQ(sink->recovered_checkpoints().size(), 1u);
        ASSERT_EQ(ring_->retire(kKeyId), RetireStatus::Retired);
        const OperatorResolutionAppend r = sink->append_operator_resolution(operator_resolved(kA), 5000);
        EXPECT_EQ(r.status, OperatorAppendStatus::AppendFailed);
        EXPECT_FALSE(r.append.acked());
        EXPECT_FALSE(sink->fenced()) << "an unresolvable key is a correctable mistake, not damage";
        EXPECT_EQ(sink->write_format_version(), kFrameFormatVersion);
        EXPECT_FALSE(sink->operator_resolved_this_session(0));
    }
    EXPECT_EQ(file_bytes(base_path_), before);
}

// --- the writer's pre-check and the reader never disagree --------------------------------------------------------------------

TEST_F(OperatorAppendTest, WhatTheWriterAcceptsTheReaderAcceptsAndWhatItRefusesTheReaderCallsCorrupt) {
    using operator_resolution::kAbsentRequiredBits;
    using operator_resolution::kKindAbsent;
    struct Variant {
        const char* why;
        void (*mutate)(AuditRecord&);
    };
    const Variant variants[] = {
        {"as is", [](AuditRecord&) {}},
        {"other informational bits", [](AuditRecord& r) { r.detail_code = good_detail_code(65535, 65535); }},
        {"zero informational bits", [](AuditRecord& r) { r.detail_code = good_detail_code(0, 0); }},
        {"another message", [](AuditRecord& r) { r.set_detail("anything at all"); }},
        {"another timestamp", [](AuditRecord& r) { r.timestamp_ms += 12345; }},
        {"state Accepted", [](AuditRecord& r) { r.resulting_state = OrderState::Accepted; }},
        {"state Cancelled", [](AuditRecord& r) { r.resulting_state = OrderState::Cancelled; }},
        {"state Intent", [](AuditRecord& r) { r.resulting_state = OrderState::Intent; }},
        {"state Ambiguous", [](AuditRecord& r) { r.resulting_state = OrderState::Ambiguous; }},
        {"state EscalatedToOperator", [](AuditRecord& r) { r.resulting_state = OrderState::EscalatedToOperator; }},
        {"exchange id", [](AuditRecord& r) { r.exchange_order_id = 9; }},
        {"fill", [](AuditRecord& r) { r.filled_qty_ticks = 1; }},
        {"average price", [](AuditRecord& r) { r.avg_fill_price_ticks = 1; }},
        {"symbol", [](AuditRecord& r) { r.symbol_id = 8; }},
        {"side", [](AuditRecord& r) { r.side = OrderSide::Buy; }},
        {"price", [](AuditRecord& r) { r.price_ticks = 500'001; }},
        {"quantity", [](AuditRecord& r) { r.qty_ticks = 99; }},
        {"mode", [](AuditRecord& r) { r.mode = ExecutionMode::DryRun; }},
        {"detail code 0", [](AuditRecord& r) { r.detail_code = 0; }},
        {"detail code -1", [](AuditRecord& r) { r.detail_code = -1; }},
        {"kind 0", [](AuditRecord& r) { r.detail_code = static_cast<std::int64_t>(kAbsentRequiredBits); }},
        {"kind 2", [](AuditRecord& r) { r.detail_code = static_cast<std::int64_t>((kAbsentRequiredBits) | 2u); }},
        {"kind 15", [](AuditRecord& r) { r.detail_code = static_cast<std::int64_t>((kAbsentRequiredBits) | 15u); }},
        {"a reserved low bit", [](AuditRecord& r) { r.detail_code = static_cast<std::int64_t>((kKindAbsent | kAbsentRequiredBits) | (std::uint64_t{1} << 10)); }},
        {"a reserved high bit", [](AuditRecord& r) { r.detail_code = static_cast<std::int64_t>((kKindAbsent | kAbsentRequiredBits) | (std::uint64_t{1} << 50)); }},
        {"the sign bit", [](AuditRecord& r) { r.detail_code = static_cast<std::int64_t>((kKindAbsent | kAbsentRequiredBits) | (std::uint64_t{1} << 63)); }},
        {"no evidence bits", [](AuditRecord& r) { r.detail_code = static_cast<std::int64_t>(kKindAbsent); }},
        {"half the evidence bits",
         [](AuditRecord& r) { r.detail_code = static_cast<std::int64_t>(kKindAbsent | operator_resolution::kFenceSatisfied | operator_resolution::kLockHeld); }},
    };
    int accepted = 0;
    int refused = 0;
    for (const Variant& v : variants) {
        SCOPED_TRACE(v.why);
        AuditRecord r = operator_resolved(kA);
        v.mutate(r);

        // the writer: a fresh store with the order Ambiguous, the operator sink asked to append the variant
        write_history(ambiguous_history(kA));
        bool writer_accepts = false;
        {
            const auto sink = open(AuditSinkOpenMode::OperatorOpen);
            writer_accepts = sink->append_operator_resolution(r, 5000).status == OperatorAppendStatus::Appended;
        }

        // the reader: the same history, then the same record as a v6 frame, laid down without asking the writer
        write_crafted({v5(intent(kA), 1000), v5(prepared(kA), 2000), v5(ambiguous(kA), 3000), v6(r, 5000)});
        bool reader_accepts = false;
        {
            const auto sink = open(AuditSinkOpenMode::Trading);
            reader_accepts = sink->recovery_status() != RecoveryScanStatus::Corrupt;
        }

        EXPECT_EQ(writer_accepts, reader_accepts) << "the writer's pre-check and the reader's rules must be the same rules";
        (writer_accepts ? accepted : refused) += 1;
    }
    EXPECT_GT(accepted, 3) << "the table must contain frames both sides accept";
    EXPECT_GT(refused, 15) << "...and frames both sides refuse";
}

// --- the checkpoint capacity ------------------------------------------------------------------------------------------------

TEST_F(OperatorAppendTest, AnOperatorOpenOfMoreThanKMaxInFlightOrdersCanResolveTheListedOnesAndTheStoreThenOpensForTrading) {
    char buf[40];
    std::vector<Timed> frames;
    for (std::size_t i = 0; i < kMaxInFlight + 1; ++i) {
        std::snprintf(buf, sizeof(buf), "HY-OPAPP-%04zu", i);
        const OrderSpec o{std::string(buf)};
        for (const Timed& t : ambiguous_history(o, static_cast<std::int64_t>(1000 + 10 * i))) frames.push_back(t);
    }
    write_history(frames);
    {
        const auto trading = open(AuditSinkOpenMode::Trading);
        EXPECT_EQ(trading->recovery_status(), RecoveryScanStatus::CapacityExceeded)
            << "one more unresolved order than the table holds: the store the operator is needed for";
        EXPECT_TRUE(trading->fenced());
    }
    {
        const auto sink = open(AuditSinkOpenMode::OperatorOpen);
        ASSERT_FALSE(sink->fenced());
        ASSERT_EQ(sink->recovered_checkpoints().size(), kMaxInFlight);
        ASSERT_EQ(sink->recovery_overflow().checkpoints, 1u);
        // the one that did not fit cannot be resolved in this session...
        std::snprintf(buf, sizeof(buf), "HY-OPAPP-%04zu", kMaxInFlight);
        EXPECT_EQ(sink->append_operator_resolution(operator_resolved(OrderSpec{std::string(buf)}), 5000).status,
                  OperatorAppendStatus::NoSuchUnresolvedOrder);
        // ...a listed one can
        EXPECT_EQ(sink->append_operator_resolution(operator_resolved(OrderSpec{"HY-OPAPP-0000"}), 5100).status,
                  OperatorAppendStatus::Appended);
    }
    {
        // one fewer unresolved order: the store fits the table again, and the next trading start is allowed
        const auto trading = open(AuditSinkOpenMode::Trading);
        EXPECT_EQ(trading->recovery_status(), RecoveryScanStatus::Recovered);
        EXPECT_FALSE(trading->fenced());
        EXPECT_EQ(trading->recovered_checkpoints().size(), kMaxInFlight);
        EXPECT_EQ(trading->recovered_operator_resolved().size(), 1u);
    }
}

TEST_F(OperatorAppendTest, TheWriterNeverFillsTheResolvedTablePastWhatATradingStartCanHold) {
    // 1023 orders already resolved and two still open: the 1024th resolution fits, the 1025th would leave a store that no
    // trading start can open (the table behind recovered_operator_resolved() must be complete) and no way back.
    ASSERT_NO_FATAL_FAILURE(write_crafted(store_with(kMaxOperatorResolved - 1, 2)));
    {
        const auto trading = open(AuditSinkOpenMode::Trading);
        ASSERT_EQ(trading->recovery_status(), RecoveryScanStatus::Recovered);
        EXPECT_EQ(trading->recovered_operator_resolved().size(), kMaxOperatorResolved - 1);
        EXPECT_EQ(trading->recovered_checkpoints().size(), 2u);
    }
    const OrderSpec fits = capacity_order(kMaxOperatorResolved - 1);
    const OrderSpec too_many = capacity_order(kMaxOperatorResolved);
    const std::vector<std::byte> initial = file_bytes(base_path_);
    {
        const auto sink = open(AuditSinkOpenMode::OperatorOpen);
        ASSERT_EQ(sink->recovered_checkpoints().size(), 2u);
        ASSERT_EQ(sink->write_format_version(), kFrameFormatVersionV6);
        EXPECT_EQ(sink->append_operator_resolution(operator_resolved(fits), 5000).status, OperatorAppendStatus::Appended);
        // the same session: 1023 recovered + 1 appended = the most a trading start can hold
        const OperatorResolutionAppend refused = sink->append_operator_resolution(operator_resolved(too_many), 5100);
        EXPECT_EQ(refused.status, OperatorAppendStatus::ResolvedTableFull);
        EXPECT_FALSE(refused.append.acked());
        EXPECT_FALSE(sink->fenced());
        EXPECT_TRUE(sink->operator_resolved_this_session(0));
        EXPECT_FALSE(sink->operator_resolved_this_session(1));
    }
    const std::vector<std::byte> after = file_bytes(base_path_);
    ASSERT_EQ(after.size(), initial.size() + kOrderEventFrameSize) << "exactly one frame was appended: the 1024th resolution";
    EXPECT_TRUE(std::equal(initial.begin(), initial.end(), after.begin())) << "append-only";
    {
        // the store the writer left is one a trading start still accepts: exactly kMaxOperatorResolved resolved, one open
        const auto trading = open(AuditSinkOpenMode::Trading);
        EXPECT_EQ(trading->recovery_status(), RecoveryScanStatus::Recovered);
        EXPECT_FALSE(trading->fenced());
        EXPECT_EQ(trading->recovered_operator_resolved().size(), kMaxOperatorResolved);
        ASSERT_EQ(trading->recovered_checkpoints().size(), 1u);
        EXPECT_STREQ(trading->recovered_checkpoints()[0].client_order_id.id, too_many.coid.c_str());
    }
}

TEST_F(OperatorAppendTest, AStoreThatAlreadyHoldsTheMostRefusesFurtherResolutions) {
    for (const std::size_t resolved : {kMaxOperatorResolved, kMaxOperatorResolved + 3}) {
        SCOPED_TRACE(resolved);
        ASSERT_NO_FATAL_FAILURE(write_crafted(store_with(resolved, 1)));
        {
            // a trading start holds exactly kMaxOperatorResolved and not one more
            const auto trading = open(AuditSinkOpenMode::Trading);
            EXPECT_EQ(trading->recovery_status(),
                      resolved == kMaxOperatorResolved ? RecoveryScanStatus::Recovered : RecoveryScanStatus::CapacityExceeded);
        }
        {
            const auto sink = open(AuditSinkOpenMode::OperatorOpen);
            ASSERT_FALSE(sink->fenced());
            EXPECT_EQ(sink->recovered_operator_resolved_total(), resolved);
            EXPECT_EQ(sink->recovery_overflow().operator_resolved, resolved - kMaxOperatorResolved);
        }
        expect_refused_untouched(attempt(operator_resolved(capacity_order(resolved))), OperatorAppendStatus::ResolvedTableFull,
                                 "the table is already as full as a trading start can hold", kFrameFormatVersionV6);
    }
}

// --- the rule on its own ----------------------------------------------------------------------------------------------------

TEST(OperatorResolutionRules, AResultNobodyFilledInIsNotAPass) {
    using namespace operator_resolution;
    EXPECT_EQ(ResolutionCheck{}, ResolutionCheck::NotChecked);
    EXPECT_NE(ResolutionCheck{}, ResolutionCheck::Ok);
    EXPECT_EQ(OperatorResolutionAppend{}.check, ResolutionCheck::NotChecked);
    EXPECT_EQ(OperatorResolutionAppend{}.status, OperatorAppendStatus::NotAttempted);
    EXPECT_NE(OperatorResolutionAppend{}.status, OperatorAppendStatus::Appended);
    EXPECT_FALSE(OperatorResolutionAppend{}.append.acked());
    // a default target is an Intent: it is not resolvable
    EXPECT_EQ(check_resolution(ResolutionTarget{}, operator_resolved(kA)), ResolutionCheck::NotInAResolvableState);
}

TEST(OperatorResolutionRules, OnlyThreeStatesCanBeResolved) {
    using namespace operator_resolution;
    for (const OrderState state :
         {OrderState::Intent, OrderState::Submitting, OrderState::Accepted, OrderState::Rejected, OrderState::Ambiguous,
          OrderState::PartialFill, OrderState::Filled, OrderState::CancelRequested, OrderState::Cancelled,
          OrderState::Expired, OrderState::Reconciled, OrderState::EscalatedToOperator}) {
        ResolutionTarget t{};
        t.state = state;
        t.symbol_id = kA.symbol_id;
        t.side = kA.side;
        t.mode = kA.mode;
        t.price_ticks = kA.price;
        t.qty_ticks = kA.qty;
        const bool resolvable = state == OrderState::Submitting || state == OrderState::Ambiguous ||
                                state == OrderState::EscalatedToOperator;
        EXPECT_EQ(check_resolution(t, operator_resolved(kA)),
                  resolvable ? ResolutionCheck::Ok : ResolutionCheck::NotInAResolvableState)
            << order_state_name(state);
    }
    // an out-of-range state byte is no state at all
    ResolutionTarget bogus{};
    bogus.state = static_cast<OrderState>(200);
    EXPECT_EQ(check_resolution(bogus, operator_resolved(kA)), ResolutionCheck::NotInAResolvableState);
}

TEST(OperatorResolutionRules, TheFirstBrokenRuleIsTheOneReported) {
    using namespace operator_resolution;
    ResolutionTarget t{};
    t.state = OrderState::Ambiguous;
    t.symbol_id = kA.symbol_id;
    t.side = kA.side;
    t.mode = kA.mode;
    t.price_ticks = kA.price;
    t.qty_ticks = kA.qty;
    AuditRecord r = operator_resolved(kA);
    EXPECT_EQ(check_resolution(t, r), ResolutionCheck::Ok);
    // break them one at a time, in the order the design lists them, keeping the earlier ones broken
    r.detail_code = 0;
    EXPECT_EQ(check_resolution(t, r), ResolutionCheck::BadDetailCode);
    r.qty_ticks += 1;
    EXPECT_EQ(check_resolution(t, r), ResolutionCheck::NotTheSameOrder);
    r.avg_fill_price_ticks = 1;
    EXPECT_EQ(check_resolution(t, r), ResolutionCheck::FrameClaimsExistence);
    r.resulting_state = OrderState::Accepted;
    EXPECT_EQ(check_resolution(t, r), ResolutionCheck::FrameNotRejected);
    t.filled_qty_ticks = 1;
    EXPECT_EQ(check_resolution(t, r), ResolutionCheck::OrderShowsExistence);
    t.state = OrderState::Filled;
    EXPECT_EQ(check_resolution(t, r), ResolutionCheck::NotInAResolvableState);
}

TEST(OperatorResolutionRules, AnExchangeIdOrAFillOnTheOrderMakesItUnresolvable) {
    using namespace operator_resolution;
    ResolutionTarget t{};
    t.state = OrderState::EscalatedToOperator;
    t.symbol_id = kA.symbol_id;
    t.side = kA.side;
    t.mode = kA.mode;
    t.price_ticks = kA.price;
    t.qty_ticks = kA.qty;
    EXPECT_EQ(check_resolution(t, operator_resolved(kA)), ResolutionCheck::Ok);
    t.exchange_order_id = 1;
    EXPECT_EQ(check_resolution(t, operator_resolved(kA)), ResolutionCheck::OrderShowsExistence);
    t.exchange_order_id = -1;
    EXPECT_EQ(check_resolution(t, operator_resolved(kA)), ResolutionCheck::OrderShowsExistence);
    t.exchange_order_id = 0;
    t.filled_qty_ticks = 1;
    EXPECT_EQ(check_resolution(t, operator_resolved(kA)), ResolutionCheck::OrderShowsExistence);
    t.filled_qty_ticks = -1;
    EXPECT_EQ(check_resolution(t, operator_resolved(kA)), ResolutionCheck::OrderShowsExistence);
}
