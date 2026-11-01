// SPDX-License-Identifier: proprietary
// store_binding_writer.hpp -- D5 slice R5, the WRITE side of the store binding (store_binding.hpp has the format, the reader and the
// judgement; read its header comment first).
//
// WHO MAY INCLUDE THIS. The trading process, when it opens its store -- and nothing that judges the store for somebody else. The offline
// operator tool includes store_binding.hpp only (test_operator_resolution_guard.cpp pins that no operator-tool source includes this header
// or names bind_store()/ensure_store_binding()): a check the checker could satisfy by writing the answer itself would check nothing.
//
// THE RULES.
//   * Written ONCE (bind_store()) and never changed: a second bind with other values is a Conflict; a file that cannot be believed is
//     never overwritten (Unreadable); only an absent file is created. Whoever needs another environment or account needs another store.
//   * ensure_store_binding(): a store with no history is bound to the environment and account the process runs as; a bound store must match
//     both or the process must not use it. A store that has history and no binding is a legacy store: binding it to "whatever this process
//     happens to be" would be trust on first use over orders nobody can attribute. For Testnet that is allowed (BoundLegacyTestnet: a
//     drill store carries nothing a wrong guess could hurt); for Production it is refused (UnboundProductionStoreWithHistory) -- no
//     Production store has ever been written, so every one that will exist can be bound from its first frame, and there is deliberately no
//     override: a Production store that lost its binding is archived and a new one started.
//   * The claim (environment, account uid) comes from the caller: the credentials' environment binding and GET /api/v3/account. This header
//     records and compares it; it cannot verify it. The first caller wiring (private-stream migration M5) must pass the uid the exchange
//     reported for the key it is about to trade with, never a configured one.
//
// THREAD OWNERSHIP: none -- synchronous calls on the caller's thread. The caller holds the store open (the audit sink's exclusive lock is
// what keeps a second writer out); ensure_store_binding(sink, ...) checks that, bind_store() on its own cannot.

#pragma once

#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/store_binding.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace hy {

namespace detail {

inline void encode_store_binding(std::span<std::byte, kStoreBindingSize> out, std::uint64_t uuid_lo, std::uint64_t uuid_hi,
                                 const StoreBinding& b, std::uint32_t key_id, std::span<const std::byte> hmac_key) noexcept {
    std::byte* p = out.data();
    const std::byte* const content_start = p;
    write_u8(p, kStoreBindingFormatVersion);
    write_u64_le(p, uuid_lo);
    write_u64_le(p, uuid_hi);
    write_u8(p, static_cast<std::uint8_t>(b.environment));
    write_u64_le(p, b.account_uid);
    write_i64_le(p, b.bound_at_ms);
    write_u32_le(p, key_id);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    write_bytes(p, mac.bytes.data(), kMacLen);
}

}  // namespace detail

enum class BindStatus : std::uint8_t {
    NotRun = 0,              // the zero value is not a verdict
    Written = 1,             // the binding is durable and reads back as what was asked for
    AlreadyBound = 2,        // the store was already bound to exactly this environment and account: nothing written
    Conflict = 3,            // the store is bound to another environment or account: nothing written, and a binding is never changed
    Unreadable = 4,          // a binding file exists and cannot be believed: nothing written (what cannot be read is never overwritten)
    StoreNotIdentified = 5,  // the store has no identity (uuid 0/0): there is nothing to bind the file to
    BadArguments = 6,        // no such environment, account uid 0
    WriteFailed = 7,         // the key is not in the ring, the write failed, or what was written did not read back
};

constexpr std::string_view bind_status_name(BindStatus s) noexcept {
    switch (s) {
        case BindStatus::NotRun: return "NotRun";
        case BindStatus::Written: return "Written";
        case BindStatus::AlreadyBound: return "AlreadyBound";
        case BindStatus::Conflict: return "Conflict";
        case BindStatus::Unreadable: return "Unreadable";
        case BindStatus::StoreNotIdentified: return "StoreNotIdentified";
        case BindStatus::BadArguments: return "BadArguments";
        case BindStatus::WriteFailed: return "WriteFailed";
    }
    return "Unknown";
}

// Writes the binding of a store that has none. THE only writer of a binding file. The caller must hold the store open: the audit sink's
// exclusive lock is what keeps a second writer out; ensure_store_binding(sink, ...) checks that, this primitive cannot. `key_id` is the key
// the binding is signed under (the sink's active key); `uuid_lo/hi` are the store's own identity.
inline BindStatus bind_store(const std::string& log_path, const KeyRing& key_ring, std::uint32_t key_id, std::uint64_t uuid_lo,
                             std::uint64_t uuid_hi, const StoreBinding& wanted) noexcept {
    try {
        if (!is_valid_store_environment(wanted.environment) || wanted.account_uid == 0) return BindStatus::BadArguments;
        if (uuid_lo == 0 && uuid_hi == 0) return BindStatus::StoreNotIdentified;
        const StoreBindingRead existing = read_store_binding(log_path, key_ring, uuid_lo, uuid_hi);
        if (existing.status == StoreBindingStatus::Unreadable) return BindStatus::Unreadable;
        if (existing.status == StoreBindingStatus::Bound) {
            return (existing.binding.environment == wanted.environment && existing.binding.account_uid == wanted.account_uid)
                       ? BindStatus::AlreadyBound
                       : BindStatus::Conflict;
        }
        std::array<std::byte, kKeyBlockSize> key_block{};
        if (!key_ring.active_key(key_id, key_block)) return BindStatus::WriteFailed;
        std::array<std::byte, kStoreBindingSize> buf{};
        detail::encode_store_binding(buf, uuid_lo, uuid_hi, wanted, key_id, std::span<const std::byte>(key_block.data(), key_block.size()));
        secure_wipe(key_block.data(), key_block.size());
        DurableLogStore store = detail::make_binding_store(log_path);
        if (!store.write_tip_anchor(buf)) return BindStatus::WriteFailed;
        const StoreBindingRead back = read_store_binding(log_path, key_ring, uuid_lo, uuid_hi);
        if (back.status != StoreBindingStatus::Bound || back.binding.environment != wanted.environment ||
            back.binding.account_uid != wanted.account_uid) {
            return BindStatus::WriteFailed;
        }
        return BindStatus::Written;
    } catch (...) {
        return BindStatus::WriteFailed;
    }
}

// --- the trading process's side -----------------------------------------------------------------------------------------------------------

enum class EnsureBindingStatus : std::uint8_t {
    NotRun = 0,  // the zero value is not a verdict
    // the store may be used
    Verified = 1,            // it was bound, to exactly this environment and account
    BoundNew = 2,            // it held no frames: it is bound now
    BoundLegacyTestnet = 3,  // a Testnet store with history and no binding: bound now, trusting that it is this account's (trust on first use)
    // the store must not be used
    EnvironmentMismatch = 10,
    AccountMismatch = 11,
    Unreadable = 12,                         // a binding file that cannot be believed
    UnboundProductionStoreWithHistory = 13,  // frames nobody can attribute to this environment and account: not bound after the fact
    StoreNotIdentified = 14,                 // no store identity to bind to
    WriteFailed = 15,
    BadArguments = 16,
    StoreNotUsable = 17,  // the sink is not open, is fenced, or is not a Trading-mode sink
};

constexpr bool store_may_be_used(EnsureBindingStatus s) noexcept {
    return s == EnsureBindingStatus::Verified || s == EnsureBindingStatus::BoundNew || s == EnsureBindingStatus::BoundLegacyTestnet;
}

constexpr std::string_view ensure_binding_status_name(EnsureBindingStatus s) noexcept {
    switch (s) {
        case EnsureBindingStatus::NotRun: return "NotRun";
        case EnsureBindingStatus::Verified: return "Verified";
        case EnsureBindingStatus::BoundNew: return "BoundNew";
        case EnsureBindingStatus::BoundLegacyTestnet: return "BoundLegacyTestnet";
        case EnsureBindingStatus::EnvironmentMismatch: return "EnvironmentMismatch";
        case EnsureBindingStatus::AccountMismatch: return "AccountMismatch";
        case EnsureBindingStatus::Unreadable: return "Unreadable";
        case EnsureBindingStatus::UnboundProductionStoreWithHistory: return "UnboundProductionStoreWithHistory";
        case EnsureBindingStatus::StoreNotIdentified: return "StoreNotIdentified";
        case EnsureBindingStatus::WriteFailed: return "WriteFailed";
        case EnsureBindingStatus::BadArguments: return "BadArguments";
        case EnsureBindingStatus::StoreNotUsable: return "StoreNotUsable";
    }
    return "Unknown";
}

// The policy, on the store's identity and history as plain values (the sink overload below fills them in).
inline EnsureBindingStatus ensure_store_binding(const std::string& log_path, const KeyRing& key_ring, std::uint32_t key_id,
                                                std::uint64_t uuid_lo, std::uint64_t uuid_hi, bool store_has_history,
                                                StoreEnvironment environment, std::uint64_t account_uid, std::int64_t now_ms) noexcept {
    if (!is_valid_store_environment(environment) || account_uid == 0) return EnsureBindingStatus::BadArguments;
    if (uuid_lo == 0 && uuid_hi == 0) return EnsureBindingStatus::StoreNotIdentified;
    const StoreBindingRead read = read_store_binding(log_path, key_ring, uuid_lo, uuid_hi);
    switch (judge_store_binding(read, environment, account_uid)) {
        case BindingJudgement::Match: return EnsureBindingStatus::Verified;
        case BindingJudgement::EnvironmentMismatch: return EnsureBindingStatus::EnvironmentMismatch;
        case BindingJudgement::AccountMismatch: return EnsureBindingStatus::AccountMismatch;
        case BindingJudgement::Unreadable: return EnsureBindingStatus::Unreadable;
        case BindingJudgement::Unbound: break;
    }
    if (store_has_history && environment == StoreEnvironment::Production) return EnsureBindingStatus::UnboundProductionStoreWithHistory;
    switch (bind_store(log_path, key_ring, key_id, uuid_lo, uuid_hi, StoreBinding{environment, account_uid, now_ms})) {
        case BindStatus::Written: return store_has_history ? EnsureBindingStatus::BoundLegacyTestnet : EnsureBindingStatus::BoundNew;
        case BindStatus::Unreadable: return EnsureBindingStatus::Unreadable;
        case BindStatus::StoreNotIdentified: return EnsureBindingStatus::StoreNotIdentified;
        case BindStatus::BadArguments: return EnsureBindingStatus::BadArguments;
        // AlreadyBound / Conflict here mean somebody bound the store between the read above and the write (the sink's lock is what keeps
        // that from happening; if it did, the store is not ours to use). NotRun is not a result bind_store() returns.
        case BindStatus::AlreadyBound:
        case BindStatus::Conflict:
        case BindStatus::NotRun:
        case BindStatus::WriteFailed: break;
    }
    return EnsureBindingStatus::WriteFailed;
}

// What a trading process calls right after it opened its store and knows what it runs as. `log_path` and `key_ring` are the ones the sink
// was constructed with. The sink must be open, unfenced and a Trading-mode sink: its lock is what makes this the only writer.
inline EnsureBindingStatus ensure_store_binding(const DurableAuditSink& sink, const std::string& log_path, const KeyRing& key_ring,
                                                StoreEnvironment environment, std::uint64_t account_uid, std::int64_t now_ms) noexcept {
    if (!sink.is_open() || sink.fenced() || sink.open_mode() != AuditSinkOpenMode::Trading) return EnsureBindingStatus::StoreNotUsable;
    return ensure_store_binding(log_path, key_ring, sink.active_key_id(), sink.store_uuid_lo(), sink.store_uuid_hi(), sink.frame_count() != 0,
                                environment, account_uid, now_ms);
}

}  // namespace hy
