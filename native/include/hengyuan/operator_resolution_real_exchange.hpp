// SPDX-License-Identifier: proprietary
// operator_resolution_real_exchange.hpp -- the offline operator tool's real exchange: credentials, rate limiter, TLS client, port (D5 slice R3b-2).
//
// Governance: offline tool only; the one header of the tool that needs Boost and OpenSSL (it builds the authenticated REST client). It
// implements ExchangeProvider (operator_resolution_tool.hpp) and nothing else, and it is the only file that names the tool's two
// bindings, EnvironmentBinding::operator_testnet() and ::operator_production() (test_operator_resolution_guard.cpp pins that): a READ-ONLY
// API key the owner made for the tool, under names of its own. The trading names are not read, and a credentials file that holds
// anything but the tool's two names for the chosen environment is refused by the loader's allowlist (UnknownKey) before any value is
// looked at -- so the trading key, which can place orders, cannot be loaded here by accident or by a shared .env file.
//
// What it builds, in this order, and why:
//   1. the binding of the environment the operator named (--env): its host is the only host the client may connect to;
//   2. the credentials: SecureEnvLoader (owner-only permissions, no symlink, a size cap, locked memory, wipe) over the allowlist of this
//      binding's two names, then BoundHmacCredentials (key format checked, secret handed to the signer). The loader is wiped as soon as
//      the credentials exist;
//   3. a SpotRateLimitTracker, configured like the trading demos' (6000 weight per minute less a margin of 500; the Reconciliation lane
//      the client charges holds 10% of what is left, 550, and the worst run of the tool -- a point query, fourteen allOrders windows, openOrders
//      and the account -- weighs 384);
//   4. the client, whose init() proves the machine's TLS trust store is not empty (an empty one would fail every handshake for a reason
//      that looks like a bad certificate). A Windows machine whose OpenSSL has no CA bundle fails here, loudly -- the client has no other
//      source of trust and the tool does not add one (binance_tls.hpp, the L4 spec's section 8) -- and the way out is to run the tool on a host
//      that has one, a Linux host or WSL;
//   5. BinanceExchangeReadPort over the client and the sleeper the tool was given.
// The credentials are bound to the thread that builds them: the tool runs on one thread, main()'s.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <hengyuan/binance_environment.hpp>
#include <hengyuan/binance_private_rest.hpp>
#include <hengyuan/env_loader.hpp>
#include <hengyuan/operator_resolution_tool.hpp>
#include <hengyuan/operator_resolution_transport.hpp>
#include <hengyuan/spot_rate_limit_budget.hpp>

namespace hy::operator_resolution {

constexpr std::string_view env_load_text(const EnvLoadResult& r) noexcept {
    switch (r.status) {
        case EnvLoadStatus::Ok: return "Ok";
        case EnvLoadStatus::FileNotFound: return "no such file";
        case EnvLoadStatus::PermissionTooWide: return "group or other can read it: make it owner-only (chmod 600)";
        case EnvLoadStatus::IsSymlink: return "it is a symbolic link";
        case EnvLoadStatus::FileTooLarge: return "the file is too large to be a credentials file";
        case EnvLoadStatus::ReadError: return "it could not be read";
        case EnvLoadStatus::ParseError:
            switch (r.parse_result.status) {
                case EnvParseStatus::UnknownKey:
                    return "it holds a name this tool does not read: the file may hold only the tool's own two HENGYUAN_OPERATOR_... names";
                case EnvParseStatus::MalformedLine: return "a line is not NAME=value";
                case EnvParseStatus::DuplicateKey: return "a name is given twice";
                case EnvParseStatus::KeyTooLong: return "a name is too long";
                case EnvParseStatus::TooManyEntries: return "too many entries";
                case EnvParseStatus::Ok: break;
            }
            return "it could not be parsed";
        case EnvLoadStatus::LockFailed: return "its memory could not be locked";
    }
    return "unknown";
}

// Does open() insist that the machine's own TLS trust store holds a certificate? main() leaves it at Required. A test that talks to a local
// exchange trusts that exchange's certificate through the client's test seam, which the check cannot see, and says Skip.
enum class TrustStoreCheck : std::uint8_t {
    Required = 0,
    Skip = 1,
};

class RealExchangeProvider final : public ExchangeProvider {
public:
    // `client_config`: the client's network timeouts and port (PrivateRestConfig). The HOST is not part of it -- it is the environment's, from
    // the binding -- and main() passes the default; a test passes the port of its local exchange.
    explicit RealExchangeProvider(PrivateRestConfig client_config = {}, TrustStoreCheck trust_store = TrustStoreCheck::Required)
        : client_config_(std::move(client_config)), trust_store_(trust_store) {}
    RealExchangeProvider(const RealExchangeProvider&) = delete;
    RealExchangeProvider& operator=(const RealExchangeProvider&) = delete;

    ExchangeReadPort* open(const ToolOptions& options, SleepPort& sleeper, ConsolePort& err) override {
        port_.reset();
        client_.reset();

        std::optional<EnvironmentBinding> binding;
        switch (options.environment) {
            case OperatorEnvironment::Testnet: binding = EnvironmentBinding::operator_testnet(); break;
            case OperatorEnvironment::Production: binding = EnvironmentBinding::operator_production(); break;
        }
        if (!binding) {
            err.write_line("error: --env is neither testnet nor production");
            return nullptr;
        }

        const std::array<std::string_view, 2> allowed{binding->api_key_env_key(), binding->secret_env_key()};
        const EnvAllowlist allowlist{allowed.data(), allowed.size()};
        const EnvLoadResult loaded = loader_.load(options.credentials_file.c_str(), allowlist);
        if (loaded.status != EnvLoadStatus::Ok) {
            err.write_line("error: --credentials-file: " + std::string(env_load_text(loaded)));
            return nullptr;
        }
        auto [bind_error, credentials] = BoundHmacCredentials::load_and_bind_credentials(*binding, loader_);
        loader_.wipe();  // the credentials hold their own locked copies
        if (bind_error != QuerySigningError::Ok || !credentials) {
            err.write_line(bind_error == QuerySigningError::InvalidApiKeyFormat
                               ? "error: --credentials-file: the API key is missing or is not a 64-character alphanumeric key"
                               : "error: --credentials-file: the credentials could not be bound (" +
                                     std::to_string(static_cast<int>(bind_error)) + ")");
            return nullptr;
        }

        if (!tracker_.configure(/*weight*/ 6000, 500, /*raw*/ 60000, 5000, /*orders*/ 100, 10)) {
            err.write_line("error: the rate limiter could not be configured");
            return nullptr;
        }
        client_ = std::make_unique<BinancePrivateRestClient>(*binding, std::move(credentials), &tracker_, EndpointWeightTable{}, client_config_);
        if (trust_store_ == TrustStoreCheck::Required && !client_->init()) {
            client_.reset();
            err.write_line("error: the TLS trust store of this machine is empty, so every handshake would fail: run the tool where OpenSSL has a "
                           "CA bundle (a Linux host or WSL)");
            return nullptr;
        }
        port_ = std::make_unique<BinanceExchangeReadPort>(*client_, sleeper);
        return port_.get();
    }

    // The client behind the port (null before a successful open()): a test points it at a loopback exchange.
    BinancePrivateRestClient* client() noexcept { return client_.get(); }

private:
    // Declared in the order they must outlive each other: the loader first, the tracker before the client that points at it.
    PrivateRestConfig client_config_;
    TrustStoreCheck trust_store_;
    SecureEnvLoader loader_;
    SpotRateLimitTracker tracker_;
    std::unique_ptr<BinancePrivateRestClient> client_;
    std::unique_ptr<BinanceExchangeReadPort> port_;
};

}  // namespace hy::operator_resolution
