// SPDX-License-Identifier: proprietary
// rest_account_fetcher.hpp — private-stream migration M5a-2a: the real AccountFetchPort (account_refresh.hpp) over BinancePrivateRestClient::fetch_account(),
// and the one startup read of the account's uid for the store binding.
//
// WHAT IT IS. account_refresh.hpp reads the account through an injected function pointer so that the refresher can be tested without a network. This is the
// production side of that pointer: a thin adapter that asks the REST client for GET /api/v3/account, never lets an exception out (the port is noexcept),
// and tells the refresher in three words what happened --
//   Fetched   the exchange answered and the snapshot is complete (fetch_account() builds it whole or not at all);
//   NotSent   the request never left the process: the entry guards, the clock gate, the local rate-limit budget or the signing refused it. Nothing was resolved,
//             connected or paid for on the wire;
//   Failed    the network was tried (or an answer came back that cannot be used): connect/TLS/read errors, an HTTP status other than 200, malformed JSON, an
//             account with more assets than the snapshot holds, an exception.
// The refresher treats NotSent and Failed alike (retry at the retry interval); the split is for the counters and for whoever reads them.
//
// THE THREAD IT RUNS ON. fetch_account() signs, and BoundHmacCredentials answers only on the thread that bound it (F11, PRIVATE_STREAM_MIGRATION_DESIGN.md); the
// client's rate-limit tracker, when one is wired in, belongs to that same thread (spot_rate_limit_budget.hpp, THREAD OWNERSHIP). So this adapter may only be
// called on the thread that owns the client's credentials. Today that is the single loop thread of the trading harnesses, which tick the hot and the reconcile
// halves of the pipeline from one place (live_submit_preflight_harness.cpp does exactly that). A SEPARATE reconcile thread would need credentials bound on
// it and a rate-limit tracker both threads can reach -- a design fork this slice does not take. Called from the wrong thread the adapter does not misbehave:
// the signing step refuses, fetch_account() returns SigningFailed, the port says NotSent, and the sweep's barrier stays Sweeping (fail-closed, and visible in
// the counters).
//
// THE UID. fetch_exchange_account_uid() is the startup read for store_binding_writer.hpp::ensure_store_binding(): the uid is whatever the exchange reports
// for the key the process is about to trade with, never a configured value. It is a separate call from the port on purpose -- the binding is made once, before
// the loops run, and the periodic and per-sweep fetches need nothing but the snapshot.
//
// Governance: L4 (network I/O through the REST client; it holds no secret and signs nothing itself). No thread, no heap of its own.

#pragma once

#include <hengyuan/account_refresh.hpp>
#include <hengyuan/binance_private_rest.hpp>

#include <cstdint>

namespace hy {

// What a PrivateRestError means to the account refresher. Every enumerator is named, so a new one cannot slip into Failed unseen (-Wswitch).
[[nodiscard]] constexpr AccountFetchOutcome account_fetch_outcome_of(PrivateRestError e) noexcept {
    switch (e) {
        case PrivateRestError::None:
            return AccountFetchOutcome::Fetched;
        // refused before the network was touched
        case PrivateRestError::InvalidConfig:
        case PrivateRestError::ClockNotFresh:
        case PrivateRestError::SigningFailed:
        case PrivateRestError::ClockSkewTooLarge:
        case PrivateRestError::RateLimitExhausted:
            return AccountFetchOutcome::NotSent;
        // the network was tried, or the answer cannot be used
        case PrivateRestError::Resolve:
        case PrivateRestError::Connect:
        case PrivateRestError::TlsHandshake:
        case PrivateRestError::Write:
        case PrivateRestError::Read:
        case PrivateRestError::HttpStatus:
        case PrivateRestError::JsonParse:
        case PrivateRestError::MalformedResponse:
        case PrivateRestError::CapacityExceeded:
            return AccountFetchOutcome::Failed;
    }
    return AccountFetchOutcome::Failed;  // an out-of-range value is no success
}

struct RestAccountFetcherStats {
    std::uint64_t calls{0};
    std::uint64_t fetched{0};
    std::uint64_t not_sent{0};
    std::uint64_t failed{0};
    std::uint64_t threw{0};                  // fetch_account() let an exception out: counted in `failed` as well
    PrivateRestError last_error{PrivateRestError::None};  // of the most recent call that did not fetch (None until one fails)
};

// A template only so that a test can hand it a client that throws or answers with any error, without a network: the production type is
// RestAccountFetcher below. `Client` needs PrivateRestError fetch_account(AccountSnapshot&).
template <typename Client>
class BasicRestAccountFetcher {
public:
    // The client outlives the fetcher and is used only on the thread that owns its credentials (see the header comment).
    explicit BasicRestAccountFetcher(Client& client) noexcept : client_(client) {}

    BasicRestAccountFetcher(const BasicRestAccountFetcher&) = delete;
    BasicRestAccountFetcher& operator=(const BasicRestAccountFetcher&) = delete;

    [[nodiscard]] AccountFetchPort port() noexcept { return AccountFetchPort{&BasicRestAccountFetcher::call, this}; }

    [[nodiscard]] const RestAccountFetcherStats& stats() const noexcept { return stats_; }

private:
    static AccountFetchOutcome call(void* self, AccountSnapshot& out) noexcept { return static_cast<BasicRestAccountFetcher*>(self)->fetch(out); }

    AccountFetchOutcome fetch(AccountSnapshot& out) noexcept {
        ++stats_.calls;
        AccountSnapshot fetched{};  // `out` is assigned on Fetched only, whatever fetch_account() does with its own argument
        PrivateRestError err = PrivateRestError::Read;
        try {
            err = client_.fetch_account(fetched);
        } catch (...) {
            ++stats_.threw;
            ++stats_.failed;
            stats_.last_error = PrivateRestError::Read;
            return AccountFetchOutcome::Failed;
        }
        const AccountFetchOutcome outcome = account_fetch_outcome_of(err);
        switch (outcome) {
            case AccountFetchOutcome::Fetched:
                out = fetched;
                ++stats_.fetched;
                return outcome;
            case AccountFetchOutcome::NotSent:
                ++stats_.not_sent;
                break;
            case AccountFetchOutcome::Failed:
                ++stats_.failed;
                break;
        }
        stats_.last_error = err;
        return outcome;
    }

    Client& client_;
    RestAccountFetcherStats stats_{};
};

using RestAccountFetcher = BasicRestAccountFetcher<BinancePrivateRestClient>;

// The startup read for the store binding: GET /api/v3/account once, and the uid the exchange reports for this key. `uid_out` is assigned only when the answer
// was good (None); the snapshot that came with it is thrown away here -- the refresher's own first fetch is the one the barrier and the trading gates see.
// Never throws: anything that escapes the client is a Read failure.
template <typename Client>
[[nodiscard]] inline PrivateRestError fetch_exchange_account_uid(Client& client, std::uint64_t& uid_out) noexcept {
    try {
        AccountSnapshot ignored{};
        std::uint64_t uid = 0;
        const PrivateRestError err = client.fetch_account(ignored, uid);
        if (err != PrivateRestError::None) return err;
        uid_out = uid;
        return PrivateRestError::None;
    } catch (...) {
        return PrivateRestError::Read;
    }
}

}  // namespace hy
