// Q23 slices S2/S3: the recovery reader's handling of OrderNeverSent frames and of the order state NeverSent
// (docs/NOTSENT_CLOSURE_DESIGN.md 4.3 and 4.6, 5).
//
// There is deliberately NO production writer of event 23 in this slice (the writers are S4, the final gate's refusal, and S6,
// a refusal before the Prepared frame), so the logs here are CRAFTED, the way test_operator_resolution_reader.cpp crafts the
// operator's: every frame is made by the production encoder (encode_order_event_frame with an explicit version), chained and
// anchored the way DurableAuditSink does it, and every log is then read by the production sink and verifier. The one pairing
// the encoder refuses (event 23 or state 12 in a v5 frame) is made by relabelling a frame and signing it again, so the ONLY
// wrong thing about it is the pairing -- a MAC failure would be caught later than the check under test and could hide its
// absence.
//
// The table of design 4.6, row by row:
//   Intent                         orphan (unchanged)
//   Intent, NeverSent              closed: neither a checkpoint nor an orphan
//   Intent, Prepared               Ambiguous checkpoint (unchanged)
//   Intent, Prepared, NeverSent    closed, no checkpoint
//   Intent, Prepared, outcome ...  by the outcome (unchanged)
//   anything after NeverSent       Corrupt
//   NeverSent as a COID's first    Corrupt
// plus: not from Ambiguous (recovery cannot close an order that may have crossed), not for an order that showed existence,
// not a frame that contradicts the Intent, not half of the pair, not in a v5 frame.
//
// A scenario that reopens the same store many times in one test uses a fresh file name each time (see the fixture): deleting a
// log and creating a file of the same name a moment later races a real-time antivirus scanner on a Windows host.

#include <gtest/gtest.h>
#include <hengyuan/durable_audit_export.hpp>
#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/operator_resolution_frame.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
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
    static const char kKey[] = "never-sent-reader-test-key";
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
AuditRecord partial_fill(const OrderSpec& o, std::int64_t filled = 10) {
    AuditRecord r = make_record(o, AuditEventType::OrderPartialFill, OrderState::PartialFill);
    r.exchange_order_id = 77;
    r.filled_qty_ticks = filled;
    r.avg_fill_price_ticks = o.price;
    return r;
}
AuditRecord state_frame(const OrderSpec& o, AuditEventType ev, OrderState st, std::int64_t exchange_id = 77) {
    AuditRecord r = make_record(o, ev, st);
    r.exchange_order_id = exchange_id;
    return r;
}
AuditRecord rejected_by_exchange(const OrderSpec& o) {
    return make_record(o, AuditEventType::OrderRejected, OrderState::Rejected);
}

// The closing frame: the NeverSent pair, nothing filled, no exchange id, the Intent's order, a reason in detail_code.
AuditRecord closure(const OrderSpec& o, std::int64_t reason = 3) {
    AuditRecord r = make_record(o, AuditEventType::OrderNeverSent, OrderState::NeverSent);
    r.detail_code = reason;
    r.set_detail("final gate refused");
    return r;
}

AuditRecord operator_resolved(const OrderSpec& o) {
    AuditRecord r = make_record(o, AuditEventType::OrderOperatorResolved, OrderState::Rejected);
    r.detail_code = static_cast<std::int64_t>(operator_resolution::kKindAbsent | operator_resolution::kAbsentRequiredBits);
    r.set_detail("op=test;ev=00000000000000000000000000000000");
    return r;
}

// --- crafted logs ---------------------------------------------------------------------------------------------------

struct CraftedFrame {
    std::uint8_t version;
    AuditRecord rec;
    std::optional<std::uint8_t> relabel;  // after encoding at `version`: overwrite the version byte and sign again
};

CraftedFrame v5f(const AuditRecord& r) { return {kFrameFormatVersion, r, std::nullopt}; }
CraftedFrame v6f(const AuditRecord& r) { return {kFrameFormatVersionV6, r, std::nullopt}; }
// A frame the encoder refuses to produce: made at `encode_as`, then labelled `label` and signed again.
CraftedFrame relabelled(const AuditRecord& r, std::uint8_t encode_as, std::uint8_t label) {
    return {encode_as, r, label};
}

struct Crafted {
    std::vector<std::byte> log;
    std::array<std::byte, kTipAnchorSize> anchor{};
    std::size_t frames{0};
    bool ok{false};
};

Crafted craft(const KeyRing& ring, const std::vector<CraftedFrame>& frames) {
    Crafted c;
    std::array<std::byte, kKeyBlockSize> key_block{};
    if (!ring.active_key(kKeyId, key_block)) return c;
    const std::span<const std::byte> key(key_block.data(), key_block.size());
    std::array<std::byte, kMacLen> prev{};
    std::uint64_t seq = 0;
    c.log.reserve(frames.size() * kOrderEventFrameSize);
    for (const CraftedFrame& f : frames) {
        std::array<std::byte, kOrderEventFrameSize> buf{};
        const auto n = encode_order_event_frame(buf, kKeyId, seq, FrameTimeKind::ServerCorrectedUtc,
                                                static_cast<std::int64_t>(1000 + seq), f.rec, prev, key, f.version);
        if (n != kOrderEventFrameSize) return c;
        if (f.relabel.has_value()) {
            buf[0] = static_cast<std::byte>(*f.relabel);
            const auto mac = crypto::hmac_sha256(
                key, std::span<const std::byte>(buf.data(), kOrderEventFrameSize - kMacLen));
            std::memcpy(buf.data() + kOrderEventFrameSize - kMacLen, mac.bytes.data(), kMacLen);
        }
        std::memcpy(prev.data(), buf.data() + kOrderEventFrameSize - kMacLen, kMacLen);
        c.log.insert(c.log.end(), buf.begin(), buf.end());
        ++seq;
    }
    c.frames = frames.size();
    if (!frames.empty()) detail::encode_tip_anchor(c.anchor, seq - 1, prev, kKeyId, key);
    c.ok = true;
    return c;
}

}  // namespace

class NeverSentReaderTest : public ::testing::Test {
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
        stem_ = std::string(tmp) + "hy_nsr_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter);
#else
        stem_ = "/tmp/hy_nsr_" + std::to_string(getpid()) + "_" + std::to_string(counter);
#endif
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x30 + i);
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

    Crafted craft_log(const std::vector<CraftedFrame>& frames) const { return craft(*ring_, frames); }

    // Lays `c` down as a log + tip anchor on a fresh path, the way a writer that died right after an append would have
    // left it.
    void write_log(const Crafted& c) {
        ASSERT_TRUE(c.ok) << "the crafted log could not be built";
        fresh_path();
        DurableLogStore store(base_path_, base_path_ + ".lock", base_path_ + ".tip");
        ASSERT_TRUE(store.acquire_lock());
        ASSERT_TRUE(store.open_log());
        if (!c.log.empty()) {
            ASSERT_TRUE(store.append_and_fsync(std::span<const std::byte>(c.log.data(), c.log.size())));
            ASSERT_TRUE(store.write_tip_anchor(std::span<const std::byte>(c.anchor.data(), c.anchor.size())));
        }
        store.close_log();
        store.release_lock();
    }

    void write_frames(const std::vector<CraftedFrame>& frames) { write_log(craft_log(frames)); }

    std::unique_ptr<DurableAuditSink> open(AuditSinkOpenMode mode = AuditSinkOpenMode::Trading,
                                           std::size_t max_orphans = kMaxIntentOrphans) {
        return std::make_unique<DurableAuditSink>(base_path_, *ring_, kKeyId, max_orphans, mode);
    }

    // The whole log file as it is on disk (after every sink has been closed).
    std::vector<std::byte> log_bytes() const {
        std::ifstream in(base_path_, std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::byte> out(raw.size());
        if (!raw.empty()) std::memcpy(out.data(), raw.data(), raw.size());
        return out;
    }

    void expect_corrupt(const std::vector<CraftedFrame>& frames, const char* why,
                        AuditSinkOpenMode mode = AuditSinkOpenMode::Trading) {
        write_frames(frames);
        const auto sink = open(mode);
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Corrupt) << why;
        EXPECT_TRUE(sink->fenced()) << why;
        EXPECT_EQ(sink->fence_reason(), FenceReason::IoError) << why;
    }

    // The log is read, the one order is closed: no fence, no checkpoint, no orphan, one closure counted.
    void expect_closed_cleanly(const std::vector<CraftedFrame>& frames, const char* why) {
        write_frames(frames);
        const auto sink = open();
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean) << why;
        EXPECT_FALSE(sink->fenced()) << why;
        EXPECT_TRUE(sink->recovered_checkpoints().empty()) << why;
        EXPECT_EQ(sink->recovered_intent_orphans(), 0u) << why;
        EXPECT_EQ(sink->recovered_never_sent(), 1u) << why;
    }
};

namespace {
const OrderSpec kA{"HY-NSR-A"};
const OrderSpec kB{"HY-NSR-B"};
const OrderSpec kC{"HY-NSR-C"};

std::string numbered_coid(std::size_t n) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "HY-NSR-%06zu", n);
    return std::string(buf);
}
}  // namespace

// --- what the reader accepts (design 4.6) -----------------------------------------------------------------------------

TEST_F(NeverSentReaderTest, ClosesAnIntentOnlyOrderAsClassA) {
    write_frames({v5f(intent(kA)), v6f(closure(kA))});
    const auto sink = open();
    EXPECT_FALSE(sink->fenced());
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean) << "nothing is left to reconcile and nothing is an orphan";
    EXPECT_TRUE(sink->recovered_checkpoints().empty()) << "a closed order is final: no checkpoint";
    EXPECT_EQ(sink->recovered_intent_orphans(), 0u) << "...and not an Intent-only orphan either";
    EXPECT_EQ(sink->recovered_never_sent(), 1u);
    EXPECT_TRUE(sink->recovered_operator_resolved().empty());
    EXPECT_FALSE(sink->recovery_overflow().any());
    EXPECT_EQ(sink->write_format_version(), kFrameFormatVersionV6) << "the store's tip is v6 now";
}

TEST_F(NeverSentReaderTest, ClosesAPreparedOrderAsClassB) {
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v6f(closure(kA))});
    const auto sink = open();
    EXPECT_FALSE(sink->fenced());
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean) << "a Prepared order that was refused is not Ambiguous";
    EXPECT_TRUE(sink->recovered_checkpoints().empty());
    EXPECT_EQ(sink->recovered_intent_orphans(), 0u);
    EXPECT_EQ(sink->recovered_never_sent(), 1u);
}

TEST_F(NeverSentReaderTest, ARepeatedIntentFrameBeforeTheClosureIsStillFine) {
    expect_closed_cleanly({v5f(intent(kA)), v5f(intent(kA)), v6f(closure(kA))}, "Intent repeated");
}

TEST_F(NeverSentReaderTest, TheReasonCodeMayHoldAnyValue) {
    for (const std::int64_t reason :
         {std::int64_t{0}, std::int64_t{1}, std::int64_t{-1}, std::int64_t{1} << 40, std::numeric_limits<std::int64_t>::max(),
          std::numeric_limits<std::int64_t>::min()}) {
        expect_closed_cleanly({v5f(intent(kA)), v5f(prepared(kA)), v6f(closure(kA, reason))},
                              "the reason is informational: an unknown value is not corruption");
    }
}

TEST_F(NeverSentReaderTest, ClosesOnlyTheNamedOrderAndTheOthersStayRecovered) {
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v5f(intent(kB)), v5f(prepared(kB)), v5f(intent(kC)),
                  v6f(closure(kA))});
    const auto sink = open();
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    ASSERT_EQ(sink->recovered_checkpoints().size(), 1u);
    EXPECT_STREQ(sink->recovered_checkpoints()[0].client_order_id.id, "HY-NSR-B");
    EXPECT_EQ(sink->recovered_checkpoints()[0].resulting_state, OrderState::Ambiguous);
    EXPECT_EQ(sink->recovered_intent_orphans(), 1u) << "C has an Intent frame and nothing else";
    EXPECT_EQ(sink->recovered_never_sent(), 1u);
}

TEST_F(NeverSentReaderTest, AnIntentOnlyOrderNextToAClosedOneIsStillAnOrphan) {
    write_frames({v5f(intent(kA)), v6f(closure(kA)), v6f(intent(kB))});
    const auto sink = open();
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(sink->recovered_intent_orphans(), 1u);
    EXPECT_EQ(sink->recovered_never_sent(), 1u);
}

TEST_F(NeverSentReaderTest, AClosedOrderStaysClosedAcrossRestarts) {
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v6f(closure(kA))});
    for (int pass = 0; pass < 3; ++pass) {
        const auto sink = open();
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean) << "pass " << pass;
        EXPECT_EQ(sink->recovered_never_sent(), 1u) << "pass " << pass;
        EXPECT_TRUE(sink->recovered_checkpoints().empty()) << "pass " << pass;
    }
}

TEST_F(NeverSentReaderTest, ARecoveredClosedOrderIsNotRegisteredInFlight) {
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v6f(closure(kA)), v6f(intent(kB)), v6f(prepared(kB))});
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    InFlightRegistry registry;
    EXPECT_EQ(repopulate_in_flight_registry(registry, sink->recovered_checkpoints()), 1u) << "only B is handed to reconciliation";
    EXPECT_FALSE(registry.is_in_flight("HY-NSR-A")) << "the closed order must not stall StartupRecovery";
    EXPECT_TRUE(registry.is_in_flight("HY-NSR-B"));
}

TEST_F(NeverSentReaderTest, ClosedOrdersTakeNoCheckpointSlot) {
    // Exactly kMaxInFlight unresolved orders fill the checkpoint table; ten closed orders -- class A and class B -- next to
    // them change nothing. (The first v6 frame comes after every v5 frame: versions never go back.)
    std::vector<CraftedFrame> frames;
    for (std::size_t i = 0; i < kMaxInFlight; ++i) {
        const OrderSpec o{numbered_coid(i)};
        frames.push_back(v5f(intent(o)));
        frames.push_back(v5f(prepared(o)));
        frames.push_back(v5f(ambiguous(o)));
    }
    for (std::size_t i = 0; i < 10; ++i) {
        const OrderSpec o{numbered_coid(1000 + i)};
        frames.push_back(v6f(intent(o)));
        if (i % 2 == 1) frames.push_back(v6f(prepared(o)));
        frames.push_back(v6f(closure(o)));
    }
    write_frames(frames);
    {
        const auto sink = open();
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered) << "the closed orders did not overflow the table";
        EXPECT_FALSE(sink->fenced());
        EXPECT_EQ(sink->recovered_checkpoints().size(), kMaxInFlight);
        EXPECT_EQ(sink->recovered_never_sent(), 10u);
        EXPECT_EQ(sink->recovered_intent_orphans(), 0u);
    }
    // The control: one more UNRESOLVED order does overflow it.
    frames.push_back(v6f(intent(OrderSpec{numbered_coid(2000)})));
    frames.push_back(v6f(prepared(OrderSpec{numbered_coid(2000)})));
    write_frames(frames);
    const auto sink = open();
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::CapacityExceeded);
    EXPECT_TRUE(sink->fenced());
}

TEST_F(NeverSentReaderTest, ClosedOrdersAreNotOrphansSoTheOrphanBudgetIsUntouched) {
    // Five class A closures and one Intent-only order against a budget of ONE orphan.
    std::vector<CraftedFrame> frames = {v5f(intent(OrderSpec{numbered_coid(0)})), v6f(closure(OrderSpec{numbered_coid(0)}))};
    for (std::size_t i = 1; i < 5; ++i) {
        const OrderSpec o{numbered_coid(i)};
        frames.push_back(v6f(intent(o)));
        frames.push_back(v6f(closure(o)));
    }
    frames.push_back(v6f(intent(OrderSpec{numbered_coid(99)})));
    write_frames(frames);
    {
        const auto sink = open(AuditSinkOpenMode::Trading, /*max_orphans=*/1);
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean) << "one orphan fits a budget of one";
        EXPECT_FALSE(sink->fenced());
        EXPECT_EQ(sink->recovered_intent_orphans(), 1u);
        EXPECT_EQ(sink->recovered_never_sent(), 5u);
    }
    // The control: a second Intent-only order does exceed it.
    frames.push_back(v6f(intent(OrderSpec{numbered_coid(100)})));
    write_frames(frames);
    const auto sink = open(AuditSinkOpenMode::Trading, /*max_orphans=*/1);
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::CapacityExceeded);
}

TEST_F(NeverSentReaderTest, ANeverSentOrderAndAnOperatorResolvedOneAreBothFinal) {
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v5f(intent(kB)), v5f(prepared(kB)), v6f(closure(kA)),
                  v6f(operator_resolved(kB))});
    const auto sink = open();
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_TRUE(sink->recovered_checkpoints().empty());
    EXPECT_EQ(sink->recovered_never_sent(), 1u);
    EXPECT_EQ(sink->recovered_operator_resolved().size(), 1u);
    EXPECT_STREQ(sink->recovered_operator_resolved()[0].client_order_id.id, "HY-NSR-B")
        << "only the operator's closure is listed for the late-fill alarm";
}

TEST_F(NeverSentReaderTest, AnOperatorOpenReadsTheSameFramesTheSameWay) {
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v6f(closure(kA)), v6f(intent(kB)), v6f(prepared(kB))});
    const auto sink = open(AuditSinkOpenMode::OperatorOpen);
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    ASSERT_EQ(sink->recovered_checkpoints().size(), 1u);
    EXPECT_STREQ(sink->recovered_checkpoints()[0].client_order_id.id, "HY-NSR-B");
    EXPECT_EQ(sink->recovered_never_sent(), 1u);
}

TEST_F(NeverSentReaderTest, AnOperatorOpenDoesNotRelaxAnyRuleAboutTheClosure) {
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(ambiguous(kA)), v6f(closure(kA))},
                   "a closure of an Ambiguous order, OperatorOpen", AuditSinkOpenMode::OperatorOpen);
    expect_corrupt({v5f(intent(kA)), v6f(closure(kA)), v6f(prepared(kA))}, "a frame after the closure, OperatorOpen",
                   AuditSinkOpenMode::OperatorOpen);
}

// --- what the reader refuses: every "no" is Corrupt for the whole log, and the sink fences ---------------------------

TEST_F(NeverSentReaderTest, RefusesTheClosureOfAnOrderThatMayHaveCrossed) {
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(ambiguous(kA)), v6f(closure(kA))},
                   "Ambiguous: recovery cannot close an order that may have crossed");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(ambiguous(kA)), v5f(escalated(kA)), v6f(closure(kA))},
                   "EscalatedToOperator");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(accepted(kA)), v6f(closure(kA))}, "Accepted: the exchange has it");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(accepted(kA)), v5f(partial_fill(kA)), v6f(closure(kA))},
                   "PartialFill");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(accepted(kA)),
                    v5f(state_frame(kA, AuditEventType::OrderCancelRequested, OrderState::CancelRequested)), v6f(closure(kA))},
                   "CancelRequested");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(accepted(kA)),
                    v5f(state_frame(kA, AuditEventType::OrderCancelled, OrderState::Cancelled)), v6f(closure(kA))},
                   "Cancelled");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(accepted(kA)),
                    v5f(state_frame(kA, AuditEventType::OrderExpired, OrderState::Expired)), v6f(closure(kA))},
                   "Expired");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(rejected_by_exchange(kA)), v6f(closure(kA))},
                   "Rejected by the exchange: already final");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)),
                    v5f(state_frame(kA, AuditEventType::OrderFilled, OrderState::Filled)), v6f(closure(kA))},
                   "Filled");
}

TEST_F(NeverSentReaderTest, RefusesAnOrderThatHadShownAnyTraceOfExistingOnTheExchange) {
    {
        AuditRecord p = prepared(kA);
        p.exchange_order_id = 77;
        expect_corrupt({v5f(intent(kA)), v5f(p), v6f(closure(kA))}, "an exchange order id was recorded before");
    }
    {
        AuditRecord p = prepared(kA);
        p.filled_qty_ticks = 5;
        p.avg_fill_price_ticks = 500'000;
        expect_corrupt({v5f(intent(kA)), v5f(p), v6f(closure(kA))}, "a fill was recorded before");
    }
    {
        AuditRecord i = intent(kA);
        i.exchange_order_id = 9;
        expect_corrupt({v5f(i), v6f(closure(kA))}, "even a class A order whose Intent frame carries an id");
    }
}

TEST_F(NeverSentReaderTest, TheClosureNeverOpensAnOrder) {
    expect_corrupt({v6f(closure(kA))}, "the very first frame of the log");
    expect_corrupt({v5f(intent(kB)), v6f(closure(kA))}, "first frame of a COID, resulting_state NeverSent");
    AuditRecord claims_intent = closure(kA);
    claims_intent.resulting_state = OrderState::Intent;
    expect_corrupt({v6f(claims_intent)}, "first frame of a COID that is event 23 but claims resulting_state Intent");
    expect_corrupt({v5f(intent(kB)), v6f(claims_intent)}, "same, not at the head of the log");
    AuditRecord state_only = intent(kA);
    state_only.resulting_state = OrderState::NeverSent;
    expect_corrupt({v6f(state_only)}, "first frame of a COID: an Intent event that says NeverSent");
}

TEST_F(NeverSentReaderTest, TheClosureFrameIsTheLastFrameOfItsOrder) {
    for (const bool prepared_first : {false, true}) {
        std::vector<CraftedFrame> head = {v5f(intent(kA))};
        if (prepared_first) head.push_back(v5f(prepared(kA)));
        head.push_back(v6f(closure(kA)));
        auto after = [&](const CraftedFrame& extra, const char* why) {
            std::vector<CraftedFrame> frames = head;
            frames.push_back(extra);
            expect_corrupt(frames, why);
        };
        after(v6f(closure(kA)), "a second closure");
        after(v6f(intent(kA)), "a second Intent");
        after(v6f(prepared(kA)), "a Prepared: it was proven never sent");
        after(v6f(ambiguous(kA)), "an Ambiguous");
        after(v6f(escalated(kA)), "an EscalatedToOperator");
        after(v6f(accepted(kA)), "an Accepted");
        after(v6f(rejected_by_exchange(kA)), "a Rejected");
        after(v6f(state_frame(kA, AuditEventType::OrderFilled, OrderState::Filled)), "a Filled");
        after(v6f(operator_resolved(kA)), "the operator's closure on top of it");
    }
    // Frames of OTHER orders after it are of course fine.
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v6f(closure(kA)), v6f(intent(kB)), v6f(prepared(kB))});
    const auto sink = open();
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    EXPECT_EQ(sink->recovered_checkpoints().size(), 1u);
}

TEST_F(NeverSentReaderTest, AnOperatorResolvedOrderCannotBeClosedAsNeverSentAfterwards) {
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v6f(operator_resolved(kA)), v6f(closure(kA))},
                   "the operator closed it as absent; a closure as never-sent on top of that is a contradiction");
}

TEST_F(NeverSentReaderTest, RefusesTheEventWithoutTheStateAndTheStateWithoutTheEvent) {
    for (const OrderState wrong : {OrderState::Intent, OrderState::Submitting, OrderState::Ambiguous, OrderState::Accepted,
                                   OrderState::Rejected, OrderState::Filled, OrderState::EscalatedToOperator}) {
        AuditRecord r = closure(kA);
        r.resulting_state = wrong;
        expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v6f(r)},
                       (std::string("event 23 with state ") + order_state_name(wrong)).c_str());
    }
    for (const AuditEventType wrong :
         {AuditEventType::OrderIntentCreated, AuditEventType::OrderSubmitPrepared, AuditEventType::OrderRejected,
          AuditEventType::OrderAmbiguous, AuditEventType::OrderReconciled, AuditEventType::OrderEscalated,
          AuditEventType::UserDataStreamEventObserved, AuditEventType::OrderOperatorResolved}) {
        AuditRecord r = closure(kA);
        r.event_type = wrong;
        expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v6f(r)},
                       (std::string("state NeverSent under ") + audit_event_name(wrong)).c_str());
    }
}

TEST_F(NeverSentReaderTest, RefusesAFrameThatClaimsTheOrderExistedOrFilled) {
    {
        AuditRecord r = closure(kA);
        r.exchange_order_id = 5;
        expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v6f(r)}, "exchange_order_id != 0");
    }
    {
        AuditRecord r = closure(kA);
        r.filled_qty_ticks = 1;
        expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v6f(r)}, "filled_qty_ticks != 0");
    }
    {
        AuditRecord r = closure(kA);
        r.avg_fill_price_ticks = 1;
        expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v6f(r)}, "avg_fill_price_ticks != 0");
    }
}

TEST_F(NeverSentReaderTest, RefusesAFrameThatDescribesAnotherOrderThanTheIntent) {
    for (const bool prepared_first : {false, true}) {
        std::vector<CraftedFrame> head = {v5f(intent(kA))};
        if (prepared_first) head.push_back(v5f(prepared(kA)));
        auto with = [&](auto mutate, const char* why) {
            AuditRecord r = closure(kA);
            mutate(r);
            std::vector<CraftedFrame> frames = head;
            frames.push_back(v6f(r));
            expect_corrupt(frames, why);
        };
        with([](AuditRecord& r) { r.symbol_id += 1; }, "symbol_id");
        with([](AuditRecord& r) { r.side = OrderSide::Buy; }, "side");
        with([](AuditRecord& r) { r.price_ticks += 1; }, "price_ticks");
        with([](AuditRecord& r) { r.qty_ticks += 1; }, "qty_ticks");
        with([](AuditRecord& r) { r.mode = ExecutionMode::DryRun; }, "mode");
    }
}

// --- format versions in one store (durable_frame_codec.hpp's FORMAT v5 -> v6 note) -----------------------------------

TEST_F(NeverSentReaderTest, TheClosureInAV5FrameIsCorruptNotEarly) {
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), relabelled(closure(kA), kFrameFormatVersionV6, kFrameFormatVersion)},
                   "event 23 and state 12 inside a frame labelled v5, correctly signed");
    AuditRecord event_only = closure(kA);
    event_only.resulting_state = OrderState::Submitting;
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), relabelled(event_only, kFrameFormatVersionV6, kFrameFormatVersion)},
                   "event 23 alone inside a frame labelled v5");
    AuditRecord state_only = prepared(kA);
    state_only.resulting_state = OrderState::NeverSent;
    expect_corrupt({v5f(intent(kA)), relabelled(state_only, kFrameFormatVersionV6, kFrameFormatVersion)},
                   "state 12 alone inside a frame labelled v5");
}

TEST_F(NeverSentReaderTest, AV5FrameAfterAClosureIsCorruptBecauseVersionsNeverGoBack) {
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v6f(closure(kA)), v5f(intent(kB))}, "v5, v6, then v5 again");
    expect_corrupt({v5f(intent(kA)), v6f(closure(kA)), v5f(intent(kB)), v5f(prepared(kB))}, "class A, then a v5 frame");
}

TEST_F(NeverSentReaderTest, AClosureMayStartAV6StoreAndTheStoreKeepsWritingV6) {
    {
        write_frames({v6f(intent(kA)), v6f(closure(kA))});
        const auto sink = open();
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
        EXPECT_EQ(sink->write_format_version(), kFrameFormatVersionV6);
    }
    // After a closure the trading process picks the store up: its own appends must be v6, or the next start would find a v5
    // frame after a v6 one and refuse the whole log.
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v6f(closure(kA))});
    {
        const auto sink = open();
        ASSERT_EQ(sink->write_format_version(), kFrameFormatVersionV6);
        ASSERT_TRUE(sink->append_durable(intent(kB), 2000).acked());
        ASSERT_TRUE(sink->append_durable(prepared(kB), 2001).acked());
    }
    const std::vector<std::byte> bytes = log_bytes();
    ASSERT_EQ(bytes.size(), 5 * kOrderEventFrameSize);
    EXPECT_EQ(bytes[0], std::byte{5});
    EXPECT_EQ(bytes[2 * kOrderEventFrameSize], std::byte{6});
    EXPECT_EQ(bytes[3 * kOrderEventFrameSize], std::byte{6});
    EXPECT_EQ(bytes[4 * kOrderEventFrameSize], std::byte{6});
    const auto reopened = open();
    EXPECT_EQ(reopened->recovery_status(), RecoveryScanStatus::Recovered);
    ASSERT_EQ(reopened->recovered_checkpoints().size(), 1u);
    EXPECT_STREQ(reopened->recovered_checkpoints()[0].client_order_id.id, "HY-NSR-B");
    EXPECT_EQ(reopened->recovered_never_sent(), 1u);
}

// --- nothing in this build writes the closure frame --------------------------------------------------------------------------

TEST_F(NeverSentReaderTest, AV5StoreRefusesToAppendTheClosureFrameAndWritesNothing) {
    fresh_path();
    std::size_t size_before = 0;
    {
        const auto sink = open();
        ASSERT_TRUE(sink->append_durable(intent(kA), 1000).acked());
        ASSERT_TRUE(sink->append_durable(prepared(kA), 1001).acked());
        size_before = 2 * kOrderEventFrameSize;
        const AuditAppendResult r = sink->append_durable(closure(kA), 1002);
        EXPECT_FALSE(r.acked()) << "a v5 store must not take an event only v6 may carry";
        EXPECT_TRUE(sink->fenced());
        EXPECT_EQ(sink->fence_reason(), FenceReason::IoError);
        EXPECT_FALSE(sink->append_durable(intent(kB), 1003).acked()) << "fenced for good";
    }
    EXPECT_EQ(log_bytes().size(), size_before) << "nothing of the refused frame may reach the log";
    const auto reopened = open();
    EXPECT_EQ(reopened->recovery_status(), RecoveryScanStatus::Recovered) << "the store itself is untouched";
    EXPECT_FALSE(reopened->fenced());
    EXPECT_EQ(reopened->recovered_never_sent(), 0u);
    EXPECT_EQ(reopened->write_format_version(), kFrameFormatVersion);
}

TEST_F(NeverSentReaderTest, AnOrdinaryLogIsReadExactlyAsBefore) {
    // The regression guard of the slice: a v5 log with no closure frame recovers as it always did, and no counter moves.
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v5f(accepted(kA)), v5f(intent(kB)), v5f(intent(kC)), v5f(prepared(kC)),
                  v5f(ambiguous(kC))});
    const auto sink = open();
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    EXPECT_EQ(sink->recovered_checkpoints().size(), 2u) << "A (Accepted) and C (Ambiguous)";
    EXPECT_EQ(sink->recovered_intent_orphans(), 1u) << "B";
    EXPECT_EQ(sink->recovered_never_sent(), 0u);
    EXPECT_EQ(sink->write_format_version(), kFrameFormatVersion);
}

// --- the verifier (durable_audit_export.hpp) reads the same frames --------------------------------------------------------

TEST_F(NeverSentReaderTest, TheVerifierReadsAClosureLogAndReportsVersion6) {
    const std::vector<CraftedFrame> frames = {v5f(intent(kA)), v5f(prepared(kA)), v6f(closure(kA)), v6f(intent(kB)),
                                              v6f(closure(kB))};
    const Crafted c = craft_log(frames);
    ASSERT_TRUE(c.ok);
    const AuditLogVerification v = verify_audit_log(c.log, *ring_, true, c.anchor, false, {});
    ASSERT_TRUE(v.ok()) << export_status_name(v.status);
    EXPECT_EQ(v.frames, frames.size());
    EXPECT_EQ(v.max_frame_version, 6u);
    EXPECT_NE(audit_export_manifest_line(v).find("\"frame_format_version\":6,"), std::string::npos);
}

TEST_F(NeverSentReaderTest, TheVerifierRefusesTheClosureInAV5Frame) {
    const Crafted c = craft_log(
        {v5f(intent(kA)), v5f(prepared(kA)), relabelled(closure(kA), kFrameFormatVersionV6, kFrameFormatVersion)});
    ASSERT_TRUE(c.ok);
    const AuditLogVerification v = verify_audit_log(c.log, *ring_, true, c.anchor, false, {});
    EXPECT_EQ(v.status, ExportStatus::MalformedFrame) << "event 23 and state 12 in a v5 frame";
    EXPECT_EQ(v.error_sequence, 2u);
}

TEST_F(NeverSentReaderTest, TheExportNamesTheNeverSentEventAndState) {
    const Crafted c = craft_log({v5f(intent(kA)), v5f(prepared(kA)), v6f(closure(kA))});
    ASSERT_TRUE(c.ok);
    std::array<std::byte, kKeyBlockSize> key_block{};
    ASSERT_TRUE(ring_->active_key(kKeyId, key_block));
    DecodedOrderFrame frame{};
    std::size_t frame_size = 0;
    ASSERT_EQ(decode_order_event_frame(std::span<const std::byte>(c.log).subspan(2 * kOrderEventFrameSize),
                                       std::span<const std::byte>(key_block.data(), key_block.size()), frame, frame_size),
              FrameDecodeStatus::Ok);
    EXPECT_EQ(frame.format_version, kFrameFormatVersionV6);
    const std::string line = audit_export_frame_line(frame);
    EXPECT_NE(line.find("\"event\":\"ORDER_NEVER_SENT\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"resulting_state\":\"NeverSent\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"detail_msg\":\"final gate refused\""), std::string::npos) << line;
}

TEST(NeverSentNames, TheEventAndTheStateHaveTheirOwnNames) {
    EXPECT_STREQ(audit_event_name(AuditEventType::OrderNeverSent), "ORDER_NEVER_SENT");
    EXPECT_STREQ(order_state_name(OrderState::NeverSent), "NeverSent");
    EXPECT_STREQ(audit_event_name(static_cast<AuditEventType>(24)), "UNKNOWN") << "one past the newest event names nothing";
}
