// D5 slice R1: the recovery reader's handling of OrderOperatorResolved frames and of frame format version 6
// (docs/OPERATOR_RESOLUTION_DESIGN.md 7.2 and 10, docs/NOTSENT_CLOSURE_DESIGN.md 5).
//
// There is deliberately NO production writer of a v6 frame or of event 22 in R1 (the offline tool is slice R3), so the
// logs here are CRAFTED. They are crafted with as little invention as possible: every frame is made by the production
// encoder (encode_order_event_frame with an explicit version), chained and anchored the way DurableAuditSink does it,
// and every log is then read by the production sink and verifier. The one pairing the encoder refuses (event 22 in a v5
// frame, an unread version) is made by relabelling a frame and signing it again, so the ONLY wrong thing about it is
// the pairing -- a MAC failure would be caught later than the check under test and could hide its absence.
//
// A scenario that reopens the same store many times in one test uses a fresh file name each time (see the fixture):
// deleting a log and creating a file of the same name a moment later races a real-time antivirus scanner on a
// Windows host (docs/SPEC_INVARIANTS.md; DurableAuditSinkTest::use_fresh_base_path).

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
#include <utility>
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
    static const char kKey[] = "operator-resolution-reader-test-key";
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

class OperatorResolutionReaderTest : public ::testing::Test {
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
        stem_ = std::string(tmp) + "hy_opres_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter);
#else
        stem_ = "/tmp/hy_opres_" + std::to_string(getpid()) + "_" + std::to_string(counter);
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

    // Lays `c` down as a log + tip anchor on a fresh path, the way a writer that died right after an append would
    // have left it.
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

    void expect_resolved_cleanly(const std::vector<CraftedFrame>& frames, const char* why) {
        write_frames(frames);
        const auto sink = open();
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean) << why;
        EXPECT_FALSE(sink->fenced()) << why;
        EXPECT_EQ(sink->recovered_operator_resolved().size(), 1u) << why;
        EXPECT_TRUE(sink->recovered_checkpoints().empty()) << why;
    }
};

namespace {
const OrderSpec kA{"HY-OPRES-A"};
const OrderSpec kB{"HY-OPRES-B"};
const OrderSpec kC{"HY-OPRES-C"};
}  // namespace

// --- what the reader accepts (design 7.2 rule 1: the three states nothing automatic can finish) -----------------------

TEST_F(OperatorResolutionReaderTest, ClosesAnOrderThatWasSubmittingAfterPrepared) {
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v6f(operator_resolved(kA))});
    const auto sink = open();
    EXPECT_FALSE(sink->fenced());
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean) << "nothing is left to reconcile";
    EXPECT_TRUE(sink->recovered_checkpoints().empty()) << "a resolved order is final: no checkpoint";
    EXPECT_EQ(sink->recovered_intent_orphans(), 0u);
    ASSERT_EQ(sink->recovered_operator_resolved().size(), 1u);
    const OperatorResolvedOrder& e = sink->recovered_operator_resolved()[0];
    EXPECT_STREQ(e.client_order_id.id, "HY-OPRES-A");
    EXPECT_EQ(e.symbol_id, kA.symbol_id);
    EXPECT_EQ(e.side, kA.side);
    EXPECT_EQ(e.qty_ticks, kA.qty);
    EXPECT_EQ(e.frame_sequence, 2u) << "the sequence number of the OrderOperatorResolved frame itself";
    EXPECT_EQ(sink->recovered_operator_resolved_total(), 1u);
    EXPECT_FALSE(sink->recovery_overflow().any());
    EXPECT_EQ(sink->write_format_version(), kFrameFormatVersionV6);
}

TEST_F(OperatorResolutionReaderTest, ClosesAnAmbiguousOrder) {
    expect_resolved_cleanly(
        {v5f(intent(kA)), v5f(prepared(kA)), v5f(ambiguous(kA)), v6f(operator_resolved(kA))}, "Ambiguous");
}

TEST_F(OperatorResolutionReaderTest, ClosesAnEscalatedOrderThroughTheOperatorEdge) {
    expect_resolved_cleanly({v5f(intent(kA)), v5f(prepared(kA)), v5f(ambiguous(kA)), v5f(escalated(kA)),
                             v6f(operator_resolved(kA))},
                            "EscalatedToOperator");
}

TEST_F(OperatorResolutionReaderTest, ARepeatedAmbiguousOrEscalatedFrameBeforeTheResolutionIsStillFine) {
    expect_resolved_cleanly({v5f(intent(kA)), v5f(prepared(kA)), v5f(ambiguous(kA)), v5f(ambiguous(kA)),
                             v5f(escalated(kA)), v5f(escalated(kA)), v6f(operator_resolved(kA))},
                            "repeats");
}

TEST_F(OperatorResolutionReaderTest, InformationalDetailBitsMayHoldAnyValue) {
    for (const std::int64_t code : {good_detail_code(0, 0), good_detail_code(0xFFFF, 0xFFFF), good_detail_code(1, 0xFFFF),
                                    good_detail_code(0xFFFF, 1)}) {
        expect_resolved_cleanly({v5f(intent(kA)), v5f(prepared(kA)), v6f(operator_resolved(kA, code))},
                                "waited seconds / tool version are informational");
    }
}

TEST_F(OperatorResolutionReaderTest, ClosesOnlyTheNamedOrderAndTheOthersStayRecovered) {
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v5f(intent(kB)), v5f(prepared(kB)), v5f(ambiguous(kB)),
                  v6f(operator_resolved(kA))});
    const auto sink = open();
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    ASSERT_EQ(sink->recovered_checkpoints().size(), 1u);
    EXPECT_STREQ(sink->recovered_checkpoints()[0].client_order_id.id, "HY-OPRES-B");
    EXPECT_EQ(sink->recovered_checkpoints()[0].resulting_state, OrderState::Ambiguous);
    ASSERT_EQ(sink->recovered_operator_resolved().size(), 1u);
    EXPECT_STREQ(sink->recovered_operator_resolved()[0].client_order_id.id, "HY-OPRES-A");
}

TEST_F(OperatorResolutionReaderTest, TheListIsInLogOrderOfTheResolutionFrames) {
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v5f(intent(kB)), v5f(prepared(kB)), v5f(intent(kC)),
                  v5f(prepared(kC)), v6f(operator_resolved(kB)), v6f(operator_resolved(kC)),
                  v6f(operator_resolved(kA))});
    const auto sink = open();
    ASSERT_EQ(sink->recovered_operator_resolved().size(), 3u);
    EXPECT_STREQ(sink->recovered_operator_resolved()[0].client_order_id.id, "HY-OPRES-B");
    EXPECT_STREQ(sink->recovered_operator_resolved()[1].client_order_id.id, "HY-OPRES-C");
    EXPECT_STREQ(sink->recovered_operator_resolved()[2].client_order_id.id, "HY-OPRES-A");
    EXPECT_EQ(sink->recovered_operator_resolved()[0].frame_sequence, 6u);
    EXPECT_EQ(sink->recovered_operator_resolved()[2].frame_sequence, 8u);
}

TEST_F(OperatorResolutionReaderTest, AResolvedOrderStaysResolvedAcrossRestarts) {
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v6f(operator_resolved(kA))});
    for (int pass = 0; pass < 3; ++pass) {
        const auto sink = open();
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean) << "pass " << pass;
        EXPECT_EQ(sink->recovered_operator_resolved().size(), 1u) << "pass " << pass;
        EXPECT_TRUE(sink->recovered_checkpoints().empty()) << "pass " << pass;
    }
}

TEST_F(OperatorResolutionReaderTest, ARecoveredResolvedOrderIsNotRegisteredInFlight) {
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v5f(ambiguous(kA)), v5f(escalated(kA)),
                  v6f(operator_resolved(kA)), v6f(intent(kB)), v6f(prepared(kB)), v6f(ambiguous(kB))});
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    InFlightRegistry registry;
    EXPECT_EQ(repopulate_in_flight_registry(registry, sink->recovered_checkpoints()), 1u)
        << "only B is handed to reconciliation";
    EXPECT_FALSE(registry.is_in_flight("HY-OPRES-A")) << "the resolved order must not stall StartupRecovery";
    EXPECT_TRUE(registry.is_in_flight("HY-OPRES-B"));
}

TEST_F(OperatorResolutionReaderTest, AnIntentOnlyOrderNextToAResolvedOneIsStillAnOrphan) {
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v6f(operator_resolved(kA)), v6f(intent(kB))});
    const auto sink = open();
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(sink->recovered_intent_orphans(), 1u);
    EXPECT_EQ(sink->recovered_operator_resolved().size(), 1u);
}

// --- what the reader refuses: every "no" is Corrupt for the whole log, and the sink fences ---------------------------

TEST_F(OperatorResolutionReaderTest, RefusesAnOrderThatIsNotInOneOfTheThreeStates) {
    expect_corrupt({v5f(intent(kA)), v6f(operator_resolved(kA))}, "Intent only: nothing was ever prepared");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(accepted(kA)), v6f(operator_resolved(kA))},
                   "Accepted: the exchange has it");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(accepted(kA)), v5f(partial_fill(kA)),
                    v6f(operator_resolved(kA))},
                   "PartialFill");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(accepted(kA)),
                    v5f(state_frame(kA, AuditEventType::OrderCancelRequested, OrderState::CancelRequested)),
                    v6f(operator_resolved(kA))},
                   "CancelRequested");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(accepted(kA)),
                    v5f(state_frame(kA, AuditEventType::OrderCancelled, OrderState::Cancelled)),
                    v6f(operator_resolved(kA))},
                   "Cancelled");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(accepted(kA)),
                    v5f(state_frame(kA, AuditEventType::OrderExpired, OrderState::Expired)),
                    v6f(operator_resolved(kA))},
                   "Expired");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(rejected_by_exchange(kA)), v6f(operator_resolved(kA))},
                   "Rejected by the exchange: already final");
}

TEST_F(OperatorResolutionReaderTest, RefusesAFrameThatDoesNotSayRejected) {
    for (const OrderState wrong : {OrderState::Accepted, OrderState::Cancelled, OrderState::Expired,
                                   OrderState::EscalatedToOperator, OrderState::Ambiguous, OrderState::Intent,
                                   OrderState::Submitting, OrderState::Filled}) {
        AuditRecord r = operator_resolved(kA);
        r.resulting_state = wrong;
        expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v6f(r)}, order_state_name(wrong));
    }
}

TEST_F(OperatorResolutionReaderTest, RefusesAFrameThatClaimsTheOrderExistedOrFilled) {
    {
        AuditRecord r = operator_resolved(kA);
        r.exchange_order_id = 5;
        expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v6f(r)}, "exchange_order_id != 0");
    }
    {
        AuditRecord r = operator_resolved(kA);
        r.filled_qty_ticks = 1;
        expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v6f(r)}, "filled_qty_ticks != 0");
    }
    {
        AuditRecord r = operator_resolved(kA);
        r.avg_fill_price_ticks = 1;
        expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v6f(r)}, "avg_fill_price_ticks != 0");
    }
}

TEST_F(OperatorResolutionReaderTest, RefusesAnOrderThatHadShownAnyTraceOfExistingOnTheExchange) {
    {
        AuditRecord amb = ambiguous(kA);
        amb.exchange_order_id = 77;
        expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(amb), v6f(operator_resolved(kA))},
                       "an exchange order id was recorded before");
    }
    {
        AuditRecord amb = ambiguous(kA);
        amb.filled_qty_ticks = 5;
        amb.avg_fill_price_ticks = 500'000;
        expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v5f(amb), v6f(operator_resolved(kA))},
                       "a fill was recorded before");
    }
}

TEST_F(OperatorResolutionReaderTest, RefusesAFrameThatDescribesAnotherOrderThanTheIntent) {
    const std::vector<CraftedFrame> head = {v5f(intent(kA)), v5f(prepared(kA))};
    auto with = [&](auto mutate, const char* why) {
        AuditRecord r = operator_resolved(kA);
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

TEST_F(OperatorResolutionReaderTest, RefusesEveryMalformedDetailCode) {
    constexpr std::uint64_t good = operator_resolution::kKindAbsent | operator_resolution::kAbsentRequiredBits;
    std::vector<std::pair<std::int64_t, const char*>> bad = {
        {0, "zero"},
        {static_cast<std::int64_t>(operator_resolution::kAbsentRequiredBits), "kind 0 (no kind)"},
        {static_cast<std::int64_t>((good & ~operator_resolution::kKindMask) | 2), "kind 2"},
        {static_cast<std::int64_t>((good & ~operator_resolution::kKindMask) | 15), "kind 15"},
        {static_cast<std::int64_t>((good & ~operator_resolution::kKindMask) | 9), "kind 9 (bit 3 set, low bits = 1)"},
        {static_cast<std::int64_t>((good & ~operator_resolution::kKindMask) | 3), "kind 3"},
        {static_cast<std::int64_t>(good | 2), "kind bits 1|2 together"},
        {static_cast<std::int64_t>(good | (std::uint64_t{1} << 10)), "reserved bit 10"},
        {static_cast<std::int64_t>(good | (std::uint64_t{1} << 15)), "reserved bit 15"},
        {static_cast<std::int64_t>(good | (std::uint64_t{1} << 48)), "reserved bit 48"},
        {static_cast<std::int64_t>(good | (std::uint64_t{1} << 62)), "reserved bit 62"},
        {static_cast<std::int64_t>(good | (std::uint64_t{1} << 63)), "the sign bit (negative)"},
        {-1, "-1"},
    };
    for (const std::uint64_t required :
         {operator_resolution::kFenceSatisfied, operator_resolution::kPointQueryUnknownOrder,
          operator_resolution::kAllOrdersAbsent, operator_resolution::kOpenOrdersAbsent,
          operator_resolution::kOperatorConfirmed, operator_resolution::kLockHeld}) {
        bad.emplace_back(static_cast<std::int64_t>(good & ~required), "a required evidence bit missing");
    }
    for (const auto& [code, why] : bad) {
        expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v6f(operator_resolved(kA, code))}, why);
    }
}

TEST_F(OperatorResolutionReaderTest, TheResolutionFrameIsTheLastFrameOfItsOrder) {
    const std::vector<CraftedFrame> head = {v5f(intent(kA)), v5f(prepared(kA)), v6f(operator_resolved(kA))};
    auto after = [&](const CraftedFrame& extra, const char* why) {
        std::vector<CraftedFrame> frames = head;
        frames.push_back(extra);
        expect_corrupt(frames, why);
    };
    after(v6f(operator_resolved(kA)), "a second resolution");
    after(v6f(rejected_by_exchange(kA)), "a stray Rejected: same state as the resolved order");
    after(v6f(intent(kA)), "a second Intent");
    after(v6f(prepared(kA)), "a Prepared");
    after(v6f(ambiguous(kA)), "an Ambiguous");
    after(v6f(escalated(kA)), "an EscalatedToOperator");
    after(v6f(accepted(kA)), "an Accepted");
    // Frames of OTHER orders after it are of course fine.
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v6f(operator_resolved(kA)), v6f(intent(kB)), v6f(prepared(kB))});
    const auto sink = open();
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    EXPECT_EQ(sink->recovered_checkpoints().size(), 1u);
}

TEST_F(OperatorResolutionReaderTest, TheResolutionNeverOpensAnOrder) {
    expect_corrupt({v6f(operator_resolved(kA))}, "the very first frame of the log");
    expect_corrupt({v5f(intent(kB)), v6f(operator_resolved(kA))}, "first frame of a COID, resulting_state Rejected");
    AuditRecord claims_intent = operator_resolved(kA);
    claims_intent.resulting_state = OrderState::Intent;
    expect_corrupt({v6f(claims_intent)}, "first frame of a COID that claims resulting_state Intent");
    expect_corrupt({v5f(intent(kB)), v6f(claims_intent)}, "same, not at the head of the log");
}

// --- format versions in one store (durable_frame_codec.hpp's FORMAT v5 -> v6 note) -----------------------------------

TEST_F(OperatorResolutionReaderTest, TheOperatorEventInAV5FrameIsCorruptNotEarly) {
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)),
                    relabelled(operator_resolved(kA), kFrameFormatVersionV6, kFrameFormatVersion)},
                   "event 22 inside a frame labelled v5, correctly signed");
}

TEST_F(OperatorResolutionReaderTest, AVersionThisBuildDoesNotReadIsCorrupt) {
    constexpr std::array<std::uint8_t, 6> kUnread = {0, 3, 4, 7, 8, 255};
    for (const std::uint8_t label : kUnread) {
        expect_corrupt({v5f(intent(kA)), relabelled(prepared(kA), kFrameFormatVersion, label)},
                       "an unread version label, correctly signed");
    }
}

TEST_F(OperatorResolutionReaderTest, VersionsNeverGoBackInsideOneStore) {
    expect_corrupt({v6f(intent(kA)), v5f(prepared(kA))}, "v6 then v5");
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v6f(operator_resolved(kA)), v5f(intent(kB))},
                   "v5, v6, then v5 again");
    expect_corrupt({v5f(intent(kA)), v6f(intent(kB)), v6f(intent(kC)), v5f(prepared(kA))},
                   "a v5 frame for an OLD order after v6 frames");
}

TEST_F(OperatorResolutionReaderTest, VersionsMayStayAndMayGoUp) {
    {
        write_frames({v5f(intent(kA)), v5f(prepared(kA)), v6f(operator_resolved(kA)), v6f(intent(kB)),
                      v6f(prepared(kB))});
        const auto sink = open();
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
        EXPECT_EQ(sink->write_format_version(), kFrameFormatVersionV6);
    }
    {
        // A store may also START at v6 (a build that writes v6 from the beginning; none does yet).
        write_frames({v6f(intent(kA)), v6f(prepared(kA)), v6f(ambiguous(kA))});
        const auto sink = open();
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
        EXPECT_EQ(sink->write_format_version(), kFrameFormatVersionV6);
    }
    {
        write_frames({v5f(intent(kA)), v5f(prepared(kA)), v5f(ambiguous(kA))});
        const auto sink = open();
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
        EXPECT_EQ(sink->write_format_version(), kFrameFormatVersion);
    }
}

// --- the writer follows the store's tip ----------------------------------------------------------------------------------

TEST_F(OperatorResolutionReaderTest, AnEmptyStoreWritesV5AndSoDoesEveryStoreThatExistsToday) {
    fresh_path();
    {
        const auto sink = open();
        ASSERT_TRUE(sink->is_open());
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
        EXPECT_EQ(sink->write_format_version(), kFrameFormatVersion);
        ASSERT_TRUE(sink->append_durable(intent(kA), 1000).acked());
        ASSERT_TRUE(sink->append_durable(prepared(kA), 1001).acked());
        EXPECT_EQ(sink->write_format_version(), kFrameFormatVersion);
    }
    const std::vector<std::byte> bytes = log_bytes();
    ASSERT_EQ(bytes.size(), 2 * kOrderEventFrameSize);
    EXPECT_EQ(bytes[0], std::byte{5});
    EXPECT_EQ(bytes[kOrderEventFrameSize], std::byte{5});
    const auto reopened = open();
    EXPECT_EQ(reopened->recovery_status(), RecoveryScanStatus::Recovered);
    EXPECT_EQ(reopened->write_format_version(), kFrameFormatVersion);
}

TEST_F(OperatorResolutionReaderTest, AStoreWhoseTipIsV6KeepsWritingV6AndSurvivesARestart) {
    write_frames({v5f(intent(kA)), v5f(prepared(kA)), v6f(operator_resolved(kA))});
    {
        const auto sink = open();
        ASSERT_EQ(sink->write_format_version(), kFrameFormatVersionV6);
        // The trading process picks the store up after the operator tool: its own appends must be v6 too, or the
        // next start would find a v5 frame after a v6 one and refuse the whole log.
        ASSERT_TRUE(sink->append_durable(intent(kB), 2000).acked());
        ASSERT_TRUE(sink->append_durable(prepared(kB), 2001).acked());
    }
    const std::vector<std::byte> bytes = log_bytes();
    ASSERT_EQ(bytes.size(), 5 * kOrderEventFrameSize);
    EXPECT_EQ(bytes[0], std::byte{5});
    EXPECT_EQ(bytes[1 * kOrderEventFrameSize], std::byte{5});
    EXPECT_EQ(bytes[2 * kOrderEventFrameSize], std::byte{6});
    EXPECT_EQ(bytes[3 * kOrderEventFrameSize], std::byte{6});
    EXPECT_EQ(bytes[4 * kOrderEventFrameSize], std::byte{6});

    const auto reopened = open();
    EXPECT_EQ(reopened->recovery_status(), RecoveryScanStatus::Recovered);
    ASSERT_EQ(reopened->recovered_checkpoints().size(), 1u);
    EXPECT_STREQ(reopened->recovered_checkpoints()[0].client_order_id.id, "HY-OPRES-B");
    EXPECT_EQ(reopened->recovered_operator_resolved().size(), 1u);
    EXPECT_EQ(reopened->write_format_version(), kFrameFormatVersionV6);
}

TEST_F(OperatorResolutionReaderTest, AV5StoreRefusesToAppendTheOperatorEventAndWritesNothing) {
    fresh_path();
    std::size_t size_before = 0;
    {
        const auto sink = open();
        ASSERT_TRUE(sink->append_durable(intent(kA), 1000).acked());
        ASSERT_TRUE(sink->append_durable(prepared(kA), 1001).acked());
        size_before = 2 * kOrderEventFrameSize;
        const AuditAppendResult r = sink->append_durable(operator_resolved(kA), 1002);
        EXPECT_FALSE(r.acked()) << "a v5 store must not take an event only v6 may carry";
        EXPECT_TRUE(sink->fenced());
        EXPECT_EQ(sink->fence_reason(), FenceReason::IoError);
        EXPECT_FALSE(sink->append_durable(intent(kB), 1003).acked()) << "fenced for good";
    }
    EXPECT_EQ(log_bytes().size(), size_before) << "nothing of the refused frame may reach the log";
    const auto reopened = open();
    EXPECT_EQ(reopened->recovery_status(), RecoveryScanStatus::Recovered) << "the store itself is untouched";
    EXPECT_FALSE(reopened->fenced());
    EXPECT_TRUE(reopened->recovered_operator_resolved().empty());
}

TEST_F(OperatorResolutionReaderTest, ATradingOpenIsTheDefault) {
    fresh_path();
    DurableAuditSink defaulted(base_path_, *ring_, kKeyId);  // what every existing call site writes
    EXPECT_EQ(defaulted.open_mode(), AuditSinkOpenMode::Trading);
    EXPECT_FALSE(defaulted.recovery_overflow().any());
    EXPECT_TRUE(defaulted.recovered_operator_resolved().empty());
}

// --- capacity: hard for the trading process, counted for the operator tool ------------------------------------------------

namespace {

std::string numbered_coid(std::size_t n) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "HY-OPRES-%06zu", n);
    return std::string(buf);
}

// n orders, each Intent + Prepared + OrderOperatorResolved. The first order's Intent and Prepared are v5 and every
// later frame v6, so the whole log is a legal v5 -> v6 sequence.
std::vector<CraftedFrame> many_resolved(std::size_t n) {
    std::vector<CraftedFrame> frames;
    frames.reserve(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        const OrderSpec o{numbered_coid(i)};
        const bool first = (i == 0);
        frames.push_back(first ? v5f(intent(o)) : v6f(intent(o)));
        frames.push_back(first ? v5f(prepared(o)) : v6f(prepared(o)));
        frames.push_back(v6f(operator_resolved(o)));
    }
    return frames;
}

std::vector<CraftedFrame> many_ambiguous(std::size_t n) {
    std::vector<CraftedFrame> frames;
    frames.reserve(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        const OrderSpec o{numbered_coid(i)};
        frames.push_back(v5f(intent(o)));
        frames.push_back(v5f(prepared(o)));
        frames.push_back(v5f(ambiguous(o)));
    }
    return frames;
}

}  // namespace

TEST_F(OperatorResolutionReaderTest, ExactlyTheBoundOfResolvedOrdersIsAccepted) {
    write_frames(many_resolved(kMaxOperatorResolved));
    const auto sink = open();
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_FALSE(sink->fenced());
    EXPECT_EQ(sink->recovered_operator_resolved().size(), kMaxOperatorResolved);
    EXPECT_EQ(sink->recovered_operator_resolved_total(), kMaxOperatorResolved);
    EXPECT_STREQ(sink->recovered_operator_resolved().front().client_order_id.id, numbered_coid(0).c_str());
    EXPECT_STREQ(sink->recovered_operator_resolved().back().client_order_id.id,
                 numbered_coid(kMaxOperatorResolved - 1).c_str());
}

TEST_F(OperatorResolutionReaderTest, OneResolvedOrderPastTheBoundStopsATradingOpen) {
    write_frames(many_resolved(kMaxOperatorResolved + 1));
    const auto sink = open();
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::CapacityExceeded)
        << "the late-fill alarm's list must be complete or the process must not start";
    EXPECT_TRUE(sink->fenced());
    EXPECT_EQ(sink->fence_reason(), FenceReason::IoError);
}

TEST_F(OperatorResolutionReaderTest, AnOperatorOpenCountsPastTheBoundAndStaysUsable) {
    write_frames(many_resolved(kMaxOperatorResolved + 1));
    const auto sink = open(AuditSinkOpenMode::OperatorOpen);
    EXPECT_EQ(sink->open_mode(), AuditSinkOpenMode::OperatorOpen);
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_FALSE(sink->fenced());
    EXPECT_EQ(sink->recovered_operator_resolved().size(), kMaxOperatorResolved) << "the first N, in log order";
    EXPECT_EQ(sink->recovered_operator_resolved_total(), kMaxOperatorResolved + 1);
    EXPECT_EQ(sink->recovery_overflow().operator_resolved, 1u);
    EXPECT_EQ(sink->recovery_overflow().checkpoints, 0u);
    EXPECT_EQ(sink->recovery_overflow().intent_orphans, 0u);
    EXPECT_TRUE(sink->recovery_overflow().any());
    EXPECT_EQ(sink->write_format_version(), kFrameFormatVersionV6);
    EXPECT_TRUE(sink->append_durable(intent(kA), 5000).acked()) << "the tool must still be able to append";
}

TEST_F(OperatorResolutionReaderTest, MoreUnresolvedOrdersThanKMaxInFlightStopATradingOpenButNotAnOperatorOpen) {
    write_frames(many_ambiguous(kMaxInFlight + 3));
    {
        const auto sink = open();
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::CapacityExceeded);
        EXPECT_TRUE(sink->fenced());
    }
    const auto sink = open(AuditSinkOpenMode::OperatorOpen);
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    EXPECT_FALSE(sink->fenced());
    ASSERT_EQ(sink->recovered_checkpoints().size(), kMaxInFlight) << "the first kMaxInFlight, in scan order";
    EXPECT_STREQ(sink->recovered_checkpoints()[0].client_order_id.id, numbered_coid(0).c_str());
    EXPECT_STREQ(sink->recovered_checkpoints()[kMaxInFlight - 1].client_order_id.id,
                 numbered_coid(kMaxInFlight - 1).c_str());
    EXPECT_EQ(sink->recovery_overflow().checkpoints, 3u);
    EXPECT_TRUE(sink->append_durable(intent(kA), 5000).acked());
}

TEST_F(OperatorResolutionReaderTest, MoreIntentOnlyOrphansThanTheBudgetStopATradingOpenButNotAnOperatorOpen) {
    std::vector<CraftedFrame> frames;
    for (std::size_t i = 0; i < 5; ++i) frames.push_back(v5f(intent(OrderSpec{numbered_coid(i)})));
    write_frames(frames);
    {
        const auto sink = open(AuditSinkOpenMode::Trading, /*max_orphans=*/2);
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::CapacityExceeded);
        EXPECT_TRUE(sink->fenced());
    }
    const auto sink = open(AuditSinkOpenMode::OperatorOpen, /*max_orphans=*/2);
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_FALSE(sink->fenced());
    EXPECT_EQ(sink->recovered_intent_orphans(), 2u);
    EXPECT_EQ(sink->recovery_overflow().intent_orphans, 3u);
}

TEST_F(OperatorResolutionReaderTest, AnOperatorOpenDoesNotRelaxAnythingButCapacity) {
    expect_corrupt({v6f(intent(kA)), v5f(prepared(kA))}, "version regression, OperatorOpen", AuditSinkOpenMode::OperatorOpen);
    expect_corrupt({v5f(intent(kA)), v5f(prepared(kA)), v6f(operator_resolved(kA, 0))},
                   "malformed detail_code, OperatorOpen", AuditSinkOpenMode::OperatorOpen);
}

// --- the verifier (durable_audit_export.hpp) applies the same version rules -----------------------------------------------

TEST_F(OperatorResolutionReaderTest, TheVerifierReportsTheHighestFrameVersionInTheManifest) {
    const std::vector<std::vector<CraftedFrame>> logs = {
        {v5f(intent(kA)), v5f(prepared(kA))},
        {v5f(intent(kA)), v5f(prepared(kA)), v6f(operator_resolved(kA))},
        {v6f(intent(kA)), v6f(prepared(kA))},
    };
    const std::array<std::uint8_t, 3> expected = {5, 6, 6};
    for (std::size_t i = 0; i < logs.size(); ++i) {
        const Crafted c = craft_log(logs[i]);
        ASSERT_TRUE(c.ok);
        const AuditLogVerification v = verify_audit_log(c.log, *ring_, true, c.anchor, false, {});
        ASSERT_TRUE(v.ok()) << export_status_name(v.status) << " log " << i;
        EXPECT_EQ(v.frames, logs[i].size());
        EXPECT_EQ(v.max_frame_version, expected[i]) << "log " << i;
        const std::string manifest = audit_export_manifest_line(v);
        const std::string needle = "\"frame_format_version\":" + std::to_string(expected[i]) + ",";
        EXPECT_NE(manifest.find(needle), std::string::npos) << manifest;
    }
}

TEST_F(OperatorResolutionReaderTest, AnEmptyLogIsReportedAtTheVersionAWriterWouldStartIt) {
    const Crafted c = craft_log({});
    ASSERT_TRUE(c.ok);
    const AuditLogVerification v = verify_audit_log(c.log, *ring_, false, {}, false, {});
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(v.max_frame_version, 0u);
    EXPECT_NE(audit_export_manifest_line(v).find("\"frame_format_version\":5,"), std::string::npos);
}

TEST_F(OperatorResolutionReaderTest, TheVerifierRefusesAVersionRegressionAndAnUnreadVersion) {
    {
        const Crafted c = craft_log({v5f(intent(kA)), v6f(intent(kB)), v5f(prepared(kA))});
        const AuditLogVerification v = verify_audit_log(c.log, *ring_, true, c.anchor, false, {});
        EXPECT_EQ(v.status, ExportStatus::MalformedFrame);
        EXPECT_EQ(v.error_sequence, 2u);
        EXPECT_EQ(v.error_offset, 2 * kOrderEventFrameSize);
    }
    {
        const Crafted c = craft_log({v5f(intent(kA)), relabelled(prepared(kA), kFrameFormatVersion, 7)});
        const AuditLogVerification v = verify_audit_log(c.log, *ring_, true, c.anchor, false, {});
        EXPECT_EQ(v.status, ExportStatus::UnknownVersion);
        EXPECT_EQ(v.error_sequence, 1u);
    }
    {
        const Crafted c = craft_log(
            {v5f(intent(kA)), v5f(prepared(kA)),
             relabelled(operator_resolved(kA), kFrameFormatVersionV6, kFrameFormatVersion)});
        const AuditLogVerification v = verify_audit_log(c.log, *ring_, true, c.anchor, false, {});
        EXPECT_EQ(v.status, ExportStatus::MalformedFrame) << "event 22 in a v5 frame";
        EXPECT_EQ(v.error_sequence, 2u);
    }
}

TEST_F(OperatorResolutionReaderTest, TheExportNamesTheOperatorEvent) {
    const Crafted c = craft_log({v5f(intent(kA)), v5f(prepared(kA)), v6f(operator_resolved(kA))});
    ASSERT_TRUE(c.ok);
    std::array<std::byte, kKeyBlockSize> key_block{};
    ASSERT_TRUE(ring_->active_key(kKeyId, key_block));
    DecodedOrderFrame frame{};
    std::size_t frame_size = 0;
    ASSERT_EQ(decode_order_event_frame(std::span<const std::byte>(c.log).subspan(2 * kOrderEventFrameSize),
                                       std::span<const std::byte>(key_block.data(), key_block.size()), frame,
                                       frame_size),
              FrameDecodeStatus::Ok);
    EXPECT_EQ(frame.format_version, kFrameFormatVersionV6);
    const std::string line = audit_export_frame_line(frame);
    EXPECT_NE(line.find("\"event\":\"ORDER_OPERATOR_RESOLVED\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"resulting_state\":\"Rejected\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"detail_msg\":\"op=test;ev="), std::string::npos) << line;
}

// --- the shape of the machine field, on its own ---------------------------------------------------------------------------

TEST(OperatorResolutionDetailCode, TheLayoutIsFrozen) {
    using namespace operator_resolution;
    EXPECT_EQ(kKindAbsent, 1u);
    EXPECT_EQ(kFenceSatisfied, 0x10u);
    EXPECT_EQ(kPointQueryUnknownOrder, 0x20u);
    EXPECT_EQ(kAllOrdersAbsent, 0x40u);
    EXPECT_EQ(kOpenOrdersAbsent, 0x80u);
    EXPECT_EQ(kOperatorConfirmed, 0x100u);
    EXPECT_EQ(kLockHeld, 0x200u);
    EXPECT_EQ(kAbsentRequiredBits, 0x3F0u);
    EXPECT_EQ(kWaitedSecondsShift, 16u);
    EXPECT_EQ(kToolVersionShift, 32u);
    EXPECT_EQ(kReservedMask, 0xFFFF00000000FC00ull);
    EXPECT_TRUE(is_valid_absent_detail_code(0x3F1));
    EXPECT_FALSE(is_valid_absent_detail_code(0x3F0)) << "no kind";
    EXPECT_FALSE(is_valid_absent_detail_code(0x1F1)) << "a required bit missing";
    EXPECT_FALSE(is_valid_absent_detail_code(0x7F1)) << "bit 10 is reserved";
    EXPECT_TRUE(is_valid_absent_detail_code(good_detail_code(0xFFFF, 0xFFFF)));
    EXPECT_FALSE(is_valid_absent_detail_code(std::numeric_limits<std::int64_t>::min() | 0x3F1));
}

TEST(OperatorResolutionBounds, TheListBoundIsTheDocumentedOne) {
    // 1024 is a deliberate default (docs/OPERATOR_RESOLUTION_DESIGN.md 7.3, durable_control_plane.hpp): the late-fill
    // alarm's list must be complete or a Trading open does not start, and an operator procedure -- not an accident --
    // produced every entry. The tests above are relative to the constant; this one pins its value, so changing it
    // is a decision somebody makes on purpose.
    EXPECT_EQ(kMaxOperatorResolved, 1024u);
    EXPECT_GT(kMaxOperatorResolved, kMaxInFlight);
}
