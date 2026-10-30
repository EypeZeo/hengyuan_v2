// SPDX-License-Identifier: proprietary
// tls_routing_acceptor.hpp -- a local TLS "exchange" that answers MANY requests, each by a caller-supplied handler (D5 slice R3b-1).
//
// tls_response_acceptor.hpp answers every request with the same canned response. The offline operator tool talks to several endpoints
// in a row and its flow depends on WHICH answer comes back for which request (a 400 for the point query, a page for allOrders, an
// account, the time), and on the ways a transport can go wrong; this fixture lets a test decide per request:
//   * Answer       -- a status and a body, as a real exchange sends them (any status, including the 4xx/5xx the client's trading path
//                     folds into an error);
//   * Silent       -- the TLS handshake and the request are accepted, no answer ever comes (the client's read timeout);
//   * CutShort     -- the headers promise the whole body, half of it arrives and the connection is dropped (a body cut off in transit);
//   * Drop         -- the connection is closed after the request, without a byte of answer.
// Every request is recorded (method, target, the X-MBX-APIKEY and Host headers, body) so a test can assert on what the client really sent.
//
// The handler runs on the fixture's own thread under a mutex, one request at a time (it must not call back into the fixture). Same threading pattern as tls_response_acceptor.hpp
// (accept loop on a dedicated io_context thread); test-only, so the destructor stops the io_context directly.

#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hy::test_helpers {

namespace routing_net = boost::asio;
namespace routing_beast = boost::beast;
namespace routing_http = routing_beast::http;
namespace routing_ssl = routing_net::ssl;
using routing_tcp = routing_net::ip::tcp;

class TlsRoutingAcceptor {
public:
    struct Request {
        std::string method;
        std::string target;  // path and query exactly as sent
        std::string api_key_header;
        std::string host_header;
        std::string body;
    };

    struct Reply {
        enum class Kind : std::uint8_t { Answer, Silent, CutShort, Drop };
        Kind kind{Kind::Answer};
        int status{200};
        std::string body;
        std::size_t header_padding{0};  // Answer only: one extra response header of this many bytes (a response whose header section is too large)

        static Reply answer(int status, std::string body) { return Reply{Kind::Answer, status, std::move(body), 0}; }
        static Reply answer_with_header_padding(int status, std::string body, std::size_t padding) {
            return Reply{Kind::Answer, status, std::move(body), padding};
        }
        static Reply silent() { return Reply{Kind::Silent, 0, {}, 0}; }
        static Reply cut_short(std::string body) { return Reply{Kind::CutShort, 200, std::move(body), 0}; }
        static Reply drop() { return Reply{Kind::Drop, 0, {}, 0}; }
    };

    using Handler = std::function<Reply(const Request&)>;

    TlsRoutingAcceptor(const std::string& cert_pem_path, const std::string& key_pem_path, Handler handler)
        : acceptor_(ioc_, routing_tcp::endpoint(routing_net::ip::make_address("127.0.0.1"), 0)),
          ssl_ctx_(routing_ssl::context::tlsv12_server),
          handler_(std::move(handler)) {
        ssl_ctx_.use_certificate_chain_file(cert_pem_path);
        ssl_ctx_.use_private_key_file(key_pem_path, routing_ssl::context::pem);
        accept_loop();
        thread_ = std::thread([this] { ioc_.run(); });
    }

    ~TlsRoutingAcceptor() {
        ioc_.stop();
        if (thread_.joinable()) thread_.join();
    }

    TlsRoutingAcceptor(const TlsRoutingAcceptor&) = delete;
    TlsRoutingAcceptor& operator=(const TlsRoutingAcceptor&) = delete;

    unsigned short port() const { return acceptor_.local_endpoint().port(); }

    // Every request served so far, in arrival order (guarded: the accept loop runs on a background thread).
    std::vector<Request> requests() const {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_;
    }

    std::size_t request_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_.size();
    }

private:
    using Stream = routing_ssl::stream<routing_tcp::socket>;

    void accept_loop() {
        auto stream = std::make_shared<Stream>(ioc_, ssl_ctx_);
        auto& lowest = stream->lowest_layer();
        acceptor_.async_accept(lowest, [this, stream](boost::system::error_code ec) {
            if (!ec) {
                stream->async_handshake(routing_ssl::stream_base::server, [this, stream](boost::system::error_code handshake_ec) {
                    if (!handshake_ec) serve_one(stream);
                });
            }
            if (!ioc_.stopped()) accept_loop();
        });
    }

    void serve_one(const std::shared_ptr<Stream>& stream) {
        auto buffer = std::make_shared<routing_beast::flat_buffer>();
        auto req = std::make_shared<routing_http::request<routing_http::string_body>>();
        routing_http::async_read(*stream, *buffer, *req, [this, stream, buffer, req](boost::system::error_code read_ec, std::size_t) {
            if (read_ec) return;
            Request rec;
            rec.method = std::string(req->method_string());
            rec.target = std::string(req->target());
            const auto key_it = req->find("X-MBX-APIKEY");
            if (key_it != req->end()) rec.api_key_header = std::string(key_it->value());
            const auto host_it = req->find(routing_http::field::host);
            if (host_it != req->end()) rec.host_header = std::string(host_it->value());
            rec.body = req->body();
            Reply reply;
            {
                std::lock_guard<std::mutex> lk(mu_);
                requests_.push_back(rec);
                reply = handler_(rec);
            }
            answer(stream, req->version(), reply);
        });
    }

    void answer(const std::shared_ptr<Stream>& stream, unsigned version, const Reply& reply) {
        switch (reply.kind) {
            case Reply::Kind::Silent: {
                std::lock_guard<std::mutex> lk(mu_);
                held_.push_back(stream);  // stays open and silent until the fixture goes away
                return;
            }
            case Reply::Kind::Drop: {
                boost::system::error_code ignored;
                stream->lowest_layer().close(ignored);
                return;
            }
            case Reply::Kind::CutShort: {
                // Content-Length promises the whole body; only the first half is sent, then the connection is dropped.
                auto wire = std::make_shared<std::string>("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                                          std::to_string(reply.body.size()) + "\r\nConnection: close\r\n\r\n" +
                                                          reply.body.substr(0, reply.body.size() / 2));
                routing_net::async_write(*stream, routing_net::buffer(*wire), [stream, wire](boost::system::error_code, std::size_t) {
                    boost::system::error_code ignored;
                    stream->lowest_layer().close(ignored);
                });
                return;
            }
            case Reply::Kind::Answer:
                break;
        }
        auto res = std::make_shared<routing_http::response<routing_http::string_body>>(static_cast<routing_http::status>(reply.status), version);
        res->set(routing_http::field::content_type, "application/json");
        if (reply.header_padding != 0) res->set("X-Padding", std::string(reply.header_padding, 'p'));
        res->body() = reply.body;
        res->prepare_payload();
        routing_http::async_write(*stream, *res, [stream, res](boost::system::error_code, std::size_t) {
            boost::system::error_code ignored;
            stream->lowest_layer().close(ignored);
        });
    }

    routing_net::io_context ioc_;
    routing_tcp::acceptor acceptor_;
    routing_ssl::context ssl_ctx_;
    Handler handler_;
    std::thread thread_;
    mutable std::mutex mu_;
    std::vector<Request> requests_;
    std::vector<std::shared_ptr<Stream>> held_;
};

}  // namespace hy::test_helpers
