// SPDX-License-Identifier: proprietary
// ws_client_rig.hpp — 批次 6 6b-0f-7: the client half of the loopback tests, shared. Extracted from
// test_ws_loopback_sessions.cpp (6b-0f-3d) so the tricky teardown logic below exists ONCE: a second
// copy in test_public_feed_pipeline.cpp would invite the two to drift.
//
// WsClientRig is the harness-shaped I/O setup the public feeds need: one io_context, one TLS context
// that trusts the loopback certificate (test_leaf_cert_loopback.pem, SAN IP:127.0.0.1, the same file
// WsLoopbackServer presents), and ONE dedicated thread running ioc.run(), kept alive by a work guard so
// sessions created LATER (by a supervisor's poll()) still run -- without the guard run() returns at once
// and every later session sits in a queue nobody drains (WsLoopbackWorkGuard.* in
// test_ws_loopback_sessions.cpp is the standing proof).
//
// TEARDOWN ORDER is the point. Anything a session points at (its ring, its parser) must be declared
// BEFORE the rig, so the rig -- destroyed first -- has already joined the I/O thread when they are
// torn down. And stop() (not just a work-guard reset) is what makes a failed ASSERT that bailed out
// early fail the TEST rather than hang the binary: a session still connected with a read pending would
// keep run() alive forever.

#pragma once

#include <hengyuan/binance_tls.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace hy::test_helpers {

inline std::string fixture_path(const char* filename) {
    return std::string(HY_TEST_FIXTURE_DIR) + "/" + filename;
}

// Polls `pred` every millisecond until it holds or `timeout_ms` passes. Returns whether it held.
inline bool wait_until(const std::function<bool()>& pred, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

inline std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct WsClientRig {
    WsClientRig()
        : ssl_ctx(boost::asio::ssl::context::tlsv12_client), guard(boost::asio::make_work_guard(ioc)) {
        hy::configure_binance_ssl_context(ssl_ctx);
        ssl_ctx.load_verify_file(fixture_path("test_leaf_cert_loopback.pem"));
        io_thread = std::thread([this] { ioc.run(); });
    }
    ~WsClientRig() { stop_and_join(); }
    WsClientRig(const WsClientRig&) = delete;
    WsClientRig& operator=(const WsClientRig&) = delete;

    // Idempotent. Every test stops and waits out its sessions, so nothing is pending on the normal path.
    // On a failed ASSERT the test returns early and a session may still be connected with a read
    // pending: waiting for run() to drain would then hang the whole binary instead of failing the test.
    // stop() makes run() return at once; the leftover handlers (and the sessions they keep alive) are
    // destroyed with the io_context, after the thread has been joined.
    void stop_and_join() {
        guard.reset();
        ioc.stop();
        if (io_thread.joinable()) io_thread.join();
    }

    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard;
    std::thread io_thread;  // last: starts only once everything above is constructed
};

}  // namespace hy::test_helpers
