// SPDX-License-Identifier: proprietary
// ws_loopback_server.hpp — 批次 6 6b-0f-3d: a narrow local TLS WebSocket SERVER for testing the public
// sessions' POSITIVE paths, which nothing in the repo could reach before: blackhole_acceptor.hpp only ever
// accepts-and-goes-silent (a client never gets past its TLS handshake), tls_response_acceptor.hpp speaks
// plain HTTP. So "is_connected() becomes true after a real WebSocket handshake", "a closed bar reaches the
// ring", "a server-side drop ends the session" and "the supervisor reconnects a REAL session" had no test.
//
// It completes TLS with a caller-supplied certificate (test_leaf_cert_loopback.pem: SAN IP:127.0.0.1 +
// DNS:localhost, so a client can connect to host "127.0.0.1" and still verify the hostname), reads the HTTP
// upgrade request, records its target, upgrades, then lets the test push text frames and end connections:
//   * send_text()   -- a text frame to every upgraded client;
//   * drop_all()    -- an abrupt TCP close, no close frame: what a network drop looks like to the client;
//   * close_all()   -- a graceful WebSocket close (going_away);
//   * UpgradeBehavior::Stall -- TLS completes but the HTTP upgrade request is never read or answered, so a
//     client sits in its WebSocket handshake: the one stage the blackhole fixtures cannot reach.
// It is NOT a Binance emulator: no stream routing, no subscribe protocol, no message validation.
//
// Threading: one private io_context on one dedicated thread runs every handler, so the connection list and
// each connection's write queue need no locks; only the counters (atomics) and the recorded targets (mutex)
// are read from the test thread. Like the other fixtures, the destructor tears down abruptly -- test-only.

#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hy::test_helpers {

// Identical to the aliases the other test helpers declare (a repeated alias to the same target is legal),
// so a TU may include several of them.
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class WsLoopbackServer {
public:
    enum class UpgradeBehavior {
        Answer,  // upgrade normally
        Stall,   // complete TLS, then never read or answer the upgrade request
    };

    WsLoopbackServer(const std::string& cert_pem_path, const std::string& key_pem_path,
                     UpgradeBehavior behavior = UpgradeBehavior::Answer)
        : acceptor_(ioc_, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0))
        , ssl_ctx_(ssl::context::tlsv12_server)
        , behavior_(behavior)
        , work_guard_(net::make_work_guard(ioc_)) {
        ssl_ctx_.use_certificate_chain_file(cert_pem_path);
        ssl_ctx_.use_private_key_file(key_pem_path, ssl::context::pem);
        accept_loop();
        thread_ = std::thread([this] { ioc_.run(); });
    }

    ~WsLoopbackServer() {
        net::post(ioc_, [this] {
            stopping_ = true;
            boost::system::error_code ignored;
            acceptor_.close(ignored);
            for (auto& conn : conns_) conn->abort();
            conns_.clear();
        });
        work_guard_.reset();
        if (thread_.joinable()) thread_.join();
    }

    WsLoopbackServer(const WsLoopbackServer&) = delete;
    WsLoopbackServer& operator=(const WsLoopbackServer&) = delete;

    unsigned short port() const { return acceptor_.local_endpoint().port(); }

    // --- observability (safe from any thread) ---------------------------------------------------
    std::uint32_t tls_handshakes() const noexcept { return tls_handshakes_.load(std::memory_order_acquire); }
    // WebSocket upgrades that completed since construction (a reconnect adds one). Counted when the SERVER's
    // accept completes, which can lag a client that has already read the 101: wait for it (wait_until) rather
    // than reading it the instant the client reports "connected". targets() has no such lag -- the target is
    // recorded before the 101 is sent.
    std::uint32_t upgrades() const noexcept { return upgrades_.load(std::memory_order_acquire); }
    // Upgraded connections that are still open.
    std::uint32_t open_connections() const noexcept { return open_.load(std::memory_order_acquire); }
    // The request target of every upgrade, in order ("/ws/btcusdt@kline_1m").
    std::vector<std::string> targets() const {
        std::lock_guard<std::mutex> lock(targets_mutex_);
        return targets_;
    }

    // --- scripted behaviour (safe from any thread; executed on the server's own thread) ------------
    // Each takes an optional `target`: only connections whose upgrade request target equals it (e.g.
    // "/ws/btcusdt@kline_1h") are affected, so a test can drive the kline feed and the depth feed
    // separately over ONE server. Empty (the default) means every connection.
    void send_text(std::string text, std::string target = {}) {
        net::post(ioc_, [this, payload = std::move(text), filter = std::move(target)] {
            for (auto& conn : conns_) {
                if (conn->matches(filter)) conn->enqueue(payload);
            }
        });
    }
    // Abrupt TCP close with no close frame: a network drop.
    void drop_all(std::string target = {}) {
        net::post(ioc_, [this, filter = std::move(target)] {
            for (auto& conn : conns_) {
                if (conn->matches(filter)) conn->abort();
            }
        });
    }
    // A graceful WebSocket close.
    void close_all(std::string target = {}) {
        net::post(ioc_, [this, filter = std::move(target)] {
            for (auto& conn : conns_) {
                if (conn->matches(filter)) conn->close_gracefully();
            }
        });
    }

private:
    struct Connection : std::enable_shared_from_this<Connection> {
        using Ws = websocket::stream<ssl::stream<tcp::socket>>;

        explicit Connection(WsLoopbackServer& s) : server(s), ws(s.ioc_, s.ssl_ctx_) {}

        tcp::socket& socket() { return ws.next_layer().next_layer(); }

        // An empty filter matches everything (including a connection that has not upgraded yet).
        bool matches(const std::string& filter) const { return filter.empty() || target == filter; }

        void abort() {
            boost::system::error_code ignored;
            socket().shutdown(tcp::socket::shutdown_both, ignored);
            socket().close(ignored);
        }

        // A frame for a connection whose upgrade is still completing is KEPT, not dropped: the accept handler's
        // own do_write() sends it once `upgraded` is set. The client reports "connected" the moment it has read
        // the 101, but Beast's accept is a composed operation whose final completion runs several queued
        // handlers later on this thread -- so a task a test posts right after "connected" (send_text/close_all)
        // can run BEFORE `upgraded` is set. Dropping the frame there was a real, timing-dependent loss: the
        // reconnect's depth events vanished in 2 of 10 ASan runs on tokyo-vps.
        void enqueue(const std::string& message) {
            if (dead) return;
            outbox.push_back(message);
            if (upgraded) do_write();
        }

        void do_write() {
            if (writing || outbox.empty() || dead) return;
            writing = true;
            ws.text(true);
            ws.async_write(net::buffer(outbox.front()),
                           [self = this->shared_from_this()](boost::system::error_code wec, std::size_t) {
                               self->writing = false;
                               if (wec) {
                                   self->server.on_closed(self);
                                   return;
                               }
                               self->outbox.pop_front();
                               self->do_write();
                               self->maybe_close();  // a close asked for earlier waits for the queue to drain
                           });
        }

        void do_read() {
            ws.async_read(buffer, [self = this->shared_from_this()](boost::system::error_code rec, std::size_t) {
                if (rec) {  // the client closed, or the connection dropped
                    self->server.on_closed(self);
                    return;
                }
                self->buffer.consume(self->buffer.size());
                self->do_read();
            });
        }

        // Same reasoning for a graceful close: it is remembered, and performed once the connection is upgraded
        // and everything already queued has been written (maybe_close() is called after the accept and after each
        // write completes). Before, a close asked for while the upgrade or a write was in flight was ignored.
        void close_gracefully() {
            if (dead) return;
            close_requested = true;
            maybe_close();
        }

        void maybe_close() {
            if (!close_requested || closing || !upgraded || dead || writing || !outbox.empty()) return;
            closing = true;
            ws.async_close(websocket::close_code::going_away, [](boost::system::error_code) {});
        }

        WsLoopbackServer& server;
        Ws ws;
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        std::deque<std::string> outbox;
        std::string target;  // the upgrade request's target; empty until the request has been read
        bool writing{false};
        bool upgraded{false};
        bool dead{false};
        bool close_requested{false};
        bool closing{false};
    };

    void accept_loop() {
        auto conn = std::make_shared<Connection>(*this);
        acceptor_.async_accept(conn->socket(), [this, conn](boost::system::error_code aec) {
            if (stopping_) return;
            if (!aec) start(conn);
            accept_loop();
        });
    }

    void start(const std::shared_ptr<Connection>& conn) {
        conns_.push_back(conn);
        conn->ws.next_layer().async_handshake(
            ssl::stream_base::server, [this, conn](boost::system::error_code hec) {
                if (hec) {
                    on_closed(conn);
                    return;
                }
                tls_handshakes_.fetch_add(1, std::memory_order_release);
                if (behavior_ == UpgradeBehavior::Stall) return;  // TLS is up; the upgrade is never answered
                http::async_read(conn->ws.next_layer(), conn->buffer, conn->request,
                                 [this, conn](boost::system::error_code rec, std::size_t) {
                                     if (rec) {
                                         on_closed(conn);
                                         return;
                                     }
                                     conn->target = std::string(conn->request.target());
                                     {
                                         std::lock_guard<std::mutex> lock(targets_mutex_);
                                         targets_.push_back(conn->target);
                                     }
                                     conn->ws.async_accept(conn->request, [this, conn](boost::system::error_code uec) {
                                         if (uec) {
                                             on_closed(conn);
                                             return;
                                         }
                                         conn->upgraded = true;
                                         upgrades_.fetch_add(1, std::memory_order_release);
                                         open_.fetch_add(1, std::memory_order_release);
                                         conn->buffer.consume(conn->buffer.size());
                                         conn->do_read();
                                         conn->do_write();     // frames queued while the upgrade completed
                                         conn->maybe_close();  // ... and a close asked for meanwhile
                                     });
                                 });
            });
    }

    void on_closed(const std::shared_ptr<Connection>& conn) {
        if (conn->dead) return;
        conn->dead = true;
        if (conn->upgraded) open_.fetch_sub(1, std::memory_order_release);
        for (auto it = conns_.begin(); it != conns_.end(); ++it) {
            if (*it == conn) {
                conns_.erase(it);
                break;
            }
        }
    }

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    ssl::context ssl_ctx_;
    const UpgradeBehavior behavior_;
    net::executor_work_guard<net::io_context::executor_type> work_guard_;
    std::thread thread_;

    // Touched only on the server thread.
    std::vector<std::shared_ptr<Connection>> conns_;
    bool stopping_{false};

    std::atomic<std::uint32_t> tls_handshakes_{0};
    std::atomic<std::uint32_t> upgrades_{0};
    std::atomic<std::uint32_t> open_{0};
    mutable std::mutex targets_mutex_;
    std::vector<std::string> targets_;
};

}  // namespace hy::test_helpers
