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

struct KillSwitchSnapshot {
    KillState state{KillState::Normal};
    std::uint64_t revision{0};
    std::uint64_t arm_count{0};
    std::uint64_t trigger_count{0};
};

using KillSwitchPersistFn = bool (*)(void* user_data, const KillSwitchSnapshot& snapshot) noexcept;

struct KillSwitchPersistencePort {
    KillSwitchPersistFn fn{nullptr};
    void* user_data{nullptr};
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

    std::optional<std::uint64_t> open_revision() const noexcept {
        std::lock_guard lock(state_mutex_);
        if (!can_open_locked()) return std::nullopt;
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
    bool claim_normal(std::uint64_t expected_revision, Claim&& claim) const noexcept {
        std::lock_guard lock(state_mutex_);
        return permits_normal_locked(expected_revision) && claim();
    }

    // Startup only, before the switch is shared. The persistence port outlives the switch.
    void attach_persistent(KillSwitchPersistencePort port, KillSwitchSnapshot recovered,
                           KillPersistenceHealth health) noexcept {
        std::lock_guard writer(writer_mutex_);
        persistence_ = port;
        if (!port.fn) health = KillPersistenceHealth::Corrupt;
        if (health != KillPersistenceHealth::Ready) recovered.state = KillState::Latched;
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
        const auto prior = snapshot();
        auto next = prior;
        advance_revision(next);
        if (next.revision != std::numeric_limits<std::uint64_t>::max()) next.state = KillState::Normal;
        commit(prior, next);
    }

private:
    bool can_open_locked() const noexcept {
        return published_.state == KillState::Normal &&
               (persistence_health_ == KillPersistenceHealth::Disabled ||
                persistence_health_ == KillPersistenceHealth::Ready);
    }

    bool permits_normal_locked(std::uint64_t expected_revision) const noexcept {
        return published_.revision == expected_revision && can_open_locked();
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
    void commit(const KillSwitchSnapshot& prior, KillSwitchSnapshot next) noexcept {
        if (persistence_health() == KillPersistenceHealth::Corrupt) return;
        if (!persistence_.fn) {
            publish(next, KillPersistenceHealth::Disabled);
            return;
        }

        auto restricted = next;
        if (prior.state > restricted.state) restricted.state = prior.state;
        publish(restricted, KillPersistenceHealth::Writing);
        if (persistence_.fn(persistence_.user_data, next)) {
            publish(next, KillPersistenceHealth::Ready);
        } else {
            next.state = KillState::Latched;
            publish(next, KillPersistenceHealth::Degraded);
        }
    }

    std::mutex writer_mutex_;
    mutable std::mutex state_mutex_;
    KillSwitchSnapshot published_{};
    KillSwitchPersistencePort persistence_{};
    KillPersistenceHealth persistence_health_{KillPersistenceHealth::Disabled};
};

}  // namespace hy
