// SPDX-License-Identifier: proprietary
// test_operator_resolution_tool.cpp -- D5 slice R3b-2: the offline operator tool from the command line to the exit code
// (operator_resolution_tool.hpp), run over REAL audit and control-plane logs in a temporary directory, a real KEK file and wrapped-key file, the
// real evidence directory, and the flow's fake exchange, clock and console (operator_resolution_fakes.hpp). The exchange comes in through an
// ExchangeProvider, so nothing here needs a socket; the same tool over a real TLS client is test_operator_resolution_real_exchange.cpp.
//
// What these pin, in the order the tool does it:
//   * the command line: --help prints the usage and touches nothing, a bad command line exits 2 with one line and the hint, two arguments that
//     are one file on disk are refused before anything is opened;
//   * the order of events: the credentials (the provider) are asked for BEFORE a log is touched; both logs are open -- and locked -- BEFORE the
//     session reads the exchange clock; a refusal at any step leaves both logs byte for byte as they were and the locks released;
//   * --list: the unresolved orders and why one cannot be resolved, with no credentials, no exchange traffic and no write;
//   * resolving: the whole procedure to exit 0, the evidence file on disk with the digest the operator was shown, the frame readable by the
//     next trading start; every way of stopping exits with its own code (40 + the status of the procedure);
//   * the key material and the logs: a wrong KEK, a damaged or missing key file, a missing key id, a missing log (never created), a log held by
//     another process, a key id the log was not written with.

#include <gtest/gtest.h>
#include <hengyuan/operator_resolution_tool.hpp>

#include "operator_resolution_fakes.hpp"
#include "test_helpers/temp_directory.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sys/stat.h>
#endif

using namespace hy;
using namespace hy::operator_resolution;
using namespace hy::operator_resolution::testing;

namespace fs = std::filesystem;

namespace {

using Args = std::vector<std::string_view>;
using hy::test_helpers::TempDirectory;

constexpr std::int64_t kT0 = 1'790'000'000'000;  // the exchange time at which the lock is taken
constexpr std::int64_t kFenceMs = kDefaultEvidenceWaitMs;  // 270 s
constexpr std::int64_t kSendAt = kT0 - 599'000;
constexpr std::uint32_t kKeyA = 1;
constexpr std::uint32_t kKeyB = 2;
constexpr std::uint64_t kUid = kFakeAccountUid;

struct OrderSpec {
    std::string coid;
    std::uint32_t symbol_id{0};
    ExecutionMode mode{ExecutionMode::Live};
};

const OrderSpec kA{"HY-1789999399000-1-0A0A"};

AuditRecord make_record(const OrderSpec& o, AuditEventType ev, OrderState st) {
    AuditRecord r{};
    r.timestamp_ms = kT0 - 600'000;
    r.event_type = ev;
    r.mode = o.mode;
    r.symbol_id = o.symbol_id;
    r.set_client_order_id(o.coid);
    r.price_ticks = 500'000;
    r.qty_ticks = 100;
    r.resulting_state = st;
    r.side = OrderSide::Sell;
    return r;
}

struct Timed {
    AuditRecord rec;
    std::int64_t at_ms;
};

// send_at == 0 is a log without a trustworthy send-start time (the frames carry no wall time: every frame at 0).
std::vector<Timed> ambiguous_history(const OrderSpec& o, std::int64_t send_at = kSendAt) {
    const std::int64_t before = send_at > 0 ? send_at - 1000 : 0;
    const std::int64_t after = send_at > 0 ? send_at + 1000 : 0;
    return {{make_record(o, AuditEventType::OrderIntentCreated, OrderState::Intent), before},
            {make_record(o, AuditEventType::OrderSubmitPrepared, OrderState::Submitting), send_at},
            {make_record(o, AuditEventType::OrderAmbiguous, OrderState::Ambiguous), after}};
}

// An Ambiguous order that once showed an exchange id: it existed, so nobody can say it is absent.
std::vector<Timed> shown_history(const OrderSpec& o) {
    std::vector<Timed> v = ambiguous_history(o);
    v.back().rec.exchange_order_id = 77;
    return v;
}

// Nothing was ever sent for it: the last frame of this id is the Intent.
std::vector<Timed> intent_only_history(const OrderSpec& o) {
    return {{make_record(o, AuditEventType::OrderIntentCreated, OrderState::Intent), kSendAt - 1000}};
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

FakeExchangeOrder visible_order(const OrderSpec& o) {
    FakeExchangeOrder e;
    e.client_order_id = o.coid;
    e.symbol = "BTCUSDT";
    e.created_ms = kSendAt + 500;
    e.point_visible_ms = 0;
    e.list_visible_ms = 0;
    return e;
}

std::vector<std::byte> file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    if (!raw.empty()) std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

std::string file_text(const std::string& path) {
    const std::vector<std::byte> bytes = file_bytes(path);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void write_bytes(const std::string& path, const void* data, std::size_t size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

void write_text(const std::string& path, std::string_view text) { write_bytes(path, text.data(), text.size()); }

void make_owner_only(const std::string& path) {
#ifdef __linux__
    ::chmod(path.c_str(), 0600);
#else
    static_cast<void>(path);
#endif
}

std::vector<std::string> listing(const fs::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

// The exchange of a test that wants to look at what happens around a request: it delegates to the fake and can run something at the
// first clock reading or throw from the point query.
class ProbingExchange final : public ExchangeReadPort {
public:
    explicit ProbingExchange(ExchangeReadPort& inner) : inner_(inner) {}

    std::function<void()> on_first_time;
    bool throw_on_query{false};
    int time_reads{0};

    std::optional<ExchangeMs> server_time() override {
        if (time_reads++ == 0 && on_first_time) on_first_time();
        return inner_.server_time();
    }
    HttpRead query_order(std::string_view symbol, std::string_view client_order_id) override {
        if (throw_on_query) throw std::runtime_error("the exchange port threw");
        return inner_.query_order(symbol, client_order_id);
    }
    HttpRead all_orders(std::string_view symbol, ExchangeMs start, ExchangeMs end, std::size_t limit) override {
        return inner_.all_orders(symbol, start, end, limit);
    }
    HttpRead open_orders() override { return inner_.open_orders(); }
    AccountView account() override { return inner_.account(); }

private:
    ExchangeReadPort& inner_;
};

class FakeProvider final : public ExchangeProvider {
public:
    ExchangeReadPort* port{nullptr};
    bool fail{false};
    bool throw_on_open{false};
    std::function<void()> on_open;
    int opens{0};
    ToolOptions last_options;

    ExchangeReadPort* open(const ToolOptions& options, SleepPort&, ConsolePort& err) override {
        ++opens;
        last_options = options;
        if (on_open) on_open();
        if (throw_on_open) throw std::runtime_error("the provider threw");
        if (fail) {
            err.write_line("error: --credentials-file: no such file");
            return nullptr;
        }
        return port;
    }
};

}  // namespace

class ToolTest : public ::testing::Test {
protected:
    TempDirectory dir_;
    std::string audit_path_;
    std::string cp_path_;
    std::string kek_path_;
    std::string key_path_;
    std::string credentials_path_;
    std::array<std::byte, kKekSize> kek_{};
    std::unique_ptr<KeyRing> ring_;  // the test's own ring: it writes the logs and opens them again to look
    std::array<WrappedKeyRecord, 2> records_{};  // the two keys of the key file
    FakeWorld world_;
    FakeSleeper sleeper_{world_};
    FakeExchange exchange_{world_};
    FakeConsole out_;
    FakeConsole err_;
    FakeProvider provider_;
    FenceParams fence_{};
    std::chrono::milliseconds confirm_timeout_{kDefaultConfirmTimeout};
    std::vector<std::byte> audit_before_;
    std::vector<std::byte> cp_before_;

    void SetUp() override {
        audit_path_ = dir_.file("audit.log");
        cp_path_ = dir_.file("control.log");
        kek_path_ = dir_.file("kek.bin");
        key_path_ = dir_.file("keys.bin");
        credentials_path_ = dir_.file("operator.env");
        for (std::size_t i = 0; i < kek_.size(); ++i) kek_[i] = static_cast<std::byte>(0x40 + i);
        write_bytes(kek_path_, kek_.data(), kek_.size());
        make_owner_only(kek_path_);
        ring_ = std::make_unique<KeyRing>(kek_);
        const std::vector<std::byte> key_a(40, std::byte{0x11});
        const std::vector<std::byte> key_b(40, std::byte{0x22});
        ASSERT_EQ(ring_->add_key(kKeyA, key_a, records_[0]), KeyRingAddStatus::Ok);
        ASSERT_EQ(ring_->add_key(kKeyB, key_b, records_[1]), KeyRingAddStatus::Ok);
        const std::vector<std::byte> key_file = encode_audit_key_file(records_);
        write_bytes(key_path_, key_file.data(), key_file.size());
        world_.now_ms = kT0;
        provider_.port = &exchange_;
    }

    // --- the logs ---

    void write_logs(const std::vector<Timed>& history, const std::vector<std::vector<std::string>>& snapshots = {{"BTCUSDT"}}) {
        {
            DurableAuditSink sink(audit_path_, *ring_, kKeyA);
            ASSERT_TRUE(sink.is_open());
            for (const Timed& f : history) ASSERT_TRUE(sink.append_durable(f.rec, f.at_ms).acked()) << f.rec.client_order_id;
        }
        {
            ControlPlaneLogSink sink(cp_path_, *ring_, kKeyA);
            ASSERT_TRUE(sink.is_open());
            std::int64_t t = kT0 - 3'600'000;
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
        audit_before_ = file_bytes(audit_path_);
        cp_before_ = file_bytes(cp_path_);
    }

    bool logs_untouched() const { return file_bytes(audit_path_) == audit_before_ && file_bytes(cp_path_) == cp_before_; }

    bool audit_lock_free() {
        DurableAuditSink probe(audit_path_, *ring_, kKeyA);
        return probe.is_open();
    }
    bool control_lock_free() {
        ControlPlaneLogSink probe(cp_path_, *ring_, kKeyA);
        return probe.is_open();
    }

    // After a run that must have written nothing: the logs as they were, both locks free, the order still waiting for somebody.
    void expect_nothing_written() {
        EXPECT_TRUE(logs_untouched());
        DurableAuditSink after(audit_path_, *ring_, kKeyA);
        ASSERT_TRUE(after.is_open());
        EXPECT_EQ(after.recovered_operator_resolved().size(), 0u);
        EXPECT_EQ(after.recovered_checkpoints().size(), 1u);
        EXPECT_TRUE(control_lock_free());
    }

    // The tool stopped with `want`: its exit code, the line that says so, and nothing written.
    void expect_stops_with(ResolveStatus want, const Args& args) {
        const int code = run(args);
        EXPECT_EQ(code, exit_code_of(want)) << transcript();
        EXPECT_EQ(code, 40 + static_cast<int>(want));
        EXPECT_TRUE(out_.printed("result: " + std::string(resolve_status_name(want)) + " (exit code " + std::to_string(code) + ")")) << transcript();
        expect_nothing_written();
    }

    // --- the command line ---

    Args base_args() const {
        return {"--audit-log", audit_path_, "--control-log", cp_path_,        "--kek-file",       kek_path_,
                "--key-file",  key_path_,   "--audit-key-id", "1", "--control-key-id", "1"};
    }
    Args list_args() const {
        Args a = base_args();
        a.push_back("--list");
        return a;
    }
    Args resolve_args(std::string_view coid, std::string_view uid = "123456789", std::string_view env = "testnet") const {
        Args a = base_args();
        for (const std::string_view s : std::initializer_list<std::string_view>{"--coid", coid, "--env", env, "--uid", uid, "--operator",
                                                                                "alice", "--credentials-file", credentials_path_}) {
            a.push_back(s);
        }
        return a;
    }

    // The last frame of the audit log, decoded (the frame the tool appended, if it appended one).
    bool decode_last_frame(DecodedOrderFrame& frame) const {
        const std::vector<std::byte> bytes = file_bytes(audit_path_);
        if (bytes.size() < kOrderEventFrameSize) return false;
        std::array<std::byte, kKeyBlockSize> key_block{};
        if (!ring_->active_key(kKeyA, key_block)) return false;
        std::size_t size = 0;
        return decode_order_event_frame(std::span<const std::byte>(bytes).subspan(bytes.size() - kOrderEventFrameSize),
                                        std::span<const std::byte>(key_block.data(), key_block.size()), frame, size) == FrameDecodeStatus::Ok;
    }

    int run(const Args& args) {
        ToolEnvironment env{out_, err_, sleeper_, provider_};
        env.fence = fence_;
        env.confirm_timeout = confirm_timeout_;
        return run_operator_tool(args, env);
    }

    // What the tool said, for the message of a failed expectation.
    std::string transcript() const {
        std::string s = "--- out ---\n";
        for (const std::string& l : out_.lines) s += l + "\n";
        s += "--- err ---\n";
        for (const std::string& l : err_.lines) s += l + "\n";
        return s;
    }

    bool err_says(std::string_view fragment) const { return err_.printed(fragment); }

    // The first line the tool wrote to the operator that contains `fragment` (empty if none does).
    std::string out_line_with(std::string_view fragment) const {
        for (const std::string& l : out_.lines) {
            if (l.find(fragment) != std::string::npos) return l;
        }
        return {};
    }
};

// ============================================================ 1. the command line ===================================================================

TEST_F(ToolTest, HelpPrintsTheUsageAndTouchesNothing) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    for (const std::string_view flag : {"--help", "-h", "--usage"}) {
        out_.lines.clear();
        EXPECT_EQ(run(Args{flag}), 0) << flag;
        ASSERT_EQ(out_.lines.size(), std::size(kToolUsage));
        for (std::size_t i = 0; i < out_.lines.size(); ++i) EXPECT_EQ(out_.lines[i], kToolUsage[i]);
        EXPECT_TRUE(err_.lines.empty());
    }
    EXPECT_EQ(provider_.opens, 0);
    EXPECT_TRUE(exchange_.calls.empty());
    EXPECT_TRUE(logs_untouched());
}

TEST_F(ToolTest, ABadCommandLineExitsTwoWithOneLineAndTheHintAndTouchesNothing) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    Args args = list_args();
    args.push_back("--bogus");
    EXPECT_EQ(run(args), exit_code_of(ToolExit::Usage));
    ASSERT_EQ(err_.lines.size(), 2u) << transcript();
    EXPECT_EQ(err_.lines[0], "error: unknown argument --bogus");
    EXPECT_EQ(err_.lines[1], "run with --help for the arguments");
    EXPECT_TRUE(out_.lines.empty());
    EXPECT_EQ(provider_.opens, 0);
    EXPECT_TRUE(logs_untouched());
    EXPECT_TRUE(audit_lock_free());

    err_.lines.clear();
    EXPECT_EQ(run(Args{}), 2);
    EXPECT_EQ(err_.lines.front(), "error: --audit-log is required");
}

TEST_F(ToolTest, TwoArgumentsThatAreOneFileOnDiskAreRefusedBeforeAnythingIsOpened) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    const std::string audit_alias = (dir_.path / "." / "audit.log").string();  // another spelling of the audit log
    const std::string kek_alias = (dir_.path / "." / "kek.bin").string();
    {
        Args args = list_args();
        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == "--control-log") args[i + 1] = audit_alias;
        }
        EXPECT_EQ(run(args), 2) << transcript();
        EXPECT_TRUE(err_says("--audit-log and --control-log name the same file"));
        EXPECT_TRUE(logs_untouched());
        EXPECT_TRUE(audit_lock_free());
    }
    {
        err_.lines.clear();
        Args args = resolve_args(kA.coid);
        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == "--credentials-file") args[i + 1] = kek_alias;
        }
        EXPECT_EQ(run(args), 2) << transcript();
        EXPECT_TRUE(err_says("--credentials-file and --kek-file name the same file"));
        EXPECT_EQ(provider_.opens, 0) << "refused before the credentials are asked for";
    }
    {
        err_.lines.clear();
        Args args = list_args();
        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == "--key-file") args[i + 1] = kek_alias;
        }
        EXPECT_EQ(run(args), 2) << transcript();
        EXPECT_TRUE(err_says("--kek-file and --key-file name the same file"));
    }
    {
        err_.lines.clear();
        const std::string key_alias = (dir_.path / "." / "keys.bin").string();
        Args args = resolve_args(kA.coid);
        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == "--credentials-file") args[i + 1] = key_alias;
        }
        EXPECT_EQ(run(args), 2) << transcript();
        EXPECT_TRUE(err_says("--credentials-file and --key-file name the same file"));
        EXPECT_EQ(provider_.opens, 0);
    }
}

// ============================================================ 2. --list =============================================================================

TEST_F(ToolTest, ListShowsTheUnresolvedOrdersAndNeedsNeitherCredentialsNorTheExchange) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    EXPECT_EQ(run(list_args()), 0) << transcript();
    EXPECT_TRUE(err_.lines.empty()) << transcript();
    EXPECT_TRUE(out_.printed("unresolved orders: 1"));
    const auto line_with = [&](std::string_view fragment) {
        for (const std::string& l : out_.lines) {
            if (l.find(fragment) != std::string::npos) return l;
        }
        return std::string();
    };
    const std::string order = line_with(kA.coid);
    ASSERT_FALSE(order.empty()) << transcript();
    for (const char* fragment : {"[1] ", "state Ambiguous", "Live", "BTCUSDT", "Sell", "quantity 100 ticks", "price 500000 ticks",
                                 "send started 1789999401000", "-> can be resolved"}) {
        EXPECT_NE(order.find(fragment), std::string::npos) << fragment << " in: " << order;
    }
    EXPECT_TRUE(out_.printed("intent-only orders (nothing was ever sent for them; not listed): 0"));
    EXPECT_TRUE(out_.printed("orders an operator has resolved before: 0"));

    EXPECT_EQ(provider_.opens, 0) << "no credentials in this mode";
    EXPECT_TRUE(exchange_.calls.empty()) << "no traffic";
    EXPECT_TRUE(sleeper_.sleeps.empty());
    EXPECT_EQ(out_.reads, 0);
    EXPECT_TRUE(logs_untouched());
    EXPECT_FALSE(fs::exists(audit_path_ + ".operator-evidence"));
    EXPECT_TRUE(audit_lock_free()) << "the lock is released when the tool is done";
    EXPECT_TRUE(control_lock_free());
}

TEST_F(ToolTest, ListSaysWhyAnOrderCannotBeResolvedAndWhichOnesCan) {
    const OrderSpec dry{"HY-1789999399000-2-0B0B", 0, ExecutionMode::DryRun};
    const OrderSpec nameless{"HY-1789999399000-3-0C0C", 5};
    const OrderSpec accepted{"HY-1789999399000-4-0D0D"};
    ASSERT_NO_FATAL_FAILURE(
        write_logs(concat(concat(concat(ambiguous_history(kA), ambiguous_history(dry)), ambiguous_history(nameless)), accepted_history(accepted))));
    EXPECT_EQ(run(list_args()), 0) << transcript();
    EXPECT_TRUE(out_.printed("unresolved orders: 4"));
    const auto line_with = [&](std::string_view fragment) {
        for (const std::string& l : out_.lines) {
            if (l.find(fragment) != std::string::npos) return l;
        }
        return std::string();
    };
    EXPECT_NE(line_with(kA.coid).find("-> can be resolved"), std::string::npos);
    EXPECT_NE(line_with(dry.coid).find("DryRun"), std::string::npos);
    EXPECT_NE(line_with(dry.coid).find("-> cannot be resolved: a dry-run order never reached the exchange"), std::string::npos);
    EXPECT_NE(line_with(nameless.coid).find("symbol unknown (id 5)"), std::string::npos);
    EXPECT_NE(line_with(nameless.coid).find("-> cannot be resolved: the control-plane log does not name this symbol"), std::string::npos);
    EXPECT_NE(line_with(accepted.coid).find("state Accepted"), std::string::npos);
    EXPECT_NE(line_with(accepted.coid).find("-> cannot be resolved: NotInAResolvableState"), std::string::npos);
    EXPECT_TRUE(logs_untouched());
}

TEST_F(ToolTest, ListSaysWhenTheLogCannotTellWhenTheSendStarted) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA, 0)));  // every frame at time 0: no wall time to trust
    EXPECT_EQ(run(list_args()), 0) << transcript();
    const std::string order = out_line_with(kA.coid);
    ASSERT_FALSE(order.empty()) << transcript();
    EXPECT_NE(order.find("send started unknown"), std::string::npos) << order;
    EXPECT_EQ(order.find("send started 0"), std::string::npos) << "a zero is not a time";
}

TEST_F(ToolTest, ListSaysWhyAnOrderThatOnceShowedAnExchangeIdCannotBeResolved) {
    ASSERT_NO_FATAL_FAILURE(write_logs(shown_history(kA)));
    EXPECT_EQ(run(list_args()), 0) << transcript();
    const std::string order = out_line_with(kA.coid);
    ASSERT_FALSE(order.empty()) << transcript();
    EXPECT_NE(order.find("-> cannot be resolved: OrderShowsExistence"), std::string::npos) << order;
}

TEST_F(ToolTest, ListCountsTheIntentOnlyOrdersAndNeverListsThem) {
    const OrderSpec orphan{"HY-1789999399000-7-0G0G"};
    ASSERT_NO_FATAL_FAILURE(write_logs(concat(ambiguous_history(kA), intent_only_history(orphan))));
    EXPECT_EQ(run(list_args()), 0) << transcript();
    EXPECT_TRUE(out_.printed("unresolved orders: 1"));
    EXPECT_TRUE(out_.printed("intent-only orders (nothing was ever sent for them; not listed): 1"));
    EXPECT_TRUE(out_line_with(orphan.coid).empty()) << "nothing was ever sent for it: it is not an order to resolve";
    EXPECT_FALSE(out_line_with(kA.coid).empty());
}

TEST_F(ToolTest, AfterAResolutionTheListShowsOneResolvedOrderAndNoUnresolvedOne) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    ASSERT_EQ(run(resolve_args(kA.coid)), 0) << transcript();
    out_.lines.clear();
    err_.lines.clear();
    EXPECT_EQ(run(list_args()), 0) << transcript();
    EXPECT_TRUE(out_.printed("unresolved orders: 0"));
    EXPECT_TRUE(out_.printed("orders an operator has resolved before: 1"));
    EXPECT_TRUE(out_line_with(kA.coid).empty());
}

TEST_F(ToolTest, ListOfAnEmptyLogListsNothing) {
    ASSERT_NO_FATAL_FAILURE(write_logs({}, {}));
    ASSERT_TRUE(fs::exists(audit_path_));
    ASSERT_TRUE(fs::exists(cp_path_));
    EXPECT_EQ(run(list_args()), 0) << transcript();
    EXPECT_TRUE(out_.printed("unresolved orders: 0"));
    EXPECT_FALSE(out_.printed("WARNING"));
}

TEST_F(ToolTest, ListWarnsWhenTheLogHoldsMoreThanTheToolCanList) {
    std::vector<Timed> history;
    for (int i = 0; i < static_cast<int>(kMaxInFlight) + 2; ++i) {
        OrderSpec o;
        o.coid = "HY-1789999399000-" + std::to_string(i) + "-0A0A";
        history = concat(std::move(history), ambiguous_history(o));
    }
    ASSERT_NO_FATAL_FAILURE(write_logs(history));
    EXPECT_EQ(run(list_args()), 0) << "an OperatorOpen scan counts what does not fit instead of failing: " << transcript();
    EXPECT_TRUE(out_.printed("unresolved orders: " + std::to_string(kMaxInFlight)));
    EXPECT_TRUE(out_.printed("WARNING: the log holds more than the tool can list: 2 unresolved order(s), 0 intent-only order(s) and 0 "
                             "operator-resolved order(s) beyond the tables. Resolve those by hand."));
    EXPECT_TRUE(logs_untouched());
}

// ============================================================ 3. resolving ==========================================================================

TEST_F(ToolTest, AnAbsentOrderIsResolvedAndTheEvidenceIsOnDisk) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    EXPECT_EQ(run(resolve_args(kA.coid)), 0) << transcript();
    EXPECT_TRUE(err_.lines.empty()) << transcript();
    EXPECT_TRUE(out_.printed("result: Resolved (exit code 0)"));
    EXPECT_TRUE(out_.printed("resolved: the frame is durable. Restart the trading process."));

    // what the provider was asked for is what the command line said
    EXPECT_EQ(provider_.opens, 1);
    EXPECT_EQ(provider_.last_options.mode, ToolMode::Resolve);
    EXPECT_EQ(provider_.last_options.client_order_id, kA.coid);
    EXPECT_EQ(provider_.last_options.environment, OperatorEnvironment::Testnet);
    EXPECT_EQ(provider_.last_options.account_uid, kUid);
    EXPECT_EQ(provider_.last_options.operator_label, "alice");
    EXPECT_EQ(provider_.last_options.credentials_file, credentials_path_);

    // the phrase the operator was given is the one for this order, this symbol, this environment and this uid
    EXPECT_TRUE(out_.printed("ABSENT " + kA.coid + " BTCUSDT TESTNET 123456789 "));
    EXPECT_EQ(out_.reads, 1);

    // the fence, on the exchange clock, through the sleeper the tool was given
    EXPECT_EQ(sleeper_.total(), kFenceMs);
    EXPECT_GE(exchange_.first_evidence_call_ms(), kT0 + kFenceMs);

    // the evidence file: in <audit log>.operator-evidence, named by order and read time, holding what the digest on the console says
    const std::string evidence_dir = audit_path_ + ".operator-evidence";
    const std::string stem = kA.coid + "-" + std::to_string(kT0 + kFenceMs);
    EXPECT_EQ(listing(evidence_dir), (std::vector<std::string>{stem + ".json"}));
    const std::string contents = file_text(evidence_dir + "/" + stem + ".json");
    EXPECT_NE(contents.find("\"hy-operator-evidence/1\""), std::string::npos);
    EXPECT_TRUE(out_.printed("evidence file: " + stem + "  sha256: " + sha256_hex(contents)));
    EXPECT_NE(out_.printed_phrase().find(sha256_hex(contents).substr(0, kDigestPrefixChars)), std::string::npos);

    // and the next trading start reads the frame: the order is closed, no longer waiting
    EXPECT_TRUE(audit_lock_free());
    DurableAuditSink after(audit_path_, *ring_, kKeyA);
    ASSERT_TRUE(after.is_open());
    EXPECT_FALSE(after.fenced());
    ASSERT_EQ(after.recovered_operator_resolved().size(), 1u);
    EXPECT_EQ(after.recovered_operator_resolved()[0].client_order_id.view(), kA.coid);
    EXPECT_TRUE(after.recovered_checkpoints().empty());
}

TEST_F(ToolTest, TheEnvironmentTheOperatorNamesIsTheOneInThePhraseAndInTheRequest) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    EXPECT_EQ(run(resolve_args(kA.coid, "123456789", "production")), 0) << transcript();
    EXPECT_EQ(provider_.last_options.environment, OperatorEnvironment::Production);
    EXPECT_TRUE(out_.printed("ABSENT " + kA.coid + " BTCUSDT PRODUCTION 123456789 ")) << transcript();
    EXPECT_FALSE(out_.printed(" TESTNET "));
}

TEST_F(ToolTest, TheFrameCarriesTheOperatorsLabelAndTheDigestOfTheStoredEvidence) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    Args args = resolve_args(kA.coid);
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--operator") args[i + 1] = "bob.2-ops";
    }
    ASSERT_EQ(run(args), 0) << transcript();
    const std::vector<std::string> evidence = listing(audit_path_ + ".operator-evidence");
    ASSERT_EQ(evidence.size(), 1u);
    const std::string digest = sha256_hex(file_text(audit_path_ + ".operator-evidence/" + evidence[0]));

    DecodedOrderFrame frame{};
    ASSERT_TRUE(decode_last_frame(frame));
    EXPECT_EQ(frame.record.event_type, AuditEventType::OrderOperatorResolved);
    EXPECT_EQ(frame.record.resulting_state, OrderState::Rejected);
    EXPECT_STREQ(frame.record.client_order_id, kA.coid.c_str());
    EXPECT_EQ(std::string(frame.record.detail_msg), "op=bob.2-ops;ev=" + digest.substr(0, 32))
        << "the label the operator gave, and the digest of the file that is on disk";
    EXPECT_EQ(frame.recorded_utc_ms, exchange_.calls.back().at_ms) << "stamped with the exchange's clock, read just before the append";
    EXPECT_EQ(frame.time_kind, FrameTimeKind::ServerCorrectedUtc);
}

TEST_F(ToolTest, TheCredentialsAreAskedForBeforeALogIsOpenedAndBothLogsAreLockedBeforeTheClockIsRead) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    ProbingExchange probe(exchange_);
    provider_.port = &probe;
    bool free_when_credentials_were_asked = false;
    int clock_probes = 0;
    bool audit_free_at_clock = true;
    bool control_free_at_clock = true;
    provider_.on_open = [&] { free_when_credentials_were_asked = audit_lock_free() && control_lock_free(); };
    probe.on_first_time = [&] {
        ++clock_probes;
        audit_free_at_clock = audit_lock_free();
        control_free_at_clock = control_lock_free();
    };
    EXPECT_EQ(run(resolve_args(kA.coid)), 0) << transcript();
    EXPECT_TRUE(free_when_credentials_were_asked) << "the credentials are asked for before any log is touched";
    EXPECT_EQ(clock_probes, 1);
    EXPECT_FALSE(audit_free_at_clock) << "the audit log is locked (OperatorOpen) before the session's first serverTime: that reading is the fence";
    EXPECT_FALSE(control_free_at_clock) << "and so is the control-plane log";
}

TEST_F(ToolTest, WhenTheProviderRefusesBothLogsAreExactlyAsTheyWereAndNoLockWasEverHeld) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    provider_.fail = true;
    EXPECT_EQ(run(resolve_args(kA.coid)), exit_code_of(ToolExit::Exchange));
    EXPECT_TRUE(err_says("--credentials-file: no such file")) << transcript();
    EXPECT_TRUE(out_.lines.empty());
    EXPECT_EQ(provider_.opens, 1);
    EXPECT_TRUE(exchange_.calls.empty());
    expect_nothing_written();
}

// Every way the procedure can stop has its own exit code (40 + its status), says so, and leaves both logs exactly as they were.
TEST_F(ToolTest, AnOrderTheLogDoesNotListStopsTheProcedureAtOnce) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    expect_stops_with(ResolveStatus::NoSuchUnresolvedOrder, resolve_args("HY-1789999399000-9-0Z0Z"));
    EXPECT_TRUE(sleeper_.sleeps.empty());
}

TEST_F(ToolTest, AUidThatIsNotTheKeysAccountStopsTheProcedureBeforeTheWait) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    expect_stops_with(ResolveStatus::AccountUidMismatch, resolve_args(kA.coid, "999"));
    EXPECT_TRUE(sleeper_.sleeps.empty()) << "refused before the fence is waited for";
    EXPECT_TRUE(out_.printed("this API key belongs to account uid 123456789, not to the uid 999 you stated: nothing was written"));
}

TEST_F(ToolTest, AnAccountTheExchangeDoesNotNameStopsTheProcedureBeforeTheWait) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    exchange_.account_uid.reset();
    expect_stops_with(ResolveStatus::AccountUnverified, resolve_args(kA.coid));
    EXPECT_TRUE(sleeper_.sleeps.empty());
}

TEST_F(ToolTest, AnOrderTheExchangeKnowsIsReportedWithItsEvidenceAndNobodyIsAsked) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    exchange_.orders.push_back(visible_order(kA));
    expect_stops_with(ResolveStatus::EvidenceNotAbsent, resolve_args(kA.coid));
    EXPECT_EQ(out_.reads, 0);
    EXPECT_EQ(listing(audit_path_ + ".operator-evidence").size(), 1u) << "the evidence of the refusal is kept";
}

TEST_F(ToolTest, AnEvidenceReadThatFailsStopsTheProcedure) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    exchange_.fail_kind = "allOrders";
    expect_stops_with(ResolveStatus::EvidenceReadFailed, resolve_args(kA.coid));
}

TEST_F(ToolTest, APhraseThatIsNotTypedConfirmsNothing) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    for (const FakeConsole::Mode mode : {FakeConsole::Mode::TypeNothing, FakeConsole::Mode::Timeout, FakeConsole::Mode::Eof,
                                         FakeConsole::Mode::Error, FakeConsole::Mode::TooLong}) {
        out_.mode = mode;
        out_.lines.clear();
        exchange_.calls.clear();
        world_.now_ms = kT0;
        std::error_code ec;
        fs::remove_all(audit_path_ + ".operator-evidence", ec);
        expect_stops_with(ResolveStatus::NotConfirmed, resolve_args(kA.coid));
    }
}

TEST_F(ToolTest, AnEvidenceDirectoryThatIsAFileStopsTheProcedureBeforeAnyoneIsAsked) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    write_text(audit_path_ + ".operator-evidence", "in the way");
    expect_stops_with(ResolveStatus::EvidenceStoreFailed, resolve_args(kA.coid));
    EXPECT_FALSE(out_.printed("To resolve this order as ABSENT")) << "nobody is asked to confirm what could not be stored";
    EXPECT_EQ(out_.reads, 0);
    EXPECT_EQ(file_text(audit_path_ + ".operator-evidence"), "in the way");
}

TEST_F(ToolTest, ADryRunOrderIsRefusedWithoutTrafficOrWait) {
    OrderSpec dry = kA;
    dry.mode = ExecutionMode::DryRun;
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(dry)));
    EXPECT_EQ(run(resolve_args(dry.coid)), exit_code_of(ResolveStatus::DryRunOrder));
    EXPECT_TRUE(out_.printed("result: DryRunOrder (exit code 47)"));
    EXPECT_EQ(exchange_.calls.size(), 1u) << "only the session's clock reading";
    EXPECT_TRUE(sleeper_.sleeps.empty());
    EXPECT_TRUE(logs_untouched());
}

TEST_F(ToolTest, ASessionThatCannotStartExitsSixAndSaysWhy) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    exchange_.fail_time_from_read = 0;  // the very first serverTime reading fails
    EXPECT_EQ(run(resolve_args(kA.coid)), exit_code_of(ToolExit::Session));
    EXPECT_TRUE(err_says("error: the session could not start: ServerTimeUnavailable. Nothing was written.")) << transcript();
    expect_nothing_written();
}

TEST_F(ToolTest, TheFenceParametersAndTheConfirmationTimeoutOfTheEnvironmentReachTheProcedure) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    fence_.read_lag_ms = 300'000;  // a parameter may be raised
    confirm_timeout_ = std::chrono::seconds{90};
    EXPECT_EQ(run(resolve_args(kA.coid)), 0) << transcript();
    EXPECT_EQ(sleeper_.total(), kRecvWindowFloorMs + kClockSkewFloorMs + kFenceMarginFloorMs + 300'000);
    EXPECT_EQ(out_.last_timeout, std::chrono::seconds{90});
}

TEST_F(ToolTest, ABadFenceParameterIsASessionRefusalAndWritesNothing) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    fence_.read_lag_ms = kReadLagFloorMs - 1;  // a parameter may never be lowered
    EXPECT_EQ(run(resolve_args(kA.coid)), exit_code_of(ToolExit::Session));
    EXPECT_TRUE(err_says("BadFenceParams")) << transcript();
    expect_nothing_written();
}

TEST_F(ToolTest, TheExitCodesAreTheDocumentedOnes) {
    EXPECT_EQ(exit_code_of(ToolExit::Ok), 0);
    EXPECT_EQ(exit_code_of(ToolExit::Usage), 2);
    EXPECT_EQ(exit_code_of(ToolExit::KeyMaterial), 3);
    EXPECT_EQ(exit_code_of(ToolExit::Logs), 4);
    EXPECT_EQ(exit_code_of(ToolExit::Exchange), 5);
    EXPECT_EQ(exit_code_of(ToolExit::Session), 6);
    EXPECT_EQ(exit_code_of(ToolExit::Internal), 70);
    EXPECT_EQ(exit_code_of(ResolveStatus::Resolved), 0);
    EXPECT_EQ(exit_code_of(ResolveStatus::EvidenceNotAbsent), 55);
    EXPECT_EQ(exit_code_of(ResolveStatus::NotConfirmed), 57);
    EXPECT_EQ(exit_code_of(ResolveStatus::AppendRefused), 58);
    EXPECT_EQ(exit_code_of(ResolveStatus::AppendFailed), 59);
    EXPECT_EQ(exit_code_of(ResolveStatus::AccountUnverified), 60);
    EXPECT_EQ(exit_code_of(ResolveStatus::AccountUidMismatch), 61);
    // every status but Resolved is 40 plus its number, and none of them collides with a code of the tool itself
    for (int s = 0; s <= static_cast<int>(ResolveStatus::AccountUidMismatch); ++s) {
        const ResolveStatus status = static_cast<ResolveStatus>(s);
        if (status == ResolveStatus::Resolved) continue;
        EXPECT_EQ(exit_code_of(status), 40 + s);
        EXPECT_GT(exit_code_of(status), exit_code_of(ToolExit::Session));
        EXPECT_LT(exit_code_of(status), exit_code_of(ToolExit::Internal));
    }
}

TEST_F(ToolTest, AnExceptionAnywhereIsAnInternalErrorAndTheMessageSaysToLook) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    provider_.throw_on_open = true;
    EXPECT_EQ(run(resolve_args(kA.coid)), exit_code_of(ToolExit::Internal));
    EXPECT_TRUE(err_says("an unexpected exception stopped the tool. Run --list to see whether the order is still listed.")) << transcript();
    expect_nothing_written();

    // out of the middle of the procedure, with both logs open and locked: they are released and the order is still waiting
    provider_.throw_on_open = false;
    ProbingExchange probe(exchange_);
    probe.throw_on_query = true;
    provider_.port = &probe;
    err_.lines.clear();
    EXPECT_EQ(run(resolve_args(kA.coid)), 70);
    EXPECT_TRUE(err_says("an unexpected exception"));
    expect_nothing_written();
}

// ============================================================ 4. the key material ===================================================================

TEST_F(ToolTest, AWrongKekIsRefusedAsKeyMaterialBeforeALogIsOpened) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    std::array<std::byte, kKekSize> other{};
    for (std::size_t i = 0; i < other.size(); ++i) other[i] = static_cast<std::byte>(0x90 + i);
    write_bytes(kek_path_, other.data(), other.size());
    make_owner_only(kek_path_);
    EXPECT_EQ(run(list_args()), exit_code_of(ToolExit::KeyMaterial));
    EXPECT_TRUE(err_says("error: --key-file: a key does not unwrap under this KEK (wrong KEK, or a damaged file)")) << transcript();
    EXPECT_TRUE(out_.lines.empty());
    EXPECT_TRUE(logs_untouched());
    EXPECT_TRUE(audit_lock_free());
}

TEST_F(ToolTest, AKekFileThatIsMissingOrTheWrongSizeIsRefused) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    fs::remove(kek_path_);
    EXPECT_EQ(run(list_args()), exit_code_of(ToolExit::KeyMaterial));
    EXPECT_TRUE(err_says("error: --kek-file: no such file")) << transcript();
    for (const std::size_t size : {0u, 1u, 31u, 33u, 64u}) {
        err_.lines.clear();
        const std::vector<std::byte> bytes(size, std::byte{0x41});
        write_bytes(kek_path_, bytes.data(), bytes.size());
        make_owner_only(kek_path_);
        EXPECT_EQ(run(list_args()), 3) << size;
        EXPECT_TRUE(err_says("error: --kek-file: it is not exactly 32 bytes")) << size << transcript();
    }
    EXPECT_TRUE(logs_untouched());
}

#ifdef __linux__
TEST_F(ToolTest, AKekFileThatOthersCanReadIsRefused) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    ::chmod(kek_path_.c_str(), 0644);
    EXPECT_EQ(run(list_args()), exit_code_of(ToolExit::KeyMaterial));
    EXPECT_TRUE(err_says("error: --kek-file: group or other can read it: make it owner-only (chmod 600)")) << transcript();
    EXPECT_TRUE(logs_untouched());
}
#endif

TEST_F(ToolTest, AKeyFileThatIsMissingDamagedOrTooBigIsRefused) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    const std::vector<std::byte> good = file_bytes(key_path_);

    fs::remove(key_path_);
    EXPECT_EQ(run(list_args()), exit_code_of(ToolExit::KeyMaterial));
    EXPECT_TRUE(err_says("error: --key-file: no such file")) << transcript();

    std::vector<std::byte> bad_magic = good;
    bad_magic[0] = std::byte{'X'};
    write_bytes(key_path_, bad_magic.data(), bad_magic.size());
    err_.lines.clear();
    EXPECT_EQ(run(list_args()), 3);
    EXPECT_TRUE(err_says("error: --key-file: not a HYKEYS01 wrapped-key file")) << transcript();

    write_bytes(key_path_, good.data(), good.size() - 1);
    err_.lines.clear();
    EXPECT_EQ(run(list_args()), 3);
    EXPECT_TRUE(err_says("error: --key-file: the file is not the length its own key count says")) << transcript();

    const std::vector<std::byte> at_the_bound(64 * 1024, std::byte{0});
    write_bytes(key_path_, at_the_bound.data(), at_the_bound.size());
    err_.lines.clear();
    EXPECT_EQ(run(list_args()), 3);
    EXPECT_TRUE(err_says("error: --key-file: not a HYKEYS01 wrapped-key file")) << "64 KiB exactly is read, and refused for what it holds: " << transcript();

    const std::vector<std::byte> huge(64 * 1024 + 1, std::byte{0});
    write_bytes(key_path_, huge.data(), huge.size());
    err_.lines.clear();
    EXPECT_EQ(run(list_args()), 3);
    EXPECT_TRUE(err_says("error: --key-file: too large to be a wrapped-key file")) << transcript();

    const std::vector<std::byte> empty;
    write_bytes(key_path_, empty.data(), empty.size());
    err_.lines.clear();
    EXPECT_EQ(run(list_args()), 3);
    EXPECT_TRUE(err_says("error: --key-file: the file is not the length its own key count says")) << transcript();
    EXPECT_TRUE(logs_untouched());
}

TEST_F(ToolTest, AKeyFileWithTooManyKeysOrTheSameKeyTwiceIsRefused) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    // the count in the header alone: more keys than the ring holds
    std::vector<std::byte> too_many;
    for (const char c : std::string_view("HYKEYS01")) too_many.push_back(static_cast<std::byte>(c));
    for (const std::uint8_t b : {static_cast<std::uint8_t>(kMaxLiveKeys + 1), std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{0}}) {
        too_many.push_back(static_cast<std::byte>(b));
    }
    write_bytes(key_path_, too_many.data(), too_many.size());
    EXPECT_EQ(run(list_args()), exit_code_of(ToolExit::KeyMaterial));
    EXPECT_TRUE(err_says("error: --key-file: more keys than the key ring holds")) << transcript();

    // the same record twice
    const std::array<WrappedKeyRecord, 2> twice{records_[0], records_[0]};
    const std::vector<std::byte> duplicated = encode_audit_key_file(twice);
    write_bytes(key_path_, duplicated.data(), duplicated.size());
    err_.lines.clear();
    EXPECT_EQ(run(list_args()), 3);
    EXPECT_TRUE(err_says("error: --key-file: the same key id twice")) << transcript();
    EXPECT_TRUE(logs_untouched());
}

#ifdef __linux__
TEST_F(ToolTest, AKekFileThatIsASymbolicLinkIsRefused) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    const std::string link = dir_.file("kek.link");
    fs::create_symlink(kek_path_, link);
    Args args = list_args();
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--kek-file") args[i + 1] = link;
    }
    EXPECT_EQ(run(args), exit_code_of(ToolExit::KeyMaterial));
    EXPECT_TRUE(err_says("error: --kek-file: it is a symbolic link")) << transcript();
}
#endif

TEST_F(ToolTest, AKeyIdTheKeyFileDoesNotHoldIsRefusedForEachOfTheTwoLogs) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    Args audit = list_args();
    for (std::size_t i = 0; i + 1 < audit.size(); ++i) {
        if (audit[i] == "--audit-key-id") audit[i + 1] = "9";
    }
    EXPECT_EQ(run(audit), exit_code_of(ToolExit::KeyMaterial));
    EXPECT_TRUE(err_says("error: --key-file does not hold the key --audit-key-id names")) << transcript();

    err_.lines.clear();
    Args control = list_args();
    for (std::size_t i = 0; i + 1 < control.size(); ++i) {
        if (control[i] == "--control-key-id") control[i + 1] = "9";
    }
    EXPECT_EQ(run(control), 3);
    EXPECT_TRUE(err_says("error: --key-file does not hold the key --control-key-id names")) << transcript();
    EXPECT_TRUE(logs_untouched());
}

// ============================================================ 5. the logs ===========================================================================

TEST_F(ToolTest, AnAuditLogThatDoesNotExistIsNeverCreated) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    std::error_code ec;
    fs::remove(audit_path_, ec);
    const std::vector<std::string> before = listing(dir_.path);
    EXPECT_EQ(run(list_args()), exit_code_of(ToolExit::Logs));
    EXPECT_TRUE(err_says("error: --audit-log: there is no log file at that path (a mistyped path would be created empty)")) << transcript();
    EXPECT_EQ(listing(dir_.path), before) << "nothing was created: not the log, not a lock, not a tip";
}

TEST_F(ToolTest, AControlPlaneLogThatDoesNotExistIsNeverCreatedAndTheAuditLogIsNotLocked) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    std::error_code ec;
    fs::remove(cp_path_, ec);
    const std::vector<std::string> before = listing(dir_.path);
    EXPECT_EQ(run(list_args()), exit_code_of(ToolExit::Logs));
    EXPECT_TRUE(err_says("error: --control-log: there is no log file at that path (a mistyped path would be created empty)")) << transcript();
    EXPECT_EQ(listing(dir_.path), before);
    EXPECT_TRUE(audit_lock_free());
    EXPECT_TRUE(logs_untouched() || file_bytes(audit_path_) == audit_before_);
}

TEST_F(ToolTest, ADirectoryOrAnythingElseThatIsNotAFileIsNotALog) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    const std::string a_directory = dir_.file("adir");
    fs::create_directory(a_directory);
    Args audit = list_args();
    for (std::size_t i = 0; i + 1 < audit.size(); ++i) {
        if (audit[i] == "--audit-log") audit[i + 1] = a_directory;
    }
    EXPECT_EQ(run(audit), exit_code_of(ToolExit::Logs));
    EXPECT_TRUE(err_says("error: --audit-log: there is no log file at that path")) << transcript();
    err_.lines.clear();
    Args control = list_args();
    for (std::size_t i = 0; i + 1 < control.size(); ++i) {
        if (control[i] == "--control-log") control[i + 1] = a_directory;
    }
    EXPECT_EQ(run(control), 4);
    EXPECT_TRUE(err_says("error: --control-log: there is no log file at that path")) << transcript();
    EXPECT_TRUE(logs_untouched());
}

TEST_F(ToolTest, ALogHeldByAnotherProcessIsRefusedAndNothingIsWritten) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    {
        DurableAuditSink running(audit_path_, *ring_, kKeyA);  // the trading process, still alive
        ASSERT_TRUE(running.is_open());
        EXPECT_EQ(run(list_args()), exit_code_of(ToolExit::Logs));
        EXPECT_TRUE(err_says("error: the audit log could not be opened. Is the trading process still running?")) << transcript();
        EXPECT_TRUE(out_.lines.empty());
    }
    EXPECT_TRUE(logs_untouched());
    {
        err_.lines.clear();
        ControlPlaneLogSink running(cp_path_, *ring_, kKeyA);
        ASSERT_TRUE(running.is_open());
        EXPECT_EQ(run(list_args()), 4);
        EXPECT_TRUE(err_says("error: the control-plane log could not be opened (is the trading process still running?)")) << transcript();
    }
    expect_nothing_written();  // and the audit log's lock, taken a moment before, was released
}

TEST_F(ToolTest, AResolveRunOverALockedLogNeverReachesTheExchange) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    DurableAuditSink running(audit_path_, *ring_, kKeyA);
    ASSERT_TRUE(running.is_open());
    EXPECT_EQ(run(resolve_args(kA.coid)), 4);
    EXPECT_TRUE(exchange_.calls.empty()) << "no clock reading without the lock: it would be a fence that protects nothing";
    EXPECT_TRUE(sleeper_.sleeps.empty());
}

TEST_F(ToolTest, AKeyIdTheLogWasNotWrittenWithIsRefusedAsALogThatDidNotRecover) {
    ASSERT_NO_FATAL_FAILURE(write_logs(ambiguous_history(kA)));
    Args args = list_args();
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--audit-key-id") args[i + 1] = "2";  // a key that is in the file, but is not the one the log was written with
    }
    EXPECT_EQ(run(args), exit_code_of(ToolExit::Logs));
    EXPECT_TRUE(err_says("error: the audit log did not recover cleanly (Corrupt). Nothing was written.")) << transcript();
    EXPECT_TRUE(logs_untouched());

    err_.lines.clear();
    Args control = list_args();
    for (std::size_t i = 0; i + 1 < control.size(); ++i) {
        if (control[i] == "--control-key-id") control[i + 1] = "2";
    }
    EXPECT_EQ(run(control), 4);
    EXPECT_TRUE(err_says("error: the control-plane log did not recover cleanly (Corrupt). Nothing was written.")) << transcript();
    EXPECT_TRUE(logs_untouched());
    EXPECT_TRUE(audit_lock_free());
    EXPECT_TRUE(control_lock_free());
}
