// SPDX-License-Identifier: proprietary
// binance_ws_api_session_factory.hpp — private-stream migration M2 (docs/PRIVATE_STREAM_MIGRATION_DESIGN.md §3, §5.1): the hot-thread side of
// a WebSocket API user-data generation. PublicFeedSupervisor<BinanceWsApiUserDataSession> calls this factory, on the thread that polls it,
// once per generation; the factory does everything that needs the credentials, the clock and the rate-limit budget — all of which belong to
// that thread — and hands the session only finished, checked proofs and finished, signed bytes:
//
//   1. a fresh ClockReadyToken (issue_clock_ready_token: a stale or never-published clock yields nothing, so no generation starts);
//   2. the ban check: if the previous generation was refused with a retryAfter (HTTP 418/429: the IP is rate limited or banned, P-26), no
//      new connection is started until that moment — a retry during a ban only extends it, so the attempt is refused locally and costs
//      nothing;
//   3. a WsEndpointPermit for the endpoint's host (only a host on the policy's WebSocket-API allowlist gets one);
//   4. two RateLimitPermits from the tracker's Reconciliation lane: one for the connection (weight 2) and one for the subscribe request
//      (weight 2), P-26, design D7;
//   5. the signed subscribe request (build_subscribe_signature_request: timestamp only from the token, signature only from the bound
//      credentials), signed as late as possible, with a request id that is new for every generation;
//   6. the session, with the subscribe frame already in its mailbox. The supervisor then calls start().
// Any step that fails returns null: PublicFeedSupervisor counts that as a failed attempt and backs off — it never spins, and a refused
// attempt has not touched the network.
//
// Why the signature is made BEFORE the connection exists (not just-in-time after the handshake): the supervisor's factory contract is
// "construct, do not start", and the hot thread is the only place that can sign. The price is that a slow handshake can leave the frame too
// old for the exchange's recvWindow; the session then refuses to send it (max_signed_age) and the next generation signs again. A just-in-time
// variant needs no change to the session — deposit_subscribe() may be called after start() too.
//
// Threads: operator() runs on the supervisor's thread — the one that owns the credentials (BoundHmacCredentials answers only there), the
// clock publisher's consumer side and the rate-limit tracker (single-thread-owned). The factory object must outlive the supervisor.
// It holds the previous generation's session (shared) until the next call only to read why it ended; that session's handlers have nothing
// left to do by then (the supervisor started this call because it had stopped).
//
// Governance: L4 (it creates sessions that perform network I/O once started; it holds no secret itself).

#pragma once

#include <hengyuan/binance_clock_sync.hpp>
#include <hengyuan/binance_environment.hpp>
#include <hengyuan/binance_ws_api_signing.hpp>
#include <hengyuan/binance_ws_api_user_data_session.hpp>
#include <hengyuan/spot_rate_limit_budget.hpp>
#include <hengyuan/transport_policy.hpp>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace hy {

// Where the WebSocket API of an environment is, and the policy that says which hosts a permit may be issued for.
struct WsApiFeedEndpoint {
    TransportPolicy policy{};
    std::string host;
    std::string port;
    std::string target;
};

[[nodiscard]] inline WsApiFeedEndpoint ws_api_endpoint_of(const EnvironmentBinding& binding) {
    WsApiFeedEndpoint e;
    e.policy = binding.transport_policy();
    e.host = std::string(binding.ws_api_host());
    e.port = std::string(binding.ws_api_port());
    e.target = std::string(EnvironmentBinding::ws_api_target());
    return e;
}

// What the factory refused to do, and why (hot-thread diagnostics).
struct WsApiSessionFactoryStats {
    std::uint64_t attempts{0};
    std::uint64_t created{0};
    std::uint64_t refused_clock{0};     // no fresh ClockReadyToken
    std::uint64_t refused_ban{0};       // the previous generation was told to wait (retryAfter)
    std::uint64_t refused_endpoint{0};  // the host is not on the WebSocket-API allowlist
    std::uint64_t refused_budget{0};    // the Reconciliation lane could not pay for the connection and the subscribe request
    std::uint64_t sign_failed{0};       // build_subscribe_signature_request refused (wrong thread, bad key, ...)
    std::uint64_t create_failed{0};     // the session could not be built or its mailbox refused the frame
    std::int64_t ban_until_server_ms{0};
};

class WsApiSessionFactory {
public:
    struct Deps {
        net::io_context& ioc;
        ssl::context& ssl_ctx;
        UserDataWsEventRing& ring;
        WsApiCodec& codec;
        BoundHmacCredentials& credentials;
        const ClockOffsetPublisher& clock;
        SpotRateLimitTracker& limiter;
    };

    // recv_window_ms = 0 leaves recvWindow out of the request (the exchange's default, 5000 ms, applies); otherwise it must be <= 60000.
    WsApiSessionFactory(Deps deps, WsApiFeedEndpoint endpoint, WsApiUserDataSessionConfig session_config = {},
                        std::uint32_t recv_window_ms = 0)
        : deps_(deps)
        , endpoint_(std::move(endpoint))
        , session_config_(std::move(session_config))
        , recv_window_ms_(recv_window_ms) {
        session_config_.port = endpoint_.port;
        session_config_.target = endpoint_.target;
    }

    WsApiSessionFactory(const WsApiSessionFactory&) = delete;
    WsApiSessionFactory& operator=(const WsApiSessionFactory&) = delete;

    // The PublicFeedSupervisor<BinanceWsApiUserDataSession>::Factory call. Never throws (the supervisor's poll() is noexcept).
    [[nodiscard]] std::shared_ptr<BinanceWsApiUserDataSession> operator()(std::uint64_t generation) noexcept {
        try {
            return create(generation);
        } catch (const std::exception&) {
            ++stats_.create_failed;
            return nullptr;
        } catch (...) {
            ++stats_.create_failed;
            return nullptr;
        }
    }

    [[nodiscard]] const WsApiSessionFactoryStats& stats() const noexcept { return stats_; }

private:
    std::shared_ptr<BinanceWsApiUserDataSession> create(std::uint64_t generation) {
        ++stats_.attempts;
        absorb_previous_generation();

        // 1. a fresh clock, or no generation at all
        const std::optional<ClockReadyToken> token = issue_clock_ready_token(deps_.clock, fetch_clock_pair());
        if (!token) {
            ++stats_.refused_clock;
            return nullptr;
        }
        // 2. a ban in force: no connection, no budget spent
        if (stats_.ban_until_server_ms > token->server_ms()) {
            ++stats_.refused_ban;
            return nullptr;
        }
        // 3. the endpoint permit
        const std::optional<WsEndpointPermit> endpoint_permit = issue_ws_endpoint_permit(endpoint_.policy, endpoint_.host);
        if (!endpoint_permit) {
            ++stats_.refused_endpoint;
            return nullptr;
        }
        // 4. the budget: the connection and the subscribe request, both on the Reconciliation lane
        const std::optional<RateLimitPermit> connect_permit =
            deps_.limiter.reserve_weight_only_explicit(RateLimitLane::Reconciliation, ws_api_weight(WsApiRequestKind::Connect));
        if (!connect_permit) {
            ++stats_.refused_budget;
            return nullptr;
        }
        const std::optional<RateLimitPermit> subscribe_permit = deps_.limiter.reserve_weight_only_explicit(
            RateLimitLane::Reconciliation, ws_api_weight(WsApiRequestKind::SubscribeSignature));
        if (!subscribe_permit) {
            ++stats_.refused_budget;
            return nullptr;
        }
        // 5. sign, as late as possible, with a request id that is new for every generation
        const std::uint64_t request_id = next_request_id_++;
        auto [err, request] = build_subscribe_signature_request(deps_.credentials, request_id, *token, recv_window_ms_);
        if (err != WsApiSigningError::Ok) {
            ++stats_.sign_failed;
            return nullptr;
        }
        const std::int64_t signed_at = BinanceWsApiUserDataSession::steady_now_ms();
        // 6. the session, with its subscribe frame already waiting
        auto session = std::make_shared<BinanceWsApiUserDataSession>(deps_.ioc, deps_.ssl_ctx, deps_.ring, deps_.codec, *endpoint_permit,
                                                                     *connect_permit, generation, session_config_);
        if (!session->deposit_subscribe(request, *subscribe_permit, signed_at)) {
            ++stats_.create_failed;
            return nullptr;
        }
        last_ = session;
        ++stats_.created;
        return session;
    }

    // The previous generation is over. If the exchange refused it with a retryAfter, remember until when.
    void absorb_previous_generation() {
        if (!last_) return;
        const WsApiUserDataSessionStats s = last_->stats_snapshot();
        if (s.retry_after_ms > stats_.ban_until_server_ms) stats_.ban_until_server_ms = s.retry_after_ms;
        last_.reset();
    }

    Deps deps_;
    WsApiFeedEndpoint endpoint_;
    WsApiUserDataSessionConfig session_config_;
    std::uint32_t recv_window_ms_;
    std::uint64_t next_request_id_{1};
    std::shared_ptr<BinanceWsApiUserDataSession> last_;
    WsApiSessionFactoryStats stats_{};
};

}  // namespace hy
