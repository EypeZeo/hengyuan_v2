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
#include <hengyuan/secure_memory_lock.hpp>
#include <hengyuan/secure_wipe.hpp>
#include <hengyuan/sha256.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
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

// Round D (docs/SPEC_INVARIANTS.md's "Seal-journal Round D" entry, review
// P0-4/P1-2): retire() used to return bool, which can't distinguish "no such
// key" from "this key is currently pinned and cannot be retired yet" -- a
// caller silently treating KeyPinned as "already gone" would be a real bug.
enum class RetireStatus : std::uint8_t {
    Retired = 0,    // was the old `true`
    NotFound = 1,   // was the old `false`
    KeyPinned = 2,  // new: cannot retire while a PinnedKeyHandle for this
                    // key_id is still alive
};

enum class PinStatus : std::uint8_t {
    Pinned = 0,
    NotFound = 1,
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

class KeyRing;

// A pinned, cold-path-safe snapshot of one key_id's K0-normalized key
// material (Round D review P0-4/P0-2). Obtained only via
// KeyRing::pin_key(key_id) -- there is no public constructor, matching this
// codebase's established "verified/derived types have no public constructor"
// idiom (compaction_intent_codec.hpp's Verified* types). While a
// PinnedKeyHandle for a given key_id is alive, KeyRing::retire() on that
// key_id fails closed with RetireStatus::KeyPinned instead of silently
// wiping key material a concurrent reader is using.
//
// LIFETIME CONTRACT (read this before storing one): a PinnedKeyHandle MUST
// NOT outlive the KeyRing that produced it. This is a real use-after-free if
// violated, not a documented-but-harmless precondition -- KeyRing::~KeyRing()
// enforces it by calling std::terminate() in ALL build types (not a
// debug-only assert) if any pin is still live at destruction time, so a
// violation fails loudly and immediately rather than corrupting memory
// later when the dangling handle's own destructor runs. The owning runtime
// must destroy every IntentStore (and anything else holding a
// PinnedKeyHandle) before destroying the KeyRing they share.
class PinnedKeyHandle {
public:
    ~PinnedKeyHandle() noexcept;

    PinnedKeyHandle(const PinnedKeyHandle&) = delete;
    PinnedKeyHandle& operator=(const PinnedKeyHandle&) = delete;

    PinnedKeyHandle(PinnedKeyHandle&& other) noexcept
        : ring_(other.ring_), key_id_(other.key_id_), key_(other.key_) {
        other.ring_ = nullptr;  // moved-from: destructor becomes a no-op, never double-releases
    }
    PinnedKeyHandle& operator=(PinnedKeyHandle&& other) noexcept;

    std::uint32_t key_id() const noexcept { return key_id_; }
    std::span<const std::byte> key_bytes() const noexcept {
        return std::span<const std::byte>(key_.data(), key_.size());
    }

private:
    friend class KeyRing;
    PinnedKeyHandle(KeyRing& ring, std::uint32_t key_id,
                     const std::array<std::byte, kKeyBlockSize>& key) noexcept
        : ring_(&ring), key_id_(key_id), key_(key) {}

    KeyRing* ring_{nullptr};
    std::uint32_t key_id_{0};
    std::array<std::byte, kKeyBlockSize> key_{};
};

struct PinResult {
    PinStatus status{PinStatus::NotFound};
    std::optional<PinnedKeyHandle> handle{};
};

class KeyRing {
public:
    explicit KeyRing(std::span<const std::byte, kKekSize> kek) noexcept {
        // AUDIT SEC-KEKCOPY-019: everything secret this object owns -- the retained
        // KEK copy, the two derived subkeys, and every unwrapped key in entries_ --
        // is locked out of swap before anything is written into it. kek_loader.hpp
        // mlocks the ORIGINAL KEK; without this, copying it in here silently undid
        // that for the whole process lifetime. Best-effort by necessity: this is a
        // noexcept constructor with no error channel, so the outcome is recorded
        // (memory_locked()) rather than made fatal the way kek_loader.hpp can.
        memory_locked_ = try_lock_memory(this, sizeof(KeyRing));

        const auto enc = detail::hmac_over(kek, "HY-KEKWRAP-v1-ENC");
        const auto tag = detail::hmac_over(kek, "HY-KEKWRAP-v1-TAG");
        std::memcpy(enc_subkey_.data(), enc.bytes.data(), enc.bytes.size());
        std::memcpy(tag_subkey_.data(), tag.bytes.data(), tag.bytes.size());
        kek_copy_ = std::array<std::byte, kKekSize>{};
        std::memcpy(kek_copy_.data(), kek.data(), kek.size());
    }

    // Round D review P0-2: fail-closed in ALL build types, not a debug-only
    // assert. A live PinnedKeyHandle outliving its KeyRing is a real
    // use-after-free waiting to happen the moment that handle's destructor
    // runs (it would call release_pin() on already-freed memory); terminate
    // now, loudly and diagnosably, rather than let that UAF happen silently
    // later. See PinnedKeyHandle's own doc comment for the shutdown-ordering
    // contract this enforces.
    ~KeyRing() {
        {
            std::lock_guard<std::mutex> lock(entries_mutex_);
            for (const auto& e : entries_) {
                if (e.pin_count > 0) {
                    std::terminate();
                }
            }
        }
        wipe_all();
        if (memory_locked_) unlock_memory(this, sizeof(KeyRing));
    }

    // False means the platform refused to lock this object's pages (RLIMIT_MEMLOCK,
    // working-set quota, or an unsupported platform). Key material is still wiped on
    // destruction; it is only the swap guarantee that is absent. Callers that
    // require it should surface this rather than assume it.
    bool memory_locked() const noexcept { return memory_locked_; }

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
        std::array<std::byte, kKeyBlockSize> key_block = normalize_to_block(plaintext_key);
        out = wrap(key_id, key_block);  // wrap() only touches enc_subkey_/tag_subkey_/kek_copy_,
                                          // immutable after construction -- no lock needed here

        std::lock_guard<std::mutex> lock(entries_mutex_);
        if (find_slot(key_id) != kNotFound) {
            secure_wipe(key_block.data(), key_block.size());
            return KeyRingAddStatus::DuplicateKeyId;
        }
        const std::size_t slot = find_free_slot();
        if (slot == kNotFound) {
            secure_wipe(key_block.data(), key_block.size());
            return KeyRingAddStatus::TableFull;
        }

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
        // Tag verification and unwrap only touch enc_subkey_/tag_subkey_/
        // kek_copy_ (immutable after construction) plus the caller-supplied
        // record -- no lock needed for this part.
        const auto expected_tag = compute_tag(record.key_id, record.wrapped_key_blob);
        if (!constant_time_equal(expected_tag, record.tag)) {
            return KeyRingLoadStatus::TagMismatch;
        }
        std::array<std::byte, kKeyBlockSize> key_block =
            unwrap_verified(record.key_id, record.wrapped_key_blob);

        std::lock_guard<std::mutex> lock(entries_mutex_);
        if (find_slot(record.key_id) != kNotFound) {
            secure_wipe(key_block.data(), key_block.size());
            return KeyRingLoadStatus::DuplicateKeyId;
        }
        const std::size_t slot = find_free_slot();
        if (slot == kNotFound) {
            secure_wipe(key_block.data(), key_block.size());
            return KeyRingLoadStatus::TableFull;
        }

        entries_[slot].active = true;
        entries_[slot].key_id = record.key_id;
        entries_[slot].key = key_block;
        secure_wipe(key_block.data(), key_block.size());
        return KeyRingLoadStatus::Ok;
    }

    // Looks up the K0-normalized key material for key_id. Returns false
    // (out left untouched) if key_id is unknown or already retired -- the
    // caller (DurableAuditSink/ControlPlaneSink recovery, Phase 2/4) MUST
    // fence on false, never fall back to "try another key." Prefer
    // pin_key() for any caller whose use of the returned bytes might race
    // with a concurrent retire() (Round D review P0-4) -- this method
    // returns a plain copy with no such protection.
    bool active_key(std::uint32_t key_id, std::array<std::byte, kKeyBlockSize>& out) const noexcept {
        std::lock_guard<std::mutex> lock(entries_mutex_);
        const std::size_t slot = find_slot(key_id);
        if (slot == kNotFound) return false;
        out = entries_[slot].key;
        return true;
    }

    // Round D (review P0-4): snapshots key_id's K0-normalized key material
    // under entries_mutex_ and defers retire() of that key_id until every
    // PinnedKeyHandle referencing it has been destroyed. Callers whose use
    // of the key material might overlap with a concurrent rotate/retire
    // (compaction_intent_codec.hpp's encode/decode, via IntentStore) should
    // use this instead of active_key() -- keyed by the specific key_id read
    // off the wire (peek_compaction_candidate_intent_kek_key_id() etc.), not
    // a parameterless "the current active key," since this ring supports
    // multiple simultaneously-live keys and a parameterless pin would select
    // the wrong one after rotation.
    PinResult pin_key(std::uint32_t key_id) noexcept {
        std::lock_guard<std::mutex> lock(entries_mutex_);
        const std::size_t slot = find_slot(key_id);
        if (slot == kNotFound) return PinResult{PinStatus::NotFound, std::nullopt};
        ++entries_[slot].pin_count;
        // Direct-construct here (inside a KeyRing member, where the private
        // constructor's `friend class KeyRing` grants access) rather than
        // std::optional::emplace(...) -- emplace is a member of
        // std::optional, not of KeyRing, so it does not inherit this
        // friendship. The subsequent move into the optional uses
        // PinnedKeyHandle's public move constructor, which needs no
        // friendship.
        PinnedKeyHandle handle(*this, key_id, entries_[slot].key);
        PinResult result;
        result.status = PinStatus::Pinned;
        result.handle = std::move(handle);
        return result;
    }

    // Internal -- only PinnedKeyHandle's destructor/move-assignment calls
    // this (public rather than friend-only because std::optional's internal
    // machinery doesn't inherit friendship, same reasoning as pin_key()'s
    // comment above; nothing else in this codebase should call it). A slot
    // no longer found is a defensive no-op, not a crash -- releasing a pin
    // must never be able to fail loudly.
    void release_pin(std::uint32_t key_id) noexcept {
        std::lock_guard<std::mutex> lock(entries_mutex_);
        const std::size_t slot = find_slot(key_id);
        if (slot != kNotFound && entries_[slot].pin_count > 0) {
            --entries_[slot].pin_count;
        }
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
    // Returns RetireStatus::KeyPinned (not a silent no-op, not conflated
    // with NotFound) if a PinnedKeyHandle for this key_id is still alive --
    // Round D review P0-4: wiping key material a concurrent pin_key() caller
    // is actively reading would be a real use-after-free of the key bytes.
    RetireStatus retire(std::uint32_t key_id) noexcept {
        std::lock_guard<std::mutex> lock(entries_mutex_);
        const std::size_t slot = find_slot(key_id);
        if (slot == kNotFound) return RetireStatus::NotFound;
        if (entries_[slot].pin_count > 0) return RetireStatus::KeyPinned;
        secure_wipe(entries_[slot].key.data(), entries_[slot].key.size());
        entries_[slot].active = false;
        entries_[slot].key_id = 0;
        return RetireStatus::Retired;
    }

    // Live key count and the hard ceiling, so a caller can see the kMaxLiveKeys
    // limit approaching instead of discovering it as a TableFull at rotation time
    // (audit KEY-RETIRE-009).
    std::size_t live_key_count() const noexcept {
        std::lock_guard<std::mutex> lock(entries_mutex_);
        std::size_t n = 0;
        for (const auto& e : entries_) {
            if (e.active) ++n;
        }
        return n;
    }
    static constexpr std::size_t capacity() noexcept { return kMaxLiveKeys; }

    void wipe_all() noexcept {
        std::lock_guard<std::mutex> lock(entries_mutex_);
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
        std::uint32_t pin_count{0};  // Round D: entries_mutex_-protected; retire()
                                       // refuses (KeyPinned) while this is nonzero
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
    // Round D (review P0-3/P0-4): entries_ (including the new per-entry
    // pin_count) is read/written from add_key/load_wrapped_key/active_key/
    // pin_key/release_pin/retire/live_key_count/wipe_all/~KeyRing, all of
    // which now take this lock before touching entries_. enc_subkey_/
    // tag_subkey_/kek_copy_ are set once at construction and never mutated
    // again until wipe_all() (destruction), so they don't need it -- this
    // mutex protects exactly the mutable state that's actually shared,
    // never held across a filesystem call (there is none in this file) or
    // any hot-path operation.
    mutable std::mutex entries_mutex_;
    std::array<Entry, kMaxLiveKeys> entries_{};
    bool memory_locked_{false};  // audit SEC-KEKCOPY-019
};

inline PinnedKeyHandle::~PinnedKeyHandle() noexcept {
    if (ring_ != nullptr) {
        secure_wipe(key_.data(), key_.size());
        ring_->release_pin(key_id_);
    }
}

inline PinnedKeyHandle& PinnedKeyHandle::operator=(PinnedKeyHandle&& other) noexcept {
    if (this == &other) return *this;  // self-move guard: without this, the release
                                         // below would release, then the "copy from
                                         // other" would read already-wiped bytes
    if (ring_ != nullptr) {
        secure_wipe(key_.data(), key_.size());
        ring_->release_pin(key_id_);
    }
    ring_ = other.ring_;
    key_id_ = other.key_id_;
    key_ = other.key_;
    other.ring_ = nullptr;
    return *this;
}

}  // namespace hy
