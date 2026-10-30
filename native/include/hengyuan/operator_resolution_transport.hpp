// SPDX-License-Identifier: proprietary
// operator_resolution_transport.hpp -- the offline operator tool's real exchange port and real sleep (D5 slice R3b-1).
//
// Governance: offline tool only. It allocates and it pulls in Boost/Beast/OpenSSL through the authenticated REST client; no production
// header includes it and nothing in the trading process may (test_operator_resolution_guard.cpp pins that, and pins that the raw reads of
// BinancePrivateRestClient -- fetch_order_raw, fetch_all_orders_raw, fetch_open_orders_raw, fetch_account_raw, read_server_time -- are named
// by nothing but the client itself and this header). docs/OPERATOR_RESOLUTION_DESIGN.md 6 is the procedure; operator_resolution_ports.hpp is
// the contract this file implements: GETs only, the five of them, signed with a read-only key that never leaves the client, and the
// exchange's answer handed back exactly as it came.
//
// What the port adds to the client, and why each part is here:
//   * The status and the body of ANY answer. The client's trading-path reads turn every status but 200 into an error and drop the body;
//     the tool's one piece of evidence of absence is the body of a 400 ({"code":-2013}), so it reads through the client's raw methods
//     (binance_private_rest.hpp, D5 R3b-1). A request that did not complete is transport_ok == false -- never an empty 200.
//   * Pacing. The tool is a short burst of requests (a point query, up to 14 allOrders windows, openOrders, the account), and
//     PrivateRestEndpoint has no slot for allOrders or openOrders, so the client charges them explicit weights; on top of that the port
//     leaves a fixed gap between two requests (kDefaultRequestSpacing) so the burst never comes near the exchange's per-minute weight. The
//     gap is slept through the injected SleepPort, which is what lets the tests run it on a fake clock.
//   * A rate-limit latch. An answer with status 429 (over the limit), 418 (banned for going on after that) or 403 (the exchange's firewall)
//     is handed back as it is -- the flow classifies it as "says nothing" and refuses -- and from then on this port sends NOTHING more:
//     every later request fails as not-completed. The next run starts a new session and a new fence anyway; hammering an exchange that
//     just said stop is how a 429 becomes a 418.
//   * One clock refresh. A signed request needs a fresh clock-offset snapshot (the client refuses to sign without one) and a snapshot
//     ages out after kOffsetTtlMs (five minutes). Every serverTime reading of the flow publishes a new one, so this matters only if the
//     operator sat on a prompt for long; when the client says ClockNotFresh the port takes one new reading (GET /api/v3/time, which is
//     what publishes the snapshot) and repeats the request ONCE. Nothing was signed or sent before that, so nothing is repeated on the
//     exchange.
//
// summarize_account() turns the account answer into the two things the flow uses -- the uid it cross-checks against the one the operator
// stated, and ONE line of printable ASCII for the operator's eyes -- without ever passing a piece of the response through.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <simdjson.h>

#include <hengyuan/binance_private_rest.hpp>
#include <hengyuan/operator_resolution_confirmation.hpp>
#include <hengyuan/operator_resolution_ports.hpp>
#include <hengyuan/operator_resolution_response.hpp>

namespace hy::operator_resolution {

// The least time between one request and the next one this port sends (see "Pacing" above). Whole seconds of weight per minute stay
// far below the exchange's limit at this rate even if every request were an openOrders.
inline constexpr std::chrono::milliseconds kDefaultRequestSpacing{500};

// Statuses after which the port stops sending (see "A rate-limit latch" above).
constexpr bool is_back_off_status(std::uint16_t http_status) noexcept {
    return http_status == 429 || http_status == 418 || http_status == 403;
}

// The real wait. (Local time is only ever used to decide how long to sleep; the fence itself is counted on the exchange clock.)
class RealSleeper final : public SleepPort {
public:
    void sleep_for(std::chrono::milliseconds duration) override {
        if (duration.count() > 0) std::this_thread::sleep_for(duration);
    }
};

// --- the account answer ---------------------------------------------------------------------------------------------------------------

namespace detail {

// A member that must be there exactly once, as an unsigned integer that fits 64 bits (a float, a negative number, a string, a duplicate
// key: Invalid).
inline FieldRead read_unique_uint64(const simdjson::dom::object& obj, std::string_view key, std::uint64_t& out) noexcept {
    FieldRead result = FieldRead::Missing;
    for (const auto field : obj) {
        if (field.key != key) continue;
        if (result != FieldRead::Missing) return FieldRead::Invalid;  // a duplicate key: which one is meant?
        std::uint64_t v = 0;
        if (field.value.get(v) != simdjson::SUCCESS) return FieldRead::Invalid;
        out = v;
        result = FieldRead::Ok;
    }
    return result;
}

// The one member named `key`, whatever it holds.
inline FieldRead find_unique_member(const simdjson::dom::object& obj, std::string_view key, simdjson::dom::element& out) noexcept {
    FieldRead result = FieldRead::Missing;
    for (const auto field : obj) {
        if (field.key != key) continue;
        if (result != FieldRead::Missing) return FieldRead::Invalid;
        out = field.value;
        result = FieldRead::Ok;
    }
    return result;
}

inline constexpr std::size_t kSummaryTypeChars = 16;
inline constexpr std::size_t kSummaryNameChars = 16;     // an asset or a permission
inline constexpr std::size_t kSummaryDecimalChars = 24;  // a free or locked amount ("9999999999.99999999" is 19)
inline constexpr std::size_t kSummaryMaxItems = 8;       // permissions, and balances, shown
inline constexpr std::size_t kSummaryMaxChars = 640;     // the whole line

// A name as the summary shows it: letters, digits, '.', '_' and '-' are themselves, every other byte is '?', and it is cut at `max`.
inline void append_summary_token(std::string& out, std::string_view s, std::size_t max) {
    const std::size_t n = std::min(s.size(), max);
    for (std::size_t i = 0; i < n; ++i) {
        const char c = s[i];
        out.push_back((is_ascii_alnum(c) || c == '.' || c == '_' || c == '-') ? c : '?');
    }
}

// An amount as the summary shows it: digits with at most one point inside them, as the exchange writes them; anything else is '?'.
inline void append_summary_amount(std::string& out, std::string_view s) {
    bool ok = !s.empty() && s.size() <= kSummaryDecimalChars && s.front() != '.' && s.back() != '.';
    std::size_t points = 0;
    for (const char c : s) {
        if (c == '.') {
            ++points;
        } else if (c < '0' || c > '9') {
            ok = false;
        }
    }
    if (points > 1) ok = false;
    if (ok) {
        out.append(s);
    } else {
        out.push_back('?');
    }
}

}  // namespace detail

// What a 200 answer of GET /api/v3/account says about the account: its uid (exactly one member "uid", an integer, positive) and a short
// summary line composed from parsed fields only -- the account type, the permissions and the first balances with their free and locked
// amounts, each cut and filtered (detail::append_summary_token / append_summary_amount). Anything else -- not JSON, not an object, no
// single positive uid -- leaves the uid unset, and a body that is not an object at all leaves the summary empty too. It never throws.
// The caller checks the status: a body that comes with a status other than 200 is not an account.
inline AccountView summarize_account(std::string_view body) {
    AccountView view;
    try {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        if (!detail::parse_document(parser, body, root)) return AccountView{};
        simdjson::dom::object obj;
        if (root.get(obj) != simdjson::SUCCESS) return AccountView{};

        std::uint64_t uid = 0;
        if (detail::read_unique_uint64(obj, "uid", uid) == detail::FieldRead::Ok && uid != 0) view.uid = uid;

        std::string line = "account type ";
        std::string_view type;
        if (detail::read_unique_string(obj, "accountType", type) == detail::FieldRead::Ok) {
            detail::append_summary_token(line, type, detail::kSummaryTypeChars);
        } else {
            line.push_back('?');
        }

        line += "; permissions ";
        simdjson::dom::element member;
        simdjson::dom::array permissions;
        if (detail::find_unique_member(obj, "permissions", member) == detail::FieldRead::Ok && member.get(permissions) == simdjson::SUCCESS) {
            std::size_t shown = 0;
            std::size_t total = 0;
            for (const simdjson::dom::element item : permissions) {
                ++total;
                if (shown == detail::kSummaryMaxItems) continue;
                if (shown != 0) line.push_back(',');
                std::string_view name;
                if (item.get(name) == simdjson::SUCCESS) {
                    detail::append_summary_token(line, name, detail::kSummaryNameChars);
                } else {
                    line.push_back('?');
                }
                ++shown;
            }
            if (total == 0) line += "none";
            if (total > shown) line += " and " + std::to_string(total - shown) + " more";
        } else {
            line.push_back('?');
        }

        line += "; balances (free/locked): ";
        simdjson::dom::array balances;
        if (detail::find_unique_member(obj, "balances", member) == detail::FieldRead::Ok && member.get(balances) == simdjson::SUCCESS) {
            std::size_t shown = 0;
            std::size_t total = 0;
            for (const simdjson::dom::element item : balances) {
                ++total;
                if (shown == detail::kSummaryMaxItems) continue;
                if (shown != 0) line += ", ";
                ++shown;
                simdjson::dom::object entry;
                std::string_view asset;
                std::string_view free_amount;
                std::string_view locked_amount;
                if (item.get(entry) != simdjson::SUCCESS || detail::read_unique_string(entry, "asset", asset) != detail::FieldRead::Ok ||
                    detail::read_unique_string(entry, "free", free_amount) != detail::FieldRead::Ok ||
                    detail::read_unique_string(entry, "locked", locked_amount) != detail::FieldRead::Ok) {
                    line.push_back('?');
                    continue;
                }
                detail::append_summary_token(line, asset, detail::kSummaryNameChars);
                line.push_back(' ');
                detail::append_summary_amount(line, free_amount);
                line.push_back('/');
                detail::append_summary_amount(line, locked_amount);
            }
            if (total == 0) line += "none";
            if (total > shown) line += " and " + std::to_string(total - shown) + " more";
        } else {
            line.push_back('?');
        }

        if (line.size() > detail::kSummaryMaxChars) line.resize(detail::kSummaryMaxChars);
        view.summary = std::move(line);
        return view;
    } catch (...) {
        return AccountView{};  // fail closed: no uid, no summary
    }
}

// --- the exchange port -------------------------------------------------------------------------------------------------------------------

// The five reads of the flow over the authenticated REST client. One instance per tool run, on the thread that owns the client (the client's
// credentials are bound to the thread that made them). The client must have been built for the environment the operator named and with the
// read-only key; this class neither checks that nor can it: it holds no key.
class BinanceExchangeReadPort final : public ExchangeReadPort {
public:
    BinanceExchangeReadPort(BinancePrivateRestClient& client, SleepPort& sleeper,
                            std::chrono::milliseconds spacing = kDefaultRequestSpacing) noexcept
        : client_(client), sleeper_(sleeper), spacing_(spacing) {}

    BinanceExchangeReadPort(const BinanceExchangeReadPort&) = delete;
    BinanceExchangeReadPort& operator=(const BinanceExchangeReadPort&) = delete;

    // GET /api/v3/time. This also publishes the client's clock-offset snapshot, which every signed read below needs.
    std::optional<ExchangeMs> server_time() override {
        if (halted_) return std::nullopt;
        std::int64_t ms = 0;
        if (!read_time(ms)) return std::nullopt;
        return ExchangeMs{ms};
    }

    HttpRead query_order(std::string_view symbol, std::string_view client_order_id) override {
        return signed_read([&](RawHttpAnswer& out) { return client_.fetch_order_raw(symbol, client_order_id, out); });
    }

    HttpRead all_orders(std::string_view symbol, ExchangeMs start, ExchangeMs end, std::size_t limit) override {
        return signed_read([&](RawHttpAnswer& out) { return client_.fetch_all_orders_raw(symbol, start.count(), end.count(), limit, out); });
    }

    HttpRead open_orders() override {
        return signed_read([&](RawHttpAnswer& out) { return client_.fetch_open_orders_raw(out); });
    }

    AccountView account() override {
        const HttpRead r = signed_read([&](RawHttpAnswer& out) { return client_.fetch_account_raw(out); });
        if (!r.transport_ok || r.http_status != 200) return AccountView{};
        return summarize_account(r.body);
    }

    // Has the exchange told this port to stop (429, 418, 403)? From then on every request fails as not completed, without being sent.
    bool halted() const noexcept { return halted_; }

private:
    // The gap between two requests: slept before a request when one went out since the last gap, never before the first.
    void wait_gap() {
        if (!gap_pending_) return;
        gap_pending_ = false;
        sleeper_.sleep_for(spacing_);
    }

    // Did a request with this outcome reach the network? These four are refusals before anything was resolved or connected.
    static constexpr bool went_out(PrivateRestError e) noexcept {
        return e != PrivateRestError::InvalidConfig && e != PrivateRestError::SigningFailed && e != PrivateRestError::ClockNotFresh &&
               e != PrivateRestError::RateLimitExhausted;
    }

    bool read_time(std::int64_t& ms) {
        wait_gap();
        const PrivateRestError e = client_.read_server_time(ms);
        if (went_out(e)) gap_pending_ = true;
        return e == PrivateRestError::None;
    }

    template <class Call>
    HttpRead signed_read(Call&& call) {
        if (halted_) return HttpRead{};
        for (int attempt = 0; attempt < 2; ++attempt) {
            wait_gap();
            RawHttpAnswer answer;
            const PrivateRestError e = call(answer);
            if (went_out(e)) gap_pending_ = true;
            if (e == PrivateRestError::None) {
                if (is_back_off_status(answer.http_status)) halted_ = true;
                HttpRead r;
                r.transport_ok = true;
                r.http_status = answer.http_status;
                r.body = std::move(answer.body);
                return r;
            }
            if (e != PrivateRestError::ClockNotFresh || attempt != 0) return HttpRead{};
            // The snapshot aged out between two requests and nothing was signed or sent: take a new one, then repeat the request once.
            std::int64_t ignored = 0;
            if (!read_time(ignored)) return HttpRead{};
        }
        return HttpRead{};
    }

    BinancePrivateRestClient& client_;
    SleepPort& sleeper_;
    std::chrono::milliseconds spacing_;
    bool gap_pending_{false};
    bool halted_{false};
};

}  // namespace hy::operator_resolution
