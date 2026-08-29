// SPDX-License-Identifier: proprietary
// binance_signer.hpp — HMAC-SHA256 signing for Binance Private API.
//
// SIMULATION INFRASTRUCTURE — NOT LIVE READY.
// D3-LIVE prerequisites ⑧(heartbeat)✅ ⑨(this) ⑩(manual confirm + regression).
// This code implements the cryptographic layer only. Actual private API
// submission requires explicit L5 authorization + D3-LIVE completion.
//
// Design (per markdown spec):
//   - OpenSSL HMAC_CTX pre-initialization: secret key pre-hashed at startup
//     (cold path), hot signing only resets+updates (30%+ crypto speedup)
//   - Zero heap allocation: pre-allocated fixed buffer, std::span interface
//   - Fail-closed: mlock() key memory (via hy::try_lock_memory), refuse on
//     any signing failure
//   - IO-thread bound: signer lives on the I/O thread, not hot path
//
// Governance: L5 code artifact (no network call, no real order).
// The signer produces HMAC signatures but does NOT submit them.

#pragma once

#include <hengyuan/secure_memory_lock.hpp>
#include <hengyuan/secure_wipe.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace hy {

// Pre-allocated signing buffer. No heap allocation during signing.
static constexpr std::size_t kSignBufSize = 2048;
static constexpr std::size_t kHmacLen = 32;  // SHA-256 = 32 bytes
static constexpr std::size_t kHexLen = 64;   // 32 bytes hex-encoded

class BinanceSigner {
public:
    enum class SignFailure : std::uint8_t {
        None = 0,
        NotInitialized = 1,      // !initialized_ or empty sign() payload
        InvalidSecret = 2,       // init() secret empty or exceeds secret_buf_
        LockFailed = 3,          // MemoryLockBehavior::lock() returned false
                                  // (or lock/unlock function pointer was null)
        CryptoInitFailed = 4,    // EVP_MAC_init / HMAC_Init_ex
        CryptoUpdateFailed = 5,  // EVP_MAC_update / HMAC_Update
        CryptoFinalFailed = 6,   // EVP_MAC_final / HMAC_Final
    };

    // Injection seam for hy::try_lock_memory/hy::unlock_memory, defaulted to
    // the real implementations. Exists so tests can deterministically drive
    // the lock-failure path (real mlock/VirtualLock failure depends on
    // system quota, not reproducible portably in a test).
    struct MemoryLockBehavior {
        bool (*lock)(void*, std::size_t) noexcept = &hy::try_lock_memory;
        void (*unlock)(void*, std::size_t) noexcept = &hy::unlock_memory;
    };

    BinanceSigner() = default;
    ~BinanceSigner() { destroy(); }

    BinanceSigner(const BinanceSigner&) = delete;
    BinanceSigner& operator=(const BinanceSigner&) = delete;

    // Initialize with API secret. Pre-hashes the key into the MAC context.
    // Call once at startup (cold path). Returns false on failure.
    bool init(std::string_view api_secret) noexcept { return init_impl(api_secret, {}); }

    // Sign a query string payload. Returns hex-encoded HMAC-SHA256.
    // Zero-alloc: uses pre-allocated internal buffer.
    // Fail-closed: returns empty span on any error.
    std::span<const char> sign(std::string_view payload) noexcept {
        last_failure_ = SignFailure::None;

        if (!initialized_ || payload.empty()) {
            last_failure_ = SignFailure::NotInitialized;
            ++stats_.failures;
            return {};
        }

        unsigned char hmac_raw[kHmacLen]{};

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        // Reset context (preserves pre-hashed key state)
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_utf8_string(
                "digest", const_cast<char*>("SHA256"), 0),
            OSSL_PARAM_construct_end()
        };
        if (!EVP_MAC_init(ctx_, nullptr, 0, params)) {
            last_failure_ = SignFailure::CryptoInitFailed;
            ++stats_.failures;
            return {};
        }
        if (!EVP_MAC_update(ctx_,
                            reinterpret_cast<const unsigned char*>(payload.data()),
                            payload.size())) {
            last_failure_ = SignFailure::CryptoUpdateFailed;
            ++stats_.failures;
            return {};
        }
        std::size_t out_len = kHmacLen;
        if (!EVP_MAC_final(ctx_, hmac_raw, &out_len, kHmacLen)) {
            last_failure_ = SignFailure::CryptoFinalFailed;
            ++stats_.failures;
            return {};
        }
#else
        // OpenSSL 1.1.x: re-init preserves key
        if (!HMAC_Init_ex(ctx_legacy_, nullptr, 0, nullptr, nullptr)) {
            last_failure_ = SignFailure::CryptoInitFailed;
            ++stats_.failures;
            return {};
        }
        if (!HMAC_Update(ctx_legacy_,
                         reinterpret_cast<const unsigned char*>(payload.data()),
                         payload.size())) {
            last_failure_ = SignFailure::CryptoUpdateFailed;
            ++stats_.failures;
            return {};
        }
        unsigned int len = 0;
        if (!HMAC_Final(ctx_legacy_, hmac_raw, &len)) {
            last_failure_ = SignFailure::CryptoFinalFailed;
            ++stats_.failures;
            return {};
        }
#endif

        // Hex-encode into pre-allocated buffer
        static constexpr char hex[] = "0123456789abcdef";
        for (std::size_t i = 0; i < kHmacLen; ++i) {
            hex_buf_[i * 2] = hex[hmac_raw[i] >> 4];
            hex_buf_[i * 2 + 1] = hex[hmac_raw[i] & 0x0F];
        }

        ++stats_.signed_ok;
        return std::span<const char>(hex_buf_.data(), kHexLen);
    }

    bool is_initialized() const noexcept { return initialized_; }
    SignFailure last_failure() const noexcept { return last_failure_; }

    struct SignerStats {
        std::uint64_t signed_ok{0};
        std::uint64_t failures{0};
    };
    const SignerStats& stats() const noexcept { return stats_; }

private:
    // Reset time is at entry, not inside destroy() — destroy() is also
    // called on the lock-failure path right after last_failure_ is set to
    // LockFailed, and a reset inside destroy() would clobber that value.
    bool init_impl(std::string_view api_secret, MemoryLockBehavior b) noexcept {
        last_failure_ = SignFailure::None;
        destroy();

        if (!b.lock || !b.unlock) {
            last_failure_ = SignFailure::LockFailed;
            return false;
        }
        if (api_secret.empty() || api_secret.size() > secret_buf_.size()) {
            last_failure_ = SignFailure::InvalidSecret;
            return false;
        }

        // AUDIT L4-LOCK-ORDER-003: lock the (still all-zero, post-destroy())
        // buffer BEFORE writing the secret into it, not after — the old
        // order left the plaintext secret sitting in ordinary (swappable)
        // memory for the entire gap between memcpy() and a successful
        // lock(). Locking empty memory first and only then copying in the
        // real bytes closes that window entirely.
        is_locked_ = b.lock(secret_buf_.data(), secret_buf_.size());
        if (!is_locked_) {
            last_failure_ = SignFailure::LockFailed;
            destroy();
            return false;
        }
        unlock_fn_ = b.unlock;

        std::memcpy(secret_buf_.data(), api_secret.data(), api_secret.size());
        secret_len_ = api_secret.size();

        // Pre-initialize HMAC context with the key (EVP API, OpenSSL 3.x safe)
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        mac_ = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
        if (!mac_) {
            last_failure_ = SignFailure::CryptoInitFailed;
            destroy();
            return false;
        }
        ctx_ = EVP_MAC_CTX_new(mac_);
        if (!ctx_) {
            last_failure_ = SignFailure::CryptoInitFailed;
            destroy();
            return false;
        }
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_utf8_string(
                "digest", const_cast<char*>("SHA256"), 0),
            OSSL_PARAM_construct_end()
        };
        if (!EVP_MAC_init(ctx_, reinterpret_cast<const unsigned char*>(secret_buf_.data()),
                          secret_len_, params)) {
            last_failure_ = SignFailure::CryptoInitFailed;
            destroy();
            return false;
        }
#else
        // OpenSSL 1.1.x fallback
        ctx_legacy_ = HMAC_CTX_new();
        if (!ctx_legacy_) {
            last_failure_ = SignFailure::CryptoInitFailed;
            destroy();
            return false;
        }
        if (!HMAC_Init_ex(ctx_legacy_,
                          secret_buf_.data(), static_cast<int>(secret_len_),
                          EVP_sha256(), nullptr)) {
            last_failure_ = SignFailure::CryptoInitFailed;
            destroy();
            return false;
        }
#endif

        initialized_ = true;
        return true;
    }

    void destroy() noexcept {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        if (ctx_) { EVP_MAC_CTX_free(ctx_); ctx_ = nullptr; }
        if (mac_) { EVP_MAC_free(mac_); mac_ = nullptr; }
#else
        if (ctx_legacy_) { HMAC_CTX_free(ctx_legacy_); ctx_legacy_ = nullptr; }
#endif
        hy::secure_wipe(secret_buf_.data(), secret_buf_.size());
        secret_len_ = 0;
        initialized_ = false;
        if (is_locked_) {
            unlock_fn_(secret_buf_.data(), secret_buf_.size());
            is_locked_ = false;
        }
        // last_failure_ is NOT touched here — reset happens at init_impl()
        // entry so a caller can still observe why the previous attempt
        // failed after destroy() has already run as part of that failure.
    }

    bool initialized_{false};
    bool is_locked_ = false;
    void (*unlock_fn_)(void*, std::size_t) noexcept = &hy::unlock_memory;
    SignFailure last_failure_ = SignFailure::None;
    std::array<char, 256> secret_buf_{};
    std::size_t secret_len_{0};
    std::array<char, kHexLen> hex_buf_{};
    SignerStats stats_{};

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    EVP_MAC* mac_{nullptr};
    EVP_MAC_CTX* ctx_{nullptr};
#else
    HMAC_CTX* ctx_legacy_{nullptr};
#endif

    friend class BinanceSignerTestHooks;   // unconditional; literal-identical
                                            // across every TU, no ODR risk.
    // BoundHmacCredentials (binance_environment.hpp) drives init_impl()
    // directly so it can forward its own injected MemoryLockBehavior
    // through to the internal signer, keeping both locks (api key +
    // secret) under a single test-injectable behavior instead of two
    // independently configured ones.
    friend class BoundHmacCredentials;
};

}  // namespace hy
