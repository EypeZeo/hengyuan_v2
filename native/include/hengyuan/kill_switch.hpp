// SPDX-License-Identifier: proprietary
#pragma once

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace hy {

enum class KillState : std::uint8_t {
    Normal = 0,
    Armed = 1,
    Triggered = 2,
    Latched = 3,
};

enum class KillPersistenceHealth : std::uint8_t {
    Disabled = 0,
    Ready = 1,
    Degraded = 2,
    Corrupt = 3,
    Writing = 4,
};

inline const char* kill_persistence_health_name(KillPersistenceHealth health) noexcept {
    switch (health) {
        case KillPersistenceHealth::Disabled: return "Disabled";
        case KillPersistenceHealth::Ready: return "Ready";
        case KillPersistenceHealth::Degraded: return "Degraded";
        case KillPersistenceHealth::Corrupt: return "Corrupt";
        case KillPersistenceHealth::Writing: return "Writing";
    }
    return "?";
}

struct TradeScopeEpoch {
    std::uint64_t value{0};

    bool operator==(const TradeScopeEpoch&) const noexcept = default;
};

struct KillSwitchSnapshot {
    KillState state{KillState::Normal};
    std::uint64_t revision{0};
    std::uint64_t arm_count{0};
    std::uint64_t trigger_count{0};
    TradeScopeEpoch scope_epoch{};
};

using KillSwitchPersistFn = bool (*)(void* user_data, const KillSwitchSnapshot& snapshot) noexcept;

struct KillSwitchPersistencePort {
    KillSwitchPersistFn fn{nullptr};
    void* user_data{nullptr};
};

enum class PersistentResetResult {
    Applied,
    Unavailable,
    StateChanged,
    Refused,
    WriteFailed,
};

class KillSwitch {
public:
    KillState state() const noexcept { return snapshot().state; }
    std::uint64_t revision() const noexcept { return snapshot().revision; }
    std::uint64_t arm_count() const noexcept { return snapshot().arm_count; }
    std::uint64_t trigger_count() const noexcept { return snapshot().trigger_count; }

    bool can_open() const noexcept {
        std::lock_guard lock(state_mutex_);
        return can_open_locked();
    }

    std::optional<TradeScopeEpoch> trade_scope_epoch() const noexcept {
        std::lock_guard lock(state_mutex_);
        if (!can_open_locked() || persistence_health_ != KillPersistenceHealth::Ready) return std::nullopt;
        return published_.scope_epoch;
    }

    std::optional<std::uint64_t> open_revision(std::optional<TradeScopeEpoch> scope = {}) const noexcept {
        std::lock_guard lock(state_mutex_);
        if (!can_open_locked() || !scope_is_current_locked(scope)) return std::nullopt;
        return published_.revision;
    }

    KillPersistenceHealth persistence_health() const noexcept {
        std::lock_guard lock(state_mutex_);
        return persistence_health_;
    }

    KillSwitchSnapshot snapshot() const noexcept {
        std::lock_guard lock(state_mutex_);
        return published_;
    }

    std::optional<KillSwitchSnapshot> cancel_snapshot() const noexcept {
        std::lock_guard lock(state_mutex_);
        if (persistence_health_ == KillPersistenceHealth::Writing) return std::nullopt;
        return published_;
    }

    // The callback consumes the final in-memory authorization. It cannot do I/O
    // or call back into the switch. A successful claim precedes the next state publication.
    template <typename Claim>
    bool claim_normal(std::uint64_t expected_revision, Claim&& claim,
                      std::optional<TradeScopeEpoch> scope = {}) const noexcept {
        std::lock_guard lock(state_mutex_);
        return published_.revision == expected_revision && can_open_locked() && scope_is_current_locked(scope) && claim();
    }

    // Startup only, before the switch is shared. The persistence port outlives the switch.
    void attach_persistent(KillSwitchPersistencePort port, KillSwitchSnapshot recovered,
                           KillPersistenceHealth health) noexcept {
        std::lock_guard writer(writer_mutex_);
        persistence_ = port;
        if (!port.fn || recovered.scope_epoch.value > recovered.revision) health = KillPersistenceHealth::Corrupt;
        if (health != KillPersistenceHealth::Ready) {
            recovered.state = KillState::Latched;
            recovered.scope_epoch = TradeScopeEpoch{recovered.revision};
        }
        publish(recovered, health);
    }

    void arm() noexcept {
        std::lock_guard writer(writer_mutex_);
        const auto prior = snapshot();
        auto next = prior;
        advance_revision(next);
        if (next.state == KillState::Normal) {
            next.state = KillState::Armed;
            ++next.arm_count;
        }
        commit(prior, next);
    }

    void trigger() noexcept {
        std::lock_guard writer(writer_mutex_);
        const auto prior = snapshot();
        auto next = prior;
        advance_revision(next);
        if (next.state != KillState::Latched) {
            next.state = KillState::Triggered;
            ++next.trigger_count;
        }
        commit(prior, next);
    }

    void latch() noexcept {
        std::lock_guard writer(writer_mutex_);
        const auto prior = snapshot();
        auto next = prior;
        advance_revision(next);
        next.state = KillState::Latched;
        commit(prior, next);
    }

    void operator_reset() noexcept {
        std::lock_guard writer(writer_mutex_);
        if (persistence_health() != KillPersistenceHealth::Disabled) return;
        const auto prior = snapshot();
        auto next = prior;
        advance_revision(next);
        if (next.revision != std::numeric_limits<std::uint64_t>::max()) next.state = KillState::Normal;
        commit(prior, next);
    }

    [[nodiscard]] std::optional<std::uint64_t> persistent_reset_revision() const noexcept {
        std::lock_guard lock(state_mutex_);
        if (!reset_store_available(persistence_health_) || !resettable(published_)) return std::nullopt;
        return published_.revision;
    }

    // Run the final authorization check after entering the writer order.
    // The check cannot call back into the switch or perform disk or network I/O.
    template <typename Authorize>
    [[nodiscard]] PersistentResetResult reset_persistent(std::uint64_t expected_revision,
                                                         Authorize&& authorize) noexcept {
        std::lock_guard writer(writer_mutex_);
        const auto prior = snapshot();
        if (!reset_store_available(persistence_health())) return PersistentResetResult::Unavailable;
        if (prior.revision != expected_revision || !resettable(prior)) return PersistentResetResult::StateChanged;
        if (!authorize()) return PersistentResetResult::Refused;

        auto next = prior;
        advance_revision(next);
        next.state = KillState::Normal;
        return commit(prior, next) ? PersistentResetResult::Applied : PersistentResetResult::WriteFailed;
    }

private:
    static bool reset_store_available(KillPersistenceHealth health) noexcept {
        return health == KillPersistenceHealth::Ready || health == KillPersistenceHealth::Degraded;
    }

    static bool resettable(const KillSwitchSnapshot& snapshot) noexcept {
        return snapshot.state != KillState::Normal && snapshot.revision < std::numeric_limits<std::uint64_t>::max() - 1;
    }

    bool can_open_locked() const noexcept {
        return published_.state == KillState::Normal &&
               (persistence_health_ == KillPersistenceHealth::Disabled ||
                persistence_health_ == KillPersistenceHealth::Ready);
    }

    bool scope_is_current_locked(std::optional<TradeScopeEpoch> scope) const noexcept {
        return !scope || (persistence_health_ == KillPersistenceHealth::Ready && published_.scope_epoch == *scope);
    }

    static void advance_revision(KillSwitchSnapshot& next) noexcept {
        if (next.revision != std::numeric_limits<std::uint64_t>::max()) ++next.revision;
        if (next.revision == std::numeric_limits<std::uint64_t>::max()) next.state = KillState::Latched;
    }

    void publish(const KillSwitchSnapshot& snapshot, KillPersistenceHealth health) noexcept {
        std::lock_guard lock(state_mutex_);
        published_ = snapshot;
        persistence_health_ = health;
    }

    // The writer lock serializes transitions. Readers contend only with the
    // short publication and final-claim sections.
    bool commit(const KillSwitchSnapshot& prior, KillSwitchSnapshot next) noexcept {
        if (persistence_health() == KillPersistenceHealth::Corrupt) return false;
        if (next.state == KillState::Triggered || next.state == KillState::Latched) {
            next.scope_epoch = TradeScopeEpoch{next.revision};
        }
        if (!persistence_.fn) {
            publish(next, KillPersistenceHealth::Disabled);
            return true;
        }

        auto restricted = next;
        if (prior.state > restricted.state) restricted.state = prior.state;
        publish(restricted, KillPersistenceHealth::Writing);
        if (persistence_.fn(persistence_.user_data, next)) {
            publish(next, KillPersistenceHealth::Ready);
            return true;
        }
        next.state = KillState::Latched;
        next.scope_epoch = TradeScopeEpoch{next.revision};
        publish(next, KillPersistenceHealth::Degraded);
        return false;
    }

    std::mutex writer_mutex_;
    mutable std::mutex state_mutex_;
    KillSwitchSnapshot published_{};
    KillSwitchPersistencePort persistence_{};
    KillPersistenceHealth persistence_health_{KillPersistenceHealth::Disabled};
};

}  // namespace hy
