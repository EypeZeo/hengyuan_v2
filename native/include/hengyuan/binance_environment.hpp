// SPDX-License-Identifier: proprietary
// binance_environment.hpp — Binance L4 §1 environment binding + credential
// loading/binding (BoundHmacCredentials).
//
// EnvironmentBinding: chosen once at process startup via ::testnet() or
// ::production() (no arguments) and is immutable for the process lifetime —
// no setter, no runtime flag, no public constructor to bypass the
// factories. Each factory hardcodes its own host/allowlist/credential-env-
// var-name tuple so a caller cannot mix testnet and production identifiers.
//
// BoundHmacCredentials: binds a loaded API key + HMAC secret to an
// EnvironmentBinding on a single owning thread. Construction is a heap
// allocation (via a private-constructor factory, not std::make_unique —
// see load_and_bind_credentials_impl()'s comment) that happens once at
// process startup, not on any signing/order hot path.
//
// QuerySigningError is defined here (not in binance_query_signing.hpp,
// which depends on this header) so that binance_environment.hpp does not
// need to #include binance_query_signing.hpp — CanonicalUnsignedQuery and
// SignedQuery are only forward-declared here, sufficient for the friend
// declaration build_signed_query() needs (a friend declaration doesn't
// require a complete type; only build_signed_query()'s own definition,
// which lives in binance_query_signing.hpp after it #includes this header,
// needs them complete). This one-directional include avoids a header cycle.
//
// Governance: L4 (offline logic only — no network call in this file; the
// actual GET/POST request construction lives elsewhere).

#pragma once

#include <hengyuan/binance_signer.hpp>
#include <hengyuan/env_loader.hpp>
#include <hengyuan/secure_memory_lock.hpp>
#include <hengyuan/secure_wipe.hpp>
#include <hengyuan/transport_policy.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace hy {

// Forward declarations only — BoundHmacCredentials's friend declaration for
// build_signed_query() names these types but never constructs or calls
// into them itself, so incomplete types are sufficient here.
class CanonicalUnsignedQuery;
class SignedQuery;

enum class QuerySigningError : std::uint8_t {
    Ok = 0,
    LockUnavailable = 1,      // MemoryLockBehavior's lock/unlock function
                               // pointers were null
    LockFailed = 2,           // a real lock (api key or secret) failed
    SignerInitFailed = 3,     // BinanceSigner::init_impl failed (crypto
                               // layer, or the secret itself was rejected)
    DuplicateParam = 4,       // build_canonical_query found a repeated key
    QueryTooLarge = 5,        // params count/length out of range; also used
                               // for a fresh_ts_ms value outside [0, 10^13)
    SigningFailed = 6,        // build_signed_query's internal creds.sign()
                               // call returned an empty span
    InvalidApiKeyFormat = 7,  // the API key string itself is malformed —
                               // distinct from SignerInitFailed (which
                               // covers the secret/crypto layer)
    ReservedParamName = 8,    // AUDIT L4-RESERVED-PARAM-004: a caller-
                               // supplied param key collided with "timestamp"
                               // or "signature" — the two names
                               // build_signed_query() appends itself after
                               // canonicalization. Distinct from
                               // DuplicateParam (a caller-vs-caller
                               // collision): this is a caller-vs-the-
                               // signing-machinery collision, which would
                               // otherwise produce a wire string with two
                               // "timestamp="/"signature=" occurrences —
                               // not forgeable (the signature covers both),
                               // but the server's choice of which one
                               // "wins" would diverge from caller intent.
};

enum class BinanceEnvironment : std::uint8_t {
    Testnet = 0,
    Production = 1,
};

// Only ::testnet()/::production() (no parameters) can produce a valid
// instance; the constructor is private. Both factories hardcode their own
// host, transport policy, and credential env-var names, so there is no
// parameter a caller could pass to accidentally cross environments.
class EnvironmentBinding {
public:
    static EnvironmentBinding testnet() noexcept {
        return EnvironmentBinding(BinanceEnvironment::Testnet, "testnet.binance.vision",
                                   testnet_transport_policy(),
                                   "HENGYUAN_BINANCE_TESTNET_API_KEY",
                                   "HENGYUAN_BINANCE_TESTNET_SECRET");
    }
    static EnvironmentBinding production() noexcept {
        return EnvironmentBinding(BinanceEnvironment::Production, "api.binance.com",
                                   production_transport_policy(),
                                   "HENGYUAN_BINANCE_LIVE_API_KEY",
                                   "HENGYUAN_BINANCE_LIVE_SECRET");
    }

    BinanceEnvironment environment() const noexcept { return environment_; }
    std::string_view base_host() const noexcept { return base_host_; }
    const TransportPolicy& transport_policy() const noexcept { return transport_policy_; }
    std::string_view api_key_env_key() const noexcept { return api_key_env_key_; }
    std::string_view secret_env_key() const noexcept { return secret_env_key_; }

private:
    EnvironmentBinding(BinanceEnvironment env, std::string_view host,
                       TransportPolicy policy, std::string_view api_key_env,
                       std::string_view secret_env) noexcept
        : environment_(env), base_host_(host), transport_policy_(policy),
          api_key_env_key_(api_key_env), secret_env_key_(secret_env) {}

    // Endpoint allowlist contains ONLY testnet.binance.vision — never
    // shares state with production_transport_policy().
    static TransportPolicy testnet_transport_policy() noexcept {
        TransportPolicy p{};
        p.endpoint_allowlist = EndpointAllowlist{};
        p.endpoint_allowlist.hosts[0] = "testnet.binance.vision";
        p.endpoint_allowlist.count = 1;
        return p;
    }
    // Default TransportPolicy already uses binance_default_endpoints()
    // (api.binance.com + api1/2/3.binance.com).
    static TransportPolicy production_transport_policy() noexcept { return TransportPolicy{}; }

    BinanceEnvironment environment_;
    std::string_view base_host_;
    TransportPolicy transport_policy_;
    std::string_view api_key_env_key_;
    std::string_view secret_env_key_;
};

static_assert(std::is_trivially_copyable_v<EnvironmentBinding>);

// Binance API keys are 64-character alphanumeric strings (NOT restricted to
// hex digits — unlike the HMAC signature, which is hex-encoded). Length +
// character-set check only; no attempt to validate it against a live key.
inline constexpr std::size_t kApiKeyLen = 64;
inline bool is_valid_api_key(std::string_view key) noexcept {
    if (key.size() != kApiKeyLen) return false;
    for (char c : key) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!ok) return false;
    }
    return true;
}

class BoundHmacCredentials {
public:
    // AUDIT L4-BINDING-REF-005: this used to take `const EnvironmentBinding&`
    // and store it in a `binding_` member for the object's whole lifetime —
    // a dangling-reference hazard if a caller passed a temporary. That
    // member was never actually read anywhere (dead weight, not a real
    // need), so it's gone: `binding` below is used only transiently, inside
    // this call, to read its env-var-key strings — nothing outlives the
    // call. A temporary EnvironmentBinding (e.g.
    // load_and_bind_credentials(EnvironmentBinding::testnet(), loader)) is
    // therefore now perfectly safe to pass, and compiles.
    static std::pair<QuerySigningError, std::unique_ptr<BoundHmacCredentials>>
    load_and_bind_credentials(const EnvironmentBinding& binding,
                               SecureEnvLoader& loader) noexcept {
        return load_and_bind_credentials_impl(binding, loader, {});
    }

    // true with written possibly 0: a legitimately empty key was read
    // successfully. false: wrong thread or destination buffer too small —
    // these two rejection reasons no longer share a single ambiguous "0"
    // result with a legitimate empty key.
    bool copy_api_key(std::span<char> out, std::size_t& written) const noexcept {
        if (std::this_thread::get_id() != owning_thread_id_) return false;
        if (api_key_len_ == 0) { written = 0; return true; }
        if (out.size() < api_key_len_) return false;
        std::memcpy(out.data(), api_key_buf_.data(), api_key_len_);
        written = api_key_len_;
        return true;
    }

    ~BoundHmacCredentials() noexcept { wipe_api_key(); }

private:
    BinanceSigner signer_{};
    std::array<char, 128> api_key_buf_{};
    std::size_t api_key_len_{0};
    // Deployment-visible contract: this factory must be called from the
    // same thread that will later call sign()/copy_api_key(). Violating
    // this doesn't crash — it silently fails closed on every subsequent
    // call, which is easy to misdiagnose as an unrelated bug.
    std::thread::id owning_thread_id_ = std::this_thread::get_id();

    // api_key_buf_ is a second, process-lifetime credential copy that gets
    // read on every signed request to build the X-MBX-APIKEY header — the
    // same shape of gap secure_memory_lock.hpp's AUDIT SEC-KEKCOPY-019
    // comment records as a real prior incident (KeyRing/
    // LastRemoteAckedTipStore each kept an unlocked copy of an already-
    // mlock'd KEK). Can't rely on heap layout accidentally colocating it
    // with signer_'s own locked secret_buf_ — lock it explicitly.
    bool is_api_key_locked_ = false;
    void (*api_key_unlock_fn_)(void*, std::size_t) noexcept = &hy::unlock_memory;

    // Private — only load_and_bind_credentials_impl() (a static member of
    // this class) can call it. Only this static member function can
    // construct a BoundHmacCredentials; nothing external can `new` one
    // out uninitialized.
    BoundHmacCredentials() noexcept = default;

    // Order: null-behavior gate first -> validate the api key format ->
    // lock the (still all-zero) api_key_buf_ (fail closed on lock failure)
    // -> only then copy the api key into it -> only then initialize the
    // internal signer_ (reusing the same injected behavior for both locks,
    // not two independent configurations; signer_.init_impl() applies the
    // identical lock-before-copy discipline to its own secret_buf_ — see
    // AUDIT L4-LOCK-ORDER-003 there). Locking empty memory first and only
    // then writing the real bytes in means neither credential ever sits in
    // ordinary (swappable) memory even for the gap between copy and lock.
    //
    // secret handling: borrowed as a string_view straight out of loader's
    // own already-protected buffer (SecureEnvLoader itself mlocks + wipes,
    // see env_loader.hpp) the entire time — never copied into a local
    // temporary. The first real copy of the secret happens inside
    // signer_.init_impl()'s own memcpy, which by then has already locked
    // its destination secret_buf_, and that path already has its own
    // destroy()-based wipe on failure.
    static std::pair<QuerySigningError, std::unique_ptr<BoundHmacCredentials>>
    load_and_bind_credentials_impl(const EnvironmentBinding& binding,
                                    SecureEnvLoader& loader,
                                    BinanceSigner::MemoryLockBehavior b) noexcept {
        if (!b.lock || !b.unlock) {
            return {QuerySigningError::LockUnavailable, nullptr};
        }

        std::string_view api_key = loader.get(binding.api_key_env_key());
        if (!is_valid_api_key(api_key)) {
            return {QuerySigningError::InvalidApiKeyFormat, nullptr};
        }

        // Direct new, not std::make_unique: make_unique<T>'s internal new
        // expression performs its access check in make_unique's own
        // definition context, not the caller's — it can never reach a
        // private constructor. A static member function of this class
        // does have that access, so the private constructor is only
        // reachable this way.
        auto creds = std::unique_ptr<BoundHmacCredentials>(
            new BoundHmacCredentials());

        // AUDIT L4-LOCK-ORDER-003: lock the (still all-zero, freshly
        // constructed) buffer BEFORE writing the api key into it, not
        // after — see binance_signer.hpp's init_impl() for the same fix and
        // its rationale. wipe_api_key() below is now a defensive no-op on
        // this path (nothing has been written yet), kept for minimal diff.
        if (!b.lock(creds->api_key_buf_.data(), creds->api_key_buf_.size())) {
            creds->wipe_api_key();
            return {QuerySigningError::LockFailed, nullptr};
        }
        creds->api_key_unlock_fn_ = b.unlock;
        creds->is_api_key_locked_ = true;

        std::memcpy(creds->api_key_buf_.data(), api_key.data(), api_key.size());
        creds->api_key_len_ = api_key.size();

        std::string_view secret = loader.get(binding.secret_env_key());
        if (!creds->signer_.init_impl(secret, b)) {
            creds->wipe_api_key();   // already-locked api key: precise
                                      // unlock + wipe, not left dangling
            return {QuerySigningError::SignerInitFailed, nullptr};
        }
        return {QuerySigningError::Ok, std::move(creds)};
    }

    void wipe_api_key() noexcept {
        hy::secure_wipe(api_key_buf_.data(), api_key_buf_.size());
        api_key_len_ = 0;
        if (is_api_key_locked_) {
            api_key_unlock_fn_(api_key_buf_.data(), api_key_buf_.size());
            is_api_key_locked_ = false;
        }
    }

    // Same single-owning-thread discipline as copy_api_key() above — sign()
    // mutates BinanceSigner's internal state (hex_buf_/stats_), so it must
    // never be called concurrently with itself or copy_api_key() from a
    // second thread.
    std::span<const char> sign(std::string_view payload) noexcept {
        if (std::this_thread::get_id() != owning_thread_id_) return {};
        return signer_.sign(payload);
    }

    friend class BoundHmacCredentialsTestHooks;   // unconditional; literal-
                                                    // identical across TUs.
    // build_signed_query() (binance_query_signing.hpp) needs to call
    // sign() above — its own header #includes this one, so the signature
    // below matches build_signed_query's real declaration exactly once
    // that header is visible.
    friend std::pair<QuerySigningError, SignedQuery> build_signed_query(
        BoundHmacCredentials&, const CanonicalUnsignedQuery&, std::int64_t) noexcept;
};

}  // namespace hy
