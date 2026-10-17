// SPDX-License-Identifier: proprietary
// binance_ws_api_codec.hpp — private-stream migration M1 (docs/PRIVATE_STREAM_MIGRATION_DESIGN.md §4.3): the RESPONSE and EVENT side
// of the spot WebSocket API. Pure parsing -- no network, no Boost, no clock; simdjson only, like binance_json_parser.hpp.
//
// A frame the server sends is one of two shapes (blueprint 5.7 P-24, P-28, P-29):
//   * a response to a request: {"id":N,"status":200,"result":{...}} or {"id":N,"status":4xx,"error":{"code":C,"msg":"..."}};
//   * a user data event: {"subscriptionId":N,"event":{"e":"executionReport",...}} -- and serverShutdown, a CONNECTION event,
//     {"event":{"e":"serverShutdown","E":T}} with no subscriptionId at all (so that field is optional here).
// WsApiCodec::parse() tells them apart and fills ONE WsApiFrame. It replaces UserDataJsonParser (the listenKey stream's
// unwrapped events), which stays until the listenKey stack is retired (M5).
//
// Why simdjson's DOM parser and not the on-demand one the old parsers use: on-demand reads only the fields it is asked for, so a frame
// that is cut off after the part that was read (a missing closing brace) still "parses". A DOM parse validates the WHOLE document first --
// brackets, strings, numbers, trailing garbage -- and only then is anything read, so a frame that is not entirely valid JSON is no frame.
// The price is a tape built over the frame. All of the parser's memory -- its working arrays AND the document's tape and string buffer,
// roughly 0.6 MiB together -- is sized once, in the constructor, for the largest frame a WsEndpointPermit allows (kWsApiMaxMessageBytes),
// and reused for every frame; a bigger body is refused BEFORE it is copied. (dom::parser::allocate() alone sizes only the working arrays:
// the document is allocated by the first parse() at THAT input's size and regrown by every larger input after it, so the constructor makes
// that first parse itself, at the maximum size. tests/test_binance_ws_api_codec.cpp counts operator new around parse() and is what found
// this: its first version failed with allocations on the first parse of two differently sized frames.)
//
// Every string is copied out of simdjson's view immediately into a fixed buffer, "reject, don't truncate": a value that does not fit
// leaves its field empty. What this parser will NOT do is guess: an executionReport whose client order id or cumulative filled quantity
// cannot be read is not returned half-filled -- parse() reports ExecutionReportIncomplete and leaves the frame empty, because applying
// (or silently dropping) such a report could lose a fill. The session counts it as a LOST event and the barrier answers it with a fresh
// sweep (design 5.2, "soft barrier"). Everything else about a field is best effort, exactly as in the old parser.
//
// Governance: L1 (pure parsing, no I/O).

#pragma once

#include <hengyuan/binance_user_data_event.hpp>
#include <hengyuan/transport_policy.hpp>  // kWsApiMaxMessageBytes

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#endif
#include <simdjson.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>

namespace hy {

// session.subscriptions lists at most this many subscriptions here; a session never holds more than a couple (one per account per
// connection, P-27), so a longer list is a protocol surprise and is flagged, not stored.
inline constexpr std::size_t kMaxListedSubscriptions = 16;

enum class WsApiFrameKind : std::uint8_t {
    Unknown = 0,   // parse() did not accept the frame
    Response = 1,  // WsApiFrame::response is filled
    Event = 2,     // WsApiFrame::event is filled
};

enum class WsApiParseResult : std::uint8_t {
    Ok = 0,
    FrameTooLarge = 1,               // above kWsApiMaxMessageBytes: refused before any copy
    MalformedJson = 2,               // not (entirely) valid JSON, not a JSON object, or a response whose "status" is not an HTTP status
    UnrecognizedFrame = 3,           // a JSON object with neither an "event" object nor a "status"
    MissingEventType = 4,            // an event object without a string "e"
    ExecutionReportIncomplete = 5,   // an executionReport without a usable "c" or "z": NOT applied, counts as a lost event
};

struct WsApiResponse {
    bool id_known{false};         // false: the echoed id was absent, null, a string, or not a non-negative integer
    std::uint64_t id{0};
    std::int32_t status{0};       // the HTTP-style status ("status")
    bool has_result{false};
    bool has_error{false};
    std::int64_t error_code{0};   // "error"."code" -- the exchange's own code (-1003, -2015, ...); the message text is deliberately not kept
    std::int64_t retry_after_ms{0};  // "error"."data"."retryAfter": an ABSOLUTE epoch-ms timestamp on a 429/418; 0 = none
    bool has_subscription_id{false};
    std::int64_t subscription_id{0};  // "result"."subscriptionId" (userDataStream.subscribe.signature)
    bool has_subscription_list{false};  // "result" was an array (session.subscriptions)
    bool subscription_list_truncated{false};
    std::uint8_t subscription_count{0};
    std::array<std::int64_t, kMaxListedSubscriptions> subscription_ids{};

    [[nodiscard]] bool ok() const noexcept { return status == 200 && has_result && !has_error; }
};

// One parsed frame: `kind` says which member is meaningful.
struct WsApiFrame {
    WsApiFrameKind kind{WsApiFrameKind::Unknown};
    WsApiResponse response{};
    UserDataWsEvent event{};
};
static_assert(std::is_trivially_copyable_v<WsApiFrame>);

namespace detail {

// A client order id as Binance allows one: 1 to 36 characters from [A-Za-z0-9._:/-]. Ours (HY-<ms>-<seq>-<hex>) fit. Anything else
// -- above all a NUL, which ClientOrderId::view() would take for the end of the id -- is not an order id this system can hold.
[[nodiscard]] constexpr bool is_valid_ws_client_order_id(std::string_view s) noexcept {
    if (s.empty() || s.size() > kClientOrderIdLen) return false;
    for (const char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                        c == ':' || c == '/' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// Copies `sv` into dst[0..cap) as a C string when it is non-empty and leaves room for the NUL; otherwise leaves dst untouched
// ("reject, don't truncate").
inline void ws_copy_cstr(std::string_view sv, char* dst, std::size_t cap) noexcept {
    if (sv.empty() || sv.size() >= cap) return;
    std::memcpy(dst, sv.data(), sv.size());
    dst[sv.size()] = '\0';
}

inline bool ws_field_string(const simdjson::dom::object& obj, std::string_view key, std::string_view& out) noexcept {
    simdjson::dom::element e;
    return obj.at_key(key).get(e) == simdjson::SUCCESS && e.get_string().get(out) == simdjson::SUCCESS;
}

inline bool ws_field_int64(const simdjson::dom::object& obj, std::string_view key, std::int64_t& out) noexcept {
    simdjson::dom::element e;
    return obj.at_key(key).get(e) == simdjson::SUCCESS && e.get_int64().get(out) == simdjson::SUCCESS;
}

}  // namespace detail

class WsApiCodec {
public:
    // Allocates ALL of the parser's memory (one time); throws std::bad_alloc if that fails -- a codec that could not allocate would be a
    // session that silently drops every frame, and that is worse than one that does not start.
    WsApiCodec() : padded_buf_(kWsApiMaxMessageBytes + simdjson::SIMDJSON_PADDING, '\0') {
        if (parser_.allocate(kWsApiMaxMessageBytes) != simdjson::SUCCESS) throw std::bad_alloc();
        // allocate() sized the working arrays only. The first parse() allocates the document (tape and string buffer) at the size of its
        // input and every larger input regrows it -- on the order-confirmation path, at a moment nobody chose. So make that first parse
        // here with a body of exactly the largest size accepted (all NUL bytes: it is not a document and fails, which is fine -- the
        // document is sized before the content is looked at). Whether nothing is allocated afterwards is measured by WsApiCodecAllocation.
        const simdjson::error_code sized =
            parser_.parse(simdjson::padded_string_view(padded_buf_.data(), kWsApiMaxMessageBytes, padded_buf_.size())).error();
        if (sized == simdjson::MEMALLOC || sized == simdjson::CAPACITY) throw std::bad_alloc();
    }
    WsApiCodec(const WsApiCodec&) = delete;
    WsApiCodec& operator=(const WsApiCodec&) = delete;

    // `out` is reset first and left empty (kind == Unknown) on every failure -- never partially filled.
    [[nodiscard]] WsApiParseResult parse(std::string_view body, WsApiFrame& out) noexcept {
        out = WsApiFrame{};
        if (body.size() > kWsApiMaxMessageBytes) return WsApiParseResult::FrameTooLarge;

        const std::size_t n = body.size();
        if (n > 0) std::memcpy(padded_buf_.data(), body.data(), n);
        std::memset(padded_buf_.data() + n, 0, simdjson::SIMDJSON_PADDING);
        const simdjson::padded_string_view padded(padded_buf_.data(), n, padded_buf_.size());

        simdjson::dom::element doc;
        if (parser_.parse(padded).get(doc) != simdjson::SUCCESS) return WsApiParseResult::MalformedJson;
        simdjson::dom::object top;
        if (doc.get_object().get(top) != simdjson::SUCCESS) return WsApiParseResult::MalformedJson;

        simdjson::dom::element event_element;
        simdjson::dom::object ev;
        if (top.at_key("event").get(event_element) == simdjson::SUCCESS && event_element.get_object().get(ev) == simdjson::SUCCESS) {
            WsApiFrame frame{};
            frame.kind = WsApiFrameKind::Event;
            std::int64_t sid = 0;
            if (detail::ws_field_int64(top, "subscriptionId", sid) && sid >= 0) frame.event.subscription_id = sid;
            const WsApiParseResult r = fill_event(ev, frame.event);
            if (r != WsApiParseResult::Ok) return r;
            out = frame;
            return WsApiParseResult::Ok;
        }

        std::int64_t status = 0;
        if (detail::ws_field_int64(top, "status", status)) {
            if (status < 100 || status > 599) return WsApiParseResult::MalformedJson;
            WsApiFrame frame{};
            frame.kind = WsApiFrameKind::Response;
            frame.response.status = static_cast<std::int32_t>(status);
            fill_response(top, frame.response);
            out = frame;
            return WsApiParseResult::Ok;
        }
        return WsApiParseResult::UnrecognizedFrame;
    }

private:
    // The fields of one event object.
    static WsApiParseResult fill_event(const simdjson::dom::object& ev, UserDataWsEvent& out) noexcept {
        std::string_view type;
        if (!detail::ws_field_string(ev, "e", type)) return WsApiParseResult::MissingEventType;

        std::int64_t i64 = 0;
        if (detail::ws_field_int64(ev, "E", i64)) out.event_time_ms = i64;

        std::string_view sv;
        if (type == "executionReport") {
            std::string_view coid_sv;
            const bool have_coid = detail::ws_field_string(ev, "c", coid_sv) && detail::is_valid_ws_client_order_id(coid_sv);
            if (have_coid) {
                std::memcpy(out.coid.id, coid_sv.data(), coid_sv.size());
                out.coid.id[coid_sv.size()] = '\0';
            }

            if (detail::ws_field_string(ev, "S", sv)) out.side_known = parse_binance_order_side(sv, out.side);
            if (detail::ws_field_string(ev, "f", sv)) out.tif_is_gtc = (sv == "GTC");
            if (detail::ws_field_string(ev, "q", sv)) detail::ws_copy_cstr(sv, out.order_qty_raw, sizeof(out.order_qty_raw));
            if (detail::ws_field_string(ev, "p", sv)) detail::ws_copy_cstr(sv, out.order_price_raw, sizeof(out.order_price_raw));
            if (detail::ws_field_string(ev, "x", sv)) (void)parse_binance_execution_type(sv, out.exec_type);
            if (detail::ws_field_string(ev, "X", sv)) out.order_status_known = map_binance_order_status(sv, out.order_status);
            if (detail::ws_field_int64(ev, "i", i64)) out.exchange_order_id = i64;
            if (detail::ws_field_string(ev, "l", sv)) detail::ws_copy_cstr(sv, out.last_qty_raw, sizeof(out.last_qty_raw));
            if (detail::ws_field_string(ev, "z", sv)) {
                detail::ws_copy_cstr(sv, out.cumulative_filled_qty_raw, sizeof(out.cumulative_filled_qty_raw));
            }
            if (detail::ws_field_string(ev, "L", sv)) detail::ws_copy_cstr(sv, out.last_price_raw, sizeof(out.last_price_raw));
            // "n" the commission of this fill, "N" its asset (null when there is none -> not a string -> left empty), "t" the trade id
            // (-1 when the event is not a fill).
            if (detail::ws_field_string(ev, "n", sv)) detail::ws_copy_cstr(sv, out.commission_raw, sizeof(out.commission_raw));
            if (detail::ws_field_string(ev, "N", sv)) detail::ws_copy_cstr(sv, out.commission_asset, sizeof(out.commission_asset));
            if (detail::ws_field_int64(ev, "T", i64)) out.transaction_time_ms = i64;
            if (detail::ws_field_int64(ev, "t", i64)) out.trade_id = i64;
            if (detail::ws_field_string(ev, "Z", sv)) {
                detail::ws_copy_cstr(sv, out.cumulative_quote_qty_raw, sizeof(out.cumulative_quote_qty_raw));
            }

            if (!have_coid || out.cumulative_filled_qty_raw[0] == '\0') return WsApiParseResult::ExecutionReportIncomplete;
            out.kind = UserDataEventKind::ExecutionReport;
        } else if (type == "outboundAccountPosition") {
            out.kind = UserDataEventKind::OutboundAccountPosition;
            if (detail::ws_field_int64(ev, "u", i64)) out.transaction_time_ms = i64;
        } else if (type == "balanceUpdate" || type == "externalLockUpdate") {
            out.kind = type == "balanceUpdate" ? UserDataEventKind::BalanceUpdate : UserDataEventKind::ExternalLockUpdate;
            if (detail::ws_field_string(ev, "a", sv)) detail::ws_copy_cstr(sv, out.asset, sizeof(out.asset));
            if (detail::ws_field_string(ev, "d", sv)) detail::ws_copy_cstr(sv, out.delta_raw, sizeof(out.delta_raw));
            if (detail::ws_field_int64(ev, "T", i64)) out.transaction_time_ms = i64;
        } else if (type == "listStatus") {
            out.kind = UserDataEventKind::ListStatus;
            if (detail::ws_field_int64(ev, "g", i64)) out.order_list_id = i64;
            if (detail::ws_field_string(ev, "L", sv)) (void)parse_binance_list_order_status(sv, out.list_order_status);
            if (detail::ws_field_int64(ev, "T", i64)) out.transaction_time_ms = i64;
        } else if (type == "eventStreamTerminated") {
            out.kind = UserDataEventKind::EventStreamTerminated;
        } else if (type == "serverShutdown") {
            out.kind = UserDataEventKind::ServerShutdown;
        } else {
            out.kind = UserDataEventKind::Unknown;  // an event type this build does not know: accepted, not acted on
        }
        return WsApiParseResult::Ok;
    }

    // The envelope of a response and the two result shapes the M2 session needs (a subscriptionId, a list of them).
    static void fill_response(const simdjson::dom::object& top, WsApiResponse& r) noexcept {
        simdjson::dom::element e;
        {
            std::uint64_t id = 0;
            if (top.at_key("id").get(e) == simdjson::SUCCESS && e.get_uint64().get(id) == simdjson::SUCCESS) {
                r.id = id;
                r.id_known = true;
            }
        }
        if (top.at_key("result").get(e) == simdjson::SUCCESS) {
            r.has_result = true;
            simdjson::dom::object ro;
            simdjson::dom::array ra;
            if (e.get_object().get(ro) == simdjson::SUCCESS) {
                std::int64_t sid = 0;
                if (detail::ws_field_int64(ro, "subscriptionId", sid)) {
                    r.has_subscription_id = true;
                    r.subscription_id = sid;
                }
            } else if (e.get_array().get(ra) == simdjson::SUCCESS) {
                r.has_subscription_list = true;
                for (const simdjson::dom::element item : ra) {
                    simdjson::dom::object io;
                    std::int64_t sid = 0;
                    if (item.get_object().get(io) != simdjson::SUCCESS || !detail::ws_field_int64(io, "subscriptionId", sid)) continue;
                    if (r.subscription_count >= kMaxListedSubscriptions) {
                        r.subscription_list_truncated = true;
                        break;
                    }
                    r.subscription_ids[r.subscription_count++] = sid;
                }
            }
        }
        simdjson::dom::object err;
        if (top.at_key("error").get(e) == simdjson::SUCCESS && e.get_object().get(err) == simdjson::SUCCESS) {
            r.has_error = true;
            std::int64_t code = 0;
            if (detail::ws_field_int64(err, "code", code)) r.error_code = code;
            simdjson::dom::object data;
            std::int64_t retry = 0;
            if (err.at_key("data").get(e) == simdjson::SUCCESS && e.get_object().get(data) == simdjson::SUCCESS &&
                detail::ws_field_int64(data, "retryAfter", retry) && retry > 0) {
                r.retry_after_ms = retry;
            }
        }
    }

    simdjson::dom::parser parser_;
    std::string padded_buf_;
};

}  // namespace hy
