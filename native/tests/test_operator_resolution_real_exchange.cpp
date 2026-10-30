// SPDX-License-Identifier: proprietary
// test_operator_resolution_real_exchange.cpp -- D5 slice R3b-2: the offline operator tool's real exchange (operator_resolution_real_exchange.hpp)
// and the whole tool over it, against a local TLS "exchange".
//
// Two layers:
//   1. RealExchangeProvider on its own: what a credentials file has to hold (the tool's own two names for the chosen environment, a well-formed
//      key, and nothing else), what every way of failing says, and that the TRADING key is not loadable here -- not by name, not by sharing a
//      file with the tool's names, not by choosing the other environment. No socket is opened.
//   2. run_operator_tool() with the real provider, the real credentials loader and the real TLS client pointed at a local routing exchange: the
//      requests on the wire are GETs signed with the tool's key and sent to the host of the chosen environment, the evidence file and the frame
//      are written, and a run that cannot name its credentials sends nothing at all. The one thing replaced is the fence clock (a decorator
//      answers serverTime from a fake world clock the sleeper advances: a 270 s fence cannot be waited out, and the real client refuses to
//      calibrate against an exchange clock that far from the local one) -- the same replacement test_operator_resolution_transport.cpp makes.
//
// Synthetic credentials only; no real network access.

#include <gtest/gtest.h>
#include <hengyuan/operator_resolution_real_exchange.hpp>

#include "binance_private_rest_test_hooks.hpp"
#include "operator_resolution_fakes.hpp"
#include "test_helpers/server_time_body.hpp"
#include "test_helpers/temp_directory.hpp"
#include "test_helpers/tls_routing_acceptor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef __linux__
#include <sys/stat.h>
#endif

using namespace hy;
using namespace hy::operator_resolution;
using namespace hy::operator_resolution::testing;
using hy::test_helpers::TempDirectory;
using hy::test_helpers::TlsRoutingAcceptor;
using Reply = TlsRoutingAcceptor::Reply;
using Request = TlsRoutingAcceptor::Request;

namespace fs = std::filesystem;

namespace {

// 64 alphanumeric characters, the shape of a Binance API key; obviously synthetic.
constexpr std::string_view kOperatorApiKey = "OPERATORKEYabcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0";
constexpr std::string_view kOperatorSecret = "synthetic_operator_secret_for_tests_only";
constexpr std::string_view kTradingApiKey = "TRADINGKEYabcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01";
static_assert(kOperatorApiKey.size() == 64);
static_assert(kTradingApiKey.size() == 64);

std::string fixture_path(const char* filename) { return std::string(HY_TEST_FIXTURE_DIR) + "/" + filename; }

std::int64_t real_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void write_text(const std::string& path, std::string_view text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void write_bytes(const std::string& path, const void* data, std::size_t size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

void make_owner_only(const std::string& path) {
#ifdef __linux__
    ::chmod(path.c_str(), 0600);
#else
    static_cast<void>(path);
#endif
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

std::string credentials_text(std::string_view api_key_name, std::string_view secret_name, std::string_view key = kOperatorApiKey,
                             std::string_view secret = kOperatorSecret) {
    return std::string(api_key_name) + "=" + std::string(key) + "\n" + std::string(secret_name) + "=" + std::string(secret) + "\n";
}

const std::string kTestnetCredentials = credentials_text("HENGYUAN_OPERATOR_TESTNET_API_KEY", "HENGYUAN_OPERATOR_TESTNET_API_SECRET");
const std::string kLiveCredentials = credentials_text("HENGYUAN_OPERATOR_LIVE_API_KEY", "HENGYUAN_OPERATOR_LIVE_API_SECRET");
const std::string kTradingCredentials = credentials_text("HENGYUAN_BINANCE_TESTNET_API_KEY", "HENGYUAN_BINANCE_TESTNET_SECRET",
                                                         kTradingApiKey, "synthetic_trading_secret_for_tests_only");

}  // namespace

// ============================================================ 1. the provider =====================================================================

class ProviderTest : public ::testing::Test {
protected:
    TempDirectory dir_;
    std::string credentials_path_;
    FakeWorld world_;
    FakeSleeper sleeper_{world_};
    FakeConsole err_;
    RealExchangeProvider provider_{PrivateRestConfig{}, TrustStoreCheck::Skip};  // (no socket is opened here: the machine's trust store is not the point)

    void SetUp() override { credentials_path_ = dir_.file("operator.env"); }

    ToolOptions options(OperatorEnvironment env = OperatorEnvironment::Testnet) const {
        ToolOptions o;
        o.mode = ToolMode::Resolve;
        o.environment = env;
        o.credentials_file = credentials_path_;
        return o;
    }

    void write_credentials(std::string_view text) {
        write_text(credentials_path_, text);
        make_owner_only(credentials_path_);
    }

    ExchangeReadPort* open(OperatorEnvironment env = OperatorEnvironment::Testnet) { return provider_.open(options(env), sleeper_, err_); }

    bool err_says(std::string_view fragment) const { return err_.printed(fragment); }
};

TEST_F(ProviderTest, AnOperatorCredentialsFileGivesAPortAndAClientForTheChosenEnvironment) {
    write_credentials(kTestnetCredentials);
    EXPECT_NE(open(OperatorEnvironment::Testnet), nullptr);
    EXPECT_TRUE(err_.lines.empty());
    EXPECT_NE(provider_.client(), nullptr);

    write_credentials(kLiveCredentials);
    EXPECT_NE(open(OperatorEnvironment::Production), nullptr);
    EXPECT_TRUE(err_.lines.empty());
    EXPECT_NE(provider_.client(), nullptr);
}

TEST_F(ProviderTest, EachEnvironmentReadsItsOwnNamesAndTheOtherEnvironmentsNamesAreRefused) {
    write_credentials(kTestnetCredentials);
    EXPECT_EQ(open(OperatorEnvironment::Production), nullptr);
    EXPECT_TRUE(err_says("error: --credentials-file: it holds a name this tool does not read: the file may hold only the tool's own two "
                         "HENGYUAN_OPERATOR_... names"))
        << "the testnet names are not the production ones";
    EXPECT_EQ(provider_.client(), nullptr);

    err_.lines.clear();
    write_credentials(kLiveCredentials);
    EXPECT_EQ(open(OperatorEnvironment::Testnet), nullptr);
    EXPECT_TRUE(err_says("it holds a name this tool does not read"));
    EXPECT_EQ(provider_.client(), nullptr);
}

TEST_F(ProviderTest, TheTradingKeyIsNotLoadableHereByItsNameOrBySharingAFileWithTheToolsNames) {
    write_credentials(kTradingCredentials);
    EXPECT_EQ(open(), nullptr);
    EXPECT_TRUE(err_says("it holds a name this tool does not read")) << "a file with the trading names";
    EXPECT_EQ(provider_.client(), nullptr);

    err_.lines.clear();
    write_credentials(kTestnetCredentials + "HENGYUAN_BINANCE_TESTNET_API_KEY=" + std::string(kTradingApiKey) + "\n");
    EXPECT_EQ(open(), nullptr);
    EXPECT_TRUE(err_says("it holds a name this tool does not read")) << "the tool's names AND a trading name in one file";

    err_.lines.clear();
    write_credentials(kTestnetCredentials + "SOMETHING_ELSE=1\n");
    EXPECT_EQ(open(), nullptr);
    EXPECT_TRUE(err_says("it holds a name this tool does not read"));
    EXPECT_EQ(provider_.client(), nullptr);
}

TEST_F(ProviderTest, EveryWayACredentialsFileCanBeWrongHasItsOwnMessage) {
    struct Row {
        const char* what;
        std::string text;
        const char* says;
    };
    const std::vector<Row> rows = {
        {"a key that is too short", credentials_text("HENGYUAN_OPERATOR_TESTNET_API_KEY", "HENGYUAN_OPERATOR_TESTNET_API_SECRET", "tooshort"),
         "error: --credentials-file: the API key is missing or is not a 64-character alphanumeric key"},
        {"a key with a character Binance does not use",
         credentials_text("HENGYUAN_OPERATOR_TESTNET_API_KEY", "HENGYUAN_OPERATOR_TESTNET_API_SECRET",
                          "OPERATORKEY-bcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0"),
         "is not a 64-character alphanumeric key"},
        {"no key at all", "HENGYUAN_OPERATOR_TESTNET_API_SECRET=synthetic_operator_secret_for_tests_only\n", "is not a 64-character alphanumeric key"},
        {"no secret", "HENGYUAN_OPERATOR_TESTNET_API_KEY=" + std::string(kOperatorApiKey) + "\n", "the credentials could not be bound"},
        {"an empty file", "", "is not a 64-character alphanumeric key"},
        {"a line that is not NAME=value", kTestnetCredentials + "this is not a setting\n", "a line is not NAME=value"},
        {"a name given twice", kTestnetCredentials + "HENGYUAN_OPERATOR_TESTNET_API_KEY=" + std::string(kOperatorApiKey) + "\n", "a name is given twice"},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.what);
        err_.lines.clear();
        write_credentials(row.text);
        EXPECT_EQ(open(), nullptr);
        EXPECT_TRUE(err_says(row.says)) << (err_.lines.empty() ? std::string("(nothing said)") : err_.lines.front());
        EXPECT_EQ(provider_.client(), nullptr);
        ASSERT_FALSE(err_.lines.empty());
        EXPECT_EQ(err_.lines.front().rfind("error: --credentials-file: ", 0), 0u) << "one line, naming the option";
        for (const std::string& line : err_.lines) {
            EXPECT_EQ(line.find(kOperatorApiKey), std::string::npos) << "the key is never echoed";
            EXPECT_EQ(line.find(kOperatorSecret), std::string::npos) << "the secret is never echoed";
        }
    }
}

TEST_F(ProviderTest, AMissingFileAndAFileThatIsNotAFileAreRefused) {
    EXPECT_EQ(open(), nullptr);
    EXPECT_TRUE(err_says("error: --credentials-file: no such file"));
    err_.lines.clear();
    fs::create_directory(credentials_path_);
    EXPECT_EQ(open(), nullptr);
    EXPECT_EQ(provider_.client(), nullptr);
    ASSERT_FALSE(err_.lines.empty());
    EXPECT_EQ(err_.lines.front().rfind("error: --credentials-file: ", 0), 0u);
}

#ifdef __linux__
TEST_F(ProviderTest, ACredentialsFileThatOthersCanReadIsRefused) {
    write_credentials(kTestnetCredentials);
    ::chmod(credentials_path_.c_str(), 0644);
    EXPECT_EQ(open(), nullptr);
    EXPECT_TRUE(err_says("error: --credentials-file: group or other can read it: make it owner-only (chmod 600)"));
    EXPECT_EQ(provider_.client(), nullptr);
}
#endif

TEST_F(ProviderTest, ASecondOpenReplacesTheFirstAndAFailedOneLeavesNoClientBehind) {
    write_credentials(kTestnetCredentials);
    ExchangeReadPort* first = open();
    ASSERT_NE(first, nullptr);
    BinancePrivateRestClient* first_client = provider_.client();
    ASSERT_NE(first_client, nullptr);
    write_credentials("garbage\n");
    EXPECT_EQ(open(), nullptr);
    EXPECT_EQ(provider_.client(), nullptr) << "the earlier client is gone: nothing half-built is kept";
    write_credentials(kTestnetCredentials);
    EXPECT_NE(open(), nullptr);
    EXPECT_NE(provider_.client(), nullptr);
}

TEST_F(ProviderTest, ARequiredTrustStoreIsCheckedAndAnEmptyOneEndsTheRunWithAClearMessage) {
    write_credentials(kTestnetCredentials);
    // what the client's own check says on this machine (Linux with a CA bundle: populated; a Windows OpenSSL without one: empty)
    boost::asio::ssl::context probe(boost::asio::ssl::context::tlsv12_client);
    configure_binance_ssl_context(probe);
    const bool populated = binance_tls_trust_store_populated(probe);

    RealExchangeProvider strict;  // main()'s: the trust store is Required
    ExchangeReadPort* port = strict.open(options(), sleeper_, err_);
    if (populated) {
        EXPECT_NE(port, nullptr);
        EXPECT_TRUE(err_.lines.empty());
        EXPECT_NE(strict.client(), nullptr);
    } else {
        EXPECT_EQ(port, nullptr);
        EXPECT_TRUE(err_says("error: the TLS trust store of this machine is empty, so every handshake would fail: run the tool where OpenSSL has "
                             "a CA bundle (a Linux host or WSL)"))
            << (err_.lines.empty() ? std::string("(nothing said)") : err_.lines.front());
        EXPECT_EQ(strict.client(), nullptr) << "no client is kept that could not verify a server";
    }
    std::fprintf(stderr, "[operator tool] this machine's default TLS trust store is populated: %s\n", populated ? "true" : "false");
}

TEST(EnvLoadText, SaysSomethingSpecificForEveryStatus) {
    const auto text = [](EnvLoadStatus status, EnvParseStatus parse = EnvParseStatus::Ok) {
        EnvLoadResult r;
        r.status = status;
        r.parse_result.status = parse;
        return std::string(env_load_text(r));
    };
    EXPECT_EQ(text(EnvLoadStatus::Ok), "Ok");
    EXPECT_EQ(text(EnvLoadStatus::FileNotFound), "no such file");
    EXPECT_EQ(text(EnvLoadStatus::PermissionTooWide), "group or other can read it: make it owner-only (chmod 600)");
    EXPECT_EQ(text(EnvLoadStatus::IsSymlink), "it is a symbolic link");
    EXPECT_EQ(text(EnvLoadStatus::FileTooLarge), "the file is too large to be a credentials file");
    EXPECT_EQ(text(EnvLoadStatus::ReadError), "it could not be read");
    EXPECT_EQ(text(EnvLoadStatus::LockFailed), "its memory could not be locked");
    EXPECT_EQ(text(EnvLoadStatus::ParseError, EnvParseStatus::UnknownKey),
              "it holds a name this tool does not read: the file may hold only the tool's own two HENGYUAN_OPERATOR_... names");
    EXPECT_EQ(text(EnvLoadStatus::ParseError, EnvParseStatus::MalformedLine), "a line is not NAME=value");
    EXPECT_EQ(text(EnvLoadStatus::ParseError, EnvParseStatus::DuplicateKey), "a name is given twice");
    EXPECT_EQ(text(EnvLoadStatus::ParseError, EnvParseStatus::KeyTooLong), "a name is too long");
    EXPECT_EQ(text(EnvLoadStatus::ParseError, EnvParseStatus::TooManyEntries), "too many entries");
    EXPECT_EQ(text(EnvLoadStatus::ParseError, EnvParseStatus::Ok), "it could not be parsed");
}

// ============================================================ 2. the whole tool over TLS ==========================================================

namespace {

Reply time_reply() { return Reply::answer(200, hy::test_helpers::current_server_time_body()); }

std::string path_of(const Request& r) { return r.target.substr(0, r.target.find('?')); }

constexpr std::uint64_t kUid = kFakeAccountUid;

const std::string kAccountAnswer = R"({"uid":123456789,"accountType":"SPOT","permissions":["SPOT"],)"
                                   R"("balances":[{"asset":"USDT","free":"1000.0","locked":"0"}]})";

struct Routes {
    Reply order = Reply::answer(400, R"({"code":-2013,"msg":"Order does not exist."})");
    Reply all_orders = Reply::answer(200, "[]");
    Reply open_orders = Reply::answer(200, "[]");
    Reply account = Reply::answer(200, kAccountAnswer);

    TlsRoutingAcceptor::Handler handler() const {
        const Routes r = *this;
        return [r](const Request& req) {
            const std::string path = path_of(req);
            if (path == "/api/v3/time") return time_reply();
            if (path == "/api/v3/order") return r.order;
            if (path == "/api/v3/allOrders") return r.all_orders;
            if (path == "/api/v3/openOrders") return r.open_orders;
            if (path == "/api/v3/account") return r.account;
            return Reply::answer(404, "{}");
        };
    }
};

// The flow's clock, without the real client's calibration: serverTime comes from the fake world (which the sleeper advances), while the real
// port still makes its real request for every reading -- so the traffic on the wire is what a production run produces and the client's own
// clock snapshot stays fresh. Every other read goes through the real port unchanged.
class WorldClockPort final : public ExchangeReadPort {
public:
    WorldClockPort(ExchangeReadPort& real, FakeWorld& world) : real_(real), world_(world) {}

    std::optional<ExchangeMs> server_time() override {
        if (!real_.server_time()) return std::nullopt;
        return ExchangeMs{world_.now_ms};
    }
    HttpRead query_order(std::string_view symbol, std::string_view client_order_id) override { return real_.query_order(symbol, client_order_id); }
    HttpRead all_orders(std::string_view symbol, ExchangeMs start, ExchangeMs end, std::size_t limit) override {
        return real_.all_orders(symbol, start, end, limit);
    }
    HttpRead open_orders() override { return real_.open_orders(); }
    AccountView account() override { return real_.account(); }

private:
    ExchangeReadPort& real_;
    FakeWorld& world_;
};

// The real provider, with its client pointed at the local exchange (the only thing a test does to it) and its clock replaced.
class LoopbackProvider final : public ExchangeProvider {
public:
    LoopbackProvider(RealExchangeProvider& real, std::string trusted_cert, FakeWorld& world)
        : real_(real), trusted_cert_(std::move(trusted_cert)), world_(world) {}

    int opens{0};

    ExchangeReadPort* open(const ToolOptions& options, SleepPort& sleeper, ConsolePort& err) override {
        ++opens;
        ExchangeReadPort* port = real_.open(options, sleeper, err);
        if (port == nullptr) return nullptr;
        BinancePrivateRestClientTestHooks::use_loopback(*real_.client(), trusted_cert_);
        port_ = std::make_unique<WorldClockPort>(*port, world_);
        return port_.get();
    }

private:
    RealExchangeProvider& real_;
    std::string trusted_cert_;
    FakeWorld& world_;
    std::unique_ptr<WorldClockPort> port_;
};

}  // namespace

class RealToolTest : public ::testing::Test {
protected:
    TempDirectory dir_;
    std::string audit_path_;
    std::string cp_path_;
    std::string kek_path_;
    std::string key_path_;
    std::string credentials_path_;
    std::unique_ptr<KeyRing> ring_;
    std::unique_ptr<TlsRoutingAcceptor> server_;
    std::int64_t t0_{0};
    std::string coid_;
    FakeWorld world_;
    FakeSleeper sleeper_{world_};
    FakeConsole out_;
    FakeConsole err_;
    std::vector<std::byte> audit_before_;
    std::vector<std::byte> cp_before_;

    void SetUp() override {
        audit_path_ = dir_.file("audit.log");
        cp_path_ = dir_.file("control.log");
        kek_path_ = dir_.file("kek.bin");
        key_path_ = dir_.file("keys.bin");
        credentials_path_ = dir_.file("operator.env");
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x40 + i);
        write_bytes(kek_path_, kek.data(), kek.size());
        make_owner_only(kek_path_);
        ring_ = std::make_unique<KeyRing>(kek);
        std::array<WrappedKeyRecord, 1> records{};
        const std::vector<std::byte> key(40, std::byte{0x11});
        ASSERT_EQ(ring_->add_key(1, key, records[0]), KeyRingAddStatus::Ok);
        const std::vector<std::byte> key_file = encode_audit_key_file(records);
        write_bytes(key_path_, key_file.data(), key_file.size());
        t0_ = real_now_ms();  // the exchange time at which the lock is taken: the client's clock must agree with it
        world_.now_ms = t0_;
        coid_ = "HY-" + std::to_string(t0_ - 601'000) + "-1-0A0A";
    }

    void TearDown() override { server_.reset(); }

    AuditRecord record(AuditEventType ev, OrderState st) const {
        AuditRecord r{};
        r.timestamp_ms = t0_ - 600'000;
        r.event_type = ev;
        r.mode = ExecutionMode::Live;
        r.symbol_id = 0;
        r.set_client_order_id(coid_);
        r.price_ticks = 500'000;
        r.qty_ticks = 100;
        r.resulting_state = st;
        r.side = OrderSide::Sell;
        return r;
    }

    void write_logs() {
        {
            DurableAuditSink sink(audit_path_, *ring_, 1);
            ASSERT_TRUE(sink.is_open());
            const std::int64_t send_at = t0_ - 599'000;
            ASSERT_TRUE(sink.append_durable(record(AuditEventType::OrderIntentCreated, OrderState::Intent), send_at - 1000).acked());
            ASSERT_TRUE(sink.append_durable(record(AuditEventType::OrderSubmitPrepared, OrderState::Submitting), send_at).acked());
            ASSERT_TRUE(sink.append_durable(record(AuditEventType::OrderAmbiguous, OrderState::Ambiguous), send_at + 1000).acked());
        }
        {
            ControlPlaneLogSink sink(cp_path_, *ring_, 1);
            ASSERT_TRUE(sink.is_open());
            SymbolRules rules{};
            std::memcpy(rules.symbol, "BTCUSDT", 7);
            rules.is_trading = true;
            rules.rules_version = 1;
            SymbolRegistrySnapshotPayload snap{};
            snap.timestamp_ms = t0_ - 3'600'000;
            snap.rules_version = 1;
            snap.symbol_count = 1;
            ASSERT_TRUE(sink.append_snapshot(snap, std::span<const SymbolRules>(&rules, 1)).acked());
        }
        audit_before_ = file_bytes(audit_path_);
        cp_before_ = file_bytes(cp_path_);
    }

    void start_exchange(const Routes& routes) {
        server_ = std::make_unique<TlsRoutingAcceptor>(fixture_path("test_leaf_cert_testnet_host.pem"),
                                                       fixture_path("test_leaf_key_testnet_host.pem"), routes.handler());
    }

    PrivateRestConfig client_config() const {
        PrivateRestConfig cfg;
        cfg.port = std::to_string(server_->port());
        return cfg;
    }

    std::vector<std::string_view> args(std::string_view env, std::string_view uid = "123456789") const {
        return {"--audit-log", audit_path_, "--control-log", cp_path_, "--kek-file", kek_path_, "--key-file", key_path_, "--audit-key-id", "1",
                "--control-key-id", "1", "--coid", coid_, "--env", env, "--uid", uid, "--operator", "alice", "--credentials-file", credentials_path_};
    }

    // Runs the tool the way main() does it, with the real provider (its client aimed at the local exchange).
    int run(const std::vector<std::string_view>& a) {
        RealExchangeProvider real(client_config(), TrustStoreCheck::Skip);  // the local exchange's certificate is trusted through the test seam
        LoopbackProvider provider(real, fixture_path("test_leaf_cert_testnet_host.pem"), world_);
        ToolEnvironment env{out_, err_, sleeper_, provider};
        const int code = run_operator_tool(a, env);
        opens_ = provider.opens;
        return code;
    }

    bool logs_untouched() const { return file_bytes(audit_path_) == audit_before_ && file_bytes(cp_path_) == cp_before_; }

    std::string transcript() const {
        std::string s = "--- out ---\n";
        for (const std::string& l : out_.lines) s += l + "\n";
        s += "--- err ---\n";
        for (const std::string& l : err_.lines) s += l + "\n";
        return s;
    }

    int opens_{0};
};

TEST_F(RealToolTest, AnAbsentOrderIsResolvedWithTheOperatorKeyOverRealTls) {
    ASSERT_NO_FATAL_FAILURE(write_logs());
    write_text(credentials_path_, kTestnetCredentials);
    make_owner_only(credentials_path_);
    start_exchange(Routes{});
    EXPECT_EQ(run(args("testnet")), 0) << transcript();
    EXPECT_TRUE(out_.printed("result: Resolved (exit code 0)"));
    EXPECT_TRUE(err_.lines.empty()) << transcript();

    // on the wire: GETs only, to the host of the chosen environment, and every signed one carries the TOOL's key
    const std::vector<Request> requests = server_->requests();
    ASSERT_GE(requests.size(), 8u);
    std::size_t signed_requests = 0;
    for (const Request& r : requests) {
        EXPECT_EQ(r.method, "GET");
        EXPECT_EQ(r.host_header.rfind("testnet.binance.vision", 0), 0u) << r.host_header;
        if (path_of(r) == "/api/v3/time") continue;
        ++signed_requests;
        EXPECT_EQ(r.api_key_header, kOperatorApiKey) << path_of(r);
        EXPECT_NE(r.target.find("signature="), std::string::npos) << path_of(r);
    }
    EXPECT_GE(signed_requests, 4u) << "the account, the point query, allOrders and openOrders";

    // the evidence is on disk -- one file, named by the order and the exchange time of the read (the port's own gaps between requests move
    // that time on a little past lock + 270 s) -- without a key or a secret in it, with the uid the exchange reported
    std::vector<std::string> evidence_files;
    for (const auto& entry : fs::directory_iterator(audit_path_ + ".operator-evidence")) evidence_files.push_back(entry.path().filename().string());
    ASSERT_EQ(evidence_files.size(), 1u);
    const std::string prefix = coid_ + "-";
    ASSERT_EQ(evidence_files[0].rfind(prefix, 0), 0u) << evidence_files[0];
    ASSERT_GT(evidence_files[0].size(), prefix.size() + 5);
    ASSERT_EQ(evidence_files[0].substr(evidence_files[0].size() - 5), ".json");
    const std::string stem = evidence_files[0].substr(0, evidence_files[0].size() - 5);
    const std::int64_t read_ms = std::stoll(stem.substr(prefix.size()));
    EXPECT_GE(read_ms, t0_ + kDefaultEvidenceWaitMs) << "no evidence before the fence";
    const std::string evidence = file_text(audit_path_ + ".operator-evidence/" + evidence_files[0]);
    ASSERT_FALSE(evidence.empty());
    EXPECT_NE(evidence.find("\"account_uid_reported\":123456789"), std::string::npos);
    EXPECT_EQ(evidence.find(kOperatorApiKey), std::string::npos);
    EXPECT_EQ(evidence.find(kOperatorSecret), std::string::npos);
    EXPECT_EQ(evidence.find("signature"), std::string::npos);
    EXPECT_TRUE(out_.printed("evidence file: " + stem + "  sha256: " + sha256_hex(evidence)));
    for (const std::string& line : out_.lines) {
        EXPECT_EQ(line.find(kOperatorApiKey), std::string::npos);
        EXPECT_EQ(line.find(kOperatorSecret), std::string::npos);
    }

    // and the frame: the next trading start finds the order closed
    DurableAuditSink after(audit_path_, *ring_, 1);
    ASSERT_TRUE(after.is_open());
    EXPECT_FALSE(after.fenced());
    ASSERT_EQ(after.recovered_operator_resolved().size(), 1u);
    EXPECT_EQ(after.recovered_operator_resolved()[0].client_order_id.view(), coid_);
    EXPECT_TRUE(after.recovered_checkpoints().empty());
}

TEST_F(RealToolTest, TheTradingKeyInTheCredentialsFileEndsTheRunBeforeAnythingIsSentOrAnyLogIsTouched) {
    ASSERT_NO_FATAL_FAILURE(write_logs());
    write_text(credentials_path_, kTradingCredentials);
    make_owner_only(credentials_path_);
    start_exchange(Routes{});
    EXPECT_EQ(run(args("testnet")), exit_code_of(ToolExit::Exchange));
    EXPECT_EQ(opens_, 1);
    EXPECT_TRUE(err_.printed("error: --credentials-file: it holds a name this tool does not read")) << transcript();
    EXPECT_EQ(server_->request_count(), 0u) << "nothing was sent";
    EXPECT_TRUE(logs_untouched());
    EXPECT_FALSE(fs::exists(audit_path_ + ".operator-evidence"));
}

TEST_F(RealToolTest, ProductionIsAnotherHostAndTheTestnetExchangeIsNotItsExchange) {
    ASSERT_NO_FATAL_FAILURE(write_logs());
    write_text(credentials_path_, kLiveCredentials);
    make_owner_only(credentials_path_);
    start_exchange(Routes{});  // its certificate is for testnet.binance.vision
    EXPECT_EQ(run(args("production")), exit_code_of(ToolExit::Session)) << transcript();
    EXPECT_TRUE(err_.printed("the session could not start: ServerTimeUnavailable")) << transcript();
    EXPECT_EQ(server_->request_count(), 0u) << "the client verified the host name api.binance.com against the certificate of the testnet host, and stopped";
    EXPECT_TRUE(logs_untouched());
}

TEST_F(RealToolTest, AKeyOfAnotherAccountIsRefusedBeforeTheWaitAndNothingIsWritten) {
    ASSERT_NO_FATAL_FAILURE(write_logs());
    write_text(credentials_path_, kTestnetCredentials);
    make_owner_only(credentials_path_);
    start_exchange(Routes{});
    EXPECT_EQ(run(args("testnet", "999")), exit_code_of(ResolveStatus::AccountUidMismatch)) << transcript();
    EXPECT_TRUE(sleeper_.sleeps.empty() || sleeper_.total() < kDefaultEvidenceWaitMs) << "refused before the fence was waited out";
    EXPECT_TRUE(logs_untouched());
    for (const Request& r : server_->requests()) EXPECT_NE(path_of(r), "/api/v3/order") << "no evidence request for somebody else's account";
}

TEST_F(RealToolTest, AnExchangeThatSaysStopEndsTheRunAndNothingMoreIsSent) {
    ASSERT_NO_FATAL_FAILURE(write_logs());
    write_text(credentials_path_, kTestnetCredentials);
    make_owner_only(credentials_path_);
    Routes routes;
    routes.order = Reply::answer(429, R"({"code":-1003,"msg":"Too many requests."})");
    start_exchange(routes);
    // the 429 is handed back as it is and the port latches: the next read (allOrders) is never sent and fails as not completed, so the
    // procedure stops with EvidenceReadFailed -- it never gets as far as judging the 429
    EXPECT_EQ(run(args("testnet")), exit_code_of(ResolveStatus::EvidenceReadFailed)) << transcript();
    EXPECT_TRUE(logs_untouched());
    std::size_t evidence_requests = 0;
    for (const Request& r : server_->requests()) {
        const std::string p = path_of(r);
        if (p == "/api/v3/allOrders" || p == "/api/v3/openOrders") ++evidence_requests;
    }
    EXPECT_EQ(evidence_requests, 0u) << "after a 429 the port sends nothing more";
}
