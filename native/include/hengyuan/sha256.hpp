// SPDX-License-Identifier: proprietary
// sha256.hpp — vendored SHA-256 + HMAC-SHA256 (FIPS 180-4 / RFC 2104).
//
// Governance: L1 (pure computation, no I/O, no network, no secret storage --
// the caller owns key material and this file never logs/persists it).
//
// WHY VENDORED, NOT OpenSSL: this repo's binance_signer.hpp already has a
// working OpenSSL HMAC-SHA256 wrapper, but OpenSSL is only linked when
// HY_BUILD_DEMO=ON (native/CMakeLists.txt) -- and neither this repo's
// documented MSVC runbook (CLAUDE.md) nor tools/wsl_verify.sh's TSan job pass
// that flag. Gating the durable audit log's checksum behind HY_BUILD_DEMO
// would mean it silently isn't built/tested in either of this repo's two
// "standard" verification loops. See docs/SPEC_INVARIANTS.md's "durable 审计
// 日志" entry for the full reasoning. Correctness here is pinned to the
// official NIST SHA-256 known-answer tests and RFC 4231's HMAC-SHA256 test
// vectors (test_sha256_hmac.cpp), not to "this looks right."
//
// Zero heap allocation: all state is fixed-size (std::array), matching
// CLAUDE.md's hot-path discipline even though this specific file isn't on the
// sub-microsecond hot path (it backs a blocking, once-per-append durable-log
// checksum) -- there's no engineering cost to keeping it allocation-free
// regardless.

#pragma once

#include <hengyuan/secure_wipe.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace hy::crypto {

// --- SHA-256 ---

// --- Constant-time comparison (audit SEC-MACCMP-010) ---
//
// std::memcmp returns at the FIRST differing byte, so its running time reveals
// the length of the matching prefix. For an authentication tag that is the
// classic byte-at-a-time forgery oracle: an attacker who can submit candidate
// tags and observe verification timing recovers a valid tag one byte at a time
// instead of needing 2^256 guesses.
//
// This codebase already knew that -- key_ring.hpp had a correct constant-time
// comparison and used it for its wrap tag -- but all seventeen MAC verifications
// in the durable-log codecs used std::memcmp. Living in one place now, so the two
// cannot drift apart again.
//
// The length check short-circuits deliberately: buffer LENGTHS are not secret
// here (every frame format has a fixed, publicly-known MAC width), only the
// contents are.
inline bool constant_time_equal(std::span<const std::byte> a,
                                 std::span<const std::byte> b) noexcept {
    if (a.size() != b.size()) return false;
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        // The XOR of two uint8_t operands promotes to int; the explicit cast back
        // before |= is required, not decorative -- GCC's -Wconversion flags the
        // implicit narrowing that MSVC /W4 does not (a real dual-toolchain
        // divergence this repo hit before, see key_ring.hpp's history).
        diff = static_cast<std::uint8_t>(
            diff | (static_cast<std::uint8_t>(a[i]) ^ static_cast<std::uint8_t>(b[i])));
    }
    return diff == 0;
}

struct Sha256Digest {
    std::array<std::uint8_t, 32> bytes{};

    // Byte view of this digest, for constant_time_equal() against wire bytes.
    std::span<const std::byte> as_bytes() const noexcept {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()),
                                           bytes.size());
    }

    // Constant-time, unlike std::array's own operator== (which is a lexicographic
    // element-wise compare that stops at the first difference). A digest type
    // should not hand out a timing-leaky equality by default.
    bool operator==(const Sha256Digest& other) const noexcept {
        return constant_time_equal(as_bytes(), other.as_bytes());
    }
    bool operator!=(const Sha256Digest& other) const noexcept { return !(*this == other); }
};

// Convenience: compare a freshly-computed digest against MAC bytes read off the
// wire. Both overloads exist because call sites hold the wire side either as a
// std::array or as a raw span into a decode buffer.
inline bool constant_time_equal(const Sha256Digest& digest, std::span<const std::byte> wire) noexcept {
    return constant_time_equal(digest.as_bytes(), wire);
}

namespace detail {

inline constexpr std::uint32_t rotr32(std::uint32_t x, int n) noexcept {
    return (x >> n) | (x << (32 - n));
}

// K[0..63]: fractional parts of the cube roots of the first 64 primes
// (FIPS 180-4 §4.2.2). Transcribed verbatim, not derived at runtime.
inline constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

}  // namespace detail

// Streaming SHA-256: update() any number of times, then finish() exactly once.
// Not reusable after finish() -- construct a fresh instance (or reset()) for
// the next digest.
class Sha256 {
public:
    Sha256() noexcept { reset(); }

    void reset() noexcept {
        // H[0..7]: fractional parts of the square roots of the first 8 primes
        // (FIPS 180-4 §5.3.3).
        state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                  0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        buffer_len_ = 0;
        total_len_ = 0;
        finished_ = false;
    }

    void update(std::span<const std::byte> data) noexcept {
        const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
        std::size_t n = data.size();
        total_len_ += n;

        if (buffer_len_ > 0) {
            const std::size_t take = (64 - buffer_len_) < n ? (64 - buffer_len_) : n;
            std::memcpy(buffer_.data() + buffer_len_, p, take);
            buffer_len_ += take;
            p += take;
            n -= take;
            if (buffer_len_ == 64) {
                process_block(buffer_.data());
                buffer_len_ = 0;
            }
        }

        while (n >= 64) {
            process_block(p);
            p += 64;
            n -= 64;
        }

        if (n > 0) {
            std::memcpy(buffer_.data() + buffer_len_, p, n);
            buffer_len_ += n;
        }
    }

    Sha256Digest finish() noexcept {
        // total_len_ was accumulated in bytes over every update() call BEFORE
        // padding is appended -- padding must never itself be counted.
        const std::uint64_t bit_len = total_len_ * 8;

        std::uint8_t pad = 0x80;
        update_raw(&pad, 1);

        // Pad with zeros until buffer_len_ == 56 (mod 64), leaving exactly 8
        // bytes for the length field to land at the end of a 64-byte block.
        static constexpr std::uint8_t kZero = 0;
        while (buffer_len_ != 56) {
            update_raw(&kZero, 1);
        }

        std::array<std::uint8_t, 8> len_be{};
        for (int i = 0; i < 8; ++i) {
            len_be[static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>(bit_len >> (56 - 8 * i));
        }
        update_raw(len_be.data(), 8);

        Sha256Digest out{};
        for (int i = 0; i < 8; ++i) {
            const std::uint32_t w = state_[static_cast<std::size_t>(i)];
            out.bytes[static_cast<std::size_t>(i) * 4 + 0] = static_cast<std::uint8_t>(w >> 24);
            out.bytes[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(w >> 16);
            out.bytes[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>(w >> 8);
            out.bytes[static_cast<std::size_t>(i) * 4 + 3] = static_cast<std::uint8_t>(w);
        }
        finished_ = true;
        return out;
    }

    bool finished() const noexcept { return finished_; }

private:
    // finish()'s own padding writes go through the same block-accumulation
    // path as update() -- this is that path, taking a raw pointer so finish()
    // doesn't need a std::span over a single stack byte.
    void update_raw(const std::uint8_t* p, std::size_t n) noexcept {
        std::size_t total_len_before = total_len_;  // padding must not perturb bit_len
        update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(p), n));
        total_len_ = total_len_before;
    }

    void process_block(const std::uint8_t* block) noexcept {
        using detail::rotr32;
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            const auto idx = static_cast<std::size_t>(i) * 4;
            w[static_cast<std::size_t>(i)] =
                (static_cast<std::uint32_t>(block[idx]) << 24) |
                (static_cast<std::uint32_t>(block[idx + 1]) << 16) |
                (static_cast<std::uint32_t>(block[idx + 2]) << 8) |
                static_cast<std::uint32_t>(block[idx + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t w15 = w[static_cast<std::size_t>(i) - 15];
            const std::uint32_t w2 = w[static_cast<std::size_t>(i) - 2];
            const std::uint32_t s0 = rotr32(w15, 7) ^ rotr32(w15, 18) ^ (w15 >> 3);
            const std::uint32_t s1 = rotr32(w2, 17) ^ rotr32(w2, 19) ^ (w2 >> 10);
            w[static_cast<std::size_t>(i)] = w[static_cast<std::size_t>(i) - 16] + s0 +
                                              w[static_cast<std::size_t>(i) - 7] + s1;
        }

        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + ch + detail::kRoundConstants[static_cast<std::size_t>(i)] +
                                         w[static_cast<std::size_t>(i)];
            const std::uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_len_{0};
    std::uint64_t total_len_{0};
    bool finished_{false};
};

inline Sha256Digest sha256(std::span<const std::byte> data) noexcept {
    Sha256 h;
    h.update(data);
    return h.finish();
}

// --- HMAC-SHA256 (RFC 2104) ---

// Streaming HMAC-SHA256: construct with the key, update() any number of
// times, then finish() exactly once. Not reusable after finish().
class HmacSha256 {
public:
    static constexpr std::size_t kBlockSize = 64;  // SHA-256's block size

    // AUDIT SEC-WIPE-018: key_block, ipad and opad_ are all trivially invertible
    // back to the key material (K0, K0^0x36, K0^0x5c). This repo goes to real
    // lengths to mlock and secure-wipe the KEK and the .env secret, so leaving the
    // derived HMAC key schedule sitting in memory -- opad_ for the object's whole
    // lifetime, ipad and key_block as stack residue after the constructor returns --
    // was inconsistent with its own stated discipline. All three are wiped now.
    explicit HmacSha256(std::span<const std::byte> key) noexcept {
        std::array<std::uint8_t, kBlockSize> key_block{};  // zero-padded K0
        if (key.size() > kBlockSize) {
            const Sha256Digest hashed = sha256(key);
            std::memcpy(key_block.data(), hashed.bytes.data(), hashed.bytes.size());
        } else if (!key.empty()) {
            // memcpy with a null source is UB even for size 0, and an empty span's
            // data() may legitimately be null (UBSan's nonnull check flags it).
            std::memcpy(key_block.data(), key.data(), key.size());
        }

        std::array<std::uint8_t, kBlockSize> ipad{};
        for (std::size_t i = 0; i < kBlockSize; ++i) {
            ipad[i] = static_cast<std::uint8_t>(key_block[i] ^ 0x36u);
            opad_[i] = static_cast<std::uint8_t>(key_block[i] ^ 0x5cu);
        }
        inner_.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(ipad.data()),
                                                   ipad.size()));
        secure_wipe(ipad.data(), ipad.size());
        secure_wipe(key_block.data(), key_block.size());
    }

    ~HmacSha256() { secure_wipe(opad_.data(), opad_.size()); }

    HmacSha256(const HmacSha256&) = delete;
    HmacSha256& operator=(const HmacSha256&) = delete;
    HmacSha256(HmacSha256&&) = delete;
    HmacSha256& operator=(HmacSha256&&) = delete;

    void update(std::span<const std::byte> data) noexcept { inner_.update(data); }

    Sha256Digest finish() noexcept {
        const Sha256Digest inner_digest = inner_.finish();
        Sha256 outer;
        outer.update(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(opad_.data()), opad_.size()));
        outer.update(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(inner_digest.bytes.data()), inner_digest.bytes.size()));
        return outer.finish();
    }

private:
    Sha256 inner_;
    std::array<std::uint8_t, kBlockSize> opad_{};
};

inline Sha256Digest hmac_sha256(std::span<const std::byte> key, std::span<const std::byte> data) noexcept {
    HmacSha256 h(key);
    h.update(data);
    return h.finish();
}

}  // namespace hy::crypto
