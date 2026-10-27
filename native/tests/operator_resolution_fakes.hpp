// SPDX-License-Identifier: proprietary
// operator_resolution_fakes.hpp -- a loopback exchange, a clock, a console and an evidence store for the operator tool's tests
// (D5 slice R3a).
//
// The fake exchange is not a canned-response table: it holds ORDERS and answers the way the real endpoints do, including the
// two ways a read can be wrong that the fence exists for -- an order exists but a read does not show it YET (every endpoint has
// its own "visible from" exchange time), and an order is on the books but absent from a page. It reads the same fake clock that
// the sleeper advances, so a tool that reads too early gets exactly the answer a real tool would get too early.
//
// Every call is recorded with the exchange time at which it was made; the tests assert that no evidence read happened before
// the fence.

#pragma once

#include <hengyuan/operator_resolution_ports.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hy::operator_resolution::testing {

inline constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::max();

// One exchange clock for the whole test. A sleep of d local milliseconds advances it by d * speed (1.0: a faithful clock).
struct FakeWorld {
    std::int64_t now_ms{0};
    double speed{1.0};
};

class FakeSleeper final : public SleepPort {
public:
    explicit FakeSleeper(FakeWorld& world) : world_(world) {}
    void sleep_for(std::chrono::milliseconds duration) override {
        sleeps.push_back(duration.count());
        world_.now_ms += static_cast<std::int64_t>(static_cast<double>(duration.count()) * world_.speed);
    }
    std::vector<std::int64_t> sleeps;
    std::int64_t total() const {
        std::int64_t t = 0;
        for (const std::int64_t s : sleeps) t += s;
        return t;
    }

private:
    FakeWorld& world_;
};

// An order that exists on the fake exchange, and from when each endpoint shows it.
struct FakeExchangeOrder {
    std::string client_order_id;
    std::string symbol;
    std::int64_t created_ms{0};
    std::int64_t point_visible_ms{0};  // GET order shows it from this exchange time on (kNever: never)
    std::int64_t list_visible_ms{0};   // GET allOrders
    std::int64_t open_visible_ms{kNever};  // GET openOrders (kNever: not an open order)
};

enum class FakeTimeMode : std::uint8_t {
    Normal,    // the world clock
    Stuck,     // the first reading, forever
    Backwards, // each reading earlier than the last
    Garbage,   // 0: not a time
};

class FakeExchange final : public ExchangeReadPort {
public:
    struct Call {
        std::string kind;  // "time", "order", "allOrders", "openOrders", "account"
        std::int64_t at_ms{0};
        std::string detail;
    };

    explicit FakeExchange(FakeWorld& world) : world_(world) {}

    std::optional<ExchangeMs> server_time() override {
        ++time_reads_;
        calls.push_back({"time", world_.now_ms, ""});
        if (fail_time_from_read >= 0 && time_reads_ > fail_time_from_read) return std::nullopt;
        if (garbage_time_from_read >= 0 && time_reads_ > garbage_time_from_read) return ExchangeMs{0};
        switch (time_mode) {
            case FakeTimeMode::Normal: return ExchangeMs{world_.now_ms};
            case FakeTimeMode::Stuck:
                if (!stuck_value) stuck_value = world_.now_ms;
                return ExchangeMs{*stuck_value};
            case FakeTimeMode::Backwards: return ExchangeMs{world_.now_ms - static_cast<std::int64_t>(time_reads_) * 1000};
            case FakeTimeMode::Garbage: return ExchangeMs{0};
        }
        return std::nullopt;
    }

    HttpRead query_order(std::string_view symbol, std::string_view client_order_id) override {
        calls.push_back({"order", world_.now_ms, std::string(client_order_id)});
        if (should_fail("order")) return {};
        for (const FakeExchangeOrder& o : orders) {
            if (o.symbol == symbol && o.client_order_id == client_order_id && world_.now_ms >= o.point_visible_ms) {
                return {true, 200, "{\"symbol\":\"" + o.symbol + "\",\"orderId\":4242,\"clientOrderId\":\"" + o.client_order_id +
                                       "\",\"status\":\"NEW\"}"};
            }
        }
        return {true, unknown_status, unknown_body};
    }

    HttpRead all_orders(std::string_view symbol, ExchangeMs start, ExchangeMs end, std::size_t limit) override {
        calls.push_back({"allOrders", world_.now_ms, std::to_string(start.count()) + ".." + std::to_string(end.count())});
        windows.emplace_back(start.count(), end.count());
        if (should_fail("allOrders")) return {};
        std::string body = "[";
        std::size_t n = 0;
        const auto add = [&](const std::string& coid, const std::string& sym) {
            if (n != 0) body += ',';
            body += "{\"symbol\":\"" + (list_symbol_override.empty() ? sym : list_symbol_override) + "\",\"orderId\":" +
                    std::to_string(1000 + n) + ",\"clientOrderId\":\"" + coid + "\"}";
            ++n;
        };
        for (const FakeExchangeOrder& o : orders) {
            if (n >= limit) break;
            if (o.symbol == symbol && o.created_ms >= start.count() && o.created_ms <= end.count() && world_.now_ms >= o.list_visible_ms) {
                add(o.client_order_id, o.symbol);
            }
        }
        for (std::size_t i = 0; i < list_filler && n < limit; ++i) add("FILL-" + std::to_string(i), std::string(symbol));
        body += "]";
        return {true, list_status, body};
    }

    HttpRead open_orders() override {
        calls.push_back({"openOrders", world_.now_ms, ""});
        if (should_fail("openOrders")) return {};
        std::string body = "[";
        std::size_t n = 0;
        for (const FakeExchangeOrder& o : orders) {
            if (o.open_visible_ms == kNever || world_.now_ms < o.open_visible_ms) continue;
            if (n++ != 0) body += ',';
            body += "{\"symbol\":\"" + o.symbol + "\",\"orderId\":" + std::to_string(2000 + n) + ",\"clientOrderId\":\"" +
                    o.client_order_id + "\"}";
        }
        body += "]";
        return {true, 200, body};
    }

    std::string account_summary() override {
        calls.push_back({"account", world_.now_ms, ""});
        return account;
    }

    // --- what the tests set ---
    std::vector<FakeExchangeOrder> orders;
    FakeTimeMode time_mode{FakeTimeMode::Normal};
    int fail_time_from_read{-1};  // server_time() fails from the (n+1)th reading on (-1: never)
    int garbage_time_from_read{-1};  // server_time() answers 0 (not a time) from the (n+1)th reading on (-1: never)
    std::string fail_kind;        // "order", "allOrders" or "openOrders": the nth call of that kind is a transport failure
    int fail_nth{1};
    std::uint16_t unknown_status{400};
    std::string unknown_body{"{\"code\":-2013,\"msg\":\"Order does not exist.\"}"};
    std::uint16_t list_status{200};
    std::size_t list_filler{0};         // that many unrelated orders on every allOrders page
    std::string list_symbol_override;   // every item of an allOrders page claims this symbol
    std::string account{"USDT free 1000.0"};

    // --- what the tests read ---
    std::vector<Call> calls;
    std::vector<std::pair<std::int64_t, std::int64_t>> windows;

    std::size_t count(std::string_view kind) const {
        std::size_t n = 0;
        for (const Call& c : calls) {
            if (c.kind == kind) ++n;
        }
        return n;
    }
    // The earliest exchange time at which an EVIDENCE request (anything but the clock) was made; kNever if none was.
    std::int64_t first_evidence_call_ms() const {
        for (const Call& c : calls) {
            if (c.kind != "time" && c.kind != "account") return c.at_ms;
        }
        return kNever;
    }

private:
    bool should_fail(std::string_view kind) {
        if (fail_kind != kind) return false;
        return ++fail_seen_ == fail_nth;
    }

    FakeWorld& world_;
    int time_reads_{0};
    int fail_seen_{0};
    std::optional<std::int64_t> stuck_value;
};

class FakeConsole final : public ConsolePort {
public:
    enum class Mode : std::uint8_t {
        TypeThePhrase,  // types exactly the phrase the tool printed
        TypeNothing,    // an empty line
        Timeout,
        Eof,
        Error,
        TooLong,
        TypeVariant,    // types the phrase with `variant` applied (see below)
    };

    void write_line(std::string_view line) override { lines.emplace_back(line); }

    OperatorLineResult read_line(std::chrono::milliseconds timeout) override {
        ++reads;
        last_timeout = timeout;
        if (world != nullptr) world->now_ms += typing_ms;  // the operator takes his time: the exchange clock moves on
        if (on_read) on_read();                             // and whatever else happens while he types
        switch (mode) {
            case Mode::Timeout: return OperatorLineResult{OperatorInputStatus::Timeout};
            case Mode::Eof: return OperatorLineResult{OperatorInputStatus::EndOfInput};
            case Mode::Error: return OperatorLineResult{OperatorInputStatus::Error};
            case Mode::TooLong: return OperatorLineResult{OperatorInputStatus::TooLong};
            case Mode::TypeNothing: return line_of("");
            case Mode::TypeThePhrase: return line_of(printed_phrase());
            case Mode::TypeVariant: return line_of(variant(printed_phrase()));
        }
        return OperatorLineResult{};
    }

    // The phrase the tool asked for LAST: the line after the latest prompt.
    std::string printed_phrase() const {
        for (std::size_t i = lines.size(); i-- > 1;) {
            if (lines[i - 1].rfind("To resolve this order as ABSENT type exactly", 0) == 0) return lines[i];
        }
        return {};
    }

    bool printed(std::string_view fragment) const {
        for (const std::string& l : lines) {
            if (l.find(fragment) != std::string::npos) return true;
        }
        return false;
    }

    // --- what the tests set ---
    Mode mode{Mode::TypeThePhrase};
    std::string (*variant)(const std::string&){nullptr};
    FakeWorld* world{nullptr};   // if set, every read_line() moves this exchange clock on by typing_ms
    std::int64_t typing_ms{0};
    std::function<void()> on_read;  // if set, run at the start of every read_line() (something happens behind the flow's back)

    // --- what the tests read ---
    std::vector<std::string> lines;
    int reads{0};
    std::chrono::milliseconds last_timeout{0};
    std::string last_typed;

private:
    OperatorLineResult line_of(const std::string& s) {
        last_typed = s;
        OperatorLineResult r{};
        r.status = OperatorInputStatus::Line;
        r.size = s.size() < r.bytes.size() ? s.size() : r.bytes.size();
        for (std::size_t i = 0; i < r.size; ++i) r.bytes[i] = s[i];
        return r;
    }
};

class FakeStore final : public EvidenceStorePort {
public:
    bool store(std::string_view file_stem, std::string_view contents) override {
        attempts.emplace_back(file_stem);
        if (fail) return false;
        stored.emplace_back(std::string(file_stem), std::string(contents));
        return true;
    }
    bool fail{false};
    std::vector<std::string> attempts;
    std::vector<std::pair<std::string, std::string>> stored;
};

}  // namespace hy::operator_resolution::testing
