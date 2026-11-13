// Q23 slice S4: the closure WRITER -- DurableAuditSink::append_never_sent(), behind the NeverSentWrites constructor switch
// (docs/NOTSENT_CLOSURE_DESIGN.md 4.3 to 4.5, 5.4 and 8).
//
// S2/S3 taught the recovery reader the OrderNeverSent frame (test_never_sent_reader.cpp crafts its logs, because nothing could
// write one). This slice adds the one function that can, and these are real sinks on real files:
//   * the switch: a sink built without it, or built for the offline operator tool, refuses; so does a fenced sink, an empty
//     client order id, and a frame the closure rule refuses -- in that order, and every one of them before a byte is written;
//   * a closure of a class A order (Intent only) and of a class B order (Intent, Prepared), and what it does to the store: the
//     frame is v6, the sink writes v6 from then on, the next start calls the order closed;
//   * the faults of design 4.5, made on the disk: an append that FAILS (the frame is on the disk anyway -- the tip anchor is what
//     could not be written -- and the sink is fenced, so nothing can follow it: no release, no second attempt), a frame that
//     lands late (the next start closes the order, which is still true), a frame that never lands (the order stays Prepared,
//     which is Ambiguous), a torn tail at every interesting length (cut, order not closed), and the one failure that does not
//     fence (the key is gone: nothing written);
//   * the writer's pre-check and the reader's rules are the same rules: for a table of frames and histories, the writer accepts
//     exactly the closures the next start accepts;
//   * the old AbortedPreSend double send, as a must-refuse: a closure followed by a "compensating" second attempt under the same
//     id leaves a store the next start refuses to open (docs/NOTSENT_CLOSURE_DESIGN.md 4.2 and 7, SingleAttemptCoid).
//
// A scenario that reopens a store many times uses a fresh file name each time (see the fixture): deleting a log and creating a
// file of the same name a moment later races a real-time antivirus scanner on a Windows host.

#include <gtest/gtest.h>
#include <hengyuan/durable_audit_sink.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
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

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kKeyId = 1;

std::vector<std::byte> plaintext_key() {
    static const char kKey[] = "never-sent-writer-test-key";
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
AuditRecord accepted(const OrderSpec& o) {
    AuditRecord r = make_record(o, AuditEventType::OrderAccepted, OrderState::Accepted);
    r.exchange_order_id = 77;
    return r;
}

// The closing frame: the NeverSent pair, nothing filled, no exchange id, the Intent's order, a reason in detail_code.
AuditRecord closure(const OrderSpec& o, std::int64_t reason = 3) {
    AuditRecord r = make_record(o, AuditEventType::OrderNeverSent, OrderState::NeverSent);
    r.detail_code = reason;
    r.set_detail("final gate refused");
    return r;
}

// The order as its owner knows it after `history`: the CALLER's account, which the sink cannot check (it holds no live order).
never_sent::ClosureTarget target_of(const std::vector<AuditRecord>& history) {
    never_sent::ClosureTarget t{};
    bool first = true;
    for (const AuditRecord& r : history) {
        if (first) {
            t.symbol_id = r.symbol_id;
            t.side = r.side;
            t.mode = r.mode;
            t.price_ticks = r.price_ticks;
            t.qty_ticks = r.qty_ticks;
            first = false;
        }
        t.state = r.resulting_state;
        if (r.exchange_order_id != 0) t.exchange_order_id = r.exchange_order_id;
        if (r.filled_qty_ticks > t.filled_qty_ticks) t.filled_qty_ticks = r.filled_qty_ticks;
    }
    return t;
}

// --- crafted logs (the reader's side of the agreement table) --------------------------------------------------------

struct CraftedFrame {
    std::uint8_t version;
    AuditRecord rec;
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
        const auto n = encode_order_event_frame(buf, kKeyId, seq, FrameTimeKind::ServerCorrectedUtc,
                                                static_cast<std::int64_t>(1000 + seq), f.rec, prev, key, f.version);
        if (n != kOrderEventFrameSize) return c;
        std::memcpy(prev.data(), buf.data() + kOrderEventFrameSize - kMacLen, kMacLen);
        c.log.insert(c.log.end(), buf.begin(), buf.end());
        ++seq;
    }
    if (!frames.empty()) detail::encode_tip_anchor(c.anchor, seq - 1, prev, kKeyId, key);
    c.ok = true;
    return c;
}

const OrderSpec kA{"HY-NSW-A"};
const OrderSpec kB{"HY-NSW-B"};

}  // namespace

class NeverSentWriterTest : public ::testing::Test {
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
        stem_ = std::string(tmp) + "hy_nsw_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter);
#else
        stem_ = "/tmp/hy_nsw_" + std::to_string(getpid()) + "_" + std::to_string(counter);
#endif
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x50 + i);
        ring_ = std::make_unique<KeyRing>(kek);
        WrappedKeyRecord rec{};
        ASSERT_EQ(ring_->add_key(kKeyId, plaintext_key(), rec), KeyRingAddStatus::Ok);
    }

    void TearDown() override { remove_all(); }

    void remove_all() {
        if (base_path_.empty()) return;
        std::error_code ec;
        for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".keyrotations", ".keyrotations.lock",
                                   ".keyrotations.tip", ".keyrotations.tip.tmp", ".storeid", ".storeid.lock",
                                   ".storeid.tip", ".storeid.tip.tmp"}) {
            fs::remove(base_path_ + suffix, ec);  // a file or an (empty) directory: the fault injection makes one
        }
    }

    // A store of its own for every scenario (see the file header): the previous one is deleted, the name never reused.
    void fresh_path() {
        remove_all();
        base_path_ = stem_ + "_" + std::to_string(++used_) + ".log";
    }

    std::unique_ptr<DurableAuditSink> open(AuditSinkOpenMode mode = AuditSinkOpenMode::Trading,
                                           NeverSentWrites writes = NeverSentWrites::Enabled) {
        return std::make_unique<DurableAuditSink>(base_path_, *ring_, kKeyId, kMaxIntentOrphans, mode, writes);
    }

    // Lays an order history down on a fresh path with the real writer (a plain trading sink: v5), then closes it.
    void write_history(const std::vector<AuditRecord>& frames) {
        fresh_path();
        DurableAuditSink sink(base_path_, *ring_, kKeyId);
        ASSERT_TRUE(sink.is_open());
        ASSERT_FALSE(sink.fenced());
        std::int64_t at = 1000;
        for (const AuditRecord& f : frames) ASSERT_TRUE(sink.append_durable(f, at++).acked()) << f.client_order_id;
    }

    // Lays raw log bytes and a tip anchor down on a fresh path, the way a writer that died right after an append would have
    // left them.
    void lay_down(const std::vector<std::byte>& log, const std::vector<std::byte>& anchor) {
        fresh_path();
        DurableLogStore store(base_path_, base_path_ + ".lock", base_path_ + ".tip");
        ASSERT_TRUE(store.acquire_lock());
        ASSERT_TRUE(store.open_log());
        if (!log.empty()) {
            ASSERT_TRUE(store.append_and_fsync(std::span<const std::byte>(log.data(), log.size())));
            ASSERT_TRUE(store.write_tip_anchor(std::span<const std::byte>(anchor.data(), anchor.size())));
        }
        store.close_log();
        store.release_lock();
    }

    void write_crafted(const std::vector<CraftedFrame>& frames) {
        const Crafted c = craft(*ring_, frames);
        ASSERT_TRUE(c.ok) << "the crafted log could not be built";
        lay_down(c.log, std::vector<std::byte>(c.anchor.begin(), c.anchor.end()));
    }

    static std::vector<std::byte> file_bytes(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::byte> out(raw.size());
        if (!raw.empty()) std::memcpy(out.data(), raw.data(), raw.size());
        return out;
    }

    std::vector<std::byte> log_bytes() const { return file_bytes(base_path_); }
    std::vector<std::byte> anchor_bytes() const { return file_bytes(base_path_ + ".tip"); }

    // The format version byte of frame `index` of the log on disk (offset 0 of every 256-byte frame).
    std::uint8_t version_of_frame(std::size_t index) const {
        const std::vector<std::byte> bytes = log_bytes();
        EXPECT_GE(bytes.size(), (index + 1) * kOrderEventFrameSize);
        return bytes.size() >= (index + 1) * kOrderEventFrameSize
                   ? static_cast<std::uint8_t>(bytes[index * kOrderEventFrameSize])
                   : std::uint8_t{0};
    }

    // What one append_never_sent() attempt on a freshly opened sink did, and whether it left the disk alone.
    struct Attempt {
        NeverSentAppend result{};
        bool fenced{false};
        std::uint8_t write_version{0};
        bool unchanged_on_disk{false};
    };

    Attempt attempt(AuditSinkOpenMode mode, NeverSentWrites writes, const never_sent::ClosureTarget& target,
                    const AuditRecord& rec, std::int64_t now_ms = 9000) {
        Attempt a;
        const std::vector<std::byte> log_before = log_bytes();
        const std::vector<std::byte> tip_before = anchor_bytes();
        {
            const auto sink = open(mode, writes);
            a.result = sink->append_never_sent(target, rec, now_ms);
            a.fenced = sink->fenced();
            a.write_version = sink->write_format_version();
        }
        a.unchanged_on_disk = log_bytes() == log_before && anchor_bytes() == tip_before;
        return a;
    }

    // A refusal made before anything touched the log: the status, no fence, the version the store had, not a byte changed.
    void expect_refused_untouched(const Attempt& a, NeverSentAppendStatus status, const char* why,
                                  std::uint8_t store_version = kFrameFormatVersion) {
        EXPECT_EQ(a.result.status, status) << why;
        EXPECT_FALSE(a.result.append.acked()) << why;
        EXPECT_FALSE(a.fenced) << why << ": a refusal is the caller's mistake, not damage to the store";
        EXPECT_EQ(a.write_version, store_version) << why;
        EXPECT_TRUE(a.unchanged_on_disk) << why << ": a refused call must not write a byte";
    }

    // A reopened store (the reading side needs no switch): the one order is closed -- no fence, no checkpoint, no orphan.
    void expect_closed_at_next_start(std::size_t closures, const char* why) {
        const auto sink = open(AuditSinkOpenMode::Trading, NeverSentWrites::Disabled);
        EXPECT_FALSE(sink->fenced()) << why;
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean) << why;
        EXPECT_EQ(sink->recovered_never_sent(), closures) << why;
        EXPECT_TRUE(sink->recovered_checkpoints().empty()) << why << ": a closed order is final";
        EXPECT_EQ(sink->recovered_intent_orphans(), 0u) << why << ": ...and not an Intent-only orphan either";
    }

    // The store as a failed closure leaves it: the history, then a closure whose tip anchor could not be written. Returns the
    // sink's own account (after the failing call; the sink is closed again before this returns).
    struct FailedClosure {
        NeverSentAppend result{};
        bool fenced{false};
        FenceReason reason{FenceReason::None};
        std::uint8_t write_version{0};
        std::uint64_t log_size_before{0};
        std::uint64_t log_size_after{0};
        AuditAppendResult later_append{};
        NeverSentAppend later_closure{};
        std::vector<std::byte> log_before;     // the files as they were before the failing call
        std::vector<std::byte> anchor_before;
    };

    FailedClosure fail_a_closure(const std::vector<AuditRecord>& history, const OrderSpec& order) {
        write_history(history);
        FailedClosure f;
        f.log_before = log_bytes();
        f.anchor_before = anchor_bytes();
        f.log_size_before = f.log_before.size();
        std::error_code ec;
        // The tip anchor is written to <tip>.tmp and then moved over <tip>: a DIRECTORY with that name makes the write fail, after
        // the frame itself was appended and fsynced -- exactly the "fsync of the frame happened, the anchor did not" window.
        EXPECT_TRUE(fs::create_directory(base_path_ + ".tip.tmp", ec)) << ec.message();
        {
            const auto sink = open();
            f.result = sink->append_never_sent(target_of(history), closure(order), 9000);
            f.fenced = sink->fenced();
            f.reason = sink->fence_reason();
            f.write_version = sink->write_format_version();
            f.later_append = sink->append_durable(prepared(order), 9001);     // "a second attempt under the same id"
            f.later_closure = sink->append_never_sent(target_of(history), closure(order), 9002);
        }
        f.log_size_after = log_bytes().size();
        fs::remove(base_path_ + ".tip.tmp", ec);  // the obstacle goes away, like the fault that caused it
        return f;
    }
};

// --- the switch and the refusals --------------------------------------------------------------------------------------------

TEST_F(NeverSentWriterTest, TheSwitchDefaultsToDisabledAndADisabledSinkRefusesAndWritesNothing) {
    write_history({intent(kA)});
    {
        DurableAuditSink defaulted(base_path_, *ring_, kKeyId);  // what production writes: no argument at all
        EXPECT_EQ(defaulted.never_sent_writes(), NeverSentWrites::Disabled);
        EXPECT_EQ(defaulted.open_mode(), AuditSinkOpenMode::Trading);
    }
    const Attempt a = attempt(AuditSinkOpenMode::Trading, NeverSentWrites::Disabled, target_of({intent(kA)}), closure(kA));
    expect_refused_untouched(a, NeverSentAppendStatus::NotEnabled, "a sink not told NeverSentWrites::Enabled");
    EXPECT_EQ(a.result.check, never_sent::ClosureCheck::NotChecked) << "the rule was never asked";
    const auto enabled = open();
    EXPECT_EQ(enabled->never_sent_writes(), NeverSentWrites::Enabled);
}

TEST_F(NeverSentWriterTest, AnOperatorOpenSinkRefusesEvenWhenEnabled) {
    write_history({intent(kA), prepared(kA)});
    const Attempt a = attempt(AuditSinkOpenMode::OperatorOpen, NeverSentWrites::Enabled, target_of({intent(kA), prepared(kA)}),
                              closure(kA));
    expect_refused_untouched(a, NeverSentAppendStatus::NotTradingOpen, "the offline tool writes its own frame, not this one");
}

TEST_F(NeverSentWriterTest, AFencedSinkRefusesAndWritesNothing) {
    write_history({intent(kA), prepared(kA)});
    // damage one byte inside the first frame: the MAC no longer verifies, recovery says Corrupt and the sink fences itself
    std::vector<std::byte> bytes = log_bytes();
    ASSERT_GT(bytes.size(), 100u);
    bytes[100] = static_cast<std::byte>(~static_cast<unsigned>(bytes[100]));
    {
        std::ofstream out(base_path_, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    // an empty id and a refusable order at the same time: the fence is reported first
    AuditRecord rec = closure(kA);
    rec.set_client_order_id("");
    const Attempt a = attempt(AuditSinkOpenMode::Trading, NeverSentWrites::Enabled,
                              target_of({intent(kA), prepared(kA), ambiguous(kA)}), rec);
    EXPECT_EQ(a.result.status, NeverSentAppendStatus::SinkFenced);
    EXPECT_TRUE(a.fenced);
    EXPECT_EQ(a.write_version, kFrameFormatVersion);
    EXPECT_TRUE(a.unchanged_on_disk);
}

TEST_F(NeverSentWriterTest, AnEmptyClientOrderIdNamesNoOrder) {
    write_history({intent(kA)});
    AuditRecord rec = closure(kA);
    rec.set_client_order_id("");
    const Attempt a = attempt(AuditSinkOpenMode::Trading, NeverSentWrites::Enabled, target_of({intent(kA)}), rec);
    expect_refused_untouched(a, NeverSentAppendStatus::EmptyClientOrderId, "an empty id");
    EXPECT_EQ(a.result.check, never_sent::ClosureCheck::NotChecked) << "refused before the rule was asked";
}

TEST_F(NeverSentWriterTest, AOneCharacterClientOrderIdIsAnId) {
    // "Empty" means the first byte is the terminator, not the second: the shortest real id is one character long.
    const OrderSpec one{"X"};
    write_history({intent(one)});
    {
        const auto sink = open();
        const NeverSentAppend r = sink->append_never_sent(target_of({intent(one)}), closure(one), 9000);
        EXPECT_EQ(r.status, NeverSentAppendStatus::Appended);
        EXPECT_TRUE(r.append.acked());
    }
    expect_closed_at_next_start(1u, "a one-character id");
}

TEST_F(NeverSentWriterTest, ASinkWhoseLogCouldNotBeOpenedRefusesLikeAFencedOne) {
    // The sink never got to open its log (here: the name of its lock file is taken by a directory). It is not fenced -- nothing
    // was ever written -- but it cannot append, and append_never_sent() says so itself instead of leaving it to the append.
    fresh_path();
    std::error_code ec;
    ASSERT_TRUE(fs::create_directory(base_path_ + ".lock", ec)) << ec.message();
    const auto sink = open();
    ASSERT_FALSE(sink->is_open());
    const NeverSentAppend r = sink->append_never_sent(target_of({intent(kA)}), closure(kA), 9000);
    EXPECT_EQ(r.status, NeverSentAppendStatus::SinkFenced);
    EXPECT_FALSE(r.append.acked());
    EXPECT_EQ(sink->write_format_version(), kFrameFormatVersion);
    EXPECT_FALSE(fs::exists(base_path_)) << "a sink that could not open its log writes nothing, anywhere";
}

TEST_F(NeverSentWriterTest, TheFirstBrokenConditionIsTheOneReported) {
    write_history({intent(kA), prepared(kA), ambiguous(kA)});  // an order the rule refuses
    AuditRecord empty_id = closure(kA);
    empty_id.set_client_order_id("");
    const never_sent::ClosureTarget bad = target_of({intent(kA), prepared(kA), ambiguous(kA)});
    struct Row {
        const char* why;
        AuditSinkOpenMode mode;
        NeverSentWrites writes;
        const AuditRecord* rec;
        NeverSentAppendStatus expected;
    };
    const AuditRecord good_id = closure(kA);
    const Row rows[] = {
        {"not enabled, operator open, empty id, refusable order", AuditSinkOpenMode::OperatorOpen, NeverSentWrites::Disabled, &empty_id,
         NeverSentAppendStatus::NotEnabled},
        {"enabled, operator open, empty id, refusable order", AuditSinkOpenMode::OperatorOpen, NeverSentWrites::Enabled, &empty_id,
         NeverSentAppendStatus::NotTradingOpen},
        {"enabled, trading, empty id, refusable order", AuditSinkOpenMode::Trading, NeverSentWrites::Enabled, &empty_id,
         NeverSentAppendStatus::EmptyClientOrderId},
        {"enabled, trading, an id, refusable order", AuditSinkOpenMode::Trading, NeverSentWrites::Enabled, &good_id,
         NeverSentAppendStatus::Refused},
    };
    for (const Row& r : rows) {
        const Attempt a = attempt(r.mode, r.writes, bad, *r.rec);
        expect_refused_untouched(a, r.expected, r.why);
    }
    const Attempt last = attempt(AuditSinkOpenMode::Trading, NeverSentWrites::Enabled, bad, good_id);
    EXPECT_EQ(last.result.check, never_sent::ClosureCheck::NotInAClosableState);
}

TEST_F(NeverSentWriterTest, EachBrokenRuleIsRefusedWithThatRuleAndNothingIsWritten) {
    using never_sent::ClosureCheck;
    struct Case {
        const char* why;
        std::vector<AuditRecord> history;
        void (*mutate)(AuditRecord&);
        ClosureCheck expected;
    };
    AuditRecord exchange_id_frame = prepared(kA);
    exchange_id_frame.exchange_order_id = 9;
    AuditRecord fill_frame = prepared(kA);
    fill_frame.filled_qty_ticks = 1;
    const Case cases[] = {
        {"an order that may have crossed (Ambiguous)", {intent(kA), prepared(kA), ambiguous(kA)}, [](AuditRecord&) {},
         ClosureCheck::NotInAClosableState},
        {"an order the exchange accepted", {intent(kA), prepared(kA), accepted(kA)}, [](AuditRecord&) {},
         ClosureCheck::NotInAClosableState},
        {"an order that showed an exchange id", {intent(kA), exchange_id_frame}, [](AuditRecord&) {},
         ClosureCheck::OrderShowsExistence},
        {"an order that showed a fill", {intent(kA), fill_frame}, [](AuditRecord&) {}, ClosureCheck::OrderShowsExistence},
        {"the event without the state", {intent(kA), prepared(kA)},
         [](AuditRecord& r) { r.resulting_state = OrderState::Rejected; }, ClosureCheck::FramePairingBroken},
        {"the state without the event", {intent(kA), prepared(kA)},
         [](AuditRecord& r) { r.event_type = AuditEventType::OrderRejected; }, ClosureCheck::FramePairingBroken},
        {"a frame with an exchange id", {intent(kA), prepared(kA)}, [](AuditRecord& r) { r.exchange_order_id = 5; },
         ClosureCheck::FrameClaimsExistence},
        {"a frame with a fill", {intent(kA), prepared(kA)}, [](AuditRecord& r) { r.filled_qty_ticks = 1; },
         ClosureCheck::FrameClaimsExistence},
        {"a frame with an average price", {intent(kA), prepared(kA)}, [](AuditRecord& r) { r.avg_fill_price_ticks = 1; },
         ClosureCheck::FrameClaimsExistence},
        {"another symbol", {intent(kA), prepared(kA)}, [](AuditRecord& r) { r.symbol_id = 8; }, ClosureCheck::NotTheSameOrder},
        {"another side", {intent(kA), prepared(kA)}, [](AuditRecord& r) { r.side = OrderSide::Buy; },
         ClosureCheck::NotTheSameOrder},
        {"another price", {intent(kA), prepared(kA)}, [](AuditRecord& r) { r.price_ticks += 1; }, ClosureCheck::NotTheSameOrder},
        {"another quantity", {intent(kA), prepared(kA)}, [](AuditRecord& r) { r.qty_ticks += 1; }, ClosureCheck::NotTheSameOrder},
        {"another mode", {intent(kA), prepared(kA)}, [](AuditRecord& r) { r.mode = ExecutionMode::DryRun; },
         ClosureCheck::NotTheSameOrder},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.why);
        write_history(c.history);
        AuditRecord rec = closure(kA);
        c.mutate(rec);
        const Attempt a = attempt(AuditSinkOpenMode::Trading, NeverSentWrites::Enabled, target_of(c.history), rec);
        expect_refused_untouched(a, NeverSentAppendStatus::Refused, c.why);
        EXPECT_EQ(a.result.check, c.expected) << c.why;
    }
}

// --- the successful closure --------------------------------------------------------------------------------------------------

TEST_F(NeverSentWriterTest, ClosesAClassAOrderAndTheNextStartCallsItClosed) {
    write_history({intent(kA)});
    {
        const auto sink = open();
        ASSERT_EQ(sink->write_format_version(), kFrameFormatVersion);
        const NeverSentAppend r = sink->append_never_sent(target_of({intent(kA)}), closure(kA), 9000);
        EXPECT_EQ(r.status, NeverSentAppendStatus::Appended);
        EXPECT_EQ(r.check, never_sent::ClosureCheck::Ok);
        EXPECT_TRUE(r.append.acked());
        EXPECT_FALSE(sink->fenced());
        EXPECT_EQ(sink->write_format_version(), kFrameFormatVersionV6) << "the store's tip is v6 now";
    }
    EXPECT_EQ(version_of_frame(0), kFrameFormatVersion) << "history is never rewritten";
    EXPECT_EQ(version_of_frame(1), kFrameFormatVersionV6) << "the closure is a v6 frame";
    expect_closed_at_next_start(1u, "an Intent that was closed");
}

TEST_F(NeverSentWriterTest, ClosesAClassBOrderAndTheNextStartCallsItClosed) {
    write_history({intent(kA), prepared(kA)});
    {
        const auto sink = open();
        const NeverSentAppend r = sink->append_never_sent(target_of({intent(kA), prepared(kA)}), closure(kA), 9000);
        EXPECT_EQ(r.status, NeverSentAppendStatus::Appended);
        EXPECT_TRUE(r.append.acked());
        EXPECT_EQ(sink->write_format_version(), kFrameFormatVersionV6);
    }
    EXPECT_EQ(version_of_frame(1), kFrameFormatVersion);
    EXPECT_EQ(version_of_frame(2), kFrameFormatVersionV6);
    expect_closed_at_next_start(1u, "a Prepared order that was refused is not Ambiguous");
}

TEST_F(NeverSentWriterTest, ASinkThatClosedAnOrderKeepsWritingAtV6AndTheStoreStaysReadable) {
    write_history({intent(kA), prepared(kA)});
    {
        const auto sink = open();
        ASSERT_EQ(sink->append_never_sent(target_of({intent(kA), prepared(kA)}), closure(kA), 9000).status,
                  NeverSentAppendStatus::Appended);
        // the monotone version rule: a frame after a v6 frame is a v6 frame, whatever the event
        ASSERT_TRUE(sink->append_durable(intent(kB), 9001).acked());
    }
    EXPECT_EQ(version_of_frame(3), kFrameFormatVersionV6) << "the next order's Intent is written at the store's version";
    const auto sink = open(AuditSinkOpenMode::Trading, NeverSentWrites::Disabled);
    EXPECT_FALSE(sink->fenced());
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(sink->recovered_never_sent(), 1u);
    EXPECT_TRUE(sink->recovered_checkpoints().empty());
    EXPECT_EQ(sink->recovered_intent_orphans(), 1u) << "B has an Intent frame and nothing else";
    EXPECT_EQ(sink->write_format_version(), kFrameFormatVersionV6) << "a restart keeps the store's version";
}

TEST_F(NeverSentWriterTest, AStoreThatAlreadyHoldsAClosureTakesAnotherAndCountsBoth) {
    write_history({intent(kA), prepared(kA), intent(kB)});
    {
        const auto sink = open();
        ASSERT_EQ(sink->append_never_sent(target_of({intent(kA), prepared(kA)}), closure(kA), 9000).status,
                  NeverSentAppendStatus::Appended);
    }
    {
        const auto sink = open();  // a new session: the store's tip is v6 from the first frame on
        EXPECT_EQ(sink->write_format_version(), kFrameFormatVersionV6);
        const NeverSentAppend r = sink->append_never_sent(target_of({intent(kB)}), closure(kB), 9100);
        EXPECT_EQ(r.status, NeverSentAppendStatus::Appended);
        EXPECT_EQ(sink->write_format_version(), kFrameFormatVersionV6);
    }
    expect_closed_at_next_start(2u, "two closures in two sessions");
}

TEST_F(NeverSentWriterTest, TheSwitchBelongsToTheSinkNotToTheStore) {
    write_history({intent(kA), prepared(kA)});
    {
        const auto sink = open();
        ASSERT_EQ(sink->append_never_sent(target_of({intent(kA), prepared(kA)}), closure(kA), 9000).status,
                  NeverSentAppendStatus::Appended);
    }
    // a store with a closure in it, reopened by a sink that was not told to write one: it reads, it refuses to write
    const Attempt a = attempt(AuditSinkOpenMode::Trading, NeverSentWrites::Disabled, target_of({intent(kB)}), closure(kB));
    expect_refused_untouched(a, NeverSentAppendStatus::NotEnabled, "the store is v6, the switch is still the sink's", kFrameFormatVersionV6);
}

// --- the faults of design 4.5 ------------------------------------------------------------------------------------------------

TEST_F(NeverSentWriterTest, AFailedAppendFencesTheSinkAndNothingFollowsIt) {
    const std::vector<AuditRecord> history{intent(kA), prepared(kA)};
    const FailedClosure f = fail_a_closure(history, kA);
    EXPECT_EQ(f.result.status, NeverSentAppendStatus::AppendFailed);
    EXPECT_FALSE(f.result.append.acked());
    EXPECT_EQ(f.result.check, never_sent::ClosureCheck::Ok) << "the rule had said yes: the disk said no";
    EXPECT_TRUE(f.fenced);
    EXPECT_EQ(f.reason, FenceReason::IoError);
    EXPECT_EQ(f.write_version, kFrameFormatVersion) << "an append that was not acknowledged does not move the store's version";
    // "A Failed append may have completed": the frame IS on the disk, only its tip anchor is not. That is the whole reason a
    // failed closure may not release the order, roll it back, or try again under the same id.
    EXPECT_EQ(f.log_size_after, f.log_size_before + kOrderEventFrameSize);
    // ...and the fence is what makes the first two of those impossible: nothing more is written by this sink.
    EXPECT_FALSE(f.later_append.acked()) << "a second attempt under the same id (here: the Prepared frame again) is refused";
    EXPECT_EQ(f.later_closure.status, NeverSentAppendStatus::SinkFenced);
}

TEST_F(NeverSentWriterTest, ALateLandingFrameClosesTheOrderAtTheNextStart) {
    const FailedClosure f = fail_a_closure({intent(kA), prepared(kA)}, kA);
    ASSERT_EQ(f.result.status, NeverSentAppendStatus::AppendFailed);
    // The disk is as the failed call left it: the closure frame is there, the tip anchor still names the frame before it (the
    // late-fsync window the recovery scan tolerates). The order never crossed, so reading the closure is still correct.
    expect_closed_at_next_start(1u, "a closure whose acknowledgement never came but whose frame landed");
}

TEST_F(NeverSentWriterTest, AFrameThatNeverLandedLeavesAPreparedOrderAmbiguous) {
    const FailedClosure f = fail_a_closure({intent(kA), prepared(kA)}, kA);
    ASSERT_EQ(f.result.status, NeverSentAppendStatus::AppendFailed);
    lay_down(f.log_before, f.anchor_before);  // the disk as it was before the closure: the frame did not land
    const auto sink = open(AuditSinkOpenMode::Trading, NeverSentWrites::Disabled);
    EXPECT_FALSE(sink->fenced());
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered) << "an unresolved order is what Recovered means";
    EXPECT_EQ(sink->recovered_never_sent(), 0u);
    ASSERT_EQ(sink->recovered_checkpoints().size(), 1u) << "a lost closure is an order for the reconcile path -- never a silent close";
    EXPECT_EQ(sink->recovered_checkpoints()[0].resulting_state, OrderState::Ambiguous);
    EXPECT_STREQ(sink->recovered_checkpoints()[0].client_order_id.id, "HY-NSW-A");
}

TEST_F(NeverSentWriterTest, AClassAFrameThatNeverLandedLeavesAnIntentOrphan) {
    const FailedClosure f = fail_a_closure({intent(kA)}, kA);
    ASSERT_EQ(f.result.status, NeverSentAppendStatus::AppendFailed);
    lay_down(f.log_before, f.anchor_before);
    const auto sink = open(AuditSinkOpenMode::Trading, NeverSentWrites::Disabled);
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(sink->recovered_never_sent(), 0u);
    EXPECT_TRUE(sink->recovered_checkpoints().empty());
    EXPECT_EQ(sink->recovered_intent_orphans(), 1u);
}

TEST_F(NeverSentWriterTest, ATornClosureFrameIsCutAtTheNextStartAndTheOrderIsNotClosed) {
    // The store the way a successful closure leaves it, to take the closure frame's bytes from.
    write_history({intent(kA), prepared(kA)});
    const std::vector<std::byte> base_log = log_bytes();
    const std::vector<std::byte> base_anchor = anchor_bytes();
    {
        const auto sink = open();
        ASSERT_EQ(sink->append_never_sent(target_of({intent(kA), prepared(kA)}), closure(kA), 9000).status,
                  NeverSentAppendStatus::Appended);
    }
    const std::vector<std::byte> full_log = log_bytes();
    ASSERT_EQ(full_log.size(), base_log.size() + kOrderEventFrameSize);
    // A crash in the middle of the closure's append: any prefix of the frame, the tip anchor still the one before it.
    for (const std::size_t k : {std::size_t{1}, std::size_t{5}, std::size_t{6}, std::size_t{7}, std::size_t{27}, std::size_t{28},
                                std::size_t{100}, std::size_t{128}, std::size_t{200}, kOrderEventFrameSize - 1}) {
        SCOPED_TRACE("torn after " + std::to_string(k) + " bytes");
        std::vector<std::byte> torn(full_log.begin(), full_log.begin() + static_cast<std::ptrdiff_t>(base_log.size() + k));
        lay_down(torn, base_anchor);
        {
            const auto sink = open(AuditSinkOpenMode::Trading, NeverSentWrites::Disabled);
            EXPECT_FALSE(sink->fenced());
            EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered) << "a torn tail is cut, not Corrupt";
            EXPECT_EQ(sink->recovered_torn_tail_bytes(), k);
            EXPECT_EQ(sink->recovered_never_sent(), 0u) << "half a closure closes nothing";
            ASSERT_EQ(sink->recovered_checkpoints().size(), 1u);
            EXPECT_EQ(sink->recovered_checkpoints()[0].resulting_state, OrderState::Ambiguous);
        }
        EXPECT_EQ(log_bytes().size(), base_log.size()) << "the cut bytes are gone: the log ends on a frame boundary again";
    }
}

TEST_F(NeverSentWriterTest, AClosureThatCouldNotBeAppendedBecauseTheKeyIsGoneWritesNothingAndDoesNotFence) {
    // The one append failure that is not an I/O one: the active key is gone. Nothing was written, so nothing is fenced and the
    // version must not have moved -- a sink that believed it was at v6 would write v6 frames behind a log that has none.
    write_history({intent(kA), prepared(kA)});
    const std::vector<std::byte> before = log_bytes();
    {
        const auto sink = open();
        ASSERT_EQ(ring_->retire(kKeyId), RetireStatus::Retired);
        const NeverSentAppend r = sink->append_never_sent(target_of({intent(kA), prepared(kA)}), closure(kA), 9000);
        EXPECT_EQ(r.status, NeverSentAppendStatus::AppendFailed);
        EXPECT_FALSE(r.append.acked());
        EXPECT_FALSE(sink->fenced()) << "an unresolvable key is a correctable mistake, not damage";
        EXPECT_EQ(sink->write_format_version(), kFrameFormatVersion);
    }
    EXPECT_EQ(log_bytes(), before);
}

// --- the writer's pre-check and the reader never disagree -------------------------------------------------------------------

TEST_F(NeverSentWriterTest, WhatTheWriterAcceptsTheReaderAcceptsAndWhatItRefusesTheReaderCallsCorrupt) {
    AuditRecord exchange_id_frame = prepared(kA);
    exchange_id_frame.exchange_order_id = 9;
    AuditRecord fill_frame = prepared(kA);
    fill_frame.filled_qty_ticks = 1;
    const std::vector<AuditRecord> class_a{intent(kA)};
    const std::vector<AuditRecord> class_b{intent(kA), prepared(kA)};
    struct Variant {
        const char* why;
        std::vector<AuditRecord> history;
        void (*mutate)(AuditRecord&);
    };
    const Variant variants[] = {
        {"class A, as is", class_a, [](AuditRecord&) {}},
        {"class B, as is", class_b, [](AuditRecord&) {}},
        {"another reason code", class_b, [](AuditRecord& r) { r.detail_code = 65535; }},
        {"a zero reason code", class_b, [](AuditRecord& r) { r.detail_code = 0; }},
        {"a negative reason code", class_b, [](AuditRecord& r) { r.detail_code = -1; }},
        {"another message", class_b, [](AuditRecord& r) { r.set_detail("anything at all"); }},
        {"another timestamp", class_b, [](AuditRecord& r) { r.timestamp_ms += 12345; }},
        {"an Ambiguous order", {intent(kA), prepared(kA), ambiguous(kA)}, [](AuditRecord&) {}},
        {"an Accepted order", {intent(kA), prepared(kA), accepted(kA)}, [](AuditRecord&) {}},
        {"an order that showed an exchange id", {intent(kA), exchange_id_frame}, [](AuditRecord&) {}},
        {"an order that showed a fill", {intent(kA), fill_frame}, [](AuditRecord&) {}},
        {"the event without the state", class_b, [](AuditRecord& r) { r.resulting_state = OrderState::Rejected; }},
        {"the state without the event", class_b, [](AuditRecord& r) { r.event_type = AuditEventType::OrderRejected; }},
        {"an exchange id", class_b, [](AuditRecord& r) { r.exchange_order_id = 9; }},
        {"a fill", class_b, [](AuditRecord& r) { r.filled_qty_ticks = 1; }},
        {"an average price", class_b, [](AuditRecord& r) { r.avg_fill_price_ticks = 1; }},
        {"another symbol", class_b, [](AuditRecord& r) { r.symbol_id = 8; }},
        {"another side", class_b, [](AuditRecord& r) { r.side = OrderSide::Buy; }},
        {"another price", class_b, [](AuditRecord& r) { r.price_ticks += 1; }},
        {"another quantity", class_b, [](AuditRecord& r) { r.qty_ticks += 1; }},
        {"another mode", class_b, [](AuditRecord& r) { r.mode = ExecutionMode::DryRun; }},
    };
    int accepted_by_both = 0;
    int refused_by_both = 0;
    for (const Variant& v : variants) {
        SCOPED_TRACE(v.why);
        AuditRecord rec = closure(kA);
        v.mutate(rec);

        // the writer: the history laid down by the real writer, the sink asked to close the order as its owner knows it
        write_history(v.history);
        bool writer_accepts = false;
        {
            const auto sink = open();
            writer_accepts = sink->append_never_sent(target_of(v.history), rec, 9000).status == NeverSentAppendStatus::Appended;
        }

        // the reader: first the history alone -- a legal log, so that a refusal below is the closure's and nobody else's...
        std::vector<CraftedFrame> frames;
        for (const AuditRecord& h : v.history) frames.push_back({kFrameFormatVersion, h});
        write_crafted(frames);
        {
            const auto sink = open(AuditSinkOpenMode::Trading, NeverSentWrites::Disabled);
            EXPECT_NE(sink->recovery_status(), RecoveryScanStatus::Corrupt) << "the history itself must be a log the reader accepts";
        }
        // ...then the same history and the same frame as a v6 frame, laid down without asking the writer
        frames.push_back({kFrameFormatVersionV6, rec});
        write_crafted(frames);
        bool reader_accepts = false;
        {
            const auto sink = open(AuditSinkOpenMode::Trading, NeverSentWrites::Disabled);
            reader_accepts = sink->recovery_status() != RecoveryScanStatus::Corrupt;
        }

        EXPECT_EQ(writer_accepts, reader_accepts) << "the writer's pre-check and the reader's rules must be the same rules";
        (writer_accepts ? accepted_by_both : refused_by_both) += 1;
    }
    EXPECT_GE(accepted_by_both, 7) << "the table must contain frames both sides accept";
    EXPECT_GE(refused_by_both, 14) << "...and frames both sides refuse";
}

// --- the old AbortedPreSend double send, kept as a must-refuse ----------------------------------------------------------------

TEST_F(NeverSentWriterTest, TheOldAbortedPreSendDoubleSendLeavesAStoreTheNextStartRefuses) {
    // Round 13's AbortedPreSend read a failed append as "nothing happened", released the order and let it be sent again under
    // the same id: a double send. Here the closure is real and acknowledged, and then a caller does what that design did -- it
    // goes on with the same client order id (Prepared again, then an exchange acceptance). The sink does not judge what follows
    // a frame (it never has); the next start does: a frame after the closure is Corrupt, so no trading process runs on a store
    // that holds a re-sent order. (The model's SingleAttemptCoid control and formal/durable_log_recovery_abortedpresend.cfg
    // are the protocol-level twins of this test.)
    write_history({intent(kA), prepared(kA)});
    {
        const auto sink = open();
        ASSERT_EQ(sink->append_never_sent(target_of({intent(kA), prepared(kA)}), closure(kA), 9000).status,
                  NeverSentAppendStatus::Appended);
        ASSERT_TRUE(sink->append_durable(prepared(kA), 9001).acked());  // the "compensation": the same order sent again
        ASSERT_TRUE(sink->append_durable(accepted(kA), 9002).acked());
    }
    const auto sink = open(AuditSinkOpenMode::Trading, NeverSentWrites::Disabled);
    EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Corrupt);
    EXPECT_TRUE(sink->fenced()) << "a store that holds a re-sent order is never opened for trading";
}
