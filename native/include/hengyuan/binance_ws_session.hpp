// SPDX-License-Identifier: proprietary
// binance_ws_session.hpp — Boost.Beast async WebSocket session for Binance public WS.
// Governance: L2/L4. Public WS only, no token/HMAC/Private API.
// CI compiles but does not connect to real WS. VPS manual verification = L4.

#pragma once

#include <hengyuan/binance_json_parser.hpp>
#include <hengyuan/binance_market_event.hpp>
#include <hengyuan/spsc_ring.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hy {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

struct WsSessionConfig {
    std::string host = "stream.binance.com";
    std::string port = "9443";
    std::string target = "/ws";
    std::vector<std::string> subscribe_streams;  // e.g. {"btcusdt@trade", "ethusdt@trade"}
    int reconnect_max_attempts = 10;
    int reconnect_base_delay_ms = 1000;
    int reconnect_max_delay_ms = 60000;
};

struct WsSessionStats {
    std::uint64_t messages_received{0};
    std::uint64_t bytes_received{0};
    std::uint64_t parse_ok{0};
    std::uint64_t parse_failed{0};
    std::uint64_t push_ok{0};
    std::uint64_t push_dropped{0};
    std::uint64_t reconnect_attempts{0};
    std::uint64_t errors{0};
};

template <std::size_t RingSize = 65536>
class BinanceWsSession : public std::enable_shared_from_this<BinanceWsSession<RingSize>> {
    // Binance depth@100ms typically sends ≤40 levels (20 bids + 20 asks).
    // 512 gives generous headroom without stack pressure (~32KB).
    static constexpr std::size_t kMaxEventsPerMessage = 512;

public:
    BinanceWsSession(net::io_context& ioc, ssl::context& ssl_ctx,
                     SpscRing<BinanceMarketEvent, RingSize>& ring,
                     BinanceJsonParser& parser,
                     WsSessionConfig config)
        : resolver_(net::make_strand(ioc))
        , ws_(net::make_strand(ioc), ssl_ctx)
        , ring_(ring)
        , parser_(parser)
        , config_(std::move(config)) {}

    void start() {
        resolver_.async_resolve(
            config_.host, config_.port,
            beast::bind_front_handler(&BinanceWsSession::on_resolve,
                                     this->shared_from_this()));
    }

    void stop() { stop_.store(true, std::memory_order_relaxed); }
    bool stopped() const { return stop_.load(std::memory_order_relaxed); }
    const WsSessionStats& stats() const { return stats_; }

private:
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec || stop_) return fail(ec, "resolve");
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(ws_).async_connect(
            results,
            beast::bind_front_handler(&BinanceWsSession::on_connect,
                                     this->shared_from_this()));
    }

    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
        if (ec || stop_) return fail(ec, "connect");
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(10));
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
        if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(),
                                      config_.host.c_str())) {
            return fail(beast::error_code(static_cast<int>(::ERR_get_error()),
                                          net::error::get_ssl_category()), "ssl_sni");
        }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        ws_.next_layer().async_handshake(
            ssl::stream_base::client,
            beast::bind_front_handler(&BinanceWsSession::on_ssl_handshake,
                                     this->shared_from_this()));
    }

    void on_ssl_handshake(beast::error_code ec) {
        if (ec || stop_) return fail(ec, "ssl_handshake");
        beast::get_lowest_layer(ws_).expires_never();
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        std::string target = config_.target;
        if (!config_.subscribe_streams.empty()) {
            target += "/";
            for (std::size_t i = 0; i < config_.subscribe_streams.size(); ++i) {
                if (i > 0) target += "/";
                target += config_.subscribe_streams[i];
            }
        }
        ws_.async_handshake(config_.host, target,
                            beast::bind_front_handler(&BinanceWsSession::on_handshake,
                                                     this->shared_from_this()));
    }

    void on_handshake(beast::error_code ec) {
        if (ec || stop_) return fail(ec, "ws_handshake");
        do_read();
    }

    void do_read() {
        if (stop_) return;
        buffer_.clear();
        ws_.async_read(buffer_,
                       beast::bind_front_handler(&BinanceWsSession::on_read,
                                                this->shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        if (ec || stop_) return fail(ec, "read");

        ++stats_.messages_received;
        stats_.bytes_received += bytes_transferred;

        auto data = buffer_.data();
        std::string_view sv(static_cast<const char*>(data.data()), data.size());
        std::uint64_t recv_ns = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());

        BinanceMarketEvent events[kMaxEventsPerMessage];
        std::size_t count = 0;
        auto pr = parser_.parse(sv, recv_ns, events, kMaxEventsPerMessage, count);

        if (pr == ParseResult::Ok) {
            ++stats_.parse_ok;
            for (std::size_t i = 0; i < count; ++i) {
                if (ring_.try_push(events[i])) {
                    ++stats_.push_ok;
                } else {
                    ++stats_.push_dropped;
                }
            }
        } else if (pr == ParseResult::EventIgnored) {
            // subscribe ack, etc.
        } else {
            ++stats_.parse_failed;
        }

        do_read();
    }

    void fail(beast::error_code ec, const char* what) {
        ++stats_.errors;
        (void)ec;
        (void)what;
    }

    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    beast::flat_buffer buffer_;
    SpscRing<BinanceMarketEvent, RingSize>& ring_;
    BinanceJsonParser& parser_;
    WsSessionConfig config_;
    WsSessionStats stats_{};
    std::atomic<bool> stop_{false};
};

}  // namespace hy
