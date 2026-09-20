// SPDX-License-Identifier: proprietary
// tls_response_acceptor.hpp — Deterministic local TLS fixture that completes a real TLS
// handshake, reads exactly one HTTP request, and replies with a caller-supplied canned HTTP
// response (status + body) -- unlike blackhole_acceptor.hpp's two variants (accept-then-go-
// silent, used for timeout/handshake-only tests), this one actually speaks HTTP, so it can
// exercise a real client's full success-path parsing against a real network round trip, not
// just its failure/timeout stages.
//
// Every request served is recorded (target path + X-MBX-APIKEY header, if present) so a test
// can assert on what the client under test actually sent, not just what it received back.
//
// Runs its accept loop on a dedicated background thread with its own io_context (same pattern
// blackhole_acceptor.hpp uses), so the test's main thread can call the (synchronous-signature)
// client code under test without deadlocking against the fixture. Test-only: no invariants of
// its own to preserve on shutdown, so the destructor stops the io_context directly.

#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hy::test_helpers {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class TlsResponseAcceptor {
public:
    TlsResponseAcceptor(const std::string& cert_pem_path, const std::string& key_pem_path,
                         int status_code, std::string body)
        : acceptor_(ioc_, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0)),
          ssl_ctx_(ssl::context::tlsv12_server),
          status_code_(status_code),
          body_(std::move(body)) {
        ssl_ctx_.use_certificate_chain_file(cert_pem_path);
        ssl_ctx_.use_private_key_file(key_pem_path, ssl::context::pem);
        accept_loop();
        thread_ = std::thread([this] { ioc_.run(); });
    }

    ~TlsResponseAcceptor() {
        ioc_.stop();
        if (thread_.joinable()) thread_.join();
    }

    TlsResponseAcceptor(const TlsResponseAcceptor&) = delete;
    TlsResponseAcceptor& operator=(const TlsResponseAcceptor&) = delete;

    unsigned short port() const { return acceptor_.local_endpoint().port(); }

    struct RequestRecord {
        std::string target;
        std::string api_key_header;  // empty if the header was absent
        std::string host_header;     // empty if the header was absent (public-REST clients assert on it)
    };

    // Guarded by mu_: the accept loop runs on a background thread while the test's main thread
    // typically calls this after driving the client under test to completion.
    std::vector<RequestRecord> requests() const {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_;
    }

private:
    void accept_loop() {
        auto stream = std::make_shared<ssl::stream<tcp::socket>>(ioc_, ssl_ctx_);
        auto& lowest = stream->lowest_layer();
        acceptor_.async_accept(lowest, [this, stream](boost::system::error_code ec) {
            if (!ec) {
                stream->async_handshake(
                    ssl::stream_base::server,
                    [this, stream](boost::system::error_code handshake_ec) {
                        if (!handshake_ec) serve_one(stream);
                    });
            }
            if (!ioc_.stopped()) accept_loop();
        });
    }

    void serve_one(const std::shared_ptr<ssl::stream<tcp::socket>>& stream) {
        auto buffer = std::make_shared<beast::flat_buffer>();
        auto req = std::make_shared<http::request<http::string_body>>();
        http::async_read(
            *stream, *buffer, *req,
            [this, stream, buffer, req](boost::system::error_code read_ec, std::size_t) {
                if (read_ec) return;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    RequestRecord rec;
                    rec.target = std::string(req->target());
                    auto it = req->find("X-MBX-APIKEY");
                    if (it != req->end()) rec.api_key_header = std::string(it->value());
                    const auto host_it = req->find(http::field::host);
                    if (host_it != req->end()) rec.host_header = std::string(host_it->value());
                    requests_.push_back(std::move(rec));
                }
                auto res = std::make_shared<http::response<http::string_body>>(
                    static_cast<http::status>(status_code_), req->version());
                res->set(http::field::content_type, "application/json");
                res->body() = body_;
                res->prepare_payload();
                http::async_write(*stream, *res,
                                   [stream, res](boost::system::error_code, std::size_t) {
                                       boost::system::error_code ignored;
                                       stream->lowest_layer().close(ignored);
                                   });
            });
    }

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    ssl::context ssl_ctx_;
    int status_code_;
    std::string body_;
    std::thread thread_;
    mutable std::mutex mu_;
    std::vector<RequestRecord> requests_;
};

}  // namespace hy::test_helpers
