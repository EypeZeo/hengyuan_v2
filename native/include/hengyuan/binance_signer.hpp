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
//   - Fail-closed: mlock() key memory, refuse on any signing failure
//   - IO-thread bound: signer lives on the I/O thread, not hot path
//
// Governance: L5 code artifact (no network call, no real order).
// The signer produces HMAC signatures but does NOT submit them.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#ifdef __linux__
#include <sys/mman.h>
#endif

#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace hy {

// Pre-allocated signing buffer. No heap allocation during signing.
static constexpr std::size_t kSignBufSize = 2048;
static constexpr std::size_t kHmacLen = 32;  // SHA-256 = 32 bytes
static constexpr std::size_t kHexLen = 64;   // 32 bytes hex-encoded

class BinanceSigner {
public:
    BinanceSigner() = default;
    ~BinanceSigner() { destroy(); }

    BinanceSigner(const BinanceSigner&) = delete;
    BinanceSigner& operator=(const BinanceSigner&) = delete;

    // Initialize with API secret. Pre-hashes the key into HMAC_CTX.
    // Call once at startup (cold path). Returns false on failure.
    bool init(std::string_view api_secret) noexcept {
        if (api_secret.empty()) return false;

        // Copy secret to locked memory
        if (api_secret.size() > sizeof(secret_buf_)) return false;
        std::memcpy(secret_buf_.data(), api_secret.data(), api_secret.size());
        secret_len_ = api_secret.size();

#ifdef __linux__
        // mlock() prevents secret from being swapped to disk
        mlock(secret_buf_.data(), sizeof(secret_buf_));
#endif

        // Pre-initialize HMAC context with the key (EVP API, OpenSSL 3.x safe)
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        mac_ = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
        if (!mac_) return false;
        ctx_ = EVP_MAC_CTX_new(mac_);
        if (!ctx_) return false;
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_utf8_string(
                "digest", const_cast<char*>("SHA256"), 0),
            OSSL_PARAM_construct_end()
        };
        if (!EVP_MAC_init(ctx_, reinterpret_cast<const unsigned char*>(secret_buf_.data()),
                          secret_len_, params)) {
            destroy();
            return false;
        }
#else
        // OpenSSL 1.1.x fallback
        ctx_legacy_ = HMAC_CTX_new();
        if (!ctx_legacy_) return false;
        if (!HMAC_Init_ex(ctx_legacy_,
                          secret_buf_.data(), static_cast<int>(secret_len_),
                          EVP_sha256(), nullptr)) {
            destroy();
            return false;
        }
#endif

        initialized_ = true;
        return true;
    }

    // Sign a query string payload. Returns hex-encoded HMAC-SHA256.
    // Zero-alloc: uses pre-allocated internal buffer.
    // Fail-closed: returns empty span on any error.
    std::span<const char> sign(std::string_view payload) noexcept {
        if (!initialized_ || payload.empty()) {
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
            ++stats_.failures;
            return {};
        }
        if (!EVP_MAC_update(ctx_,
                            reinterpret_cast<const unsigned char*>(payload.data()),
                            payload.size())) {
            ++stats_.failures;
            return {};
        }
        std::size_t out_len = kHmacLen;
        if (!EVP_MAC_final(ctx_, hmac_raw, &out_len, kHmacLen)) {
            ++stats_.failures;
            return {};
        }
#else
        // OpenSSL 1.1.x: re-init preserves key
        if (!HMAC_Init_ex(ctx_legacy_, nullptr, 0, nullptr, nullptr)) {
            ++stats_.failures;
            return {};
        }
        if (!HMAC_Update(ctx_legacy_,
                         reinterpret_cast<const unsigned char*>(payload.data()),
                         payload.size())) {
            ++stats_.failures;
            return {};
        }
        unsigned int len = 0;
        if (!HMAC_Final(ctx_legacy_, hmac_raw, &len)) {
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

    struct SignerStats {
        std::uint64_t signed_ok{0};
        std::uint64_t failures{0};
    };
    const SignerStats& stats() const noexcept { return stats_; }

private:
    void destroy() noexcept {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        if (ctx_) { EVP_MAC_CTX_free(ctx_); ctx_ = nullptr; }
        if (mac_) { EVP_MAC_free(mac_); mac_ = nullptr; }
#else
        if (ctx_legacy_) { HMAC_CTX_free(ctx_legacy_); ctx_legacy_ = nullptr; }
#endif
        // Zero-wipe secret
        std::memset(secret_buf_.data(), 0, sizeof(secret_buf_));
        secret_len_ = 0;
        initialized_ = false;

#ifdef __linux__
        munlock(secret_buf_.data(), sizeof(secret_buf_));
#endif
    }

    bool initialized_{false};
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
};

}  // namespace hy
