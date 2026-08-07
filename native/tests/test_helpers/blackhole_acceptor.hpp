// SPDX-License-Identifier: proprietary
// blackhole_acceptor.hpp — Deterministic local TCP/TLS test fixtures for Track C connectivity
// tests (P2-MD-02). NOT a general-purpose fake server (no HTTP/WS protocol handling) -- two
// narrow-purpose variants:
//
//   1. PlainBlackholeAcceptor: accepts a TCP connection and then does nothing (never reads,
//      writes, or closes it). The client's ClientHello is sent but never answered, so this
//      deterministically triggers a TLS-handshake-stage timeout -- a real, always-closed local
//      port (`127.0.0.1:1`-style tests) only proves *connection refused*, not that a timeout
//      that fires mid-handshake is actually honored.
//   2. TlsBlackholeAcceptor: completes a real TLS handshake using a caller-supplied
//      certificate/key and then goes silent. Two uses: (a) the REST read-stage-timeout test
//      (the HTTP request is sent, the TLS layer is fully up, but no HTTP response ever
//      arrives), and (b) the hostname-verification positive/negative control tests, which only
//      care about the handshake outcome itself.
//
// Both run their accept loop on a dedicated background thread with its own io_context, so the
// test's main thread can call the (synchronous-signature) client code under test without
// deadlocking against the fixture. The destructor stops that io_context directly (not the
// polite "cancel and let it drain" pattern binance_ws_session.hpp uses) -- that's fine here
// specifically because this is a test-only fixture with no invariants of its own to preserve on
// shutdown, unlike the production classes this repo is otherwise careful about.

#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace hy::test_helpers {

namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class PlainBlackholeAcceptor {
public:
    PlainBlackholeAcceptor()
        : acceptor_(ioc_, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0)) {
        accept_loop();
        thread_ = std::thread([this] { ioc_.run(); });
    }

    ~PlainBlackholeAcceptor() {
        ioc_.stop();
        if (thread_.joinable()) thread_.join();
    }

    PlainBlackholeAcceptor(const PlainBlackholeAcceptor&) = delete;
    PlainBlackholeAcceptor& operator=(const PlainBlackholeAcceptor&) = delete;

    unsigned short port() const { return acceptor_.local_endpoint().port(); }

private:
    void accept_loop() {
        auto socket = std::make_shared<tcp::socket>(ioc_);
        auto* socket_ptr = socket.get();
        acceptor_.async_accept(*socket_ptr, [this, socket](boost::system::error_code ec) {
            if (!ec) {
                held_sockets_.push_back(socket);  // keep alive; never read/write/close
            }
            if (!ioc_.stopped()) accept_loop();
        });
    }

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    std::vector<std::shared_ptr<tcp::socket>> held_sockets_;
};

class TlsBlackholeAcceptor {
public:
    TlsBlackholeAcceptor(const std::string& cert_pem_path, const std::string& key_pem_path)
        : acceptor_(ioc_, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0)),
          ssl_ctx_(ssl::context::tlsv12_server) {
        ssl_ctx_.use_certificate_chain_file(cert_pem_path);
        ssl_ctx_.use_private_key_file(key_pem_path, ssl::context::pem);
        accept_loop();
        thread_ = std::thread([this] { ioc_.run(); });
    }

    ~TlsBlackholeAcceptor() {
        ioc_.stop();
        if (thread_.joinable()) thread_.join();
    }

    TlsBlackholeAcceptor(const TlsBlackholeAcceptor&) = delete;
    TlsBlackholeAcceptor& operator=(const TlsBlackholeAcceptor&) = delete;

    unsigned short port() const { return acceptor_.local_endpoint().port(); }

private:
    void accept_loop() {
        auto stream = std::make_shared<ssl::stream<tcp::socket>>(ioc_, ssl_ctx_);
        auto& lowest = stream->lowest_layer();
        acceptor_.async_accept(lowest, [this, stream](boost::system::error_code ec) {
            if (!ec) {
                stream->async_handshake(
                    ssl::stream_base::server,
                    [this, stream](boost::system::error_code handshake_ec) {
                        if (!handshake_ec) {
                            held_streams_.push_back(stream);  // handshake done; go silent
                        }
                    });
            }
            if (!ioc_.stopped()) accept_loop();
        });
    }

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    ssl::context ssl_ctx_;
    std::thread thread_;
    std::vector<std::shared_ptr<ssl::stream<tcp::socket>>> held_streams_;
};

}  // namespace hy::test_helpers
