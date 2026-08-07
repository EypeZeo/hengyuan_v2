// SPDX-License-Identifier: proprietary
// binance_tls.hpp — Shared TLS setup for Binance public REST/WS clients (P2-MD-02 / Track C).
//
// Two free functions used by binance_rest_snapshot.hpp, binance_ws_session.hpp, and the two
// demos, so the same TLS hardening is written once instead of duplicated at 3-4 call sites.
//
// Neither function swallows exceptions: set_default_verify_paths()/set_verify_mode()/
// set_verify_callback() can throw boost::system::system_error (e.g. a broken system trust
// store) and the caller's own try/catch is expected to turn that into a clear diagnostic
// rather than a generic unhandled-exception crash.

#pragma once

#include <boost/asio/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <cstddef>
#include <string>

namespace hy {

inline void configure_binance_ssl_context(boost::asio::ssl::context& ctx) {
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(boost::asio::ssl::verify_peer);
}

// Chain-to-trusted-CA (set_verify_mode(verify_peer)) alone does not confirm the certificate
// was issued for `host` -- a chain-valid-but-wrong-host certificate would otherwise be
// silently accepted. host_name_verification is Boost's documented replacement for the
// deprecated rfc2818_verification (used unmodified in Beast's own shipped examples, e.g.
// libs/beast/example/http/client/awaitable-ssl/http_client_awaitable_ssl.cpp).
template <typename SslStream>
inline void configure_binance_hostname_verification(SslStream& stream, const std::string& host) {
    stream.set_verify_callback(boost::asio::ssl::host_name_verification(host));
}

// Sets a bounded idle timeout (instead of expires_never()+suggested()'s idle_timeout=none()),
// active keep-alive pings, and an explicit max incoming message size on a WebSocket stream.
// Pure configuration, no I/O -- can be exercised against a freshly-constructed, unconnected
// stream object in a unit test (see test_binance_ws_session.cpp), which is why this lives here
// rather than inline in binance_ws_session.hpp.
template <typename WebSocketStream>
inline void configure_ws_stream(WebSocketStream& ws, std::chrono::seconds idle_timeout,
                                 std::size_t max_message_bytes) {
    boost::beast::websocket::stream_base::timeout wt{};
    wt.handshake_timeout = std::chrono::seconds(30);
    wt.idle_timeout = idle_timeout;
    wt.keep_alive_pings = true;
    ws.set_option(wt);
    ws.read_message_max(max_message_bytes);
}

}  // namespace hy
