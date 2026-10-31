// SPDX-License-Identifier: proprietary
// test_operator_resolution_flow.cpp -- D5 slice R3a: the offline operator tool's procedure (operator_resolution_flow.hpp), run
// against REAL audit and control-plane logs in temporary files, a loopback fake exchange that holds orders and lags like the
// real one (operator_resolution_fakes.hpp), a fake clock the sleeper advances, a scripted console and an evidence store.
//
// What these pin, in the order the flow does it:
//   * what is refused without any traffic or wait (unknown / resolved / DryRun / unresolvable orders, a full resolved-table, a
//     symbol the control-plane log cannot name, a bad request, a session that is not ready) -- and that a refusal leaves both
//     logs byte for byte as they were;
//   * the fence: counted on the EXCHANGE clock from the session's reading, no evidence request before lock + W + B + margin + L,
//     every parameter counts, a stuck / backwards / garbage / slow clock ends in a refusal and not in a guess;
//   * the evidence: the point query, every allOrders window with no gap, openOrders, in that exchange's own lag -- an order that
//     becomes visible only after the early part of the wait is FOUND, because the tool waited for it;
//   * the conclusion: anything that is not a clean "no such order" is reported with its evidence and nobody is asked to confirm;
//   * the confirmation: exactly the phrase, once, or nothing is written;
//   * the frame: built from the CHECKPOINT, durable, readable by a trading start, carrying the digest of the stored evidence.

#include <gtest/gtest.h>
#include <hengyuan/operator_resolution_flow.hpp>

#include "operator_resolution_fakes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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
using namespace hy::operator_resolution;
using namespace hy::operator_resolution::testing;

namespace {

constexpr std::int64_t kT0 = 1'790'000'000'000;  // the exchange time at which the lock is taken
constexpr std::int64_t kFenceMs = kDefaultEvidenceWaitMs;  // 270 s
constexpr std::int64_t kSendAt = kT0 - 599'000;   // the Prepared frame's time: when the send started
// The timestamp inside kA's client order id: the EARLIER of kA's two anchors (the frame's time is 2 s later), so the one D5 / R3b-3's
// symbol-time guard measures the log's first naming against, unless a test gives the order another send time.
constexpr std::int64_t kCoidAt = kT0 - 601'000;
static_assert(client_order_id_timestamp("HY-1789999399000-1-0A0A").value().count() == kCoidAt);
// A control-plane log that has named its symbols for two months: for the tests whose orders were sent DAYS ago (the interval tests), so
// that the symbol-time guard has nothing to say about them.
constexpr std::int64_t kLongBefore = kT0 - std::int64_t{60} * 86'400'000;
constexpr std::uint32_t kKeyId = 1;
constexpr std::uint64_t kUid = kFakeAccountUid;  // the uid the fake exchange reports for its key, unless a test says otherwise

struct OrderSpec {
    std::string coid;
    std::uint32_t symbol_id{0};
    OrderSide side{OrderSide::Sell};
    std::int64_t price{500'000};
    std::int64_t qty{100};
    ExecutionMode mode{ExecutionMode::Live};
};

const OrderSpec kA{"HY-1789999399000-1-0A0A"};
const OrderSpec kB{"HY-1789999399000-2-0B0B"};
const OrderSpec kC{"HY-1789999399000-3-0C0C"};

AuditRecord make_record(const OrderSpec& o, AuditEventType ev, OrderState st) {
    AuditRecord r{};
    r.timestamp_ms = kT0 - 600'000;
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

struct Timed {
    AuditRecord rec;
    std::int64_t at_ms;
};

// send_at == 0 is a log without a trustworthy send-start time (the frame carries no wall time: every frame at 0).
std::vector<Timed> ambiguous_history(const OrderSpec& o, std::int64_t send_at = kSendAt) {
    const std::int64_t before = send_at > 0 ? send_at - 1000 : 0;
    const std::int64_t after = send_at > 0 ? send_at + 1000 : 0;
    return {{make_record(o, AuditEventType::OrderIntentCreated, OrderState::Intent), before},
            {make_record(o, AuditEventType::OrderSubmitPrepared, OrderState::Submitting), send_at},
            {make_record(o, AuditEventType::OrderAmbiguous, OrderState::Ambiguous), after}};
}

std::vector<Timed> accepted_history(const OrderSpec& o) {
    std::vector<Timed> v = ambiguous_history(o);
    v.pop_back();
    AuditRecord acc = make_record(o, AuditEventType::OrderAccepted, OrderState::Accepted);
    acc.exchange_order_id = 77;
    v.push_back({acc, kSendAt + 1000});
    return v;
}

std::vector<Timed> concat(std::vector<Timed> a, const std::vector<Timed>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

SymbolRules rule(std::string_view name) {
    SymbolRules r{};
    std::memcpy(r.symbol, name.data(), std::min(name.size(), kSymbolNameLen));
    r.is_trading = true;
    r.rules_version = 1;
    return r;
}

OperatorResolutionRequest request(const OrderSpec& o, OperatorEnvironment env = OperatorEnvironment::Testnet) {
    OperatorResolutionRequest r;
    r.client_order_id = o.coid;
    r.environment = env;
    r.account_uid = kUid;
    r.operator_label = "alice";
    return r;
}

// An exchange order that every endpoint shows from the first moment (the order the dead process managed to place).
FakeExchangeOrder visible_order(const OrderSpec& o) {
    FakeExchangeOrder e;
    e.client_order_id = o.coid;
    e.symbol = "BTCUSDT";
    e.created_ms = kSendAt + 500;
    e.point_visible_ms = 0;
    e.list_visible_ms = 0;
    return e;
}

// --- the same-order variants the console can type (plain functions: FakeConsole takes a function pointer) ---

std::string lower_first(const std::string& s) {
    std::string t = s;
    t[0] = 'a';
    return t;
}
std::string trailing_space(const std::string& s) { return s + " "; }
std::string leading_space(const std::string& s) { return " " + s; }
std::string other_digest(const std::string& s) {
    std::string t = s;
    t.back() = t.back() == '0' ? '1' : '0';
    return t;
}
std::string other_environment(const std::string& s) {
    std::string t = s;
    return t.replace(t.find("TESTNET"), 7, "PRODUCTION");
}
std::string other_uid(const std::string& s) {
    std::string t = s;
    return t.replace(t.find("123456789"), 9, "123456780");
}
std::string other_order(const std::string& s) {
    std::string t = s;
    return t.replace(t.find("HY-1789999399000-1-0A0A"), 23, "HY-1789999399000-2-0B0B");
}
std::string other_symbol(const std::string& s) {
    std::string t = s;
    return t.replace(t.find("BTCUSDT"), 7, "ETHUSDT");
}
std::string just_confirm(const std::string&) { return "CONFIRM"; }
std::string truncated(const std::string& s) { return s.substr(0, s.size() - 1); }

struct Rig {
    FakeExchange exchange;
    FakeSleeper sleeper;
    FakeConsole console;
    FakeStore store;
    OperatorPorts ports;

    explicit Rig(FakeWorld& world) : exchange(world), sleeper(world), ports{exchange, sleeper, console, store} {}
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
};

std::vector<std::byte> file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    if (!raw.empty()) std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

}  // namespace

class FlowTest : public ::testing::Test {
protected:
    std::string stem_;
    std::string audit_path_;
    std::string cp_path_;
    unsigned used_{0};
    std::unique_ptr<KeyRing> ring_;
    FakeWorld world_;
    std::unique_ptr<DurableAuditSink> audit_;
    std::unique_ptr<ControlPlaneLogSink> names_;
    std::unique_ptr<Rig> rig_;
    std::vector<std::byte> before_;  // the audit log before the operator sink was opened
    std::vector<std::byte> cp_before_;

    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        stem_ = std::string(tmp) + "hy_opflow_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter);
#else
        stem_ = "/tmp/hy_opflow_" + std::to_string(getpid()) + "_" + std::to_string(counter);
#endif
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x40 + i);
        ring_ = std::make_unique<KeyRing>(kek);
        static const char kKey[] = "operator-resolution-flow-test-key";
        std::vector<std::byte> key(sizeof(kKey) - 1);
        std::memcpy(key.data(), kKey, key.size());
        WrappedKeyRecord rec{};
        ASSERT_EQ(ring_->add_key(kKeyId, key, rec), KeyRingAddStatus::Ok);
        world_.now_ms = kT0;
        fresh_paths();
    }

    void TearDown() override {
        release();
        remove_all();
    }

    void remove_all() {
        for (const std::string& base : {audit_path_, cp_path_}) {
            if (base.empty()) continue;
            for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".cp.lock", ".cp.tip", ".cp.tip.tmp", ".keyrotations",
                                       ".keyrotations.lock", ".keyrotations.tip", ".keyrotations.tip.tmp", ".storeid",
                                       ".storeid.lock", ".storeid.tip", ".storeid.tip.tmp"}) {
                std::remove((base + suffix).c_str());
            }
        }
    }

    void fresh_paths() {
        release();
        remove_all();
        ++used_;
        audit_path_ = stem_ + "_" + std::to_string(used_) + ".audit.log";
        cp_path_ = stem_ + "_" + std::to_string(used_) + ".cp.log";
    }

    void release() {
        rig_.reset();
        names_.reset();
        audit_.reset();
    }

    void write_history(const std::vector<Timed>& frames) {
        DurableAuditSink sink(audit_path_, *ring_, kKeyId);
        ASSERT_TRUE(sink.is_open());
        ASSERT_FALSE(sink.fenced());
        for (const Timed& f : frames) ASSERT_TRUE(sink.append_durable(f.rec, f.at_ms).acked()) << f.rec.client_order_id;
    }

    // One snapshot per entry of `snapshots`, each listing the given names by position; stamped first_at, first_at + 1000, ... (the
    // default is an hour before the orders: D5 / R3b-3 judges a name by when the log first gave it, so a test about that says when).
    void write_names(const std::vector<std::vector<std::string>>& snapshots, std::int64_t first_at = kT0 - 3'600'000) {
        ControlPlaneLogSink sink(cp_path_, *ring_, kKeyId);
        ASSERT_TRUE(sink.is_open());
        std::int64_t t = first_at;
        for (const std::vector<std::string>& names : snapshots) {
            std::vector<SymbolRules> entries;
            for (const std::string& n : names) entries.push_back(rule(n));
            SymbolRegistrySnapshotPayload snap{};
            snap.timestamp_ms = t;
            snap.rules_version = 1;
            snap.symbol_count = static_cast<std::uint32_t>(entries.size());
            t += 1000;
            ASSERT_TRUE(sink.append_snapshot(snap, std::span<const SymbolRules>(entries.data(), entries.size())).acked());
        }
    }

    // Writes the logs, opens them the way the tool does (OperatorOpen) and builds the rig. Call before begin().
    void prepare(const std::vector<Timed>& history, const std::vector<std::vector<std::string>>& snapshots = {{"BTCUSDT"}},
                 std::int64_t first_snapshot_at = kT0 - 3'600'000) {
        write_history(history);
        write_names(snapshots, first_snapshot_at);
        before_ = file_bytes(audit_path_);
        cp_before_ = file_bytes(cp_path_);
        audit_ = std::make_unique<DurableAuditSink>(audit_path_, *ring_, kKeyId, kMaxIntentOrphans, AuditSinkOpenMode::OperatorOpen);
        names_ = std::make_unique<ControlPlaneLogSink>(cp_path_, *ring_, kKeyId);
        ASSERT_TRUE(audit_->is_open());
        ASSERT_FALSE(audit_->fenced());
        ASSERT_TRUE(names_->is_open());
        rig_ = std::make_unique<Rig>(world_);
    }

    OperatorSession begin(const FenceParams& fence = {}) { return begin_operator_session(*audit_, *names_, rig_->ports, fence); }

    // Both logs exactly as they were before the tool opened them?
    bool logs_untouched() {
        release();
        return file_bytes(audit_path_) == before_ && file_bytes(cp_path_) == cp_before_;
    }

    bool decode_last_frame(DecodedOrderFrame& frame) {
        const std::vector<std::byte> bytes = file_bytes(audit_path_);
        if (bytes.size() < kOrderEventFrameSize) return false;
        std::array<std::byte, kKeyBlockSize> key_block{};
        if (!ring_->active_key(kKeyId, key_block)) return false;
        std::size_t size = 0;
        return decode_order_event_frame(std::span<const std::byte>(bytes).subspan(bytes.size() - kOrderEventFrameSize),
                                        std::span<const std::byte>(key_block.data(), key_block.size()), frame, size) ==
               FrameDecodeStatus::Ok;
    }

    // Nothing but the session's own clock reading happened on the exchange, nothing slept, nothing was said.
    void expect_no_traffic() {
        EXPECT_EQ(rig_->exchange.calls.size(), 1u) << "only the session's serverTime";
        EXPECT_TRUE(rig_->sleeper.sleeps.empty());
        EXPECT_TRUE(rig_->console.lines.empty());
        EXPECT_TRUE(rig_->store.attempts.empty());
    }
};

// --- the happy path -----------------------------------------------------------------------------------------------------------

TEST_F(FlowTest, ResolvesAnAbsentOrderAfterTheFenceAndTheOperatorsPhrase) {
    prepare(ambiguous_history(kA));
    OperatorSession session = begin();
    ASSERT_EQ(session.status, SessionStatus::Ready);
    EXPECT_EQ(session.lock_ref.count(), kT0);

    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    ASSERT_EQ(o.status, ResolveStatus::Resolved) << resolve_status_name(o.status) << " / " << conclusion_name(o.conclusion);
    EXPECT_EQ(o.conclusion, AbsentConclusionStatus::Absent);
    EXPECT_EQ(o.confirmation.verdict, ConfirmationVerdict::Confirmed);
    EXPECT_EQ(o.append.status, OperatorAppendStatus::Appended);
    EXPECT_EQ(o.fence, FenceVerdict::Satisfied);
    EXPECT_EQ(o.order_check, ResolutionCheck::Ok);
    EXPECT_EQ(o.symbol_status, SymbolNameStatus::Known);

    // the fence, on the exchange clock: exactly W + B + margin + L after the session's reading, in 30 s steps
    EXPECT_EQ(o.lock_ref.count(), kT0);
    EXPECT_EQ(o.read_ref.count(), kT0 + kFenceMs);
    EXPECT_EQ(rig_->sleeper.total(), kFenceMs);
    EXPECT_EQ(rig_->sleeper.sleeps.size(), 9u);
    EXPECT_GE(rig_->exchange.first_evidence_call_ms(), kT0 + kFenceMs) << "no evidence request before the fence";
    EXPECT_EQ(rig_->exchange.count("order"), 1u);
    EXPECT_EQ(rig_->exchange.count("allOrders"), 1u);
    EXPECT_EQ(rig_->exchange.count("openOrders"), 1u);
    ASSERT_EQ(rig_->exchange.windows.size(), 1u);
    // the interval: from 2W before the earlier anchor (the id's timestamp, 2 s before the Prepared frame) to the read
    EXPECT_EQ(rig_->exchange.windows[0].first, 1'789'999'399'000 - 120'000);
    EXPECT_EQ(rig_->exchange.windows[0].second, kT0 + kFenceMs);

    // the evidence file, and what the operator typed is what he was shown
    ASSERT_EQ(rig_->store.stored.size(), 1u);
    const std::string& bundle = rig_->store.stored[0].second;
    EXPECT_EQ(rig_->store.stored[0].first, o.evidence_stem);
    EXPECT_EQ(o.evidence_stem, "HY-1789999399000-1-0A0A-" + std::to_string(kT0 + kFenceMs));
    EXPECT_EQ(sha256_hex(bundle), o.evidence_digest_hex);
    const std::string phrase = rig_->console.printed_phrase();
    EXPECT_EQ(phrase, "ABSENT HY-1789999399000-1-0A0A BTCUSDT TESTNET 123456789 " + o.evidence_digest_hex.substr(0, 8));
    EXPECT_EQ(rig_->console.last_typed, phrase);
    EXPECT_EQ(rig_->console.reads, 1);
    EXPECT_EQ(rig_->console.last_timeout, kDefaultConfirmTimeout);
    EXPECT_EQ(kDefaultConfirmTimeout, std::chrono::minutes{5}) << "five minutes: the longest the line reader accepts";
    EXPECT_TRUE(rig_->console.printed("waited 270 s"));
    // the wait is announced at every step with the exchange time still to go (seconds, rounded up)
    std::vector<std::string> waiting;
    for (const std::string& l : rig_->console.lines) {
        if (l.rfind("waiting for the fence: ", 0) == 0) waiting.push_back(l);
    }
    ASSERT_EQ(waiting.size(), 9u);
    EXPECT_EQ(waiting.front(), "waiting for the fence: 270 s of exchange time to go");
    EXPECT_EQ(waiting.back(), "waiting for the fence: 30 s of exchange time to go");
    EXPECT_TRUE(rig_->console.printed("account summary (display only): USDT free 1000.0"));
    // what the operator is shown about the order and about what the tool does NOT know
    EXPECT_TRUE(rig_->console.printed("== operator resolution: order HY-1789999399000-1-0A0A =="));
    EXPECT_TRUE(rig_->console.printed("symbol: BTCUSDT (id 0)  side: Sell  quantity: 100 ticks  price: 500000 ticks  mode: Live"));
    EXPECT_TRUE(rig_->console.printed("state in the log: Ambiguous  send started (log time, ms): 1789999401000"));
    EXPECT_TRUE(rig_->console.printed(
        "environment (stated by you): TESTNET  account uid (stated by you; the exchange reports the same for this API key): 123456789"));
    EXPECT_TRUE(rig_->console.printed("exchange clock: lock 1790000000000  read 1790000270000  waited 270 s  (W 60 s, B 60 s, margin 30 s, read lag L 120 s)"));
    EXPECT_TRUE(rig_->console.printed("point query: NoSuchOrder"));
    EXPECT_TRUE(rig_->console.printed("allOrders: 1 window(s) from 1789999279000 to 1790000270000, all Clear"));
    EXPECT_TRUE(rig_->console.printed("openOrders: Clear"));
    EXPECT_TRUE(rig_->console.printed("If you confirm, the order is closed as Rejected"));
    EXPECT_TRUE(rig_->console.printed("resolved: the frame is durable. Restart the trading process."));
    for (const char* assumption : {"A-D5-1", "A-D5-2", "A-D5-3", "A-D5-4"}) EXPECT_TRUE(rig_->console.printed(assumption)) << assumption;
    EXPECT_TRUE(rig_->console.printed(o.evidence_digest_hex)) << "the full digest is shown, the first 8 characters are typed";

    // the frame: durable, built from the checkpoint, readable by a trading start
    release();
    DecodedOrderFrame frame{};
    ASSERT_TRUE(decode_last_frame(frame));
    EXPECT_EQ(frame.format_version, kFrameFormatVersionV6);
    EXPECT_EQ(frame.record.event_type, AuditEventType::OrderOperatorResolved);
    EXPECT_EQ(frame.record.resulting_state, OrderState::Rejected);
    EXPECT_STREQ(frame.record.client_order_id, kA.coid.c_str());
    EXPECT_EQ(frame.record.symbol_id, kA.symbol_id);
    EXPECT_EQ(frame.record.side, kA.side);
    EXPECT_EQ(frame.record.price_ticks, kA.price);
    EXPECT_EQ(frame.record.qty_ticks, kA.qty);
    EXPECT_EQ(frame.record.mode, ExecutionMode::Live);
    EXPECT_EQ(frame.record.exchange_order_id, 0);
    EXPECT_EQ(frame.record.filled_qty_ticks, 0);
    EXPECT_EQ(frame.recorded_utc_ms, kT0 + kFenceMs) << "the exchange time of the last reading before the append";
    EXPECT_EQ(frame.time_kind, FrameTimeKind::ServerCorrectedUtc);
    const std::optional<AbsentDetail> detail = parse_absent_detail_code(frame.record.detail_code);
    ASSERT_TRUE(detail.has_value());
    EXPECT_EQ(detail->waited_seconds, 270);
    EXPECT_EQ(detail->tool_version, kOperatorToolVersion);
    EXPECT_EQ(std::string(frame.record.detail_msg), "op=alice;ev=" + o.evidence_digest_hex.substr(0, 32));

    DurableAuditSink trading(audit_path_, *ring_, kKeyId);
    EXPECT_EQ(trading.recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_FALSE(trading.fenced());
    EXPECT_TRUE(trading.recovered_checkpoints().empty());
    ASSERT_EQ(trading.recovered_operator_resolved().size(), 1u);
    EXPECT_STREQ(trading.recovered_operator_resolved()[0].client_order_id.id, kA.coid.c_str());
}

TEST_F(FlowTest, TheFenceBelongsToTheLockSoASecondOrderInTheSameSessionDoesNotWaitAgain) {
    prepare(concat(ambiguous_history(kA), ambiguous_history(kB)));
    OperatorSession session = begin();
    ASSERT_EQ(session.status, SessionStatus::Ready);
    ASSERT_EQ(resolve_order(session, request(kA)).status, ResolveStatus::Resolved);
    const std::size_t sleeps_after_first = rig_->sleeper.sleeps.size();
    const OperatorResolutionOutcome second = resolve_order(session, request(kB));
    ASSERT_EQ(second.status, ResolveStatus::Resolved) << resolve_status_name(second.status);
    EXPECT_EQ(rig_->sleeper.sleeps.size(), sleeps_after_first) << "the deadline had passed: no second wait";
    EXPECT_EQ(second.read_ref.count(), kT0 + kFenceMs);
    EXPECT_EQ(rig_->console.reads, 2);
    EXPECT_NE(rig_->console.printed_phrase().find(kB.coid), std::string::npos) << "and the phrase asked for is the second order's";
    EXPECT_EQ(rig_->store.stored.size(), 2u);
    EXPECT_NE(rig_->store.stored[0].second, rig_->store.stored[1].second);
    release();
    DurableAuditSink trading(audit_path_, *ring_, kKeyId);
    EXPECT_EQ(trading.recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(trading.recovered_operator_resolved().size(), 2u);
}

TEST_F(FlowTest, TheWaitIsAnnouncedInWholeSecondsRoundedUp) {
    prepare(ambiguous_history(kA));
    OperatorSession session = begin();
    world_.now_ms += 400;  // 0.4 s pass between the session's reading and the request: 269.6 s are left, not a whole number
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    ASSERT_EQ(o.status, ResolveStatus::Resolved) << resolve_status_name(o.status);
    std::vector<std::string> waiting;
    for (const std::string& l : rig_->console.lines) {
        if (l.rfind("waiting for the fence: ", 0) == 0) waiting.push_back(l);
    }
    ASSERT_FALSE(waiting.empty());
    EXPECT_EQ(waiting.front(), "waiting for the fence: 270 s of exchange time to go") << "269.6 s are announced as 270, never as 269";
    EXPECT_EQ(waiting.back(), "waiting for the fence: 30 s of exchange time to go") << "29.6 s are announced as 30";
    EXPECT_GE(rig_->exchange.first_evidence_call_ms(), kT0 + kFenceMs);
}

TEST_F(FlowTest, EachOrderIsHeldAgainstItsOwnCheckpointAndOrdersThatDifferDoNotMixUp) {
    OrderSpec other = kB;
    other.price = 777'000;
    other.qty = 5;
    other.side = OrderSide::Buy;
    other.symbol_id = 1;
    prepare(concat(ambiguous_history(kA), ambiguous_history(other)), {{"BTCUSDT", "ETHUSDT"}});
    OperatorSession session = begin();
    ASSERT_EQ(session.status, SessionStatus::Ready);
    // the second order first: its frame says Buy 5 @ 777000 on ETHUSDT, which is nothing like the first checkpoint
    const OperatorResolutionOutcome ob = resolve_order(session, request(other));
    ASSERT_EQ(ob.status, ResolveStatus::Resolved) << resolve_status_name(ob.status) << " / " << conclusion_name(ob.conclusion);
    EXPECT_EQ(ob.append.check, ResolutionCheck::Ok);
    EXPECT_TRUE(rig_->console.printed("symbol: ETHUSDT (id 1)  side: Buy  quantity: 5 ticks  price: 777000 ticks  mode: Live"));
    const OperatorResolutionOutcome oa = resolve_order(session, request(kA));
    ASSERT_EQ(oa.status, ResolveStatus::Resolved) << resolve_status_name(oa.status) << " / " << conclusion_name(oa.conclusion);
    EXPECT_TRUE(audit_->operator_resolved_this_session(0));
    EXPECT_TRUE(audit_->operator_resolved_this_session(1));
}

TEST_F(FlowTest, ARunThatStartsLaterCountsFromItsOwnReading) {
    world_.now_ms = kT0 + 12'345;
    prepare(ambiguous_history(kA));
    OperatorSession session = begin();
    ASSERT_EQ(session.status, SessionStatus::Ready);
    EXPECT_EQ(session.lock_ref.count(), kT0 + 12'345);
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    ASSERT_EQ(o.status, ResolveStatus::Resolved);
    EXPECT_EQ(o.read_ref.count(), kT0 + 12'345 + kFenceMs);
}

TEST_F(FlowTest, EveryFenceParameterAddsItsOwnTimeToTheWait) {
    struct Case {
        const char* name;
        FenceParams params;
        std::int64_t expected_total_ms;
    };
    FenceParams w;
    w.recv_window_ms += 30'000;
    FenceParams b;
    b.clock_skew_ms += 30'000;
    FenceParams m;
    m.margin_ms += 30'000;
    FenceParams l;
    l.read_lag_ms += 30'000;
    for (const Case& c : {Case{"W", w, kFenceMs + 30'000}, Case{"B", b, kFenceMs + 30'000}, Case{"margin", m, kFenceMs + 30'000},
                          Case{"L", l, kFenceMs + 30'000}, Case{"all four", FenceParams{120'000, 120'000, 60'000, 240'000}, 540'000}}) {
        SCOPED_TRACE(c.name);
        fresh_paths();
        world_.now_ms = kT0;
        prepare(ambiguous_history(kA));
        OperatorSession session = begin(c.params);
        ASSERT_EQ(session.status, SessionStatus::Ready);
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        ASSERT_EQ(o.status, ResolveStatus::Resolved);
        EXPECT_EQ(rig_->sleeper.total(), c.expected_total_ms);
        EXPECT_EQ(o.read_ref.count(), kT0 + c.expected_total_ms);
    }
}

// --- the fence against an exchange that lags --------------------------------------------------------------------------------

TEST_F(FlowTest, AnOrderThatShowsUpJustBeforeTheFenceEndsIsFoundBecauseTheToolWaited) {
    // The dead process's request was accepted, and every read shows it only from 5 s before the earliest allowed read: a tool that
    // read one step earlier (or dropped any component of the wait) would have declared the order absent.
    prepare(ambiguous_history(kA));
    FakeExchangeOrder e = visible_order(kA);
    e.point_visible_ms = kT0 + kFenceMs - 5'000;
    e.list_visible_ms = kT0 + kFenceMs - 5'000;
    rig_->exchange.orders.push_back(e);
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::EvidenceNotAbsent);
    EXPECT_EQ(o.conclusion, AbsentConclusionStatus::PointQueryNotAbsent);
    EXPECT_TRUE(rig_->console.printed("the evidence does not show that the exchange has no such order: PointQueryNotAbsent"));
    EXPECT_GE(rig_->exchange.first_evidence_call_ms(), kT0 + kFenceMs);
    EXPECT_EQ(rig_->console.reads, 0) << "nobody is asked to confirm a refusal";
    EXPECT_TRUE(rig_->console.printed_phrase().empty());
    ASSERT_EQ(rig_->store.stored.size(), 1u) << "the refusal's evidence is kept";
    EXPECT_TRUE(logs_untouched());
}

TEST_F(FlowTest, AnOrderVisibleFromTheVeryFirstAllowedReadIsFoundToo) {
    prepare(ambiguous_history(kA));
    FakeExchangeOrder e = visible_order(kA);
    e.point_visible_ms = kT0 + kFenceMs;
    e.list_visible_ms = kT0 + kFenceMs;
    rig_->exchange.orders.push_back(e);
    OperatorSession session = begin();
    EXPECT_EQ(resolve_order(session, request(kA)).status, ResolveStatus::EvidenceNotAbsent);
}

TEST_F(FlowTest, AnOrderThatOnlyAllOrdersShowsRefusesThroughAllOrders) {
    prepare(ambiguous_history(kA));
    FakeExchangeOrder e = visible_order(kA);
    e.point_visible_ms = kNever;  // the point query keeps saying "no such order"
    rig_->exchange.orders.push_back(e);
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::EvidenceNotAbsent);
    EXPECT_EQ(o.conclusion, AbsentConclusionStatus::AllOrdersNotClear);
    EXPECT_TRUE(rig_->console.printed("the evidence does not show that the exchange has no such order: AllOrdersNotClear"));
    EXPECT_EQ(rig_->console.reads, 0);
    EXPECT_TRUE(rig_->console.printed("allOrders page 1: Listed"));
    EXPECT_TRUE(logs_untouched());
}

TEST_F(FlowTest, AnOrderThatOnlyOpenOrdersShowsRefusesThroughOpenOrders) {
    prepare(ambiguous_history(kA));
    FakeExchangeOrder e = visible_order(kA);
    e.point_visible_ms = kNever;
    e.list_visible_ms = kNever;
    e.open_visible_ms = 0;
    rig_->exchange.orders.push_back(e);
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::EvidenceNotAbsent);
    EXPECT_EQ(o.conclusion, AbsentConclusionStatus::OpenOrdersNotClear);
    EXPECT_TRUE(rig_->console.printed("the evidence does not show that the exchange has no such order: OpenOrdersNotClear"));
    EXPECT_TRUE(rig_->console.printed("openOrders: Listed"));
    EXPECT_TRUE(logs_untouched());
}

TEST_F(FlowTest, AnotherOrdersOnTheBooksDoNotStandInTheWay) {
    prepare(ambiguous_history(kA));
    FakeExchangeOrder other = visible_order(kB);  // somebody else's order, open, listed
    other.open_visible_ms = 0;
    rig_->exchange.orders.push_back(other);
    OperatorSession session = begin();
    EXPECT_EQ(resolve_order(session, request(kA)).status, ResolveStatus::Resolved);
}

TEST_F(FlowTest, AnythingTheExchangeAnswersThatIsNotAClearNoSuchOrderIsARefusal) {
    struct Case {
        const char* name;
        std::uint16_t status;
        std::string body;
    };
    for (const Case& c : {Case{"archived", 400, "{\"code\":-2026,\"msg\":\"Order was archived.\"}"},
                          Case{"another error", 400, "{\"code\":-1021,\"msg\":\"Timestamp outside recvWindow.\"}"},
                          Case{"not 400", 404, "{\"code\":-2013,\"msg\":\"Order does not exist.\"}"},
                          Case{"empty body", 400, ""},
                          Case{"html", 502, "<html>bad gateway</html>"},
                          Case{"success without an order", 200, "{}"}}) {
        SCOPED_TRACE(c.name);
        fresh_paths();
        prepare(ambiguous_history(kA));
        rig_->exchange.unknown_status = c.status;
        rig_->exchange.unknown_body = c.body;
        OperatorSession session = begin();
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(o.status, ResolveStatus::EvidenceNotAbsent);
        EXPECT_EQ(o.conclusion, AbsentConclusionStatus::PointQueryNotAbsent);
        EXPECT_EQ(rig_->console.reads, 0);
        EXPECT_TRUE(logs_untouched());
    }
}

TEST_F(FlowTest, APageThatProvesNothingIsARefusal) {
    struct Case {
        const char* name;
        std::size_t filler;
        std::string symbol_override;
        std::uint16_t status;
        const char* verdict;
    };
    for (const Case& c : {Case{"a full page may be truncated", 1000, "", 200, "PossiblyTruncated"},
                          Case{"a page for another symbol", 1, "ETHUSDT", 200, "Malformed"},
                          Case{"not HTTP 200", 0, "", 500, "Malformed"}}) {
        SCOPED_TRACE(c.name);
        fresh_paths();
        prepare(ambiguous_history(kA));
        rig_->exchange.list_filler = c.filler;
        rig_->exchange.list_symbol_override = c.symbol_override;
        rig_->exchange.list_status = c.status;
        OperatorSession session = begin();
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(o.status, ResolveStatus::EvidenceNotAbsent);
        EXPECT_EQ(o.conclusion, AbsentConclusionStatus::AllOrdersNotClear);
        EXPECT_TRUE(rig_->console.printed(std::string("allOrders page 1: ") + c.verdict));
        ASSERT_EQ(rig_->store.stored.size(), 1u);
        EXPECT_NE(rig_->store.stored[0].second.find(c.verdict), std::string::npos) << "and the evidence file says so";
        EXPECT_TRUE(logs_untouched());
    }
}

TEST_F(FlowTest, AnIntervalOfSeveralDaysIsWalkedWindowByWindowWithNoGap) {
    const std::int64_t three_days = 3 * 86'400'000;
    prepare(ambiguous_history(kA, kT0 - three_days), {{"BTCUSDT"}}, kLongBefore);
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    ASSERT_EQ(o.status, ResolveStatus::Resolved) << resolve_status_name(o.status);
    const auto& w = rig_->exchange.windows;
    ASSERT_EQ(w.size(), 4u);
    for (std::size_t i = 0; i < w.size(); ++i) {
        EXPECT_LE(w[i].second - w[i].first, kAllOrdersMaxSpanMs) << i;
        if (i + 1 < w.size()) {
            EXPECT_EQ(w[i].second, w[i + 1].first) << "the next window starts where this one ended";
        }
    }
    EXPECT_EQ(w.front().first, kT0 - three_days - 120'000) << "the send start is the earlier anchor here";
    EXPECT_EQ(w.back().second, o.read_ref.count());
    EXPECT_EQ(rig_->exchange.count("allOrders"), 4u);
}

TEST_F(FlowTest, AnIntervalNoToolCanCoverIsRefusedBeforeAnyWait) {
    prepare(ambiguous_history(kA, kT0 - 20 * 86'400'000), {{"BTCUSDT"}}, kLongBefore);
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::NoUsableRange);
    EXPECT_EQ(o.plan, PlanStatus::TooManyPages);
    expect_no_traffic();
    EXPECT_TRUE(logs_untouched());
}

namespace {

// The longest interval the plan can cover: kAllOrdersMaxPages windows of kAllOrdersMaxSpanMs each. send_at_for_span() is the send
// start for which the interval from 2W (120 s) before it to the EARLIEST permitted read (lock + fence_ms) is exactly `span` long.
constexpr std::int64_t kLongestInterval = static_cast<std::int64_t>(kAllOrdersMaxPages) * kAllOrdersMaxSpanMs;
constexpr std::int64_t send_at_for_span(std::int64_t span, std::int64_t fence_ms) { return kT0 + fence_ms + 120'000 - span; }

}  // namespace

TEST_F(FlowTest, AnIntervalExactlyAtTheLimitIsWalkedInFourteenWindows) {
    prepare(ambiguous_history(kA, send_at_for_span(kLongestInterval, kFenceMs)), {{"BTCUSDT"}}, kLongBefore);
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    ASSERT_EQ(o.status, ResolveStatus::Resolved) << resolve_status_name(o.status) << " / " << conclusion_name(o.conclusion);
    EXPECT_EQ(o.plan, PlanStatus::Ok);
    EXPECT_EQ(rig_->exchange.count("allOrders"), kAllOrdersMaxPages);
}

TEST_F(FlowTest, ALongerRecvWindowWidensTheIntervalAndTheConclusionIsAskedWithTheSameParameters) {
    FenceParams fence;
    fence.recv_window_ms = 600'000;  // W = 10 min: the fence is 810 s and the interval starts 2W = 20 min before the send start
    constexpr std::int64_t fence_ms = 600'000 + 60'000 + 30'000 + 120'000;
    // one millisecond more than one window at W = 10 min -- and one window at the default W, which is the difference a conclusion
    // asked with the DEFAULT parameters would see: it would count one window against the two that were read
    const std::int64_t send_at = kT0 + fence_ms + 1'200'000 - (kAllOrdersMaxSpanMs + 1);
    prepare(ambiguous_history(kA, send_at), {{"BTCUSDT"}}, kLongBefore);
    OperatorSession session = begin(fence);
    ASSERT_EQ(session.status, SessionStatus::Ready);
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    ASSERT_EQ(o.status, ResolveStatus::Resolved) << resolve_status_name(o.status) << " / " << conclusion_name(o.conclusion);
    ASSERT_EQ(rig_->exchange.windows.size(), 2u);
    EXPECT_EQ(rig_->exchange.windows[0].first, send_at - 1'200'000);
    EXPECT_EQ(rig_->sleeper.total(), fence_ms);
    ASSERT_EQ(rig_->store.stored.size(), 1u);
    const std::string bundle = rig_->store.stored[0].second;  // the session's own parameters, not the defaults, are in the file
    for (const char* needle : {"\"recv_window_ms\":600000", "\"clock_skew_ms\":60000", "\"margin_ms\":30000", "\"read_lag_ms\":120000"}) {
        EXPECT_NE(bundle.find(needle), std::string::npos) << needle;
    }
    const std::optional<AbsentDetail> detail = [&] {
        release();
        DecodedOrderFrame frame{};
        return decode_last_frame(frame) ? parse_absent_detail_code(frame.record.detail_code) : std::nullopt;
    }();
    ASSERT_TRUE(detail.has_value());
    EXPECT_EQ(detail->waited_seconds, 810);
}

TEST_F(FlowTest, AnIntervalOneMillisecondOverTheLimitAtTheEarliestPermittedReadIsRefusedBeforeAnyWait) {
    prepare(ambiguous_history(kA, send_at_for_span(kLongestInterval + 1, kFenceMs)), {{"BTCUSDT"}}, kLongBefore);
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::NoUsableRange);
    EXPECT_EQ(o.range, RangeStatus::Ok);
    EXPECT_EQ(o.plan, PlanStatus::TooManyPages);
    expect_no_traffic();
    EXPECT_TRUE(logs_untouched());
}

TEST_F(FlowTest, AnIntervalThatOnlyGrowsOverTheLimitWhileTheToolWaitsIsRefusedAfterTheWaitAndBeforeAnyRequest) {
    FenceParams fence;
    fence.margin_ms = 45'000;  // a 285 s fence: the last sleep is shorter than a step and overshoots the fence by the pad
    constexpr std::int64_t fence_ms = 285'000;
    prepare(ambiguous_history(kA, send_at_for_span(kLongestInterval, fence_ms)), {{"BTCUSDT"}}, kLongBefore);  // fits at the earliest read, not 250 ms later
    OperatorSession session = begin(fence);
    ASSERT_EQ(session.status, SessionStatus::Ready);
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::NoUsableRange);
    EXPECT_EQ(o.fence, FenceVerdict::Satisfied);
    EXPECT_EQ(o.range, RangeStatus::Ok);
    EXPECT_EQ(o.plan, PlanStatus::TooManyPages);
    EXPECT_EQ(o.read_ref.count(), kT0 + fence_ms + kFenceWaitPadMs);
    EXPECT_EQ(rig_->sleeper.total(), fence_ms + kFenceWaitPadMs);
    EXPECT_EQ(rig_->exchange.first_evidence_call_ms(), kNever) << "not a single evidence request";
    EXPECT_TRUE(rig_->store.attempts.empty());
    EXPECT_EQ(rig_->console.reads, 0);
    EXPECT_TRUE(logs_untouched());
}

TEST_F(FlowTest, ASessionThatIsNotReadyIsNotUsedEvenWithItsPointersSet) {
    prepare(ambiguous_history(kA));
    OperatorSession session = begin();
    ASSERT_EQ(session.status, SessionStatus::Ready);
    for (const SessionStatus s : {SessionStatus::NotStarted, SessionStatus::BadFenceParams, SessionStatus::AuditNotUsable,
                                  SessionStatus::ServerTimeUnavailable}) {
        session.status = s;
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(o.status, ResolveStatus::SessionNotReady) << static_cast<int>(s);
    }
    expect_no_traffic();
    EXPECT_TRUE(logs_untouched());
}

TEST(FlowNames, EveryStatusAndEveryConclusionHasItsOwnName) {
    const std::array<std::string_view, 22> statuses{
        "NotRun",          "Resolved",           "BadRequest",       "SessionNotReady",     "AuditNotUsable",
        "NoSuchUnresolvedOrder", "AlreadyResolvedThisSession", "DryRunOrder", "NotResolvable", "TableFull",
        "SymbolNameNotKnown", "ServerTimeUnavailable", "FenceWaitTimedOut", "NoUsableRange", "EvidenceReadFailed",
        "EvidenceNotAbsent", "EvidenceStoreFailed", "NotConfirmed", "AppendRefused", "AppendFailed", "AccountUnverified",
        "AccountUidMismatch"};
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        EXPECT_EQ(resolve_status_name(static_cast<ResolveStatus>(i)), statuses[i]) << i;
    }
    EXPECT_EQ(resolve_status_name(static_cast<ResolveStatus>(statuses.size())), "Unknown");
    const std::array<std::string_view, 12> conclusions{"NotEvaluated",      "Absent",          "LockNotHeld",     "FenceNotSatisfied",
                                                       "InvalidFenceParams", "PointQueryNotAbsent", "NoUsableRange", "RangeNotPlannable",
                                                       "AllOrdersIncomplete", "AllOrdersNotClear", "OpenOrdersNotClear", "NotConfirmed"};
    for (std::size_t i = 0; i < conclusions.size(); ++i) {
        EXPECT_EQ(conclusion_name(static_cast<AbsentConclusionStatus>(i)), conclusions[i]) << i;
    }
    EXPECT_EQ(conclusion_name(static_cast<AbsentConclusionStatus>(conclusions.size())), "Unknown");
}

TEST_F(FlowTest, WithoutAnyAnchorTheToolRefusesBeforeAnyWait) {
    const OrderSpec odd{"ORDER-NOT-OURS-1"};  // no timestamp to read out of the id
    prepare(ambiguous_history(odd, 0));       // and a send-start time of 0: none
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(odd));
    EXPECT_EQ(o.status, ResolveStatus::NoUsableRange);
    EXPECT_EQ(o.range, RangeStatus::NoUsableAnchor);
    EXPECT_EQ(o.plan, PlanStatus::Unset) << "the range was unusable, so no plan was made from it";
    expect_no_traffic();
    EXPECT_TRUE(logs_untouched());
}

TEST_F(FlowTest, TheClientOrderIdsTimestampIsAnchorEnoughWhenTheLogHasNoSendTime) {
    prepare(ambiguous_history(kA, 0));  // no send-start time in the log
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    ASSERT_EQ(o.status, ResolveStatus::Resolved) << resolve_status_name(o.status);
    EXPECT_EQ(rig_->exchange.windows[0].first, 1'789'999'399'000 - 120'000);
}

// --- a clock that misbehaves ----------------------------------------------------------------------------------------------------

TEST_F(FlowTest, AClockThatStandsStillEndsInARefusalAfterABoundedWait) {
    prepare(ambiguous_history(kA));
    rig_->exchange.time_mode = FakeTimeMode::Stuck;
    OperatorSession session = begin();
    ASSERT_EQ(session.status, SessionStatus::Ready);
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::FenceWaitTimedOut);
    EXPECT_EQ(o.fence, FenceVerdict::TooEarly);
    EXPECT_EQ(rig_->sleeper.total(), kFenceMs + kFenceWaitSlackMs) << "the fence plus the slack, and not a second more";
    EXPECT_EQ(rig_->exchange.count("order"), 0u);
    EXPECT_TRUE(logs_untouched());
}

TEST_F(FlowTest, AClockThatGoesBackwardsIsNotTrusted) {
    prepare(ambiguous_history(kA));
    rig_->exchange.time_mode = FakeTimeMode::Backwards;
    OperatorSession session = begin();
    ASSERT_EQ(session.status, SessionStatus::Ready);
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::ServerTimeUnavailable);
    EXPECT_EQ(o.fence, FenceVerdict::ReadBeforeLock);
    EXPECT_EQ(rig_->exchange.count("order"), 0u);
    EXPECT_TRUE(logs_untouched());
}

TEST_F(FlowTest, AClockThatRunsSlowEndsInARefusalAndOneThatRunsALittleSlowStillWorks) {
    prepare(ambiguous_history(kA));
    world_.speed = 0.5;  // 30 s of waiting move the exchange clock by 15 s
    OperatorSession session = begin();
    EXPECT_EQ(resolve_order(session, request(kA)).status, ResolveStatus::FenceWaitTimedOut);
    EXPECT_TRUE(logs_untouched());

    fresh_paths();
    world_.now_ms = kT0;
    world_.speed = 0.9;
    prepare(ambiguous_history(kA));
    OperatorSession slow = begin();
    const OperatorResolutionOutcome o = resolve_order(slow, request(kA));
    ASSERT_EQ(o.status, ResolveStatus::Resolved) << resolve_status_name(o.status);
    EXPECT_GE(o.read_ref.count(), kT0 + kFenceMs);
    EXPECT_GE(rig_->exchange.first_evidence_call_ms(), kT0 + kFenceMs);
    EXPECT_GT(rig_->sleeper.total(), kFenceMs) << "a slow clock needs more local waiting than the fence itself";
}

TEST_F(FlowTest, AnImplausibleOrMissingTimeReadingStopsTheSessionAndTheWait) {
    prepare(ambiguous_history(kA));
    rig_->exchange.time_mode = FakeTimeMode::Garbage;
    EXPECT_EQ(begin().status, SessionStatus::ServerTimeUnavailable);
    rig_->exchange.time_mode = FakeTimeMode::Normal;
    rig_->exchange.fail_time_from_read = 1;  // the Garbage attempt above was reading 1: the next one is reading 2 and fails
    EXPECT_EQ(begin().status, SessionStatus::ServerTimeUnavailable);

    // a failure in the middle of the wait
    fresh_paths();
    prepare(ambiguous_history(kA));
    rig_->exchange.fail_time_from_read = 3;  // reading 1 is the session's, 2 and 3 are inside the wait, 4 fails
    OperatorSession session = begin();
    ASSERT_EQ(session.status, SessionStatus::Ready);
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::ServerTimeUnavailable);
    EXPECT_EQ(rig_->exchange.count("order"), 0u);
    EXPECT_TRUE(logs_untouched());

    // an implausible reading in the middle of the wait: refused, and the outcome says which kind of trouble it was
    fresh_paths();
    world_.now_ms = kT0;
    prepare(ambiguous_history(kA));
    OperatorSession garbled = begin();
    ASSERT_EQ(garbled.status, SessionStatus::Ready);
    rig_->exchange.time_mode = FakeTimeMode::Garbage;
    const OperatorResolutionOutcome g = resolve_order(garbled, request(kA));
    EXPECT_EQ(g.status, ResolveStatus::ServerTimeUnavailable);
    EXPECT_EQ(g.fence, FenceVerdict::ImplausibleTime);
    EXPECT_EQ(rig_->exchange.count("order"), 0u);
    EXPECT_TRUE(logs_untouched());
}

// --- transport failures during the evidence -------------------------------------------------------------------------------------

TEST_F(FlowTest, ARequestThatDoesNotCompleteIsNotEvidenceAndNothingIsStored) {
    for (const char* kind : {"order", "allOrders", "openOrders"}) {
        SCOPED_TRACE(kind);
        fresh_paths();
        world_.now_ms = kT0;
        prepare(ambiguous_history(kA));
        rig_->exchange.fail_kind = kind;
        OperatorSession session = begin();
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(o.status, ResolveStatus::EvidenceReadFailed);
        EXPECT_TRUE(rig_->store.attempts.empty());
        EXPECT_EQ(rig_->console.reads, 0);
        EXPECT_TRUE(logs_untouched());
    }
}

// --- refused without any traffic ----------------------------------------------------------------------------------------------------

TEST_F(FlowTest, AnOrderTheLogDoesNotHaveIsRefusedWithoutAnyTraffic) {
    prepare(ambiguous_history(kA));
    OperatorSession session = begin();
    EXPECT_EQ(resolve_order(session, request(kB)).status, ResolveStatus::NoSuchUnresolvedOrder);
    expect_no_traffic();
    EXPECT_TRUE(logs_untouched());
}

TEST_F(FlowTest, AnOrderTheSharedRuleRefusesIsRefusedWithItsReason) {
    {
        prepare(accepted_history(kA));  // the exchange has it: Accepted with an exchange id
        OperatorSession session = begin();
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(o.status, ResolveStatus::NotResolvable);
        EXPECT_EQ(o.order_check, ResolutionCheck::NotInAResolvableState);
        expect_no_traffic();
        EXPECT_TRUE(logs_untouched());
    }
    fresh_paths();
    {
        std::vector<Timed> h = ambiguous_history(kA);
        h.back().rec.exchange_order_id = 77;  // an Ambiguous order that once showed an exchange id
        prepare(h);
        OperatorSession session = begin();
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(o.status, ResolveStatus::NotResolvable);
        EXPECT_EQ(o.order_check, ResolutionCheck::OrderShowsExistence);
        expect_no_traffic();
        EXPECT_TRUE(logs_untouched());
    }
}

TEST_F(FlowTest, ADryRunOrderIsRefusedWithoutAnyTraffic) {
    OrderSpec dry = kA;
    dry.mode = ExecutionMode::DryRun;
    prepare(ambiguous_history(dry));
    OperatorSession session = begin();
    EXPECT_EQ(resolve_order(session, request(dry)).status, ResolveStatus::DryRunOrder);
    expect_no_traffic();
    EXPECT_TRUE(logs_untouched());
}

TEST_F(FlowTest, ASymbolTheControlPlaneLogCannotNameIsRefusedWithoutAnyTraffic) {
    {
        prepare(ambiguous_history(kA), {});  // no snapshot at all
        OperatorSession session = begin();
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(o.status, ResolveStatus::SymbolNameNotKnown);
        EXPECT_EQ(o.symbol_status, SymbolNameStatus::Unknown);
        expect_no_traffic();
        EXPECT_TRUE(logs_untouched());
    }
    fresh_paths();
    {
        prepare(ambiguous_history(kA), {{"BTCUSDT"}, {"ETHUSDT"}});  // two snapshots that disagree about id 0
        OperatorSession session = begin();
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(o.status, ResolveStatus::SymbolNameNotKnown);
        EXPECT_EQ(o.symbol_status, SymbolNameStatus::Conflict);
        expect_no_traffic();
        EXPECT_TRUE(logs_untouched());
    }
}

// --- the symbol must have been named before the order was sent (D5 / R3b-3, blueprint L-50) ------------------------------------
//
// "Every snapshot of the control-plane log agrees" does not say that this is the log that was in use when the order was placed: had
// it been deleted and recreated since, an old symbol_id may now mean another symbol and still be Known -- and "no such order" is true
// for a wrong symbol. So a name counts only for an order sent at or after the first snapshot that gave it; the order's send time is
// the earliest anchor it has (the id's timestamp, the frame's time). The listing and the resolution judge it through one function.

TEST_F(FlowTest, ANameTheLogFirstGaveAfterTheOrderWasSentIsRefusedWithoutAnyTraffic) {
    prepare(ambiguous_history(kA), {{"BTCUSDT"}}, kT0 - 500'000);  // the only snapshot is later than the order
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::SymbolNameNotKnown);
    EXPECT_EQ(o.symbol_status, SymbolNameStatus::NotNamedAtOrderTime);
    EXPECT_EQ(o.symbol_named_since_ms, kT0 - 500'000) << "the refusal says since when the log has had the name";
    EXPECT_EQ(o.range, RangeStatus::Unset) << "refused before the interval was worked out";
    EXPECT_EQ(o.fence, FenceVerdict::Unset);
    expect_no_traffic();
    EXPECT_TRUE(logs_untouched());
}

TEST_F(FlowTest, TheNameCountsFromTheMomentOfTheFirstSnapshotOnwardAndTheEarlierAnchorDecides) {
    struct Case {
        std::int64_t first_snapshot_at;
        bool accepted;
        const char* why;
    };
    // kA's anchors: the id's timestamp kCoidAt, the frame's time kSendAt = kCoidAt + 2000
    const Case cases[] = {
        {kCoidAt - 3'600'000, true, "an hour before"},
        {kCoidAt - 1, true, "a millisecond before the earlier anchor"},
        {kCoidAt, true, "exactly at the earlier anchor: a name is in force from the moment the log was given it"},
        {kCoidAt + 1, false, "a millisecond after the earlier anchor"},
        {kCoidAt + 1000, false, "between the two anchors: the frame's time alone would have let it through"},
        {kSendAt - 1, false, "just before the later anchor"},
        {kSendAt, false, "at the later anchor"},
        {kSendAt + 1, false, "after both"},
        {kT0, false, "at the lock"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.why);
        fresh_paths();
        prepare(ambiguous_history(kA), {{"BTCUSDT"}}, c.first_snapshot_at);
        const std::vector<UnresolvedOrderSummary> list = list_unresolved_orders(*audit_, *names_);
        ASSERT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0].symbol_status, c.accepted ? SymbolNameStatus::Known : SymbolNameStatus::NotNamedAtOrderTime);
        EXPECT_EQ(list[0].symbol, c.accepted ? "BTCUSDT" : "") << "a refused name is not handed out";
        EXPECT_EQ(list[0].symbol_named_since_ms, c.first_snapshot_at);
        EXPECT_EQ(list[0].order_check, ResolutionCheck::Ok) << "the order itself is as resolvable as before";
        // the judge itself: a refused lookup hands out no name, whatever the log's slot holds
        const auto checkpoints = audit_->recovered_checkpoints();
        ASSERT_EQ(checkpoints.size(), 1u);
        const SymbolNameLookup judged = symbol_for_order(*names_, checkpoints[0]);
        EXPECT_EQ(judged.status, list[0].symbol_status);
        EXPECT_EQ(judged.name, std::string_view(c.accepted ? "BTCUSDT" : ""));
        EXPECT_EQ(judged.named_since_ms, c.first_snapshot_at);
    }
}

TEST_F(FlowTest, AnOrderSentAtTheMomentOfTheFirstSnapshotIsResolvedAndOneMillisecondEarlierIsNot) {
    {
        prepare(ambiguous_history(kA), {{"BTCUSDT"}}, kCoidAt);
        OperatorSession session = begin();
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(o.status, ResolveStatus::Resolved) << resolve_status_name(o.status);
        EXPECT_EQ(o.symbol_status, SymbolNameStatus::Known);
    }
    fresh_paths();
    world_.now_ms = kT0;
    {
        prepare(ambiguous_history(kA), {{"BTCUSDT"}}, kCoidAt + 1);
        OperatorSession session = begin();
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(o.status, ResolveStatus::SymbolNameNotKnown);
        EXPECT_EQ(o.symbol_status, SymbolNameStatus::NotNamedAtOrderTime);
        expect_no_traffic();
        EXPECT_TRUE(logs_untouched());
    }
}

TEST_F(FlowTest, TheFramesTimeIsTheAnchorWhenItIsEarlierThanTheIdsAndWhenTheIdIsNotOurs) {
    // the send began 100 s before the id's timestamp (a clock that stepped, an id made by something else): the frame is the earlier
    const std::int64_t frame_at = kCoidAt - 100'000;
    {
        prepare(ambiguous_history(kA, frame_at), {{"BTCUSDT"}}, kCoidAt - 50'000);  // before the id's timestamp, after the frame
        const auto list = list_unresolved_orders(*audit_, *names_);
        ASSERT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0].symbol_status, SymbolNameStatus::NotNamedAtOrderTime);
    }
    fresh_paths();
    {
        prepare(ambiguous_history(kA, frame_at), {{"BTCUSDT"}}, frame_at);
        const auto list = list_unresolved_orders(*audit_, *names_);
        ASSERT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0].symbol_status, SymbolNameStatus::Known) << "at the frame's time exactly";
    }
    // an id with no timestamp to read: the frame alone
    const OrderSpec odd{"ORDER-NOT-OURS-1"};
    fresh_paths();
    {
        prepare(ambiguous_history(odd), {{"BTCUSDT"}}, kSendAt + 1);
        const auto list = list_unresolved_orders(*audit_, *names_);
        ASSERT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0].symbol_status, SymbolNameStatus::NotNamedAtOrderTime);
    }
    fresh_paths();
    {
        prepare(ambiguous_history(odd), {{"BTCUSDT"}}, kSendAt);
        const auto list = list_unresolved_orders(*audit_, *names_);
        ASSERT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0].symbol_status, SymbolNameStatus::Known);
    }
}

TEST_F(FlowTest, TheIdsTimestampIsTheAnchorWhenTheLogHasNoSendTime) {
    {
        prepare(ambiguous_history(kA, 0), {{"BTCUSDT"}}, kCoidAt + 1);
        const auto list = list_unresolved_orders(*audit_, *names_);
        ASSERT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0].send_started_utc_ms, 0);
        EXPECT_EQ(list[0].symbol_status, SymbolNameStatus::NotNamedAtOrderTime);
    }
    fresh_paths();
    {
        prepare(ambiguous_history(kA, 0), {{"BTCUSDT"}}, kCoidAt);
        const auto list = list_unresolved_orders(*audit_, *names_);
        ASSERT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0].symbol_status, SymbolNameStatus::Known);
    }
}

TEST_F(FlowTest, AFrameTimeThatIsNoPlausibleDateIsRefusedByTheRangeAndTheJudgeDoesNotTakeItForTheSendTime) {
    // The audit log is MAC-protected, so a garbage send-start time means something is wrong: the range check refuses it even though
    // the id's timestamp could stand in. The symbol judge, for its part, does not take it for the order's send time -- a tiny one
    // would otherwise make every name look too late, a huge one every name look old enough -- and goes by the id.
    for (const std::int64_t garbage : {std::int64_t{-5}, std::int64_t{1}, kMinPlausibleExchangeMs - 1, kMaxPlausibleExchangeMs + 1}) {
        SCOPED_TRACE(garbage);
        fresh_paths();
        world_.now_ms = kT0;
        prepare(ambiguous_history(kA, garbage));
        const auto list = list_unresolved_orders(*audit_, *names_);
        ASSERT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0].send_started_utc_ms, garbage);
        OperatorSession session = begin();
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(o.symbol_status, SymbolNameStatus::Known) << "judged by the id's timestamp, an hour after the names";
        EXPECT_EQ(o.status, ResolveStatus::NoUsableRange);
        EXPECT_EQ(o.range, RangeStatus::ImplausibleTime);
        expect_no_traffic();
        EXPECT_TRUE(logs_untouched());
    }
}

TEST_F(FlowTest, AnOrderWithNoAnchorIsNotJudgedForItsSymbolAndStillStopsAtTheRange) {
    // nothing to measure the log's naming against: the range check refuses this order before any request, as it always did
    const OrderSpec odd{"ORDER-NOT-OURS-1"};
    prepare(ambiguous_history(odd, 0), {{"BTCUSDT"}}, kT0 - 100);  // the name is as late as can be
    const auto list = list_unresolved_orders(*audit_, *names_);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].symbol_status, SymbolNameStatus::Known);
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(odd));
    EXPECT_EQ(o.status, ResolveStatus::NoUsableRange);
    EXPECT_EQ(o.symbol_status, SymbolNameStatus::Known);
    EXPECT_EQ(o.range, RangeStatus::NoUsableAnchor);
    expect_no_traffic();
}

TEST_F(FlowTest, ANameWithoutAUsableTimeIsRefusedForAnOrderThatHasAnAnchor) {
    // The first snapshot's frame carries no wall time, or an implausible one: the log cannot show when it began to know the name.
    for (const std::int64_t at : {std::int64_t{0}, std::int64_t{1}, std::int64_t{-5}, kMinPlausibleExchangeMs - 1}) {
        SCOPED_TRACE(at);
        fresh_paths();
        prepare(ambiguous_history(kA), {{"BTCUSDT"}}, at);
        const auto list = list_unresolved_orders(*audit_, *names_);
        ASSERT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0].symbol_status, SymbolNameStatus::NotNamedAtOrderTime);
        EXPECT_EQ(list[0].symbol_named_since_ms, at);
        EXPECT_TRUE(list[0].symbol.empty());
    }
    // the first plausible time there is, long before the order, is a time
    fresh_paths();
    prepare(ambiguous_history(kA), {{"BTCUSDT"}}, kMinPlausibleExchangeMs);
    const auto list = list_unresolved_orders(*audit_, *names_);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].symbol_status, SymbolNameStatus::Known);
}

TEST_F(FlowTest, TheFirstSnapshotThatNamedTheIdDecidesNotALaterOneAndEachIdIsJudgedByItsOwn) {
    OrderSpec second{"HY-1789999399000-4-0D0D"};  // the same timestamp as kA's id
    second.symbol_id = 1;
    // snapshot 1 names id 0 half a second before the orders; snapshot 2 (a second later, half a second after them) adds id 1
    prepare(concat(ambiguous_history(kA), ambiguous_history(second)), {{"BTCUSDT"}, {"BTCUSDT", "ETHUSDT"}}, kCoidAt - 500);
    const auto list = list_unresolved_orders(*audit_, *names_);
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].symbol_id, 0u);
    EXPECT_EQ(list[0].symbol_status, SymbolNameStatus::Known) << "id 0 was named by snapshot 1; snapshot 2, which came after the order, only agreed";
    EXPECT_EQ(list[0].symbol, "BTCUSDT");
    EXPECT_EQ(list[0].symbol_named_since_ms, kCoidAt - 500);
    EXPECT_EQ(list[1].symbol_id, 1u);
    EXPECT_EQ(list[1].symbol_status, SymbolNameStatus::NotNamedAtOrderTime) << "id 1 was first named by snapshot 2, after the order";
    EXPECT_TRUE(list[1].symbol.empty());
    EXPECT_EQ(list[1].symbol_named_since_ms, kCoidAt + 500);
}

TEST_F(FlowTest, AConflictAndAnUnknownNameAreStillWhatTheyWereWhateverTheTimes) {
    {
        prepare(ambiguous_history(kA), {{"BTCUSDT"}, {"ETHUSDT"}}, kT0 - 500'000);  // disagreeing, and late
        const auto list = list_unresolved_orders(*audit_, *names_);
        ASSERT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0].symbol_status, SymbolNameStatus::Conflict);
        EXPECT_EQ(list[0].symbol_named_since_ms, 0);
    }
    fresh_paths();
    {
        prepare(ambiguous_history(kA), {}, kT0 - 500'000);  // nothing named, and late
        const auto list = list_unresolved_orders(*audit_, *names_);
        ASSERT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0].symbol_status, SymbolNameStatus::Unknown);
        EXPECT_EQ(list[0].symbol_named_since_ms, 0);
    }
}

TEST_F(FlowTest, TheListingAndTheResolutionJudgeTheSymbolTheSameWay) {
    for (const std::int64_t at : {kCoidAt - 1000, kCoidAt, kCoidAt + 1, kSendAt, kT0 - 100, std::int64_t{0}}) {
        SCOPED_TRACE(at);
        fresh_paths();
        world_.now_ms = kT0;
        prepare(ambiguous_history(kA), {{"BTCUSDT"}}, at);
        const auto list = list_unresolved_orders(*audit_, *names_);
        ASSERT_EQ(list.size(), 1u);
        OperatorSession session = begin();
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(list[0].symbol_status, o.symbol_status);
        EXPECT_EQ(list[0].symbol_named_since_ms, o.symbol_named_since_ms);
        EXPECT_EQ(o.status, o.symbol_status == SymbolNameStatus::Known ? ResolveStatus::Resolved : ResolveStatus::SymbolNameNotKnown)
            << resolve_status_name(o.status);
    }
}

TEST_F(FlowTest, AResolvedOrderCannotBeResolvedAgainAndANewSessionDoesNotKnowItAnyMore) {
    prepare(ambiguous_history(kA));
    OperatorSession session = begin();
    ASSERT_EQ(resolve_order(session, request(kA)).status, ResolveStatus::Resolved);
    const std::size_t stored = rig_->store.stored.size();
    const std::size_t exchange_calls = rig_->exchange.calls.size();
    EXPECT_EQ(resolve_order(session, request(kA)).status, ResolveStatus::AlreadyResolvedThisSession);
    EXPECT_EQ(rig_->store.stored.size(), stored);
    EXPECT_EQ(rig_->exchange.calls.size(), exchange_calls) << "no traffic for the second attempt";

    // a new tool run over the same store: the order is final now, so it is not even listed
    release();
    audit_ = std::make_unique<DurableAuditSink>(audit_path_, *ring_, kKeyId, kMaxIntentOrphans, AuditSinkOpenMode::OperatorOpen);
    names_ = std::make_unique<ControlPlaneLogSink>(cp_path_, *ring_, kKeyId);
    rig_ = std::make_unique<Rig>(world_);
    OperatorSession again = begin();
    ASSERT_EQ(again.status, SessionStatus::Ready);
    EXPECT_EQ(resolve_order(again, request(kA)).status, ResolveStatus::NoSuchUnresolvedOrder);
}

TEST_F(FlowTest, AStoreWhoseResolvedTableIsFullRefusesBeforeAnyTraffic) {
    // kMaxOperatorResolved orders already closed by an operator frame, and kA still open: one more would leave a store no trading
    // start can load.
    std::array<std::byte, kKeyBlockSize> key_block{};
    ASSERT_TRUE(ring_->active_key(kKeyId, key_block));
    const std::span<const std::byte> key(key_block.data(), key_block.size());
    std::vector<std::byte> log;
    std::array<std::byte, kMacLen> prev{};
    std::uint64_t seq = 0;
    const auto put = [&](const AuditRecord& r, std::uint8_t version, std::int64_t at_ms) {
        std::array<std::byte, kOrderEventFrameSize> buf{};
        ASSERT_EQ(encode_order_event_frame(buf, kKeyId, seq, FrameTimeKind::ServerCorrectedUtc, at_ms, r, prev, key, version),
                  kOrderEventFrameSize);
        std::memcpy(prev.data(), buf.data() + kOrderEventFrameSize - kMacLen, kMacLen);
        log.insert(log.end(), buf.begin(), buf.end());
        ++seq;
    };
    std::vector<OrderSpec> closed;
    for (std::size_t i = 0; i < kMaxOperatorResolved; ++i) closed.push_back(OrderSpec{"HY-1789999399000-" + std::to_string(100 + i) + "-0A0A"});
    for (const OrderSpec& o : closed) {
        for (const Timed& t : ambiguous_history(o)) put(t.rec, kFrameFormatVersion, t.at_ms);
    }
    for (const Timed& t : ambiguous_history(kA)) put(t.rec, kFrameFormatVersion, t.at_ms);
    for (const OrderSpec& o : closed) {
        AuditRecord r = make_record(o, AuditEventType::OrderOperatorResolved, OrderState::Rejected);
        r.detail_code = static_cast<std::int64_t>(kKindAbsent | kAbsentRequiredBits);
        put(r, kFrameFormatVersionV6, kT0 - 1000);
    }
    {
        DurableLogStore store(audit_path_, audit_path_ + ".lock", audit_path_ + ".tip");
        ASSERT_TRUE(store.acquire_lock());
        ASSERT_TRUE(store.open_log());
        ASSERT_TRUE(store.append_and_fsync(std::span<const std::byte>(log.data(), log.size())));
        std::array<std::byte, kTipAnchorSize> anchor{};
        hy::detail::encode_tip_anchor(anchor, seq - 1, prev, kKeyId, key);
        ASSERT_TRUE(store.write_tip_anchor(std::span<const std::byte>(anchor.data(), anchor.size())));
        store.close_log();
        store.release_lock();
    }
    write_names({{"BTCUSDT"}});
    before_ = file_bytes(audit_path_);
    cp_before_ = file_bytes(cp_path_);
    audit_ = std::make_unique<DurableAuditSink>(audit_path_, *ring_, kKeyId, kMaxIntentOrphans, AuditSinkOpenMode::OperatorOpen);
    names_ = std::make_unique<ControlPlaneLogSink>(cp_path_, *ring_, kKeyId);
    rig_ = std::make_unique<Rig>(world_);
    ASSERT_FALSE(audit_->fenced());
    ASSERT_EQ(audit_->recovered_operator_resolved().size(), kMaxOperatorResolved);
    OperatorSession session = begin();
    ASSERT_EQ(session.status, SessionStatus::Ready);
    EXPECT_EQ(resolve_order(session, request(kA)).status, ResolveStatus::TableFull);
    expect_no_traffic();
    EXPECT_TRUE(logs_untouched());
}

// --- a request or a session that is not usable ------------------------------------------------------------------------------------------

TEST_F(FlowTest, ABadRequestIsRefusedBeforeAnythingElse) {
    prepare(ambiguous_history(kA));
    OperatorSession session = begin();
    ASSERT_EQ(session.status, SessionStatus::Ready);
    const auto bad = [&](OperatorResolutionRequest r, const char* why) {
        const OperatorResolutionOutcome o = resolve_order(session, r);
        EXPECT_EQ(o.status, ResolveStatus::BadRequest) << why;
    };
    OperatorResolutionRequest r = request(kA);
    r.environment = static_cast<OperatorEnvironment>(255);
    bad(r, "environment");
    r = request(kA);
    r.account_uid = 0;
    bad(r, "uid");
    r = request(kA);
    r.client_order_id = "";
    bad(r, "empty id");
    r = request(kA);
    r.client_order_id = "HY 1789999990000 1";
    bad(r, "id with spaces");
    r = request(kA);
    r.operator_label = "";
    bad(r, "empty label");
    r = request(kA);
    r.operator_label = "has space";
    bad(r, "label with a space");
    r = request(kA);
    r.confirm_timeout = std::chrono::milliseconds{0};
    bad(r, "zero timeout");
    r = request(kA);
    r.confirm_timeout = std::chrono::milliseconds{-5};
    bad(r, "negative timeout");
    expect_no_traffic();
    EXPECT_TRUE(logs_untouched());
}

TEST_F(FlowTest, ASessionThatIsNotReadyResolvesNothing) {
    prepare(ambiguous_history(kA));
    OperatorSession blank{};
    EXPECT_EQ(blank.status, SessionStatus::NotStarted);
    EXPECT_EQ(resolve_order(blank, request(kA)).status, ResolveStatus::SessionNotReady);

    FenceParams too_short;
    too_short.read_lag_ms = 119'999;
    OperatorSession bad_fence = begin(too_short);
    EXPECT_EQ(bad_fence.status, SessionStatus::BadFenceParams);
    EXPECT_EQ(resolve_order(bad_fence, request(kA)).status, ResolveStatus::SessionNotReady);

    // the session's ready flag alone is not enough
    OperatorSession half = begin();
    ASSERT_EQ(half.status, SessionStatus::Ready);
    half.names = nullptr;
    EXPECT_EQ(resolve_order(half, request(kA)).status, ResolveStatus::SessionNotReady);
    EXPECT_TRUE(logs_untouched());
}

TEST_F(FlowTest, ATradingOpenedSinkCannotStartASessionOrBeUsedByOne) {
    write_history(ambiguous_history(kA));
    write_names({{"BTCUSDT"}});
    auto trading = std::make_unique<DurableAuditSink>(audit_path_, *ring_, kKeyId);  // Trading mode
    names_ = std::make_unique<ControlPlaneLogSink>(cp_path_, *ring_, kKeyId);
    rig_ = std::make_unique<Rig>(world_);
    EXPECT_EQ(begin_operator_session(*trading, *names_, rig_->ports, {}).status, SessionStatus::AuditNotUsable);
    EXPECT_EQ(rig_->exchange.calls.size(), 0u) << "not even a clock reading";

    OperatorSession forged{};  // a session assembled by hand around the wrong kind of sink
    forged.status = SessionStatus::Ready;
    forged.audit = trading.get();
    forged.names = names_.get();
    forged.ports = &rig_->ports;
    forged.lock_ref = ExchangeMs{kT0};
    EXPECT_EQ(resolve_order(forged, request(kA)).status, ResolveStatus::AuditNotUsable);
    EXPECT_EQ(rig_->exchange.calls.size(), 0u);
    EXPECT_EQ(trading->recovered_operator_resolved().size(), 0u);
}

TEST_F(FlowTest, AnAppendTheSinkRefusesIsReportedAsRefusedNotAsFailed) {
    prepare(ambiguous_history(kA));
    OperatorSession session = begin();
    // while the operator types, the order is closed behind the flow's back, through the sink itself: the flow's own frame is then
    // a second resolution of the same order in one session, which the sink refuses
    rig_->console.on_read = [&] {
        AuditRecord rec = make_record(kA, AuditEventType::OrderOperatorResolved, OrderState::Rejected);
        rec.detail_code = static_cast<std::int64_t>(kKindAbsent | kAbsentRequiredBits | (std::uint64_t{270} << kWaitedSecondsShift) |
                                                    (std::uint64_t{1} << kToolVersionShift));
        rec.set_detail("op=other;ev=00000000000000000000000000000000");
        EXPECT_EQ(audit_->append_operator_resolution(rec, world_.now_ms).status, OperatorAppendStatus::Appended);
    };
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::AppendRefused);
    EXPECT_EQ(o.append.status, OperatorAppendStatus::AlreadyResolvedThisSession);
    EXPECT_EQ(o.confirmation.verdict, ConfirmationVerdict::Confirmed) << "he did confirm; the sink would not take a second frame";
    EXPECT_TRUE(rig_->console.printed("the frame was not written"));
    EXPECT_FALSE(rig_->console.printed("resolved: the frame is durable"));
    EXPECT_TRUE(audit_->operator_resolved_this_session(0)) << "and the first frame, the one behind the flow's back, stands";
}

TEST_F(FlowTest, AFencedOrClosedSinkCannotStartASessionOrBeUsedByOne) {
    write_history(ambiguous_history(kA));
    write_names({{"BTCUSDT"}});
    // fenced: one byte inside the first frame is damaged, recovery says Corrupt and the sink fences itself
    std::vector<std::byte> bytes = file_bytes(audit_path_);
    ASSERT_GT(bytes.size(), 100u);
    bytes[100] = static_cast<std::byte>(~static_cast<unsigned>(bytes[100]));
    {
        std::ofstream out(audit_path_, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const std::vector<std::byte> damaged = file_bytes(audit_path_);
    audit_ = std::make_unique<DurableAuditSink>(audit_path_, *ring_, kKeyId, kMaxIntentOrphans, AuditSinkOpenMode::OperatorOpen);
    names_ = std::make_unique<ControlPlaneLogSink>(cp_path_, *ring_, kKeyId);
    rig_ = std::make_unique<Rig>(world_);
    ASSERT_TRUE(audit_->is_open());
    ASSERT_TRUE(audit_->fenced());
    // closed: the directory of its path does not exist, so nothing could be opened
    DurableAuditSink closed(stem_ + "_nodir/audit.log", *ring_, kKeyId, kMaxIntentOrphans, AuditSinkOpenMode::OperatorOpen);
    ASSERT_FALSE(closed.is_open());

    DurableAuditSink* const unusable[] = {audit_.get(), &closed};
    for (DurableAuditSink* const sink : unusable) {
        EXPECT_EQ(begin_operator_session(*sink, *names_, rig_->ports, {}).status, SessionStatus::AuditNotUsable);
        EXPECT_EQ(rig_->exchange.calls.size(), 0u) << "not even a clock reading";
        OperatorSession forged{};  // a session assembled by hand around a sink nothing can be written to
        forged.status = SessionStatus::Ready;
        forged.audit = sink;
        forged.names = names_.get();
        forged.ports = &rig_->ports;
        forged.lock_ref = ExchangeMs{kT0};
        EXPECT_EQ(resolve_order(forged, request(kA)).status, ResolveStatus::AuditNotUsable);
        EXPECT_EQ(rig_->exchange.calls.size(), 0u);
    }
    EXPECT_TRUE(rig_->sleeper.sleeps.empty());
    EXPECT_TRUE(rig_->console.lines.empty());
    EXPECT_TRUE(rig_->store.attempts.empty());
    release();  // the sink holds the file; its bytes can be read once it lets go
    EXPECT_EQ(file_bytes(audit_path_), damaged);
}

TEST_F(FlowTest, AnAppendThatFailsIsReportedAsFailedNotAsRefusedAndTheOrderStaysOpen) {
    prepare(ambiguous_history(kA));
    OperatorSession session = begin();
    ASSERT_EQ(session.status, SessionStatus::Ready);
    ASSERT_EQ(ring_->retire(kKeyId), RetireStatus::Retired);  // the active key is gone: the frame cannot be made
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::AppendFailed);
    EXPECT_EQ(o.append.status, OperatorAppendStatus::AppendFailed);
    EXPECT_EQ(o.confirmation.verdict, ConfirmationVerdict::Confirmed) << "the operator did confirm; the write did not happen";
    EXPECT_FALSE(audit_->fenced()) << "an unusable key is a correctable mistake, not damage";
    EXPECT_FALSE(audit_->operator_resolved_this_session(0));
    EXPECT_EQ(rig_->store.stored.size(), 1u) << "the evidence the operator confirmed against is kept";
    EXPECT_TRUE(rig_->console.printed("the frame was not written"));
    EXPECT_FALSE(rig_->console.printed("resolved: the frame is durable"));
    EXPECT_TRUE(logs_untouched());
}

// --- the operator ---------------------------------------------------------------------------------------------------------------------------

TEST_F(FlowTest, AnythingButTheExactPhraseWritesNothing) {
    struct Case {
        const char* name;
        FakeConsole::Mode mode;
        std::string (*variant)(const std::string&);
        ConfirmationVerdict verdict;
    };
    const Case cases[] = {
        {"lower case", FakeConsole::Mode::TypeVariant, &lower_first, ConfirmationVerdict::WrongPhrase},
        {"trailing space", FakeConsole::Mode::TypeVariant, &trailing_space, ConfirmationVerdict::WrongPhrase},
        {"leading space", FakeConsole::Mode::TypeVariant, &leading_space, ConfirmationVerdict::WrongPhrase},
        {"another digest", FakeConsole::Mode::TypeVariant, &other_digest, ConfirmationVerdict::WrongPhrase},
        {"another environment", FakeConsole::Mode::TypeVariant, &other_environment, ConfirmationVerdict::WrongPhrase},
        {"another uid", FakeConsole::Mode::TypeVariant, &other_uid, ConfirmationVerdict::WrongPhrase},
        {"another symbol", FakeConsole::Mode::TypeVariant, &other_symbol, ConfirmationVerdict::WrongPhrase},
        {"a truncated phrase", FakeConsole::Mode::TypeVariant, &truncated, ConfirmationVerdict::WrongPhrase},
        {"another order's phrase", FakeConsole::Mode::TypeVariant, &other_order, ConfirmationVerdict::WrongPhrase},
        {"just CONFIRM", FakeConsole::Mode::TypeVariant, &just_confirm, ConfirmationVerdict::WrongPhrase},
        {"nothing", FakeConsole::Mode::TypeNothing, nullptr, ConfirmationVerdict::WrongPhrase},
        {"a timeout", FakeConsole::Mode::Timeout, nullptr, ConfirmationVerdict::NoLine},
        {"end of input", FakeConsole::Mode::Eof, nullptr, ConfirmationVerdict::NoLine},
        {"an I/O error", FakeConsole::Mode::Error, nullptr, ConfirmationVerdict::NoLine},
        {"a line that was too long", FakeConsole::Mode::TooLong, nullptr, ConfirmationVerdict::NoLine},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        fresh_paths();
        world_.now_ms = kT0;
        prepare(ambiguous_history(kA));
        rig_->console.mode = c.mode;
        rig_->console.variant = c.variant;
        OperatorSession session = begin();
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(o.status, ResolveStatus::NotConfirmed);
        EXPECT_EQ(o.confirmation.verdict, c.verdict);
        EXPECT_EQ(rig_->console.reads, 1) << "the operator is asked once";
        EXPECT_EQ(o.conclusion, AbsentConclusionStatus::NotConfirmed) << "the evidence was clean; only his word is missing";
        EXPECT_EQ(o.append.status, OperatorAppendStatus::NotAttempted);
        EXPECT_EQ(rig_->store.stored.size(), 1u) << "the evidence he was shown is on disk";
        EXPECT_TRUE(rig_->console.printed("not confirmed: nothing was written")) << "and he is told that nothing happened";
        EXPECT_FALSE(rig_->console.printed("resolved: the frame is durable"));
        EXPECT_TRUE(logs_untouched());
    }
}

TEST_F(FlowTest, TheOperatorOnlyHasTheTimeTheRequestGives) {
    prepare(ambiguous_history(kA));
    OperatorSession session = begin();
    OperatorResolutionRequest r = request(kA);
    r.confirm_timeout = std::chrono::seconds{42};
    ASSERT_EQ(resolve_order(session, r).status, ResolveStatus::Resolved);
    EXPECT_EQ(rig_->console.last_timeout, std::chrono::seconds{42});
}

TEST_F(FlowTest, ThePhraseNamesTheEnvironmentTheOperatorStatedAndATestnetPhraseDoesNotConfirmAProductionRun) {
    prepare(ambiguous_history(kA));
    OperatorSession session = begin();
    rig_->console.mode = FakeConsole::Mode::TypeVariant;
    rig_->console.variant = &other_environment;  // types PRODUCTION where the phrase says TESTNET ...
    EXPECT_EQ(resolve_order(session, request(kA, OperatorEnvironment::Testnet)).confirmation.verdict, ConfirmationVerdict::WrongPhrase);
    // ... and a production run asks for PRODUCTION
    fresh_paths();
    world_.now_ms = kT0;
    prepare(ambiguous_history(kA));
    OperatorSession prod = begin();
    const OperatorResolutionOutcome o = resolve_order(prod, request(kA, OperatorEnvironment::Production));
    ASSERT_EQ(o.status, ResolveStatus::Resolved);
    EXPECT_NE(rig_->console.printed_phrase().find(" PRODUCTION "), std::string::npos);
    ASSERT_EQ(rig_->store.stored.size(), 1u);
    EXPECT_NE(rig_->store.stored[0].second.find("\"environment\":\"PRODUCTION\""), std::string::npos);
}

TEST_F(FlowTest, AnEvidenceFileThatCannotBeStoredStopsTheRunBeforeAnyoneIsAsked) {
    prepare(ambiguous_history(kA));
    rig_->store.fail = true;
    OperatorSession session = begin();
    EXPECT_EQ(resolve_order(session, request(kA)).status, ResolveStatus::EvidenceStoreFailed);
    EXPECT_EQ(rig_->console.reads, 0);
    EXPECT_TRUE(rig_->console.printed_phrase().empty());
    EXPECT_EQ(rig_->store.attempts.size(), 1u);
    EXPECT_TRUE(logs_untouched());
}

TEST_F(FlowTest, ARefusalStillReportsWhenItsEvidenceCannotBeStored) {
    prepare(ambiguous_history(kA));
    rig_->exchange.orders.push_back(visible_order(kA));
    rig_->store.fail = true;
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::EvidenceNotAbsent) << "the reason stays the reason";
    EXPECT_TRUE(rig_->console.printed("could not be stored"));
    EXPECT_TRUE(logs_untouched());
}

// --- the account: the uid the operator typed against the one the key belongs to ------------------------------------------------------------------

TEST_F(FlowTest, TheAccountIsAskedOnceBeforeTheWaitAndBeforeAnyEvidenceRead) {
    prepare(ambiguous_history(kA));
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    ASSERT_EQ(o.status, ResolveStatus::Resolved) << resolve_status_name(o.status);
    EXPECT_EQ(rig_->exchange.count("account"), 1u);
    ASSERT_GE(rig_->exchange.calls.size(), 3u);
    EXPECT_EQ(rig_->exchange.calls[0].kind, "time") << "the session's own reading";
    EXPECT_EQ(rig_->exchange.calls[1].kind, "account") << "the next thing: before the first fence reading, so before the wait";
    EXPECT_EQ(rig_->exchange.calls[1].at_ms, kT0) << "not a single sleep before it";
    ASSERT_TRUE(o.exchange_account_uid.has_value());
    EXPECT_EQ(*o.exchange_account_uid, kUid);
}

// Nothing waited, nothing read as evidence, nothing stored, nobody asked, both logs as they were.
#define EXPECT_REFUSED_AT_THE_ACCOUNT_CHECK()                                                      \
    do {                                                                                           \
        EXPECT_EQ(rig_->exchange.calls.size(), 2u) << "the session's reading and the account only"; \
        EXPECT_EQ(rig_->exchange.count("account"), 1u);                                            \
        EXPECT_TRUE(rig_->sleeper.sleeps.empty());                                                 \
        EXPECT_EQ(rig_->exchange.first_evidence_call_ms(), kNever);                                \
        EXPECT_TRUE(rig_->store.attempts.empty());                                                 \
        EXPECT_EQ(rig_->console.reads, 0);                                                         \
        EXPECT_TRUE(logs_untouched());                                                             \
    } while (false)

TEST_F(FlowTest, AKeyThatBelongsToAnotherAccountIsRefusedBeforeAnyWaitAndNothingIsRead) {
    prepare(ambiguous_history(kA));
    rig_->exchange.account_uid = kUid + 1;
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::AccountUidMismatch) << resolve_status_name(o.status);
    ASSERT_TRUE(o.exchange_account_uid.has_value());
    EXPECT_EQ(*o.exchange_account_uid, kUid + 1);
    EXPECT_TRUE(rig_->console.printed("this API key belongs to account uid 123456790, not to the uid 123456789 you stated: nothing was written"));
    EXPECT_EQ(o.confirmation.verdict, ConfirmationVerdict::NotAsked);
    EXPECT_REFUSED_AT_THE_ACCOUNT_CHECK();
}

TEST_F(FlowTest, EveryUidButTheStatedOneIsAMismatchWhateverTheDifference) {
    // the comparison is on all 64 bits: a uid that differs only in a high bit, below the stated one, or at the extremes is still another account
    for (const std::uint64_t other : {kUid - 1, kUid + 1, kUid + (std::uint64_t{1} << 32), kUid ^ (std::uint64_t{1} << 63),
                                      std::uint64_t{1}, std::numeric_limits<std::uint64_t>::max()}) {
        fresh_paths();
        prepare(ambiguous_history(kA));
        rig_->exchange.account_uid = other;
        OperatorSession session = begin();
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(o.status, ResolveStatus::AccountUidMismatch) << other;
        ASSERT_TRUE(o.exchange_account_uid.has_value()) << other;
        EXPECT_EQ(*o.exchange_account_uid, other);
        EXPECT_REFUSED_AT_THE_ACCOUNT_CHECK();
    }
}

TEST_F(FlowTest, AnAccountTheExchangeDoesNotNameIsRefusedBeforeAnyWaitWhateverElseItSaid) {
    prepare(ambiguous_history(kA));
    rig_->exchange.account_uid.reset();  // the request failed, or the answer had no single positive uid -- the summary alone is no proof
    ASSERT_FALSE(rig_->exchange.account_text.empty());
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::AccountUnverified) << resolve_status_name(o.status);
    EXPECT_FALSE(o.exchange_account_uid.has_value());
    EXPECT_TRUE(rig_->console.printed("the exchange did not report the uid of the account this API key belongs to: nothing was written"));
    EXPECT_REFUSED_AT_THE_ACCOUNT_CHECK();
}

TEST_F(FlowTest, TheAccountCheckComesAfterTheRefusalsThatNeedNoTraffic) {
    // an unknown order never reaches the exchange at all, whatever the account says
    prepare(ambiguous_history(kA));
    rig_->exchange.account_uid = kUid + 1;
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kB));
    EXPECT_EQ(o.status, ResolveStatus::NoSuchUnresolvedOrder);
    EXPECT_FALSE(o.exchange_account_uid.has_value()) << "not asked";
    expect_no_traffic();
    EXPECT_EQ(rig_->exchange.count("account"), 0u);
}

TEST_F(FlowTest, AnIntervalNoToolCanCoverIsRefusedBeforeEvenTheAccountIsAsked) {
    const OrderSpec odd{"ORDER-NOT-OURS-1"};
    prepare(ambiguous_history(odd, 0));
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(odd));
    EXPECT_EQ(o.status, ResolveStatus::NoUsableRange);
    EXPECT_EQ(rig_->exchange.count("account"), 0u);
    EXPECT_FALSE(o.exchange_account_uid.has_value());
}

TEST_F(FlowTest, AnEmptyAccountSummaryIsShownAsNotAvailableAndDoesNotStopTheRun) {
    prepare(ambiguous_history(kA));
    rig_->exchange.account_text.clear();
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    EXPECT_EQ(o.status, ResolveStatus::Resolved) << resolve_status_name(o.status);
    EXPECT_TRUE(rig_->console.printed("account summary (display only): not available"));
    ASSERT_EQ(rig_->store.stored.size(), 1u);
    EXPECT_NE(rig_->store.stored[0].second.find("\"account_summary\":\"\""), std::string::npos);
}

#undef EXPECT_REFUSED_AT_THE_ACCOUNT_CHECK

// --- the evidence file ------------------------------------------------------------------------------------------------------------------------

TEST_F(FlowTest, TheStoredEvidenceHoldsWhatTheDecisionRestedOn) {
    prepare(ambiguous_history(kA));
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    ASSERT_EQ(o.status, ResolveStatus::Resolved);
    ASSERT_EQ(rig_->store.stored.size(), 1u);
    const std::string& b = rig_->store.stored[0].second;
    for (const char* needle :
         {"\"format\":\"hy-operator-evidence/1\"", "\"tool_version\":1", "\"client_order_id\":\"HY-1789999399000-1-0A0A\"",
          "\"symbol\":\"BTCUSDT\"", "\"environment\":\"TESTNET\"", "\"account_uid_claimed\":123456789", "\"account_uid_reported\":123456789",
          "\"recv_window_ms\":60000", "\"clock_skew_ms\":60000", "\"margin_ms\":30000", "\"read_lag_ms\":120000",
          "\"role\":\"lock\"", "\"role\":\"wait\"", "\"role\":\"read\"", "\"point_query\":\"NoSuchOrder\"",
          "\"all_orders\":[\"Clear\"]", "\"open_orders\":\"Clear\"", "\"name\":\"order\"", "\"name\":\"allOrders\"",
          "\"name\":\"openOrders\"", "\\\"code\\\":-2013", "\"account_summary\":\"USDT free 1000.0\"",
          "\"frame_anchor_ms\":1789999401000", "\"lock_ref_ms\":1790000000000", "\"read_ref_ms\":1790000270000",
          "\"start_ms\":1789999279000", "\"end_ms\":1790000270000",
          // each request as it was made and each answer as it came (the status, whether the request completed, the parameters)
          "\"params\":\"symbol=BTCUSDT&origClientOrderId=HY-1789999399000-1-0A0A\",\"transport_ok\":true,\"http_status\":400",
          "\"params\":\"symbol=BTCUSDT&startTime=1789999279000&endTime=1790000270000&limit=1000\",\"transport_ok\":true,\"http_status\":200",
          "\"name\":\"openOrders\",\"params\":\"\",\"transport_ok\":true,\"http_status\":200,\"body\":\"[]\""}) {
        EXPECT_NE(b.find(needle), std::string::npos) << needle;
    }
    EXPECT_EQ(b.find("signature"), std::string::npos) << "the tool never sees a signature, so none can be in the file";
    EXPECT_EQ(b.find("apiKey"), std::string::npos);
    // the session's lock reading, nine readings while the fence ran (at +0, +30 ... +240 s) and the one that satisfied it (+270 s)
    std::size_t waits = 0;
    for (std::size_t pos = b.find("\"role\":\"wait\""); pos != std::string::npos; pos = b.find("\"role\":\"wait\"", pos + 1)) ++waits;
    EXPECT_EQ(waits, 9u) << "one reading per sleep, and the one that satisfied the fence is the 'read', not a 'wait'";
}

TEST_F(FlowTest, TheFrameCarriesTheExchangeTimeOfTheConfirmationNotOfTheEvidenceRead) {
    prepare(ambiguous_history(kA));
    rig_->console.world = &world_;
    rig_->console.typing_ms = 90'000;  // the operator takes a minute and a half to type the phrase
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(kA));
    ASSERT_EQ(o.status, ResolveStatus::Resolved) << resolve_status_name(o.status);
    EXPECT_EQ(o.read_ref.count(), kT0 + kFenceMs);
    release();
    DecodedOrderFrame frame{};
    ASSERT_TRUE(decode_last_frame(frame));
    EXPECT_EQ(frame.recorded_utc_ms, kT0 + kFenceMs + 90'000) << "the time the frame was written, read from the exchange after the confirmation";
    EXPECT_EQ(frame.record.timestamp_ms, kT0 + kFenceMs + 90'000);
    const std::optional<AbsentDetail> detail = parse_absent_detail_code(frame.record.detail_code);
    ASSERT_TRUE(detail.has_value());
    EXPECT_EQ(detail->waited_seconds, 270) << "what the machine field says about the wait is the evidence's (lock to read), not the typing";
}

TEST_F(FlowTest, AFrameTimeThatCannotBeReadWritesNothingEvenAfterTheOperatorConfirmed) {
    for (const bool garbage : {false, true}) {
        SCOPED_TRACE(garbage ? "an implausible reading" : "no reading");
        fresh_paths();
        world_.now_ms = kT0;
        prepare(ambiguous_history(kA));
        // readings: 1 the session's, 2..10 inside the wait, 11 the one that satisfies the fence, 12 the frame's
        (garbage ? rig_->exchange.garbage_time_from_read : rig_->exchange.fail_time_from_read) = 11;
        OperatorSession session = begin();
        const OperatorResolutionOutcome o = resolve_order(session, request(kA));
        EXPECT_EQ(o.status, ResolveStatus::ServerTimeUnavailable);
        EXPECT_EQ(o.confirmation.verdict, ConfirmationVerdict::Confirmed) << "he confirmed; there is no time to stamp the frame with";
        EXPECT_EQ(o.append.status, OperatorAppendStatus::NotAttempted);
        EXPECT_EQ(rig_->store.stored.size(), 1u);
        EXPECT_TRUE(logs_untouched());
    }
}

TEST_F(FlowTest, TheEvidenceFileNameHoldsOnlyCharactersAFileNameCanHave) {
    const OrderSpec odd{"ODD:ID/1.x_y-z"};  // ':' and '/' are legal in an exchange client order id and in no file name
    prepare(ambiguous_history(odd));
    OperatorSession session = begin();
    const OperatorResolutionOutcome o = resolve_order(session, request(odd));
    ASSERT_EQ(o.status, ResolveStatus::Resolved) << resolve_status_name(o.status) << " / " << conclusion_name(o.conclusion);
    EXPECT_EQ(o.evidence_stem, "ODD_ID_1.x_y-z-" + std::to_string(kT0 + kFenceMs));
    ASSERT_EQ(rig_->store.stored.size(), 1u);
    EXPECT_EQ(rig_->store.stored[0].first, o.evidence_stem);
    for (const char c : o.evidence_stem) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-') << c;
    }
}

// --- listing -----------------------------------------------------------------------------------------------------------------------------------

TEST_F(FlowTest, TheListingSaysWhichOrdersTheToolCouldResolveAndWhyNotTheOthers) {
    OrderSpec dry = kC;
    dry.mode = ExecutionMode::DryRun;
    OrderSpec nameless = OrderSpec{"HY-1789999399000-4-0D0D"};
    nameless.symbol_id = 5;  // the snapshot only names id 0
    prepare(concat(concat(ambiguous_history(kA), accepted_history(kB)), concat(ambiguous_history(dry), ambiguous_history(nameless))));
    const std::vector<UnresolvedOrderSummary> list = list_unresolved_orders(*audit_, *names_);
    ASSERT_EQ(list.size(), 4u);

    EXPECT_EQ(list[0].client_order_id, kA.coid);
    EXPECT_EQ(list[0].state, OrderState::Ambiguous);
    EXPECT_EQ(list[0].mode, ExecutionMode::Live);
    EXPECT_EQ(list[0].symbol_status, SymbolNameStatus::Known);
    EXPECT_EQ(list[0].symbol, "BTCUSDT");
    EXPECT_EQ(list[0].side, OrderSide::Sell);
    EXPECT_EQ(list[0].price_ticks, 500'000);
    EXPECT_EQ(list[0].qty_ticks, 100);
    EXPECT_EQ(list[0].send_started_utc_ms, kSendAt);
    EXPECT_EQ(list[0].order_check, ResolutionCheck::Ok);
    EXPECT_FALSE(list[0].resolved_this_session);
    for (std::size_t i = 0; i < list.size(); ++i) EXPECT_EQ(list[i].checkpoint_index, i) << "each entry says where its checkpoint is";
    EXPECT_EQ(list[0].symbol_id, 0u);
    EXPECT_EQ(list[3].symbol_id, 5u);

    EXPECT_EQ(list[1].client_order_id, kB.coid);
    EXPECT_EQ(list[1].state, OrderState::Accepted);
    EXPECT_EQ(list[1].order_check, ResolutionCheck::NotInAResolvableState);

    EXPECT_EQ(list[2].client_order_id, dry.coid);
    EXPECT_EQ(list[2].mode, ExecutionMode::DryRun);
    EXPECT_EQ(list[2].order_check, ResolutionCheck::Ok) << "the shared rule does not look at the mode; the tool and the writer do";

    EXPECT_EQ(list[3].client_order_id, nameless.coid);
    EXPECT_EQ(list[3].symbol_status, SymbolNameStatus::Unknown);
    EXPECT_TRUE(list[3].symbol.empty());

    OperatorSession session = begin();
    ASSERT_EQ(resolve_order(session, request(kA)).status, ResolveStatus::Resolved);
    EXPECT_TRUE(list_unresolved_orders(*audit_, *names_)[0].resolved_this_session);
}

// --- names ---------------------------------------------------------------------------------------------------------------------------------------

TEST(OperatorResolutionFlowNames, EveryStatusHasANameAndTheZeroValueIsNotASuccess) {
    std::vector<std::string_view> seen;
    for (std::uint8_t v = 0; v <= static_cast<std::uint8_t>(ResolveStatus::AccountUidMismatch); ++v) {
        const std::string_view n = resolve_status_name(static_cast<ResolveStatus>(v));
        EXPECT_NE(n, "Unknown") << static_cast<int>(v);
        EXPECT_EQ(std::count(seen.begin(), seen.end(), n), 0) << n << " is used twice";
        seen.push_back(n);
    }
    EXPECT_EQ(resolve_status_name(static_cast<ResolveStatus>(200)), "Unknown");
    EXPECT_EQ(OperatorResolutionOutcome{}.status, ResolveStatus::NotRun);
    EXPECT_NE(OperatorResolutionOutcome{}.status, ResolveStatus::Resolved);
    EXPECT_EQ(OperatorSession{}.status, SessionStatus::NotStarted);
    for (std::uint8_t v = 0; v <= static_cast<std::uint8_t>(AbsentConclusionStatus::NotConfirmed); ++v) {
        EXPECT_NE(conclusion_name(static_cast<AbsentConclusionStatus>(v)), "Unknown") << static_cast<int>(v);
    }
    EXPECT_EQ(conclusion_name(static_cast<AbsentConclusionStatus>(200)), "Unknown");
}
