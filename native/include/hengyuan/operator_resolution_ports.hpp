// SPDX-License-Identifier: proprietary
// operator_resolution_ports.hpp -- everything the offline operator tool touches outside the two logs (D5 slice R3a).
//
// Governance: interfaces only. The flow in operator_resolution_flow.hpp reaches the exchange, the console, the evidence store and
// the clock through these four abstract ports and nothing else, which is what lets slice R3a prove the whole procedure against a
// loopback fake exchange with a fake clock (including reads that LAG behind the truth, error codes and paging) before slice R3b
// writes the real implementations: signed GETs over TLS, a terminal, a directory, std::this_thread.
//
// What the real implementations must keep, because the flow cannot check it:
//   * ExchangeReadPort only ever issues GETs, and only these five. It signs them with a READ-ONLY key (design O3); the key and
//     every signature stay inside the implementation -- nothing below sees them, so nothing can end up in the evidence file.
//   * Its answers are the exchange's, unprocessed: the HTTP status and the body bytes exactly as received. The flow classifies
//     them (operator_resolution_response.hpp) and stores them (operator_resolution_bundle.hpp); a port that "helpfully"
//     interprets a 400 would defeat both.
//   * A request that did not complete (connect failure, timeout, a body cut off) is transport_ok == false, never an empty
//     HTTP 200.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <hengyuan/operator_resolution_confirmation.hpp>
#include <hengyuan/operator_resolution_evidence.hpp>

namespace hy::operator_resolution {

// What one request produced.
struct HttpRead {
    bool transport_ok{false};  // false: no usable answer (the zero value is not an answer)
    std::uint16_t http_status{0};
    std::string body;  // exactly the bytes received
};

class ExchangeReadPort {
public:
    virtual ~ExchangeReadPort() = default;

    // GET /api/v3/time -> serverTime. nullopt: no usable reading (the flow also refuses implausible ones).
    virtual std::optional<ExchangeMs> server_time() = 0;
    // GET /api/v3/order?symbol=<symbol>&origClientOrderId=<client_order_id>
    virtual HttpRead query_order(std::string_view symbol, std::string_view client_order_id) = 0;
    // GET /api/v3/allOrders?symbol=<symbol>&startTime=<start>&endTime=<end>&limit=<limit>
    virtual HttpRead all_orders(std::string_view symbol, ExchangeMs start, ExchangeMs end, std::size_t limit) = 0;
    // GET /api/v3/openOrders -- without a symbol: every open order of the account.
    virtual HttpRead open_orders() = 0;
    // Design 6 step 5(d): a short account summary for the operator to eyeball. Display only, never evidence; empty if unavailable.
    // The flow prints it to the terminal as it is, so an implementation must return ONE line of printable ASCII that it composed
    // itself from fields it parsed (balances, permissions) -- never a piece of the raw response.
    virtual std::string account_summary() = 0;
};

// The local wait between two serverTime readings. (Local time is only ever used to decide how long to sleep; the fence itself
// is counted on the exchange clock.)
class SleepPort {
public:
    virtual ~SleepPort() = default;
    virtual void sleep_for(std::chrono::milliseconds duration) = 0;
};

class ConsolePort {
public:
    virtual ~ConsolePort() = default;
    // One line of output.
    virtual void write_line(std::string_view line) = 0;
    // One line typed by the operator, within `timeout` (OperatorLineReader over the terminal in the real implementation).
    virtual OperatorLineResult read_line(std::chrono::milliseconds timeout) = 0;
};

class EvidenceStorePort {
public:
    virtual ~EvidenceStorePort() = default;
    // Stores the evidence bundle durably under a name derived from `file_stem` (the flow passes only [A-Za-z0-9._-]); false on
    // any failure. The flow writes the frame only after this returned true.
    virtual bool store(std::string_view file_stem, std::string_view contents) = 0;
};

struct OperatorPorts {
    ExchangeReadPort& exchange;
    SleepPort& sleeper;
    ConsolePort& console;
    EvidenceStorePort& evidence;
};

}  // namespace hy::operator_resolution
