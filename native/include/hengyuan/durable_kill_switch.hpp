// SPDX-License-Identifier: proprietary
// durable_kill_switch.hpp -- SAFE-04 persistent kill-switch sidecar.
//
// One small, atomic record per audit store, signed by that store's key ring:
//   [version][store uuid][environment][account uid][state][revision]
//   [arm count][trigger count][key id][HMAC-SHA256]
//
// The tip is replaced through DurableLogStore::write_tip_anchor(), so a state
// transition is acknowledged only after its file and rename are durable. The
// audit order-frame format is deliberately untouched. The exclusive sidecar
// lock admits one recovery owner; a stale tip.tmp is an unresolved migration,
// never evidence that a fresh Normal store may be created.

#pragma once

#include <hengyuan/durable_frame_codec.hpp>
#include <hengyuan/durable_log_store.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/kill_switch.hpp>
#include <hengyuan/secure_wipe.hpp>
#include <hengyuan/sha256.hpp>
#include <hengyuan/store_binding.hpp>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace hy {

inline constexpr std::uint8_t kDurableKillSwitchVersion = 1;
inline constexpr std::size_t kDurableKillSwitchContentSize =
    1 + 8 + 8 + 1 + 8 + 1 + 8 + 8 + 8 + 4;
inline constexpr std::size_t kDurableKillSwitchRecordSize = kDurableKillSwitchContentSize + kMacLen;

struct DurableKillSwitchConfig {
    std::string audit_log_path;
    StoreEnvironment environment{static_cast<StoreEnvironment>(0)};
    std::uint64_t account_uid{0};
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint32_t key_id{0};
    bool allow_normal_start{false};
};

class DurableKillSwitchStore {
public:
    explicit DurableKillSwitchStore(DurableKillSwitchConfig config, KeyRing& keys)
        : config_(std::move(config)), keys_(keys),
          lock_path_(config_.audit_log_path + ".kill-switch.lock"),
          tip_path_(tip_path(config_.audit_log_path)),
          tmp_path_(tip_tmp_path(config_.audit_log_path)),
          store_(config_.audit_log_path + ".kill-switch", lock_path_.string(), tip_path_.string()) {}

    DurableKillSwitchStore(const DurableKillSwitchStore&) = delete;
    DurableKillSwitchStore& operator=(const DurableKillSwitchStore&) = delete;

    static std::string tip_path(const std::string& audit_log_path) {
        return audit_log_path + ".kill-switch.tip";
    }
    static std::string tip_tmp_path(const std::string& audit_log_path) {
        return tip_path(audit_log_path) + ".tmp";
    }

    KillPersistenceHealth status() const noexcept { return status_.load(std::memory_order_relaxed); }

    // Authenticate and attach the recovered state. True means the store is
    // usable; it does NOT mean the recovered state is Normal. Any unverified
    // input attaches Latched and leaves the original bytes untouched.
    bool recover(KillSwitch& kill_switch) noexcept {
        if (recovered_owner_) {
            if (recovered_owner_ != &kill_switch) {
                kill_switch.attach_persistent({}, {}, KillPersistenceHealth::Corrupt);
            }
            return false;
        }
        recovered_owner_ = &kill_switch;
        if (!valid_config() || file_state(lock_path_) == FileState::Invalid || !store_.acquire_lock()) {
            return attach(kill_switch, {}, KillPersistenceHealth::Corrupt);
        }

        const auto tip = file_state(tip_path_);
        if (tip == FileState::Invalid || file_state(tmp_path_) != FileState::Missing) {
            return attach(kill_switch, {}, KillPersistenceHealth::Corrupt);
        }

        KillSwitchSnapshot recovered{};
        if (tip == FileState::Regular) {
            std::vector<std::byte> raw;
            if (!store_.read_tip_anchor(raw) || !decode(raw, recovered)) {
                return attach(kill_switch, {}, KillPersistenceHealth::Corrupt);
            }
        }

        // A process can stop before a restriction reaches disk. An older Normal
        // record cannot authorize a new process against a persistent trading store.
        const bool restrict_at_start = !config_.allow_normal_start && recovered.state == KillState::Normal;
        if (restrict_at_start) {
            recovered.state = KillState::Latched;
            if (tip == FileState::Regular && recovered.revision != std::numeric_limits<std::uint64_t>::max()) {
                ++recovered.revision;
            }
        }
        if (tip == FileState::Missing || restrict_at_start) {
            status_ = KillPersistenceHealth::Degraded;
            if (!write(recovered)) return attach(kill_switch, recovered, KillPersistenceHealth::Degraded);
        }
        return attach(kill_switch, recovered, KillPersistenceHealth::Ready);
    }

private:
    bool valid_config() const noexcept {
        return is_valid_store_environment(config_.environment) && config_.account_uid != 0 &&
               (config_.store_uuid_lo != 0 || config_.store_uuid_hi != 0) && config_.key_id != 0;
    }

    enum class FileState { Missing, Regular, Invalid };

    static FileState file_state(const std::filesystem::path& path) noexcept {
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(path, ec);
        if (status.type() == std::filesystem::file_type::not_found &&
            (!ec || ec == std::errc::no_such_file_or_directory)) return FileState::Missing;
        return !ec && std::filesystem::is_regular_file(status) ? FileState::Regular : FileState::Invalid;
    }

    bool attach(KillSwitch& kill_switch, const KillSwitchSnapshot& snapshot,
                KillPersistenceHealth health) noexcept {
        status_ = health;
        kill_switch.attach_persistent(KillSwitchPersistencePort{&persist_adapter, this}, snapshot, health);
        return health == KillPersistenceHealth::Ready;
    }

    static bool persist_adapter(void* user_data, const KillSwitchSnapshot& snapshot) noexcept {
        return static_cast<DurableKillSwitchStore*>(user_data)->write(snapshot);
    }

    bool write(const KillSwitchSnapshot& snapshot) noexcept {
        if (status() == KillPersistenceHealth::Corrupt || !valid_config()) return false;
        if ((snapshot.state != KillState::Normal && snapshot.state != KillState::Armed &&
             snapshot.state != KillState::Triggered && snapshot.state != KillState::Latched) ||
            snapshot.arm_count > snapshot.revision || snapshot.trigger_count > snapshot.revision) {
            status_ = KillPersistenceHealth::Degraded;
            return false;
        }

        std::array<std::byte, kDurableKillSwitchRecordSize> record{};
        auto* p = record.data();
        detail::write_u8(p, kDurableKillSwitchVersion);
        detail::write_u64_le(p, config_.store_uuid_lo);
        detail::write_u64_le(p, config_.store_uuid_hi);
        detail::write_u8(p, static_cast<std::uint8_t>(config_.environment));
        detail::write_u64_le(p, config_.account_uid);
        detail::write_u8(p, static_cast<std::uint8_t>(snapshot.state));
        detail::write_u64_le(p, snapshot.revision);
        detail::write_u64_le(p, snapshot.arm_count);
        detail::write_u64_le(p, snapshot.trigger_count);
        detail::write_u32_le(p, config_.key_id);

        std::array<std::byte, kKeyBlockSize> key_block{};
        ScopedSecureWipe wipe_key_block(key_block);
        if (!keys_.active_key(config_.key_id, key_block)) {
            status_ = KillPersistenceHealth::Degraded;
            return false;
        }
        const auto mac = crypto::hmac_sha256(
            std::span<const std::byte>(key_block.data(), key_block.size()),
            std::span<const std::byte>(record.data(), kDurableKillSwitchContentSize));
        detail::write_bytes(p, mac.bytes.data(), kMacLen);
        if (file_state(tip_path_) == FileState::Invalid || file_state(tmp_path_) == FileState::Invalid ||
            !store_.write_tip_anchor(record)) {
            status_ = KillPersistenceHealth::Degraded;
            return false;
        }
        status_ = KillPersistenceHealth::Ready;
        return true;
    }

    bool decode(std::span<const std::byte> raw, KillSwitchSnapshot& out) const noexcept {
        if (raw.size() != kDurableKillSwitchRecordSize) return false;
        const std::byte* p = raw.data();
        if (detail::read_u8(p) != kDurableKillSwitchVersion) return false;
        const auto uuid_lo = detail::read_u64_le(p);
        const auto uuid_hi = detail::read_u64_le(p);
        const auto environment = static_cast<StoreEnvironment>(detail::read_u8(p));
        const auto account_uid = detail::read_u64_le(p);
        const auto state = static_cast<KillState>(detail::read_u8(p));
        const auto revision = detail::read_u64_le(p);
        const auto arm_count = detail::read_u64_le(p);
        const auto trigger_count = detail::read_u64_le(p);
        const auto key_id = detail::read_u32_le(p);
        if (uuid_lo != config_.store_uuid_lo || uuid_hi != config_.store_uuid_hi ||
            environment != config_.environment || account_uid != config_.account_uid ||
            key_id != config_.key_id ||
            (state != KillState::Normal && state != KillState::Armed &&
             state != KillState::Triggered && state != KillState::Latched) ||
            arm_count > revision || trigger_count > revision) return false;

        std::array<std::byte, kKeyBlockSize> key_block{};
        ScopedSecureWipe wipe_key_block(key_block);
        if (!keys_.active_key(key_id, key_block)) return false;
        const auto expected_mac = crypto::hmac_sha256(
            std::span<const std::byte>(key_block.data(), key_block.size()),
            std::span<const std::byte>(raw.data(), kDurableKillSwitchContentSize));
        std::array<std::byte, kMacLen> actual_mac{};
        detail::read_bytes(p, actual_mac.data(), actual_mac.size());
        if (!crypto::constant_time_equal(expected_mac, actual_mac)) return false;

        out = KillSwitchSnapshot{state, revision, arm_count, trigger_count};
        return true;
    }

    DurableKillSwitchConfig config_;
    KeyRing& keys_;
    const std::filesystem::path lock_path_;
    const std::filesystem::path tip_path_;
    const std::filesystem::path tmp_path_;
    DurableLogStore store_;
    KillSwitch* recovered_owner_{nullptr};
    std::atomic<KillPersistenceHealth> status_{KillPersistenceHealth::Corrupt};
};

}  // namespace hy
