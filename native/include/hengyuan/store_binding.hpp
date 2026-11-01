// SPDX-License-Identifier: proprietary
// store_binding.hpp -- D5 slice R5, the READ side (docs/OPERATOR_RESOLUTION_DESIGN.md 9 "R-env"; blueprint L-48): which exchange
// environment, and which account, a durable audit store belongs to, and the judgement of a process or tool against it.
//
// THE GAP. The audit store records orders by client order id and symbol id. `mode` says DryRun or Live and nothing else; the store
// identity (<log>.storeid) is a random label; no frame says "sent to Binance Production, for account 123". So the offline operator tool
// could be pointed at Testnet and read "no such order" for an order that lives on Production -- a true answer about the wrong exchange,
// which is exactly what its conclusion cannot afford -- and a trading process could be started with Production credentials over a store
// a Testnet run filled (or the other way round), or over another account's. Until now the only defence was the operator's confirmation
// phrase (A-D5-3: "I pointed this tool at the environment and the account the order was sent to").
//
// WHAT THIS IS. A sidecar file, <log>.binding.tip, written once through DurableLogStore::write_tip_anchor() (temp file, flush, rename --
// the same crash-consistent write the tip anchors and the store identity use):
//
//   [format_version: u8][store_uuid_lo: u64 LE][store_uuid_hi: u64 LE][environment: u8][account_uid: u64 LE][bound_at_ms: i64 LE]
//   [key_id: u32 LE][mac: 32 bytes = HMAC(key ring key for key_id, everything above)]                                         = 70 bytes
//
// Signed under the store's own key ring like every other structure of the store, and bound to the store by its uuid: a binding file
// copied to another store verifies as nothing. It is a sidecar, not a field of the store identity, on purpose: the identity's layout is
// shared with the export path (durable_audit_export.hpp, ExportTuple), and changing it for this would put the export ABI in play.
//
// TWO HEADERS, ON PURPOSE. This one reads and judges; store_binding_writer.hpp writes. The offline operator tool includes only this one
// (test_operator_resolution_guard.cpp pins that): a check the checker could satisfy by writing the answer itself would check nothing, so
// the tool cannot even name the function that would.
//
// THE RULES (the writer's side is in store_binding_writer.hpp).
//   * Read by the operator tool before it believes an exchange answer (operator_resolution_flow.hpp, resolve_order()): the environment it
//     was pointed at and the account the exchange reports for its key must be the store's. A store with NO binding is a legacy store.
//     For Testnet (the drill stores that exist today) the tool goes on and says so, and the evidence file records it; for Production it
//     refuses -- no Production store has ever been written, so every one that will exist can be bound from its first frame.
//   * A binding file that exists and cannot be believed (an I/O error, another size or format, a MAC that does not verify, a key the ring
//     does not have, another store's uuid, no such environment, account 0) is NOT "no binding": it is Unreadable, and everything refuses.
//     Treating it as absent would let a store with a binding it cannot show pass as a legacy one.
//
// WHAT IT DOES NOT DO. It cannot prove the exchange environment or the account of a process: both come from the caller (the credentials'
// environment binding and GET /api/v3/account), and a binding written from a wrong claim stays wrong -- the claim is checked against the
// exchange at the moment it is made, by the caller. It does not protect a store whose key ring is compromised (the file is signed under it).
//
// THREAD OWNERSHIP: none -- every function is a synchronous call on the caller's thread; it touches only the binding file.

#pragma once

#include <hengyuan/durable_frame_codec.hpp>
#include <hengyuan/durable_log_store.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/secure_wipe.hpp>
#include <hengyuan/sha256.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace hy {

// The environments a store can be bound to. The on-disk codes start at 1: 0 is never an environment (a zero-filled file is not one).
enum class StoreEnvironment : std::uint8_t {
    Testnet = 1,
    Production = 2,
};

constexpr bool is_valid_store_environment(StoreEnvironment e) noexcept {
    return e == StoreEnvironment::Testnet || e == StoreEnvironment::Production;
}

constexpr std::string_view store_environment_name(StoreEnvironment e) noexcept {
    switch (e) {
        case StoreEnvironment::Testnet: return "TESTNET";
        case StoreEnvironment::Production: return "PRODUCTION";
    }
    return "";
}

struct StoreBinding {
    StoreEnvironment environment{static_cast<StoreEnvironment>(0)};
    std::uint64_t account_uid{0};
    std::int64_t bound_at_ms{0};  // informational (the binder's wall clock when it bound the store); signed, never compared
};

enum class StoreBindingStatus : std::uint8_t {
    Unbound = 0,     // there is no binding file
    Bound = 1,       // a binding file that verifies and belongs to this store
    Unreadable = 2,  // a binding file exists and cannot be believed (see the header comment)
};

constexpr std::string_view store_binding_status_name(StoreBindingStatus s) noexcept {
    switch (s) {
        case StoreBindingStatus::Unbound: return "Unbound";
        case StoreBindingStatus::Bound: return "Bound";
        case StoreBindingStatus::Unreadable: return "Unreadable";
    }
    return "Unknown";
}

struct StoreBindingRead {
    StoreBindingStatus status{StoreBindingStatus::Unbound};  // a default-constructed read is "no file": see the Production rule above
    StoreBinding binding{};                                  // meaningful only when status == Bound
};

inline constexpr std::uint8_t kStoreBindingFormatVersion = 1;
inline constexpr std::size_t kStoreBindingSize = 1 + 8 + 8 + 1 + 8 + 8 + 4 + kMacLen;  // 70

// The file the binding lives in (the tip anchor of the binding's DurableLogStore).
inline std::string store_binding_file_path(const std::string& log_path) { return log_path + ".binding.tip"; }

namespace detail {

// The binding's store: only its tip-anchor file is ever used (no log, no lock -- the audit sink's exclusive lock is what keeps a second
// writer out, and a reader needs none).
inline DurableLogStore make_binding_store(const std::string& log_path) {
    return DurableLogStore(log_path + ".binding", log_path + ".binding.lock", store_binding_file_path(log_path));
}

// The key id is signed content, but it has to be read before the MAC can be checked: it names the key the MAC is checked with.
inline bool peek_store_binding_key_id(std::span<const std::byte> in, std::uint32_t& out_key_id) noexcept {
    constexpr std::size_t kKeyIdOffset = 1 + 8 + 8 + 1 + 8 + 8;  // format_version, uuid lo/hi, environment, account_uid, bound_at_ms
    constexpr std::size_t kMinBytes = kKeyIdOffset + 4;
    if (in.size() < kMinBytes) return false;
    const std::byte* p = in.data() + kKeyIdOffset;
    out_key_id = read_u32_le(p);
    return true;
}

struct DecodedStoreBinding {
    std::uint64_t uuid_lo{0};
    std::uint64_t uuid_hi{0};
    StoreBinding binding{};
    std::uint32_t key_id{0};
};

inline bool decode_store_binding(std::span<const std::byte> in, std::span<const std::byte> hmac_key, DecodedStoreBinding& out) noexcept {
    if (in.size() != kStoreBindingSize) return false;
    const std::byte* p = in.data();
    const std::byte* const content_start = p;
    if (read_u8(p) != kStoreBindingFormatVersion) return false;
    DecodedStoreBinding v{};
    v.uuid_lo = read_u64_le(p);
    v.uuid_hi = read_u64_le(p);
    v.binding.environment = static_cast<StoreEnvironment>(read_u8(p));
    v.binding.account_uid = read_u64_le(p);
    v.binding.bound_at_ms = read_i64_le(p);
    v.key_id = read_u32_le(p);
    const std::size_t content_len = static_cast<std::size_t>(p - content_start);
    const auto expected_mac = crypto::hmac_sha256(hmac_key, std::span<const std::byte>(content_start, content_len));
    std::array<std::byte, kMacLen> mac{};
    read_bytes(p, mac.data(), kMacLen);
    if (!crypto::constant_time_equal(expected_mac, mac)) return false;  // audit SEC-MACCMP-010
    out = v;
    return true;
}

}  // namespace detail

// What the store's binding file says. `uuid_lo/hi` are the store's own (DurableAuditSink::store_uuid_lo()/hi(); 0/0 when its identity could
// not be established, which makes every existing binding file Unreadable: there is no store for it to belong to). Never throws, never writes.
inline StoreBindingRead read_store_binding(const std::string& log_path, const KeyRing& key_ring, std::uint64_t uuid_lo,
                                           std::uint64_t uuid_hi) noexcept {
    StoreBindingRead result{};
    try {
        DurableLogStore store = detail::make_binding_store(log_path);
        std::vector<std::byte> raw;
        if (!store.read_tip_anchor(raw)) {
            // read_tip_anchor() folds "no such file" and "could not read it" into one false: tell them apart, the same way the store
            // identity does. An unreadable file is not an absent one.
            std::error_code ec;
            const bool exists = std::filesystem::exists(store_binding_file_path(log_path), ec);
            result.status = (ec || exists) ? StoreBindingStatus::Unreadable : StoreBindingStatus::Unbound;
            return result;
        }
        result.status = StoreBindingStatus::Unreadable;  // until every check below has passed
        std::uint32_t file_key_id = 0;
        std::array<std::byte, kKeyBlockSize> key_block{};
        if (!detail::peek_store_binding_key_id(raw, file_key_id) || !key_ring.active_key(file_key_id, key_block)) return result;
        detail::DecodedStoreBinding decoded{};
        const bool ok = detail::decode_store_binding(raw, std::span<const std::byte>(key_block.data(), key_block.size()), decoded);
        secure_wipe(key_block.data(), key_block.size());
        if (!ok) return result;
        if (uuid_lo == 0 && uuid_hi == 0) return result;                               // a store without an identity has nothing to belong to
        if (decoded.uuid_lo != uuid_lo || decoded.uuid_hi != uuid_hi) return result;   // another store's file
        if (!is_valid_store_environment(decoded.binding.environment) || decoded.binding.account_uid == 0) return result;
        result.status = StoreBindingStatus::Bound;
        result.binding = decoded.binding;
        return result;
    } catch (...) {
        return StoreBindingRead{StoreBindingStatus::Unreadable, {}};
    }
}

// What a store's binding says about a process or tool that runs as (environment, uid). Pure.
enum class BindingJudgement : std::uint8_t {
    Match = 0,
    EnvironmentMismatch = 1,
    AccountMismatch = 2,
    Unbound = 3,     // no binding file: the caller's policy decides (the trading process and the operator tool differ)
    Unreadable = 4,  // a binding file that cannot be believed: always a refusal
};

constexpr std::string_view binding_judgement_name(BindingJudgement j) noexcept {
    switch (j) {
        case BindingJudgement::Match: return "Match";
        case BindingJudgement::EnvironmentMismatch: return "EnvironmentMismatch";
        case BindingJudgement::AccountMismatch: return "AccountMismatch";
        case BindingJudgement::Unbound: return "Unbound";
        case BindingJudgement::Unreadable: return "Unreadable";
    }
    return "Unknown";
}

constexpr BindingJudgement judge_store_binding(const StoreBindingRead& read, StoreEnvironment environment, std::uint64_t account_uid) noexcept {
    switch (read.status) {
        case StoreBindingStatus::Unbound: return BindingJudgement::Unbound;
        case StoreBindingStatus::Unreadable: return BindingJudgement::Unreadable;
        case StoreBindingStatus::Bound: break;
    }
    // The environment first: a store of another environment is also a store of another account, and "wrong environment" is the more
    // useful thing to say.
    if (read.binding.environment != environment) return BindingJudgement::EnvironmentMismatch;
    if (read.binding.account_uid != account_uid) return BindingJudgement::AccountMismatch;
    return BindingJudgement::Match;
}

}  // namespace hy
