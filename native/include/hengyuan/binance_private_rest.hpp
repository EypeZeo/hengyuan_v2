// SPDX-License-Identifier: proprietary
// binance_private_rest.hpp — Binance L4 read-only private REST client (§3/§4 of
// docs/BINANCE_PRIVATE_REST_L4_SPEC.md rev 72).
//
// Two GET endpoints, built on the already-shipped L4 signing foundation
// (binance_signer.hpp/binance_environment.hpp/binance_query_signing.hpp/
// binance_clock_sync.hpp — all landed separately, this file is their first real
// network consumer):
//
//   - GET /api/v3/time (§3, public, unauthenticated): closes the loop
//     binance_clock_sync.hpp's own header comment describes as "out of scope" —
//     that file has offline-only ClockOffsetSnapshot/compute_clock_offset()/
//     ClockOffsetPublisher machinery that nothing has ever actually driven with a
//     real round trip until this file's sync_clock().
//   - GET /api/v3/account (§4, signed, USER_DATA): backs account_truth.hpp's
//     AccountSnapshot. Fails closed on every one of §4.1-4.4's schema/precision/
//     capacity conditions — the fetch either produces a complete, correctly-scaled
//     snapshot or it changes nothing.
//   - GET /api/v3/exchangeInfo (§5, public, unauthenticated): fetch_exchange_info()
//     below. Symbol-registry durability/hot-swap (SymbolRegistry, §5.3/§5.3.1) is
//     symbol_registry.hpp's job, not this file's — this function only fetches and
//     parses the HTTP response into a caller-owned ParsedExchangeInfo, the same
//     "write into what the caller passed in, own nothing" shape as fetch_account().
//
// Explicit non-goals for this slice (see docs/SPEC_INVARIANTS.md's L4 entries and
// the spec's own §6/§7/§9/§10 sections): GET /api/v3/order (§6, reconciliation),
// rate-limit header accounting (§7), the single-owner actor/scheduler thread model
// (§9), and a concrete durable_control_plane.hpp-backed persistence implementation
// (§10, whose ABI surface already exists per docs/SPEC_INVARIANTS.md but has no
// durability requirement from §3/§4 specifically — §5.3.1's registry snapshot is
// the first place the spec actually mandates a durable ACK-before-publish write,
// implemented in symbol_registry.hpp, not here). This client also does not itself
// enforce the single-owner-thread discipline BoundHmacCredentials/§9 require — same
// contract BoundHmacCredentials already has (construct + every sign()/copy_api_key()
// call from one thread), the caller's responsibility, not this class's.
//
// Structurally mirrors binance_rest_snapshot.hpp's coroutine skeleton (resolve ->
// connect -> TLS handshake -> write -> read, each with its own bounded deadline) —
// per spec §8, reusing that PLUMBING is correct, but the SECURITY posture is
// independently specified here: header_limit()+body_limit() both configured before
// reading (not just the latter), no Accept-Encoding is ever sent (never negotiates
// compression), the connection is always force-closed after use (single-shot calls,
// like the existing depth-snapshot fetcher), and every coroutine catches `...`
// (broader than binance_rest_snapshot.hpp's own `boost::system::system_error`-only
// catch — spec §8 explicitly requires converting ANY exception, not just Boost's).
//
// Governance: L4 (real network I/O + real HMAC signing via already-bound
// credentials; no order submission — every request here is a GET).

#pragma once

#include <hengyuan/account_truth.hpp>
#include <hengyuan/binance_clock_sync.hpp>
#include <hengyuan/binance_decimal.hpp>
#include <hengyuan/binance_environment.hpp>
#include <hengyuan/binance_query_signing.hpp>
#include <hengyuan/binance_tls.hpp>
#include <hengyuan/order_lifecycle.hpp>
#include <hengyuan/order_tracker.hpp>

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#endif
#include <simdjson.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace hy {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// host is NOT part of this config — it comes from the bound EnvironmentBinding, so there is
// exactly one source of truth for "which environment am I talking to" (the same reasoning
// EnvironmentBinding's own header comment gives for hardcoding host/credential-env-var-name
// per factory: no parameter a caller could pass to accidentally cross environments).
struct PrivateRestConfig {
    std::string port = "443";
    // Binance's own recvWindow business parameter (§2.1) — how long, from `timestamp`, the
    // server accepts this request as still fresh. Not related to this client's own network
    // timeouts below (which bound how long WE wait for the network).
    std::int64_t recv_window_ms = 5000;

    // Test-only escape hatches, matching RestSnapshotConfig's own (binance_rest_snapshot.hpp)
    // — empty by default, production callers never set either.
    std::string extra_trusted_ca_pem_path;
    std::string connect_host_override;
};

enum class PrivateRestError : std::uint8_t {
    None = 0,
    InvalidConfig = 1,
    Resolve = 2,
    Connect = 3,
    TlsHandshake = 4,
    Write = 5,
    Read = 6,
    HttpStatus = 7,
    JsonParse = 8,
    MalformedResponse = 9,   // §4.1 schema failure or §4.2 conversion failure
    CapacityExceeded = 10,   // §4.3: more nonzero-balance assets than kMaxAssets
    ClockNotFresh = 11,      // §2.2: no fresh clock-offset snapshot -- fail closed, never signs
                              // with uncalibrated local system time
    SigningFailed = 12,      // build_canonical_query()/build_signed_query()/copy_api_key()
};

// §4.2's parse_balance_decimal_to_ticks() moved to binance_decimal.hpp (L4 §5 / PR 5b): the
// exchangeInfo parser below (fetch_exchange_info()) needs the same function for the
// MIN_NOTIONAL/NOTIONAL filter, but symbol_registry.hpp — which also needs it — must not pull in
// this file's Boost/Beast/OpenSSL network dependency just to reuse a pure decimal parser. See
// binance_decimal.hpp's header comment for the full three-function split. Signature and behavior
// are unchanged; every call site in this file is unaffected.

// §3: `{"serverTime": <ms>}`. A negative value is rejected outright (Binance never returns
// one; a negative "server time" would corrupt every downstream RTT/offset computation that
// assumes a monotonically-sane epoch millisecond count).
inline bool parse_server_time_response(std::string_view body,
                                        std::int64_t& out_server_time_ms) {
    auto padded = simdjson::padded_string(body);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) return false;

    std::int64_t v = 0;
    if (doc["serverTime"].get_int64().get(v) != simdjson::SUCCESS) return false;
    if (v < 0) return false;
    out_server_time_ms = v;
    return true;
}

// §4.1-§4.3: schema validation + lossless conversion + capacity, collapsed to the single
// §4.4 outcome contract -- `out` is left completely UNCHANGED on any failure path (this
// function builds into a local AccountSnapshot and only assigns to `out` once, at the very
// end, on total success). `timestamp_ms` is NOT set here -- the caller (fetch_account())
// stamps it with the same server-calibrated fresh_ts_ms already computed for this request's
// signature, so the snapshot's timestamp and its own HTTP request's timestamp are the same
// value, not two independent clock reads.
inline PrivateRestError parse_account_response(std::string_view body, AccountSnapshot& out) {
    auto padded = simdjson::padded_string(body);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) return PrivateRestError::JsonParse;

    bool can_trade = false;
    if (doc["canTrade"].get_bool().get(can_trade) != simdjson::SUCCESS) {
        return PrivateRestError::MalformedResponse;
    }

    simdjson::ondemand::array balances;
    if (doc["balances"].get_array().get(balances) != simdjson::SUCCESS) {
        return PrivateRestError::MalformedResponse;
    }

    AccountSnapshot built{};
    built.can_trade = can_trade;

    for (auto entry_result : balances) {
        simdjson::ondemand::object entry;
        if (entry_result.get_object().get(entry) != simdjson::SUCCESS) {
            return PrivateRestError::MalformedResponse;
        }

        std::string_view asset_sv, free_sv, locked_sv;
        if (entry["asset"].get_string().get(asset_sv) != simdjson::SUCCESS) {
            return PrivateRestError::MalformedResponse;
        }
        if (entry["free"].get_string().get(free_sv) != simdjson::SUCCESS) {
            return PrivateRestError::MalformedResponse;
        }
        if (entry["locked"].get_string().get(locked_sv) != simdjson::SUCCESS) {
            return PrivateRestError::MalformedResponse;
        }

        std::int64_t free_ticks = 0, locked_ticks = 0;
        if (!parse_balance_decimal_to_ticks(free_sv, free_ticks)) {
            return PrivateRestError::MalformedResponse;
        }
        if (!parse_balance_decimal_to_ticks(locked_sv, locked_ticks)) {
            return PrivateRestError::MalformedResponse;
        }

        // §4.3: zero-balance assets don't consume a slot. Comparing the PARSED value (not the
        // raw string) so this is correct regardless of which zero spelling Binance sends
        // ("0", "0.0", "0.00000000", ...), not just the one example spelling the spec text
        // happens to use.
        if (free_ticks == 0 && locked_ticks == 0) continue;

        // Truncating an oversized asset symbol into AssetBalance::asset's fixed buffer would
        // misattribute this balance to an ambiguous/wrong asset -- reject, don't truncate,
        // same philosophy as every other check in this function.
        if (asset_sv.empty() || asset_sv.size() >= kAssetNameLen) {
            return PrivateRestError::MalformedResponse;
        }

        // §4.3: more nonzero-balance assets than kMaxAssets -> reject the whole fetch, never
        // silently truncate to the first kMaxAssets entries.
        if (built.asset_count >= kMaxAssets) {
            return PrivateRestError::CapacityExceeded;
        }

        AssetBalance& slot = built.assets[built.asset_count];
        std::memcpy(slot.asset, asset_sv.data(), asset_sv.size());
        slot.asset[asset_sv.size()] = '\0';
        slot.free_ticks = free_ticks;
        slot.locked_ticks = locked_ticks;
        ++built.asset_count;
    }

    out = built;
    return PrivateRestError::None;
}

// L4 §5: real Binance symbols are uppercase alphanumeric only (e.g. "BTCUSDT") -- rejecting
// anything else here means build_exchange_info_target() never has to build a request target
// from unvalidated caller input.
inline bool is_valid_exchange_info_symbol(std::string_view s) noexcept {
    if (s.empty() || s.size() >= kSymbolNameLen) return false;
    for (char c : s) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

// RFC 3986 percent-encoding, used only for the `symbols` JSON-array query parameter below --
// see build_exchange_info_target()'s own comment for why this is needed at all.
inline void percent_encode_append(std::string_view s, std::string& out) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);  // explicit, not an implicit
                                                         // sign-changing conversion in the loop
                                                         // variable itself (GCC -Wsign-conversion)
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                 (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                                 c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0xF]);
            out.push_back(kHex[c & 0xF]);
        }
    }
}

// L4 §5: builds the GET /api/v3/exchangeInfo request target, with an optional `symbols`
// filter -- see fetch_exchange_info()'s own doc comment for why this filter exists at all
// (this system's own kMaxSymbols design capacity vs. Binance's 2000+-symbol unfiltered
// response). Returns false (out_target left untouched) if any symbol fails
// is_valid_exchange_info_symbol() -- fails closed rather than building a request target out of
// unvalidated caller input.
//
//   - empty            -> "/api/v3/exchangeInfo" (no filter -- local testing / one-off use
//                          only, see fetch_exchange_info()'s own warning against this in
//                          production).
//   - one symbol       -> "/api/v3/exchangeInfo?symbol=BTCUSDT" (Binance's short form; the
//                          value itself is already known URI-safe by
//                          is_valid_exchange_info_symbol()'s own alphabet, so no encoding is
//                          needed here).
//   - multiple symbols -> Binance's required JSON-array-string syntax, e.g.
//                          ["BTCUSDT","ETHUSDT"] -- but '[' ']' '"' ',' are all reserved/illegal
//                          characters in a raw HTTP request target. Building that string
//                          unencoded and sending it as-is would be malformed on the wire (Beast's
//                          own parser, or any reverse proxy in front of the real endpoint, can
//                          reject it as a bad request). The percent-encoded form actually sent
//                          is: "/api/v3/exchangeInfo?symbols=%5B%22BTCUSDT%22%2C%22ETHUSDT%22%5D"
//                          (%5B='[', %22='"', %2C=',', %5D=']').
inline bool build_exchange_info_target(std::span<const std::string_view> symbols,
                                        std::string& out_target) {
    for (const auto& s : symbols) {
        if (!is_valid_exchange_info_symbol(s)) return false;
    }

    if (symbols.empty()) {
        out_target = "/api/v3/exchangeInfo";
        return true;
    }
    if (symbols.size() == 1) {
        out_target = "/api/v3/exchangeInfo?symbol=";
        out_target += symbols[0];
        return true;
    }

    std::string json;
    json += '[';
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) json += ',';
        json += '"';
        json += symbols[i];
        json += '"';
    }
    json += ']';

    out_target = "/api/v3/exchangeInfo?symbols=";
    percent_encode_append(json, out_target);
    return true;
}

// L4 §5: schema validation + per-symbol scale derivation, collapsed to a single outcome --
// `out` is left completely UNCHANGED on any failure path, exactly mirroring
// parse_account_response()'s own build-then-assign discipline.
//
// JSON parsing scope (deliberately narrower than the full exchangeInfo schema): only
// symbols[].symbol/status/quoteAssetPrecision and three filters[] entries
// (PRICE_FILTER/LOT_SIZE/MIN_NOTIONAL-or-NOTIONAL) are read. Every other filterType Binance may
// send (PERCENT_PRICE_BY_SIDE, MARKET_LOT_SIZE, MAX_NUM_ORDERS, ICEBERG_PARTS, TRAILING_DELTA,
// ...) is skipped silently, not rejected -- SymbolRules has no field for them, and Binance adds
// new filter types over time; treating an unrecognized filterType as fatal would make this
// parser brittle against Binance's own forward evolution.
inline PrivateRestError parse_exchange_info_response(std::string_view body,
                                                      ParsedExchangeInfo& out) {
    auto padded = simdjson::padded_string(body);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) return PrivateRestError::JsonParse;

    std::int64_t server_time_ms = 0;
    if (doc["serverTime"].get_int64().get(server_time_ms) != simdjson::SUCCESS) {
        return PrivateRestError::MalformedResponse;
    }
    if (server_time_ms < 0) return PrivateRestError::MalformedResponse;

    simdjson::ondemand::array symbols_arr;
    if (doc["symbols"].get_array().get(symbols_arr) != simdjson::SUCCESS) {
        return PrivateRestError::MalformedResponse;
    }

    ParsedExchangeInfo built{};
    built.server_time_ms = server_time_ms;

    for (auto sym_result : symbols_arr) {
        simdjson::ondemand::object sym_obj;
        if (sym_result.get_object().get(sym_obj) != simdjson::SUCCESS) {
            return PrivateRestError::MalformedResponse;
        }

        // L4 §5's own capacity guard -- checked BEFORE writing this entry, never after (a
        // check-after-write would already be the out-of-bounds write it exists to prevent).
        // Rejects the WHOLE fetch rather than silently truncating to the first kMaxSymbols
        // entries, same posture as parse_account_response()'s own §4.3 capacity check.
        if (built.symbol_count >= kMaxSymbols) {
            return PrivateRestError::CapacityExceeded;
        }

        std::string_view symbol_sv, status_sv;
        if (sym_obj["symbol"].get_string().get(symbol_sv) != simdjson::SUCCESS) {
            return PrivateRestError::MalformedResponse;
        }
        if (sym_obj["status"].get_string().get(status_sv) != simdjson::SUCCESS) {
            return PrivateRestError::MalformedResponse;
        }
        std::int64_t quote_precision = 0;
        if (sym_obj["quoteAssetPrecision"].get_int64().get(quote_precision) !=
            simdjson::SUCCESS) {
            return PrivateRestError::MalformedResponse;
        }
        // [0,18] mirrors every other scale's valid domain in this file (rescale_notional_ceil(),
        // pow10_i64() -- account_truth.hpp).
        if (quote_precision < 0 || quote_precision > 18) {
            return PrivateRestError::MalformedResponse;
        }

        // Truncating an oversized symbol name into SymbolRules::symbol's fixed buffer would
        // silently misattribute these rules to an ambiguous/wrong symbol -- reject, don't
        // truncate, same philosophy as every other check in this file.
        if (symbol_sv.empty() || symbol_sv.size() >= kSymbolNameLen) {
            return PrivateRestError::MalformedResponse;
        }

        SymbolRules rules{};
        std::memcpy(rules.symbol, symbol_sv.data(), symbol_sv.size());
        rules.symbol[symbol_sv.size()] = '\0';
        rules.is_trading = (status_sv == "TRADING");  // any other status -> false, per this
                                                       // file's own documented JSON-parsing scope
        rules.quote_scale = static_cast<std::uint8_t>(quote_precision);

        simdjson::ondemand::array filters_arr;
        if (sym_obj["filters"].get_array().get(filters_arr) != simdjson::SUCCESS) {
            return PrivateRestError::MalformedResponse;
        }

        for (auto filter_result : filters_arr) {
            simdjson::ondemand::object filter_obj;
            if (filter_result.get_object().get(filter_obj) != simdjson::SUCCESS) {
                return PrivateRestError::MalformedResponse;
            }
            std::string_view filter_type;
            if (filter_obj["filterType"].get_string().get(filter_type) != simdjson::SUCCESS) {
                return PrivateRestError::MalformedResponse;
            }

            if (filter_type == "PRICE_FILTER") {
                std::string_view tick_sv, min_sv, max_sv;
                if (filter_obj["tickSize"].get_string().get(tick_sv) != simdjson::SUCCESS ||
                    filter_obj["minPrice"].get_string().get(min_sv) != simdjson::SUCCESS ||
                    filter_obj["maxPrice"].get_string().get(max_sv) != simdjson::SUCCESS) {
                    return PrivateRestError::MalformedResponse;
                }
                std::uint8_t price_scale = 0;
                if (!derive_scale_from_decimal_string(tick_sv, price_scale)) {
                    return PrivateRestError::MalformedResponse;
                }
                std::int64_t tick_ticks = 0, min_ticks = 0, max_ticks = 0;
                if (!parse_decimal_to_ticks_with_scale(tick_sv, price_scale, tick_ticks) ||
                    !parse_decimal_to_ticks_with_scale(min_sv, price_scale, min_ticks) ||
                    !parse_decimal_to_ticks_with_scale(max_sv, price_scale, max_ticks)) {
                    return PrivateRestError::MalformedResponse;
                }
                rules.price_scale = price_scale;
                rules.tick_size_ticks = tick_ticks;
                rules.min_price_ticks = min_ticks;
                rules.max_price_ticks = max_ticks;
            } else if (filter_type == "LOT_SIZE") {
                std::string_view step_sv, minq_sv, maxq_sv;
                if (filter_obj["stepSize"].get_string().get(step_sv) != simdjson::SUCCESS ||
                    filter_obj["minQty"].get_string().get(minq_sv) != simdjson::SUCCESS ||
                    filter_obj["maxQty"].get_string().get(maxq_sv) != simdjson::SUCCESS) {
                    return PrivateRestError::MalformedResponse;
                }
                std::uint8_t qty_scale = 0;
                if (!derive_scale_from_decimal_string(step_sv, qty_scale)) {
                    return PrivateRestError::MalformedResponse;
                }
                std::int64_t step_ticks = 0, minq_ticks = 0, maxq_ticks = 0;
                if (!parse_decimal_to_ticks_with_scale(step_sv, qty_scale, step_ticks) ||
                    !parse_decimal_to_ticks_with_scale(minq_sv, qty_scale, minq_ticks) ||
                    !parse_decimal_to_ticks_with_scale(maxq_sv, qty_scale, maxq_ticks)) {
                    return PrivateRestError::MalformedResponse;
                }
                rules.qty_scale = qty_scale;
                rules.step_size_ticks = step_ticks;
                rules.min_qty_ticks = minq_ticks;
                rules.max_qty_ticks = maxq_ticks;
            } else if (filter_type == "MIN_NOTIONAL" || filter_type == "NOTIONAL") {
                // Both names accepted -- spec text itself notes Binance renamed this filter
                // across API versions; the field carrying the value (minNotional) is unchanged.
                std::string_view min_notional_sv;
                if (filter_obj["minNotional"].get_string().get(min_notional_sv) !=
                    simdjson::SUCCESS) {
                    return PrivateRestError::MalformedResponse;
                }
                std::int64_t min_notional_ticks = 0;
                if (!parse_balance_decimal_to_ticks(min_notional_sv, min_notional_ticks)) {
                    return PrivateRestError::MalformedResponse;
                }
                rules.min_notional_ticks = min_notional_ticks;
            }
            // Else: unrecognized filterType -- skip silently, see this function's own scope
            // comment above.
        }

        built.symbols[built.symbol_count] = rules;
        ++built.symbol_count;
    }

    out = built;
    return PrivateRestError::None;
}

// L4 §6.5's status-string -> OrderState table. Returns false (out untouched) for anything
// unrecognized -- "not a guess": an unrecognized `status` value is not evidence of any
// particular outcome and must fail schema validation in parse_order_query_response() below,
// exactly like a missing/malformed field, never be silently mapped to some default state.
// EXPIRED_IN_MATCH (self-trade-prevention-triggered expiry) is deliberately folded into the
// same terminal bucket as EXPIRED -- OrderState has no dedicated STP-expiry state; §6.5's own
// text documents this as a deliberate simplification, not an oversight.
inline bool map_binance_order_status(std::string_view status, OrderState& out) noexcept {
    if (status == "NEW") { out = OrderState::Accepted; return true; }
    if (status == "PARTIALLY_FILLED") { out = OrderState::PartialFill; return true; }
    if (status == "FILLED") { out = OrderState::Filled; return true; }
    if (status == "PENDING_CANCEL") { out = OrderState::CancelRequested; return true; }
    if (status == "CANCELED") { out = OrderState::Cancelled; return true; }
    if (status == "REJECTED") { out = OrderState::Rejected; return true; }
    if (status == "EXPIRED") { out = OrderState::Expired; return true; }
    if (status == "EXPIRED_IN_MATCH") { out = OrderState::Expired; return true; }
    return false;
}

// L4 §6.1.2: schema + field-match validation for a GET /api/v3/order response, checked
// against the OrderExpectation captured at submit time -- never a live re-lookup that could
// have moved on. Returns QueryResult directly (not a PrivateRestError like
// parse_account_response/parse_exchange_info_response) because every failure mode here --
// missing/malformed field, a field that disagrees with `expected`, an unrecognized status, an
// overflow in checked_scaled_mul_div() -- collapses to the exact same caller-visible outcome:
// QueryOutcome::Inconclusive. A richer error channel would be dead code; the caller
// (query_order() below) never distinguishes any of these cases from one another, matching
// §6.1.2's own "every one of the following collapses to Inconclusive" framing.
inline QueryResult parse_order_query_response(std::string_view body,
                                               const OrderExpectation& expected) noexcept {
    auto padded = simdjson::padded_string(body);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) return {};  // Inconclusive: JSON parse failure

    // §6.1.2 step 1 -- schema: all 11 required fields must be present.
    std::string_view symbol_sv, client_order_id_sv, price_sv, orig_qty_sv, executed_qty_sv,
        cumulative_quote_qty_sv, status_sv, side_sv, type_sv, time_in_force_sv;
    std::int64_t order_id = 0;
    if (doc["symbol"].get_string().get(symbol_sv) != simdjson::SUCCESS) return {};
    if (doc["orderId"].get_int64().get(order_id) != simdjson::SUCCESS) return {};
    if (doc["clientOrderId"].get_string().get(client_order_id_sv) != simdjson::SUCCESS) return {};
    if (doc["price"].get_string().get(price_sv) != simdjson::SUCCESS) return {};
    if (doc["origQty"].get_string().get(orig_qty_sv) != simdjson::SUCCESS) return {};
    if (doc["executedQty"].get_string().get(executed_qty_sv) != simdjson::SUCCESS) return {};
    if (doc["cummulativeQuoteQty"].get_string().get(cumulative_quote_qty_sv) != simdjson::SUCCESS) {
        return {};
    }
    if (doc["status"].get_string().get(status_sv) != simdjson::SUCCESS) return {};
    if (doc["side"].get_string().get(side_sv) != simdjson::SUCCESS) return {};
    if (doc["type"].get_string().get(type_sv) != simdjson::SUCCESS) return {};
    if (doc["timeInForce"].get_string().get(time_in_force_sv) != simdjson::SUCCESS) return {};

    if (order_id < 0) return {};

    // §6.1.2 step 2 -- field-match against `expected`, the captured snapshot, never a live
    // lookup. Any mismatch is exactly as untrustworthy as a schema failure -- a response that
    // identifies itself by clientOrderId but disagrees on symbol/side/price/qty/timeInForce is
    // not evidence about THIS order.
    if (client_order_id_sv != expected.client_order_id.view()) return {};
    if (symbol_sv != expected.rules_snapshot_at_submit.symbol_name()) return {};
    if (side_sv != std::string_view(order_side_name(expected.side))) return {};
    // OrderType is frozen to exactly one member (Limit, ADR-019 D7 M7) -- no parser needed,
    // a literal comparison is the whole check.
    if (type_sv != "LIMIT") return {};
    // This codebase only ever sends GTC (L5 §1) -- any other value is not evidence of *this*
    // order, exactly like a mismatched symbol/side (fixes the gap §6.1.2 itself calls out:
    // the field's presence was always required, but no earlier revision checked its value).
    if (time_in_force_sv != "GTC") return {};

    std::int64_t price_ticks = 0, orig_qty_ticks = 0;
    if (!parse_decimal_to_ticks_with_scale(price_sv, expected.rules_snapshot_at_submit.price_scale,
                                            price_ticks)) {
        return {};
    }
    if (price_ticks != expected.intended_price_ticks) return {};
    if (!parse_decimal_to_ticks_with_scale(orig_qty_sv, expected.rules_snapshot_at_submit.qty_scale,
                                            orig_qty_ticks)) {
        return {};
    }
    if (orig_qty_ticks != expected.intended_qty_ticks) return {};

    OrderState confirmed_state{};
    if (!map_binance_order_status(status_sv, confirmed_state)) return {};

    // §6.1.1 -- avg_fill_price_ticks, only when executed_qty_ticks > 0 (never divide by zero,
    // never fabricate a price for a fill that didn't happen).
    std::int64_t executed_qty_ticks = 0;
    if (!parse_decimal_to_ticks_with_scale(executed_qty_sv,
                                            expected.rules_snapshot_at_submit.qty_scale,
                                            executed_qty_ticks)) {
        return {};
    }

    QueryResult result{};
    result.outcome = QueryOutcome::Found;
    result.confirmed_state = confirmed_state;
    result.exchange_order_id = order_id;

    if (executed_qty_ticks == 0) {
        result.filled_qty_ticks = 0;
        result.avg_fill_price_ticks = 0;
        return result;
    }

    std::int64_t cumulative_quote_qty_ticks = 0;
    if (!parse_decimal_to_ticks_with_scale(cumulative_quote_qty_sv,
                                            expected.rules_snapshot_at_submit.quote_scale,
                                            cumulative_quote_qty_ticks)) {
        return {};
    }
    const int exponent = static_cast<int>(expected.rules_snapshot_at_submit.price_scale) +
                          static_cast<int>(expected.rules_snapshot_at_submit.qty_scale) -
                          static_cast<int>(expected.rules_snapshot_at_submit.quote_scale);
    std::int64_t avg_fill_price_ticks = 0;
    if (!checked_scaled_mul_div(cumulative_quote_qty_ticks, exponent, executed_qty_ticks,
                                 avg_fill_price_ticks)) {
        return {};  // overflow, or a scale combination too extreme to represent -- Inconclusive
    }

    result.filled_qty_ticks = executed_qty_ticks;
    result.avg_fill_price_ticks = avg_fill_price_ticks;
    return result;
}

namespace detail {

// Everything compute_clock_offset() needs from one real §3 round trip.
struct ServerTimeFetchResult {
    std::int64_t local_send_ms{0};
    std::int64_t local_recv_ms{0};
    std::int64_t server_time_ms{0};
    ClockPairSample fetch_sample{fetch_clock_pair()};  // placeholder; always overwritten
};

inline net::awaitable<std::variant<ServerTimeFetchResult, PrivateRestError>>
fetch_server_time_coro(std::string host, PrivateRestConfig cfg) {
    using namespace boost::asio::experimental::awaitable_operators;

    PrivateRestError current_stage = PrivateRestError::Resolve;
    try {
        auto executor = co_await net::this_coro::executor;

        ssl::context ssl_ctx(ssl::context::tlsv12_client);
        hy::configure_binance_ssl_context(ssl_ctx);
        if (!cfg.extra_trusted_ca_pem_path.empty()) {
            ssl_ctx.load_verify_file(cfg.extra_trusted_ca_pem_path);
        }

        tcp::resolver resolver(executor);
        beast::ssl_stream<beast::tcp_stream> stream(executor, ssl_ctx);

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            co_return PrivateRestError::TlsHandshake;
        }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        hy::configure_binance_hostname_verification(stream, host);

        const std::string& connect_host =
            cfg.connect_host_override.empty() ? host : cfg.connect_host_override;
        net::steady_timer resolve_timer(executor);
        resolve_timer.expires_after(std::chrono::seconds(5));
        auto resolve_result =
            co_await (resolver.async_resolve(connect_host, cfg.port, net::use_awaitable) ||
                      resolve_timer.async_wait(net::use_awaitable));
        if (resolve_result.index() == 1) {
            co_return PrivateRestError::Resolve;  // timer won the race
        }
        auto results = std::get<0>(resolve_result);

        current_stage = PrivateRestError::Connect;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await beast::get_lowest_layer(stream).async_connect(results, net::use_awaitable);

        current_stage = PrivateRestError::TlsHandshake;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);

        http::request<http::empty_body> req{http::verb::get, "/api/v3/time", 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "HengYuan/0.1");

        // Sampled immediately before the write -- "local_send_ms" for
        // compute_clock_offset()'s RTT calculation (spec §2.2).
        ClockPairSample t0 = fetch_clock_pair();

        current_stage = PrivateRestError::Write;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await http::async_write(stream, req, net::use_awaitable);

        current_stage = PrivateRestError::Read;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.header_limit(static_cast<std::uint32_t>(8 * 1024));
        parser.body_limit(static_cast<std::uint64_t>(64 * 1024));
        co_await http::async_read(stream, buffer, parser, net::use_awaitable);
        auto res = parser.release();

        // Sampled immediately after the read completes -- "local_recv_ms", and also the
        // joint (system, steady) instant this snapshot's later TTL/wall-clock-jump judgment
        // (is_snapshot_fresh()) is anchored to.
        ClockPairSample t1 = fetch_clock_pair();

        beast::get_lowest_layer(stream).close();

        if (res.result() != http::status::ok) {
            co_return PrivateRestError::HttpStatus;
        }

        current_stage = PrivateRestError::JsonParse;
        std::int64_t server_time_ms = 0;
        if (!parse_server_time_response(res.body(), server_time_ms)) {
            co_return PrivateRestError::JsonParse;
        }

        ServerTimeFetchResult out{};
        out.local_send_ms = t0.system_ms();
        out.local_recv_ms = t1.system_ms();
        out.server_time_ms = server_time_ms;
        out.fetch_sample = t1;
        co_return out;

    } catch (...) {
        // Spec §8: any exception, of any type, caught and converted -- never propagated.
        co_return current_stage;
    }
}

inline net::awaitable<std::variant<std::string, PrivateRestError>> fetch_signed_body_coro(
    std::string host, std::string target, std::string api_key, PrivateRestConfig cfg) {
    using namespace boost::asio::experimental::awaitable_operators;

    PrivateRestError current_stage = PrivateRestError::Resolve;
    try {
        auto executor = co_await net::this_coro::executor;

        ssl::context ssl_ctx(ssl::context::tlsv12_client);
        hy::configure_binance_ssl_context(ssl_ctx);
        if (!cfg.extra_trusted_ca_pem_path.empty()) {
            ssl_ctx.load_verify_file(cfg.extra_trusted_ca_pem_path);
        }

        tcp::resolver resolver(executor);
        beast::ssl_stream<beast::tcp_stream> stream(executor, ssl_ctx);

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            co_return PrivateRestError::TlsHandshake;
        }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        hy::configure_binance_hostname_verification(stream, host);

        const std::string& connect_host =
            cfg.connect_host_override.empty() ? host : cfg.connect_host_override;
        net::steady_timer resolve_timer(executor);
        resolve_timer.expires_after(std::chrono::seconds(5));
        auto resolve_result =
            co_await (resolver.async_resolve(connect_host, cfg.port, net::use_awaitable) ||
                      resolve_timer.async_wait(net::use_awaitable));
        if (resolve_result.index() == 1) {
            co_return PrivateRestError::Resolve;
        }
        auto results = std::get<0>(resolve_result);

        current_stage = PrivateRestError::Connect;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await beast::get_lowest_layer(stream).async_connect(results, net::use_awaitable);

        current_stage = PrivateRestError::TlsHandshake;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);

        http::request<http::empty_body> req{http::verb::get, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "HengYuan/0.1");
        // Never negotiates compression (spec §8): no Accept-Encoding header is ever sent, so
        // there is no decompression-bomb surface to bound in the first place.
        req.set("X-MBX-APIKEY", api_key);

        current_stage = PrivateRestError::Write;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await http::async_write(stream, req, net::use_awaitable);

        current_stage = PrivateRestError::Read;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.header_limit(static_cast<std::uint32_t>(8 * 1024));
        // §4.3's 32-asset cap keeps a real response tiny; 256 KiB is generous headroom, not a
        // reflection of an expected size.
        parser.body_limit(static_cast<std::uint64_t>(256 * 1024));
        co_await http::async_read(stream, buffer, parser, net::use_awaitable);
        auto res = parser.release();

        beast::get_lowest_layer(stream).close();

        if (res.result() != http::status::ok) {
            co_return PrivateRestError::HttpStatus;
        }
        co_return res.body();

    } catch (...) {
        co_return current_stage;
    }
}

// L4 §5: GET /api/v3/exchangeInfo is public/unauthenticated, but (unlike
// fetch_server_time_coro's fixed "/api/v3/time" target) needs a caller-supplied target string
// carrying the optional `symbols` filter -- this coroutine is fetch_server_time_coro's shape
// (no X-MBX-APIKEY header; never signs anything) crossed with fetch_signed_body_coro's shape
// (variable target, returns the raw response body for the caller to parse). Deliberately NOT a
// third mode bolted onto fetch_signed_body_coro (e.g. an empty api_key parameter meaning
// "skip the header"): keeping "always sets a real API key" as fetch_signed_body_coro's one
// invariant, with no conditional to accidentally weaken for a genuinely signed call later, is
// simpler to verify by inspection than a shared coroutine with a signed/unsigned branch.
inline net::awaitable<std::variant<std::string, PrivateRestError>> fetch_public_body_coro(
    std::string host, std::string target, PrivateRestConfig cfg) {
    using namespace boost::asio::experimental::awaitable_operators;

    PrivateRestError current_stage = PrivateRestError::Resolve;
    try {
        auto executor = co_await net::this_coro::executor;

        ssl::context ssl_ctx(ssl::context::tlsv12_client);
        hy::configure_binance_ssl_context(ssl_ctx);
        if (!cfg.extra_trusted_ca_pem_path.empty()) {
            ssl_ctx.load_verify_file(cfg.extra_trusted_ca_pem_path);
        }

        tcp::resolver resolver(executor);
        beast::ssl_stream<beast::tcp_stream> stream(executor, ssl_ctx);

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            co_return PrivateRestError::TlsHandshake;
        }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        hy::configure_binance_hostname_verification(stream, host);

        const std::string& connect_host =
            cfg.connect_host_override.empty() ? host : cfg.connect_host_override;
        net::steady_timer resolve_timer(executor);
        resolve_timer.expires_after(std::chrono::seconds(5));
        auto resolve_result =
            co_await (resolver.async_resolve(connect_host, cfg.port, net::use_awaitable) ||
                      resolve_timer.async_wait(net::use_awaitable));
        if (resolve_result.index() == 1) {
            co_return PrivateRestError::Resolve;
        }
        auto results = std::get<0>(resolve_result);

        current_stage = PrivateRestError::Connect;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await beast::get_lowest_layer(stream).async_connect(results, net::use_awaitable);

        current_stage = PrivateRestError::TlsHandshake;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);

        http::request<http::empty_body> req{http::verb::get, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "HengYuan/0.1");
        // Public endpoint (spec §5): deliberately no X-MBX-APIKEY header, mirroring
        // fetch_server_time_coro's own unsigned request. Never negotiates compression either
        // (spec §8) -- no Accept-Encoding header is ever sent.

        current_stage = PrivateRestError::Write;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        co_await http::async_write(stream, req, net::use_awaitable);

        current_stage = PrivateRestError::Read;
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.header_limit(static_cast<std::uint32_t>(8 * 1024));
        // exchangeInfo's response is far larger than /time or /account: Binance's full,
        // unfiltered listing runs several MiB. 4 MiB is generous headroom for the `symbols`
        // -filtered call production code is expected to make (see fetch_exchange_info()'s own
        // doc comment on why an unfiltered call isn't a supported production shape in the first
        // place) -- not a reflection of an expected size for the unfiltered case.
        parser.body_limit(static_cast<std::uint64_t>(4 * 1024 * 1024));
        co_await http::async_read(stream, buffer, parser, net::use_awaitable);
        auto res = parser.release();

        beast::get_lowest_layer(stream).close();

        if (res.result() != http::status::ok) {
            co_return PrivateRestError::HttpStatus;
        }
        co_return res.body();

    } catch (...) {
        co_return current_stage;
    }
}

}  // namespace detail

// Owns one bound credential set + one clock-offset publisher. Not internally thread-safe: the
// same single-owning-thread discipline BoundHmacCredentials itself already requires (construct
// + every sign()/copy_api_key() call from one thread, spec §9) applies transitively to this
// whole class's public methods -- this class does not add its own synchronization on top of
// that, nor does it implement the full actor/scheduler owner-thread model spec §9 describes
// (explicit non-goal for this slice; see this file's header comment).
class BinancePrivateRestClient {
public:
    BinancePrivateRestClient(EnvironmentBinding binding,
                              std::unique_ptr<BoundHmacCredentials> creds) noexcept
        : binding_(binding), creds_(std::move(creds)) {}

    // Call once, before the first sync_clock()/fetch_account(), on the same thread that will
    // make those calls. Spec §8: verifies the TLS trust store this process would actually use
    // resolves to something non-empty -- never proceeds on a silently-empty trust store, which
    // would otherwise make every handshake fail closed for a reason indistinguishable from "the
    // server's certificate is bad."
    bool init() const noexcept {
        try {
            ssl::context probe(ssl::context::tlsv12_client);
            hy::configure_binance_ssl_context(probe);
            return hy::binance_tls_trust_store_populated(probe);
        } catch (...) {
            return false;
        }
    }

    // §3: unauthenticated. On success, computes the offset (RTT > kMaxUsableRttMs is silently
    // discarded -- compute_clock_offset()'s own contract, not a bug here) and publishes it via
    // clock_publisher(), closing the previously-offline-only clock-sync loop. Never signs
    // anything and never touches creds_.
    PrivateRestError sync_clock(const PrivateRestConfig& cfg = {}) {
        net::io_context ioc;
        std::variant<detail::ServerTimeFetchResult, PrivateRestError> outcome =
            PrivateRestError::None;
        net::co_spawn(
            ioc, detail::fetch_server_time_coro(std::string(binding_.base_host()), cfg),
            [&outcome](std::exception_ptr eptr,
                       std::variant<detail::ServerTimeFetchResult, PrivateRestError> r) {
                if (eptr) {
                    // The coroutine itself catches everything (spec §8); anything that still
                    // escapes here (e.g. std::bad_alloc from Asio/Beast setup) is genuinely
                    // exceptional, matching binance_rest_snapshot.hpp's own precedent.
                    std::rethrow_exception(eptr);
                }
                outcome = std::move(r);
            });
        ioc.run();

        if (std::holds_alternative<PrivateRestError>(outcome)) {
            return std::get<PrivateRestError>(outcome);
        }
        const auto& r = std::get<detail::ServerTimeFetchResult>(outcome);
        ClockOffsetSnapshot snap{};
        if (!compute_clock_offset(r.local_send_ms, r.local_recv_ms, r.server_time_ms,
                                   r.fetch_sample, snap)) {
            // RTT over kMaxUsableRttMs, or checked-arithmetic overflow on pathological inputs.
            return PrivateRestError::JsonParse;
        }
        clock_pub_.publish(snap);
        return PrivateRestError::None;
    }

    // §4: signed. Fails closed on a stale/never-synced clock -- try_get_signing_timestamp_ms()
    // is the sole call site for that judgment, and this function never falls back to
    // uncalibrated local system time on its own failure (spec §2.2's core invariant). `out` is
    // left COMPLETELY UNCHANGED on every failure path (§4.4) -- this function only ever
    // assigns to `out` once, on total success, exactly mirroring parse_account_response()'s own
    // build-then-assign discipline.
    PrivateRestError fetch_account(AccountSnapshot& out, const PrivateRestConfig& cfg = {}) {
        std::int64_t fresh_ts_ms = 0;
        if (!try_get_signing_timestamp_ms(clock_pub_, fetch_clock_pair(), fresh_ts_ms)) {
            return PrivateRestError::ClockNotFresh;
        }

        char recv_window_buf[24];
        const int n = std::snprintf(recv_window_buf, sizeof(recv_window_buf), "%lld",
                                     static_cast<long long>(cfg.recv_window_ms));
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(recv_window_buf)) {
            return PrivateRestError::InvalidConfig;
        }
        const std::pair<std::string_view, std::string_view> params[] = {
            {"recvWindow", std::string_view(recv_window_buf, static_cast<std::size_t>(n))},
        };
        auto [qerr, unsigned_query] = build_canonical_query(params);
        if (qerr != QuerySigningError::Ok) return PrivateRestError::SigningFailed;

        auto [serr, signed_query] = build_signed_query(*creds_, unsigned_query, fresh_ts_ms);
        if (serr != QuerySigningError::Ok) return PrivateRestError::SigningFailed;

        std::array<char, kApiKeyLen> key_buf{};
        std::size_t key_len = 0;
        if (!creds_->copy_api_key(key_buf, key_len) || key_len == 0) {
            return PrivateRestError::SigningFailed;
        }

        const std::string target = "/api/v3/account?" + std::string(signed_query.wire_bytes());

        net::io_context ioc;
        std::variant<std::string, PrivateRestError> outcome = PrivateRestError::None;
        net::co_spawn(
            ioc,
            detail::fetch_signed_body_coro(std::string(binding_.base_host()), target,
                                            std::string(key_buf.data(), key_len), cfg),
            [&outcome](std::exception_ptr eptr, std::variant<std::string, PrivateRestError> r) {
                if (eptr) std::rethrow_exception(eptr);
                outcome = std::move(r);
            });
        ioc.run();

        if (std::holds_alternative<PrivateRestError>(outcome)) {
            return std::get<PrivateRestError>(outcome);
        }

        AccountSnapshot parsed{};
        const PrivateRestError perr =
            parse_account_response(std::get<std::string>(outcome), parsed);
        if (perr != PrivateRestError::None) return perr;
        parsed.timestamp_ms = fresh_ts_ms;
        out = parsed;
        return PrivateRestError::None;
    }

    // §5: unauthenticated, like sync_clock() -- never signs anything, never touches creds_.
    //
    // `symbols` should almost always be non-empty in production: Binance's unfiltered
    // GET /api/v3/exchangeInfo returns 2000+ symbols, while kMaxSymbols is this system's own
    // fixed design capacity (account_truth.hpp) -- callers are expected to name only the
    // symbols they actually trade (e.g. {"BTCUSDT", "ETHUSDT"}), not fetch everything. An empty
    // `symbols` is only meant for local testing / one-off inspection, not a production calling
    // pattern; parse_exchange_info_response()'s own CapacityExceeded check is the fail-closed
    // depth-of-defense backstop if a caller (or a future Binance response shape) violates that
    // expectation, but it is a backstop, not the intended primary control.
    //
    // `out` is left COMPLETELY UNCHANGED on every failure path, exactly mirroring
    // fetch_account()'s own build-then-assign discipline.
    PrivateRestError fetch_exchange_info(ParsedExchangeInfo& out,
                                          std::span<const std::string_view> symbols,
                                          const PrivateRestConfig& cfg = {}) {
        std::string target;
        if (!build_exchange_info_target(symbols, target)) {
            return PrivateRestError::InvalidConfig;
        }

        net::io_context ioc;
        std::variant<std::string, PrivateRestError> outcome = PrivateRestError::None;
        net::co_spawn(
            ioc, detail::fetch_public_body_coro(std::string(binding_.base_host()), target, cfg),
            [&outcome](std::exception_ptr eptr, std::variant<std::string, PrivateRestError> r) {
                if (eptr) std::rethrow_exception(eptr);
                outcome = std::move(r);
            });
        ioc.run();

        if (std::holds_alternative<PrivateRestError>(outcome)) {
            return std::get<PrivateRestError>(outcome);
        }

        return parse_exchange_info_response(std::get<std::string>(outcome), out);
    }

    // L4 §6: signed, single attempt -- the retry loop (§6.2) lives in the orchestrator/
    // order_tracker.hpp's poll_once(), which already owns query_attempts and backoff; this
    // method never loops or sleeps internally. Structurally mirrors fetch_account() (same
    // clock-freshness gate, same build_canonical_query/build_signed_query/copy_api_key
    // sequence, same detail::fetch_signed_body_coro reuse) with two deliberate additions
    // fetch_account() itself does not need:
    //
    //   1. Explicit `noexcept` -- required by QueryPort::QueryFn's ABI (order_tracker.hpp),
    //      a plain C-style function pointer with no exception-propagation path. fetch_account()
    //      is NOT noexcept and can let an exceptional (should-never-happen) exception from the
    //      coroutine's completion handler propagate to its caller; this method has no such
    //      freedom -- the same rethrow would call std::terminate() here, taking down whichever
    //      thread poll_once() runs on along with every other order it was tracking. The
    //      try/catch below folds that into Inconclusive instead, which is strictly safer: it
    //      routes even a genuinely unexpected internal fault through the same retry/backoff/
    //      eventual-EscalateToOperator path every other Inconclusive already uses, rather than
    //      crashing the reconcile thread.
    //   2. Explicit entry guards (`creds_`/`expected` non-empty) that fetch_account() also
    //      lacks today (a known, separately-tracked gap) -- no reason for this new method to
    //      replicate it.
    QueryResult query_order(const OrderExpectation& expected,
                             const PrivateRestConfig& cfg = {}) noexcept {
        if (!creds_) return {};
        if (expected.client_order_id.empty()) return {};
        if (expected.rules_snapshot_at_submit.symbol[0] == '\0') return {};

        try {
            std::int64_t fresh_ts_ms = 0;
            if (!try_get_signing_timestamp_ms(clock_pub_, fetch_clock_pair(), fresh_ts_ms)) {
                return {};  // ClockNotFresh -> Inconclusive
            }

            char recv_window_buf[24];
            const int n = std::snprintf(recv_window_buf, sizeof(recv_window_buf), "%lld",
                                         static_cast<long long>(cfg.recv_window_ms));
            if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(recv_window_buf)) {
                return {};
            }
            const std::pair<std::string_view, std::string_view> params[] = {
                {"origClientOrderId", expected.client_order_id.view()},
                {"recvWindow", std::string_view(recv_window_buf, static_cast<std::size_t>(n))},
                {"symbol", expected.rules_snapshot_at_submit.symbol_name()},
            };
            auto [qerr, unsigned_query] = build_canonical_query(params);
            if (qerr != QuerySigningError::Ok) return {};

            auto [serr, signed_query] = build_signed_query(*creds_, unsigned_query, fresh_ts_ms);
            if (serr != QuerySigningError::Ok) return {};

            std::array<char, kApiKeyLen> key_buf{};
            std::size_t key_len = 0;
            if (!creds_->copy_api_key(key_buf, key_len) || key_len == 0) {
                return {};
            }

            const std::string target = "/api/v3/order?" + std::string(signed_query.wire_bytes());

            net::io_context ioc;
            std::variant<std::string, PrivateRestError> outcome = PrivateRestError::None;
            net::co_spawn(
                ioc,
                detail::fetch_signed_body_coro(std::string(binding_.base_host()), target,
                                                std::string(key_buf.data(), key_len), cfg),
                [&outcome](std::exception_ptr eptr, std::variant<std::string, PrivateRestError> r) {
                    if (eptr) std::rethrow_exception(eptr);
                    outcome = std::move(r);
                });
            ioc.run();

            if (std::holds_alternative<PrivateRestError>(outcome)) {
                return {};  // network/HTTP-status/JSON-transport failure -> Inconclusive
            }

            return parse_order_query_response(std::get<std::string>(outcome), expected);
        } catch (...) {
            return {};  // see the noexcept note above -- never let this method throw
        }
    }

    const ClockOffsetPublisher& clock_publisher() const noexcept { return clock_pub_; }

private:
    EnvironmentBinding binding_;
    std::unique_ptr<BoundHmacCredentials> creds_;
    ClockOffsetPublisher clock_pub_;
};

// Bridges BinancePrivateRestClient::query_order() to QueryPort::QueryFn's plain
// function-pointer ABI (order_tracker.hpp) -- the production wiring: `QueryPort{
// &query_order_adapter, &client}`. Lives here, not in order_tracker.hpp, so that L1 file
// (explicitly no network dependency, per its own header comment) never has to #include
// Boost/Beast/OpenSSL/simdjson.
inline QueryResult query_order_adapter(const OrderExpectation& expected, void* user_data) noexcept {
    if (!user_data) return {};
    return static_cast<BinancePrivateRestClient*>(user_data)->query_order(expected);
}

}  // namespace hy
