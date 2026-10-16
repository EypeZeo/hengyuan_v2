// SPDX-License-Identifier: proprietary
// ws_frame_limits.hpp -- the one size the WebSocket sessions and the JSON frame parsers agree on. No Boost, so the market-data parser
// (compiled into the dataplane library without Beast) can include it too.
//
// kWsReadMessageMaxBytes is the largest WebSocket MESSAGE (all fragments reassembled) a Binance session of this system accepts. Every
// session passes it to hy::configure_ws_stream() (binance_tls.hpp), i.e. to Beast's read_message_max(): a message above it fails the
// connection, a message of exactly this size is delivered. The three frame parsers -- BinanceJsonParser, UserDataJsonParser and
// KlineJsonParser -- take ALL of their memory from the same number in their constructors (simdjson's working arrays and string buffer,
// and the padded copy of the frame), so a frame a session can deliver never makes a parser allocate (Appendix D L-38), and parse() refuses
// a body above it BEFORE copying it.
//
// That makes the limit a memory decision as well as a protocol one: each parser holds about 28 MB of address space at 4 MiB (the padded
// copy, 4 MiB of it, is touched; simdjson's arrays are only committed as far as a frame uses them). Which is why it is one constant and not
// a literal at each call site -- raising the session limit raises the parsers' memory with it, in one edit.
//
// Governance: L1 (a constant).

#pragma once

#include <cstddef>

namespace hy {

inline constexpr std::size_t kWsReadMessageMaxBytes = 4 * 1024 * 1024;

}  // namespace hy
