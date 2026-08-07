// SPDX-License-Identifier: proprietary
// key_ring.hpp — in-process HMAC key table + KEK-wrapped at-rest storage.
//
// Governance: L1 (pure computation, no file I/O -- kek_loader.hpp loads the
// KEK bytes, durable_audit_sink.hpp/a future ControlPlaneSink persist
// WrappedKeyRecord bytes to disk; this file never touches a filesystem).
// Zero heap allocation, matching sha256.hpp's own stated rationale: this
// isn't hot-path code (key rotation happens rarely, not per-order), but
// there's no engineering cost to staying allocation-free regardless.
//
// Implements docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md:1225 (§6.1.1.2)'s
// "derived HMAC keys are stored only wrapped under a process
// Key-Encryption-Key (KEK)... {key_id, wrapped_key_blob, wrap_alg}" -- see
// docs/SPEC_INVARIANTS.md's "Phase 0" entry for the design decisions this
// file implements (KEK source, wrap construction, why).
//
// ---------------------------------------------------------------------------
// THE WRAP CONSTRUCTION (HY-KEKWRAP-v1) IS NOT A STANDARD AEAD.
// ---------------------------------------------------------------------------
// This is a bespoke construction built entirely from crypto::hmac_sha256
// (sha256.hpp, already vendored and already verified against RFC 4231 +
// NIST known-answer vectors) -- not OpenSSL, not a vendored AES-GCM. There
// is no third-party known-answer test vector for THIS construction as a
// whole; its correctness is verified by round-trip tests and tamper tests
// (test_key_ring.cpp), not by comparing against a published reference
// implementation. Do not describe this as "AES" or "a standard AEAD" in any
// comment, log message, or documentation -- it is neither.
//
// Construction, given a 32-byte KEK (kek_loader.hpp):
//   enc_subkey  = HMAC-SHA256(KEK, "HY-KEKWRAP-v1-ENC")
//   tag_subkey  = HMAC-SHA256(KEK, "HY-KEKWRAP-v1-TAG")
//   salt        = HMAC-SHA256(KEK, "HY-KEKWRAP-v1-SALT" || key_id_LE)
//     (deterministic from key_id, not caller-supplied randomness -- safe
//      because key_id is spec-guaranteed unique and never reused, "MUST be
//      incremented on each API-secret rotation" per §6.1.1.2; a monotonic
//      counter used as a nonce is a standard, well-understood pattern, and
//      this avoids depending on a CSPRNG this codebase doesn't otherwise
//      provide.)
//   keystream   = HMAC-SHA256(enc_subkey, "HY-KEKWRAP-v1" || key_id_LE ||
//                   salt || block_counter_u8) for block_counter in {0, 1},
//                 concatenated to kKeyBlockSize (64) bytes
//   wrapped_key_blob = plaintext_key_block XOR keystream
//   tag         = HMAC-SHA256(tag_subkey, "HY-KEKWRAP-v1-TAG" || key_id_LE
//                   || wrapped_key_blob)
//     (key_id is bound into the tag domain so a record with its key_id
//      swapped, independent of wrapped_key_blob, also fails verification)
//
// Encrypt and tag use independently-derived subkeys (key separation) --
// never the same key material for both roles.

#pragma once

#include <hengyuan/kek_loader.hpp>  // kKekSize
#include <hengyuan/secure_wipe.hpp>
#include <hengyuan/sha256.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace hy {

// The "derived HMAC key" this whole file manages is always stored and
// wrapped in its K0-normalized (block-sized) form -- the same normalization
// HmacSha256's own constructor applies internally (hash-if-too-long,
// zero-pad-if-shorter, RFC 2104), and the same fixed representation
// DurableAuditSink already uses today for its single non-rotating key
// (durable_audit_sink.hpp's own kKeyBlockSize/key_block_).
inline constexpr std::size_t kKeyBlockSize = crypto::HmacSha256::kBlockSize;  // 64

// Maximum simultaneously-live key_ids this ring can hold. Key rotation is a
// rare, operator-driven event (one new key_id per API-secret rotation) --
// this is a generous bound for that cadence, not a hot-path capacity limit.
// A fixed array, not a heap-backed container, per this file's zero-heap rule.
inline constexpr std::size_t kMaxLiveKeys = 16;

// At-rest representation -- what actually gets persisted. Never contains
// plaintext key bytes.
struct WrappedKeyRecord {
    std::uint32_t key_id{0};
    std::array<std::byte, kKeyBlockSize> wrapped_key_blob{};
    std::array<std::byte, 32> tag{};  // HMAC-SHA256 output size
};

enum class KeyRingAddStatus : std::uint8_t {
    Ok = 0,
    DuplicateKeyId = 1,  // key_id already active -- never silently overwrite
    TableFull = 2,       // kMaxLiveKeys live entries already; operator must
                          // retire something first
};

enum class KeyRingLoadStatus : std::uint8_t {
    Ok = 0,
    TagMismatch = 1,     // tampered wrapped_key_blob, tag, or key_id
    DuplicateKeyId = 2,
    TableFull = 3,
};

namespace detail {

inline void kek_write_u32_le(std::byte* p, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::byte>(v >> (8 * i));
    }
}

// Computes HMAC-SHA256(key, domain_prefix || extra...) where extra is a
// caller-assembled byte buffer -- a small helper so the wrap/unwrap logic
// below doesn't repeat span-concatenation boilerplate for every call.
inline crypto::Sha256Digest hmac_over(std::span<const std::byte> key,
                                       std::string_view domain_prefix,
                                       std::span<const std::byte> extra = {}) noexcept {
    crypto::HmacSha256 h(key);
    h.update(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(domain_prefix.data()), domain_prefix.size()));
    if (!extra.empty()) h.update(extra);
    return h.finish();
}

}  // namespace detail

class KeyRing {
public:
    explicit KeyRing(std::span<const std::byte, kKekSize> kek) noexcept {
        const auto enc = detail::hmac_over(kek, "HY-KEKWRAP-v1-ENC");
        const auto tag = detail::hmac_over(kek, "HY-KEKWRAP-v1-TAG");
        std::memcpy(enc_subkey_.data(), enc.bytes.data(), enc.bytes.size());
        std::memcpy(tag_subkey_.data(), tag.bytes.data(), tag.bytes.size());
        kek_copy_ = std::array<std::byte, kKekSize>{};
        std::memcpy(kek_copy_.data(), kek.data(), kek.size());
    }

    ~KeyRing() { wipe_all(); }

    KeyRing(const KeyRing&) = delete;
    KeyRing& operator=(const KeyRing&) = delete;
    KeyRing(KeyRing&&) = delete;
    KeyRing& operator=(KeyRing&&) = delete;

    // Normalizes plaintext_key to K0 form, registers it as the active key
    // for key_id, and returns the wrapped record the caller must persist.
    // Refuses a duplicate key_id (never silently overwrites a live key) and
    // refuses when the table is full (caller must retire something first).
    KeyRingAddStatus add_key(std::uint32_t key_id, std::span<const std::byte> plaintext_key,
                              WrappedKeyRecord& out) noexcept {
        if (find_slot(key_id) != kNotFound) return KeyRingAddStatus::DuplicateKeyId;
        const std::size_t slot = find_free_slot();
        if (slot == kNotFound) return KeyRingAddStatus::TableFull;

        std::array<std::byte, kKeyBlockSize> key_block = normalize_to_block(plaintext_key);

        out = wrap(key_id, key_block);

        entries_[slot].active = true;
        entries_[slot].key_id = key_id;
        entries_[slot].key = key_block;
        secure_wipe(key_block.data(), key_block.size());
        return KeyRingAddStatus::Ok;
    }

    // Verifies `record`'s tag and, if valid, registers the unwrapped key as
    // active. On TagMismatch, the table is left unmodified -- never partially
    // trust a record that fails verification.
    KeyRingLoadStatus load_wrapped_key(const WrappedKeyRecord& record) noexcept {
        if (find_slot(record.key_id) != kNotFound) return KeyRingLoadStatus::DuplicateKeyId;

        const auto expected_tag = compute_tag(record.key_id, record.wrapped_key_blob);
        if (!constant_time_equal(expected_tag, record.tag)) {
            return KeyRingLoadStatus::TagMismatch;
        }

        const std::size_t slot = find_free_slot();
        if (slot == kNotFound) return KeyRingLoadStatus::TableFull;

        std::array<std::byte, kKeyBlockSize> key_block =
            unwrap_verified(record.key_id, record.wrapped_key_blob);

        entries_[slot].active = true;
        entries_[slot].key_id = record.key_id;
        entries_[slot].key = key_block;
        secure_wipe(key_block.data(), key_block.size());
        return KeyRingLoadStatus::Ok;
    }

    // Looks up the K0-normalized key material for key_id. Returns false
    // (out left untouched) if key_id is unknown or already retired -- the
    // caller (DurableAuditSink/ControlPlaneSink recovery, Phase 2/4) MUST
    // fence on false, never fall back to "try another key."
    bool active_key(std::uint32_t key_id, std::array<std::byte, kKeyBlockSize>& out) const noexcept {
        const std::size_t slot = find_slot(key_id);
        if (slot == kNotFound) return false;
        out = entries_[slot].key;
        return true;
    }

    // Removes key_id from the active table and secure-wipes its in-memory copy.
    // Returns true iff a key was actually removed.
    //
    // The CALLER is responsible for having already confirmed no retained
    // frame/tip/journal/bridge/migration record still needs this key_id
    // (docs/SPEC_INVARIANTS.md's Phase 0 entry) -- this method does not track usage,
    // it only performs the retirement once asked.
    //
    // AUDIT KEY-RETIRE-009, read this before calling: that precondition is currently
    // UNSATISFIABLE for any key that has ever signed a durable-log frame.
    // DurableAuditSink has no compaction, so its log retains every frame forever and
    // its recovery scan re-resolves each frame's own key_id from offset 0 on every
    // start (durable_audit_sink.hpp). Retiring such a key therefore does not free
    // anything -- it turns the entire log Corrupt on the next restart and fences the
    // sink permanently. Use DurableAuditSink::observed_key_ids() to check the
    // precondition before calling this; it exists for exactly that.
    //
    // The practical consequence is a ceiling: with kMaxLiveKeys slots and no safe way
    // to free one, a store supports at most kMaxLiveKeys key rotations over its
    // lifetime. Lifting that needs compaction (the CompactionCandidateIntent family
    // is declared in durable_control_plane.hpp but not implemented), not a bigger
    // array here. This used to return void, so a caller could not even observe
    // whether a retirement happened.
    bool retire(std::uint32_t key_id) noexcept {
        const std::size_t slot = find_slot(key_id);
        if (slot == kNotFound) return false;
        secure_wipe(entries_[slot].key.data(), entries_[slot].key.size());
        entries_[slot].active = false;
        entries_[slot].key_id = 0;
        return true;
    }

    // Live key count and the hard ceiling, so a caller can see the kMaxLiveKeys
    // limit approaching instead of discovering it as a TableFull at rotation time
    // (audit KEY-RETIRE-009).
    std::size_t live_key_count() const noexcept {
        std::size_t n = 0;
        for (const auto& e : entries_) {
            if (e.active) ++n;
        }
        return n;
    }
    static constexpr std::size_t capacity() noexcept { return kMaxLiveKeys; }

    void wipe_all() noexcept {
        for (auto& e : entries_) {
            secure_wipe(e.key.data(), e.key.size());
            e.active = false;
            e.key_id = 0;
        }
        secure_wipe(enc_subkey_.data(), enc_subkey_.size());
        secure_wipe(tag_subkey_.data(), tag_subkey_.size());
        secure_wipe(kek_copy_.data(), kek_copy_.size());
    }

private:
    static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

    struct Entry {
        bool active{false};
        std::uint32_t key_id{0};
        std::array<std::byte, kKeyBlockSize> key{};
    };

    std::size_t find_slot(std::uint32_t key_id) const noexcept {
        for (std::size_t i = 0; i < kMaxLiveKeys; ++i) {
            if (entries_[i].active && entries_[i].key_id == key_id) return i;
        }
        return kNotFound;
    }

    std::size_t find_free_slot() const noexcept {
        for (std::size_t i = 0; i < kMaxLiveKeys; ++i) {
            if (!entries_[i].active) return i;
        }
        return kNotFound;
    }

    static std::array<std::byte, kKeyBlockSize> normalize_to_block(
        std::span<const std::byte> raw_key) noexcept {
        std::array<std::byte, kKeyBlockSize> block{};
        if (raw_key.size() > kKeyBlockSize) {
            const auto hashed = crypto::sha256(raw_key);
            std::memcpy(block.data(), hashed.bytes.data(), hashed.bytes.size());
        } else {
            std::memcpy(block.data(), raw_key.data(), raw_key.size());
        }
        return block;
    }

    std::array<std::byte, 32> derive_salt(std::uint32_t key_id) const noexcept {
        std::array<std::byte, 4> key_id_le{};
        detail::kek_write_u32_le(key_id_le.data(), key_id);
        const auto salt = detail::hmac_over(kek_copy_, "HY-KEKWRAP-v1-SALT", key_id_le);
        std::array<std::byte, 32> out{};
        std::memcpy(out.data(), salt.bytes.data(), salt.bytes.size());
        return out;
    }

    static std::span<const std::byte> domain_bytes(std::string_view domain) noexcept {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(domain.data()),
                                           domain.size());
    }

    std::array<std::byte, kKeyBlockSize> keystream(std::uint32_t key_id,
                                                     std::span<const std::byte, 32> salt) const noexcept {
        std::array<std::byte, 4> key_id_le{};
        detail::kek_write_u32_le(key_id_le.data(), key_id);

        std::array<std::byte, kKeyBlockSize> out{};
        static_assert(kKeyBlockSize % 32 == 0, "keystream block loop assumes 32-byte HMAC blocks");
        for (std::uint8_t block_counter = 0; block_counter < kKeyBlockSize / 32; ++block_counter) {
            crypto::HmacSha256 h(enc_subkey_);
            h.update(domain_bytes("HY-KEKWRAP-v1"));
            h.update(key_id_le);
            h.update(salt);
            const std::byte counter_byte = static_cast<std::byte>(block_counter);
            h.update(std::span<const std::byte>(&counter_byte, 1));
            const auto digest = h.finish();
            std::memcpy(out.data() + (static_cast<std::size_t>(block_counter) * 32),
                        digest.bytes.data(), digest.bytes.size());
        }
        return out;
    }

    std::array<std::byte, 32> compute_tag(std::uint32_t key_id,
                                           const std::array<std::byte, kKeyBlockSize>& wrapped) const noexcept {
        std::array<std::byte, 4> key_id_le{};
        detail::kek_write_u32_le(key_id_le.data(), key_id);

        crypto::HmacSha256 h(tag_subkey_);
        h.update(domain_bytes("HY-KEKWRAP-v1-TAG"));
        h.update(key_id_le);
        h.update(wrapped);
        const auto digest = h.finish();
        std::array<std::byte, 32> out{};
        std::memcpy(out.data(), digest.bytes.data(), digest.bytes.size());
        return out;
    }

    // Was a private copy of this logic. Promoted to crypto::constant_time_equal
    // (sha256.hpp) in audit SEC-MACCMP-010, when it turned out the durable-log
    // codecs had seventeen std::memcmp tag comparisons that should have been using
    // this exact function. One definition, so they cannot drift apart again.
    static bool constant_time_equal(const std::array<std::byte, 32>& a,
                                     const std::array<std::byte, 32>& b) noexcept {
        return crypto::constant_time_equal(std::span<const std::byte>(a),
                                            std::span<const std::byte>(b));
    }

    WrappedKeyRecord wrap(std::uint32_t key_id,
                          const std::array<std::byte, kKeyBlockSize>& plaintext_block) const noexcept {
        const auto salt = derive_salt(key_id);
        const auto stream = keystream(key_id, salt);

        WrappedKeyRecord out{};
        out.key_id = key_id;
        for (std::size_t i = 0; i < kKeyBlockSize; ++i) {
            out.wrapped_key_blob[i] =
                static_cast<std::byte>(static_cast<std::uint8_t>(plaintext_block[i]) ^
                                        static_cast<std::uint8_t>(stream[i]));
        }
        out.tag = compute_tag(key_id, out.wrapped_key_blob);
        return out;
    }

    // Caller MUST have already verified the tag before calling this --
    // unwrap alone never re-checks it, matching "verify then use" discipline
    // rather than trusting the caller to check afterward.
    std::array<std::byte, kKeyBlockSize> unwrap_verified(
        std::uint32_t key_id, const std::array<std::byte, kKeyBlockSize>& wrapped) const noexcept {
        const auto salt = derive_salt(key_id);
        const auto stream = keystream(key_id, salt);

        std::array<std::byte, kKeyBlockSize> out{};
        for (std::size_t i = 0; i < kKeyBlockSize; ++i) {
            out[i] = static_cast<std::byte>(static_cast<std::uint8_t>(wrapped[i]) ^
                                             static_cast<std::uint8_t>(stream[i]));
        }
        return out;
    }

    std::array<std::byte, 32> enc_subkey_{};
    std::array<std::byte, 32> tag_subkey_{};
    std::array<std::byte, kKekSize> kek_copy_{};  // retained for salt derivation; wiped on destruction
    std::array<Entry, kMaxLiveKeys> entries_{};
};

}  // namespace hy
