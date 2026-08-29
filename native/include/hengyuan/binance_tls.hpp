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

#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

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

// L4 spec §8: chain-of-trust verification (verify_peer + set_default_verify_paths(), both
// already applied by configure_binance_ssl_context() above) is only as good as the trust store
// it's checking against. set_default_verify_paths()'s actual behavior differs meaningfully
// between this repo's two build targets (CLAUDE.md's dual-compiler setup) -- on GCC 14/Linux it
// typically resolves to the system OpenSSL CA bundle; on MSVC 19.51/Windows, the OpenSSL build
// in use may or may not bridge to the Windows Certificate Store. A build that silently ends up
// with an EMPTY trust store would make every handshake fail closed on its face (a real TLS
// error, not a bypassed check) -- but that failure mode is trivially misdiagnosable as "the
// server's certificate is bad" rather than "our own trust store never loaded," which is a much
// worse debugging experience than a clear, explicit signal at startup.
//
// This is a call-once-at-init() check, never per-request: it inspects whether the X509_STORE
// configure_binance_ssl_context() just populated actually holds at least one certificate.
// Ordering matters -- SSL_CTX_get_cert_store() must be called AFTER
// configure_binance_ssl_context(ctx), since that is what invokes set_default_verify_paths() in
// the first place; calling this before that returns an empty (not-yet-configured) store, which
// would (correctly) report false, but for the wrong reason.
inline bool binance_tls_trust_store_populated(boost::asio::ssl::context& ctx) noexcept {
    X509_STORE* store = SSL_CTX_get_cert_store(ctx.native_handle());
    if (store == nullptr) return false;
    STACK_OF(X509_OBJECT)* objs = X509_STORE_get0_objects(store);
    if (objs == nullptr) return false;
    return sk_X509_OBJECT_num(objs) > 0;
}

}  // namespace hy
