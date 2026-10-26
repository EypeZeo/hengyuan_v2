// SPDX-License-Identifier: proprietary
// operator_resolution_response.hpp -- reading the exchange's answers to the operator tool's evidence queries (D5 slice R2).
//
// Governance: Boost-free pure parsing over simdjson. It ALLOCATES (one padded copy of the body and one DOM parser per call, like
// every REST response parser in this tree), which is fine for the offline operator tool and is why it is a header of its own:
// the decisions live in operator_resolution_evidence.hpp, which allocates nothing. No production header includes this file
// (test_operator_resolution_guard.cpp pins that).
//
// It turns (HTTP status, response body) into the verdicts conclude_absent() consumes, and every doubt resolves to "no evidence":
// the DOM parser validates the WHOLE document (numbers, literals, UTF-8, trailing content), duplicate keys are an error, a field
// of the wrong type is an error, and an unexpected status or shape is an error. A page that is cut off in transit, a proxy's
// HTML error page, an empty body, a number where a string belongs -- none of them can come out as "the order is not there".
//
// What each endpoint has to say (official REST docs, checked 2026-10-25):
//   GET /api/v3/order        data source Memory => Database; an unknown order is error -2013 NO_SUCH_ORDER, an archived one
//                            (zero fills, cancelled or expired 90+ days ago) is -2026 ORDER_ARCHIVED -- a DIFFERENT code,
//                            and one that says the order existed.
//   GET /api/v3/allOrders    data source Database; startTime..endTime at most 24 hours; `limit` default 500, maximum 1000; the
//                            answer carries no marker when it was truncated, so a page with `limit` entries is "possibly
//                            truncated" (kAllOrdersPageLimit).
//   GET /api/v3/openOrders   without a symbol: every open order of the account, no paging.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <simdjson.h>

#include <hengyuan/operator_resolution_evidence.hpp>
#include <hengyuan/order_lifecycle.hpp>

namespace hy::operator_resolution {

// A body larger than this is refused unread. 1000 orders are well under 1 MiB; the headroom is for the day an order grows fields.
inline constexpr std::size_t kMaxEvidenceBodyBytes = 4u * 1024u * 1024u;

inline constexpr std::int64_t kErrorNoSuchOrder = -2013;
inline constexpr std::int64_t kErrorOrderArchived = -2026;

namespace detail {

// Parse `body` as exactly ONE JSON document. false: empty, oversize, or not valid JSON (bad UTF-8 and trailing content included).
// A leading UTF-8 byte order mark is refused here: simdjson would skip it silently, but RFC 8259 forbids a sender to add one
// and nothing the exchange sends has one, so a body that starts with it is not what we think it is.
// May throw std::bad_alloc -- every public function below catches it and answers "no evidence".
inline bool parse_document(simdjson::dom::parser& parser, std::string_view body, simdjson::dom::element& root) {
    if (body.empty() || body.size() > kMaxEvidenceBodyBytes) return false;
    if (body.starts_with("\xEF\xBB\xBF")) return false;
    const simdjson::padded_string padded(body);
    if (padded.data() == nullptr) return false;  // the copy could not be allocated
    return parser.parse(padded).get(root) == simdjson::SUCCESS;
}

enum class FieldRead : std::uint8_t {
    Missing = 0,
    Ok = 1,
    Invalid = 2,  // present twice, or of the wrong type
};

inline FieldRead read_unique_int64(const simdjson::dom::object& obj, std::string_view key, std::int64_t& out) noexcept {
    FieldRead result = FieldRead::Missing;
    for (const auto field : obj) {
        if (field.key != key) continue;
        if (result != FieldRead::Missing) return FieldRead::Invalid;  // a duplicate key: which one is meant?
        std::int64_t v = 0;
        if (field.value.get(v) != simdjson::SUCCESS) return FieldRead::Invalid;
        out = v;
        result = FieldRead::Ok;
    }
    return result;
}

inline FieldRead read_unique_string(const simdjson::dom::object& obj, std::string_view key, std::string_view& out) noexcept {
    FieldRead result = FieldRead::Missing;
    for (const auto field : obj) {
        if (field.key != key) continue;
        if (result != FieldRead::Missing) return FieldRead::Invalid;
        std::string_view v;
        if (field.value.get(v) != simdjson::SUCCESS) return FieldRead::Invalid;
        out = v;
        result = FieldRead::Ok;
    }
    return result;
}

}  // namespace detail

// GET /api/v3/order?origClientOrderId=... : what did the exchange say about this one order?
//   HTTP 400 and {"code":-2013,...}           -> NoSuchOrder (the only evidence of absence; the status is part of the claim, and
//                                                the Testnet drill, design slice R6, is where its real behaviour gets measured)
//   any 4xx and {"code":-2026,...}            -> Archived (it existed)
//   any other 4xx with an integer "code"      -> OtherError (rate limit, timestamp outside recvWindow, ...: says nothing)
//   HTTP 200 and an object with an "orderId"  -> OrderExists
//   everything else                           -> Unusable
inline PointQueryVerdict classify_point_query(std::uint16_t http_status, std::string_view body) noexcept {
    try {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        if (!detail::parse_document(parser, body, root)) return PointQueryVerdict::Unusable;
        simdjson::dom::object obj;
        if (root.get(obj) != simdjson::SUCCESS) return PointQueryVerdict::Unusable;

        if (http_status == 200) {
            std::int64_t order_id = 0;
            return detail::read_unique_int64(obj, "orderId", order_id) == detail::FieldRead::Ok
                       ? PointQueryVerdict::OrderExists
                       : PointQueryVerdict::Unusable;
        }
        if (http_status < 400 || http_status > 499) return PointQueryVerdict::Unusable;

        std::int64_t code = 0;
        if (detail::read_unique_int64(obj, "code", code) != detail::FieldRead::Ok) return PointQueryVerdict::Unusable;
        if (code == kErrorOrderArchived) return PointQueryVerdict::Archived;
        if (code == kErrorNoSuchOrder) return http_status == 400 ? PointQueryVerdict::NoSuchOrder : PointQueryVerdict::Unusable;
        return PointQueryVerdict::OtherError;
    } catch (...) {
        return PointQueryVerdict::Unusable;
    }
}

// What a list of orders (allOrders / openOrders) contained.
struct OrdersScan {
    bool ok{false};            // false: the body is not a well-formed array of order objects -- nothing else is meaningful
    bool coid_listed{false};   // some order's clientOrderId equals the one asked about (whole-string equality, after JSON unescaping)
    std::size_t item_count{0};
};

// Every element must be an object with exactly one string "clientOrderId" -- an element that is not (or lacks the field) makes
// the whole page unusable, because it could have been the order we are looking for. With a non-empty `expected_symbol`, every
// element must also carry exactly one "symbol" equal to it: a page that answers for another symbol than the one the tool meant
// to ask about (a wrong request, a filter the exchange did not apply) proves nothing, and an empty page cannot be checked this
// way but lists nothing either. An empty or over-long `client_order_id` is a caller bug and is never "clear".
inline OrdersScan scan_orders_array(std::string_view body, std::string_view client_order_id,
                                    std::string_view expected_symbol) noexcept {
    if (client_order_id.empty() || client_order_id.size() > kClientOrderIdLen) return {};
    try {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
        if (!detail::parse_document(parser, body, root)) return {};
        simdjson::dom::array arr;
        if (root.get(arr) != simdjson::SUCCESS) return {};

        OrdersScan scan{};
        for (const simdjson::dom::element item : arr) {
            simdjson::dom::object obj;
            if (item.get(obj) != simdjson::SUCCESS) return {};
            std::string_view coid;
            if (detail::read_unique_string(obj, "clientOrderId", coid) != detail::FieldRead::Ok) return {};
            if (!expected_symbol.empty()) {
                std::string_view symbol;
                if (detail::read_unique_string(obj, "symbol", symbol) != detail::FieldRead::Ok || symbol != expected_symbol) {
                    return {};
                }
            }
            ++scan.item_count;
            if (coid == client_order_id) scan.coid_listed = true;
        }
        scan.ok = true;
        return scan;
    } catch (...) {
        return {};
    }
}

// One page of GET /api/v3/allOrders?symbol=<expected_symbol>&startTime=..&endTime=..&limit=kAllOrdersPageLimit. Only HTTP 200
// counts. Listed beats PossiblyTruncated (a full page that names the order is a refusal either way).
inline PageVerdict classify_all_orders_page(std::uint16_t http_status, std::string_view body, std::string_view client_order_id,
                                            std::string_view expected_symbol) noexcept {
    if (http_status != 200 || expected_symbol.empty()) return PageVerdict::Malformed;
    const OrdersScan scan = scan_orders_array(body, client_order_id, expected_symbol);
    if (!scan.ok) return PageVerdict::Malformed;
    if (scan.coid_listed) return PageVerdict::Listed;
    if (scan.item_count >= kAllOrdersPageLimit) return PageVerdict::PossiblyTruncated;
    return PageVerdict::Clear;
}

// The whole answer of GET /api/v3/openOrders (no symbol: all of the account's open orders; it has no limit to truncate at).
inline PageVerdict classify_open_orders_page(std::uint16_t http_status, std::string_view body,
                                             std::string_view client_order_id) noexcept {
    if (http_status != 200) return PageVerdict::Malformed;
    const OrdersScan scan = scan_orders_array(body, client_order_id, {});
    if (!scan.ok) return PageVerdict::Malformed;
    return scan.coid_listed ? PageVerdict::Listed : PageVerdict::Clear;
}

}  // namespace hy::operator_resolution
