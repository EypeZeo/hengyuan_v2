// SPDX-License-Identifier: proprietary
// operator_resolution_bundle.hpp -- the evidence file of one operator resolution (D5 slice R3a).
//
// Governance: offline tool only (it allocates; no production header includes it). docs/OPERATOR_RESOLUTION_DESIGN.md 6 step 8:
// every resolution leaves a file with the raw answers the decision rested on, the tool version, the fence and read-lag
// parameters and every serverTime reading; the frame carries the first 32 hex characters of its SHA-256 in detail_msg and the
// operator types the first 8 into the confirmation phrase. This header builds that file's bytes (one JSON document) and hashes
// them; writing it somewhere is the EvidenceStorePort's job.
//
// The answers are stored as the exchange sent them. A body is put into a JSON string byte for byte: printable ASCII as it is,
// the escapes JSON requires, every other byte (control characters, DEL, anything >= 0x80) as \u00XX. That is always valid JSON,
// and a reader gets the original bytes back by taking each code point 0..255 as one byte (the Latin-1 view) -- the point being
// that a hostile or corrupt body can neither break the document nor be changed by it.
//
// Nothing secret can get in: the flow never sees the API key or a signature (the ExchangeReadPort keeps both), so the request
// parameters recorded here are the ones the flow chose.
//
// The bytes are a pure function of the inputs (fixed key order, integers only, no locale, no clock): the same evidence always
// has the same digest.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <hengyuan/operator_resolution_confirmation.hpp>
#include <hengyuan/operator_resolution_evidence.hpp>
#include <hengyuan/sha256.hpp>

namespace hy::operator_resolution {

inline constexpr std::string_view kEvidenceFormat = "hy-operator-evidence/1";

// One recorded exchange interaction.
struct BundleCall {
    std::string name;    // "order", "allOrders" or "openOrders" (clock readings are listed apart; the account summary is display only)
    std::string params;  // the request parameters the flow chose (no key, no signature: the flow never has them)
    bool transport_ok{false};
    std::uint16_t http_status{0};
    std::string body;
};

// One reading of the exchange clock (GET /api/v3/time). These are readings, not raw answers, so they are listed apart from
// the calls: "lock" is the reference the fence is counted from, "wait" a re-reading while the fence ran, "read" the last one
// before the evidence requests.
struct BundleTimeReading {
    std::string role;
    std::int64_t ms{0};
};

struct BundleHeader {
    std::uint16_t tool_version{0};
    std::string_view client_order_id;
    std::string_view symbol;
    OperatorEnvironment environment{static_cast<OperatorEnvironment>(255)};
    std::uint64_t account_uid_claimed{0};  // what the operator said (the flow refuses to build a file unless the exchange says the same)
    std::optional<std::uint64_t> account_uid_reported{};  // what GET /api/v3/account said about the key's account (null: it did not)
    FenceParams fence{};
    ExchangeMs lock_ref{};
    ExchangeMs read_ref{};
    std::optional<ExchangeMs> frame_anchor{};  // the send-start frame time, if the log has a trustworthy one
    ExchangeMs range_start{};                  // the allOrders interval
    ExchangeMs range_end{};
    std::vector<BundleTimeReading> time_readings;
    std::string_view account_summary;  // display only, never evidence: what the operator was shown next to the evidence
};

struct BundleVerdicts {
    PointQueryVerdict point_query{PointQueryVerdict::Unusable};
    std::vector<PageVerdict> all_orders;
    PageVerdict open_orders{PageVerdict::Malformed};
};

constexpr std::string_view verdict_name(PointQueryVerdict v) noexcept {
    switch (v) {
        case PointQueryVerdict::Unusable: return "Unusable";
        case PointQueryVerdict::NoSuchOrder: return "NoSuchOrder";
        case PointQueryVerdict::OrderExists: return "OrderExists";
        case PointQueryVerdict::Archived: return "Archived";
        case PointQueryVerdict::OtherError: return "OtherError";
    }
    return "Unknown";
}

constexpr std::string_view verdict_name(PageVerdict v) noexcept {
    switch (v) {
        case PageVerdict::Malformed: return "Malformed";
        case PageVerdict::Clear: return "Clear";
        case PageVerdict::Listed: return "Listed";
        case PageVerdict::PossiblyTruncated: return "PossiblyTruncated";
    }
    return "Unknown";
}

namespace detail {

inline constexpr char kHexDigits[] = "0123456789abcdef";

inline void append_json_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    out += "\\u00";
                    out.push_back(kHexDigits[c >> 4]);
                    out.push_back(kHexDigits[c & 0x0F]);
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

inline void append_int(std::string& out, std::int64_t v) { out += std::to_string(v); }
inline void append_uint(std::string& out, std::uint64_t v) { out += std::to_string(v); }

}  // namespace detail

// The lower-case hex SHA-256 of `data` (64 characters).
inline std::string sha256_hex(std::string_view data) {
    const crypto::Sha256Digest d =
        crypto::sha256(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));
    std::string out(kEvidenceDigestHexChars, '0');
    for (std::size_t i = 0; i < d.bytes.size(); ++i) {
        out[2 * i] = detail::kHexDigits[d.bytes[i] >> 4];
        out[2 * i + 1] = detail::kHexDigits[d.bytes[i] & 0x0F];
    }
    return out;
}

// The evidence document. `calls` are in the order they were made.
inline std::string build_evidence_bundle(const BundleHeader& h, std::span<const BundleCall> calls, const BundleVerdicts& v) {
    std::string out;
    out.reserve(1024);
    out += "{\n\"format\":";
    detail::append_json_string(out, kEvidenceFormat);
    out += ",\n\"tool_version\":";
    detail::append_uint(out, h.tool_version);
    out += ",\n\"client_order_id\":";
    detail::append_json_string(out, h.client_order_id);
    out += ",\n\"symbol\":";
    detail::append_json_string(out, h.symbol);
    out += ",\n\"environment\":";
    detail::append_json_string(out, environment_name(h.environment));
    out += ",\n\"account_uid_claimed\":";
    detail::append_uint(out, h.account_uid_claimed);
    out += ",\n\"account_uid_reported\":";
    if (h.account_uid_reported) {
        detail::append_uint(out, *h.account_uid_reported);
    } else {
        out += "null";
    }
    out += ",\n\"fence\":{\"recv_window_ms\":";
    detail::append_int(out, h.fence.recv_window_ms);
    out += ",\"clock_skew_ms\":";
    detail::append_int(out, h.fence.clock_skew_ms);
    out += ",\"margin_ms\":";
    detail::append_int(out, h.fence.margin_ms);
    out += ",\"read_lag_ms\":";
    detail::append_int(out, h.fence.read_lag_ms);
    out += "},\n\"server_time_readings\":[";
    for (std::size_t i = 0; i < h.time_readings.size(); ++i) {
        if (i != 0) out += ',';
        out += "{\"role\":";
        detail::append_json_string(out, h.time_readings[i].role);
        out += ",\"ms\":";
        detail::append_int(out, h.time_readings[i].ms);
        out += '}';
    }
    out += "],\n\"lock_ref_ms\":";
    detail::append_int(out, h.lock_ref.count());
    out += ",\n\"read_ref_ms\":";
    detail::append_int(out, h.read_ref.count());
    out += ",\n\"frame_anchor_ms\":";
    if (h.frame_anchor) {
        detail::append_int(out, h.frame_anchor->count());
    } else {
        out += "null";
    }
    out += ",\n\"all_orders_range\":{\"start_ms\":";
    detail::append_int(out, h.range_start.count());
    out += ",\"end_ms\":";
    detail::append_int(out, h.range_end.count());
    out += "},\n\"account_summary\":";
    detail::append_json_string(out, h.account_summary);
    out += ",\n\"verdicts\":{\"point_query\":";
    detail::append_json_string(out, verdict_name(v.point_query));
    out += ",\"all_orders\":[";
    for (std::size_t i = 0; i < v.all_orders.size(); ++i) {
        if (i != 0) out += ',';
        detail::append_json_string(out, verdict_name(v.all_orders[i]));
    }
    out += "],\"open_orders\":";
    detail::append_json_string(out, verdict_name(v.open_orders));
    out += "},\n\"calls\":[";
    for (std::size_t i = 0; i < calls.size(); ++i) {
        const BundleCall& c = calls[i];
        out += i == 0 ? "\n" : ",\n";
        out += "{\"name\":";
        detail::append_json_string(out, c.name);
        out += ",\"params\":";
        detail::append_json_string(out, c.params);
        out += ",\"transport_ok\":";
        out += c.transport_ok ? "true" : "false";
        out += ",\"http_status\":";
        detail::append_uint(out, c.http_status);
        out += ",\"body\":";
        detail::append_json_string(out, c.body);
        out += '}';
    }
    out += "\n]\n}\n";
    return out;
}

}  // namespace hy::operator_resolution
