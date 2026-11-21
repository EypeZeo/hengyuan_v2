// SPDX-License-Identifier: proprietary
#include <hengyuan/durable_kill_switch.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

constexpr int kCrashExit = 73;
constexpr std::uint64_t kNormalRevision = 1;
constexpr std::uint64_t kRestrictionRevision = 2;

int fail(const char* reason) {
    std::fprintf(stderr, "%s\n", reason);
    return 1;
}

bool final_claim_is_blocked(const hy::KillSwitch& kill) {
    bool consumed = false;
    const bool claimed = kill.claim_normal(kNormalRevision, [&]() noexcept {
        consumed = true;
        return true;
    });
    return !claimed && !consumed && !kill.open_revision();
}

[[noreturn]] void terminate_process(const char* phase) {
    std::puts(phase);
    std::fflush(stdout);
    std::_Exit(kCrashExit);
}

bool exit_before_persistence(void* context, const hy::KillSwitchSnapshot& next) noexcept {
    const auto& kill = *static_cast<const hy::KillSwitch*>(context);
    if (next.state != hy::KillState::Triggered || next.revision != kRestrictionRevision ||
        kill.state() != next.state || kill.revision() != next.revision ||
        kill.persistence_health() != hy::KillPersistenceHealth::Writing ||
        !final_claim_is_blocked(kill)) {
        std::_Exit(fail("The restriction was not published before the persistence callback."));
    }
    terminate_process("Restricted before writing the new record.");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return fail("Usage: kill_switch_process_probe <phase> <scratch-log-path>");
    const std::string_view phase{argv[1]};
    const std::array<std::byte, hy::kKekSize> kek{};
    constexpr std::array key{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    hy::KeyRing keys{kek};
    hy::WrappedKeyRecord wrapped{};
    if (keys.add_key(1, key, wrapped) != hy::KeyRingAddStatus::Ok) return fail("Test key initialization failed.");

    const hy::DurableKillSwitchConfig config{argv[2], hy::StoreEnvironment::Testnet, 41, 11, 31, 1};
    hy::DurableKillSwitchStore store(config, keys);
    hy::KillSwitch kill;
    if (!store.recover(kill)) return fail("The child process could not recover the kill-switch record.");

    if (phase == "recover-persisted" || phase == "recover-interrupted") {
        const bool persisted = phase == "recover-persisted";
        const auto expected_state = persisted ? hy::KillState::Triggered : hy::KillState::Latched;
        const auto snapshot = kill.snapshot();
        if (snapshot.state != expected_state || snapshot.revision != kRestrictionRevision ||
            snapshot.trigger_count != (persisted ? 1u : 0u) || snapshot.scope_epoch.value != 2u ||
            !final_claim_is_blocked(kill)) {
            return fail("The fresh process did not recover the expected restriction.");
        }
        return 0;
    }

    if (phase != "exit-after-persist" && phase != "exit-before-persist") return fail("Unknown phase.");
    if (kill.state() != hy::KillState::Latched || kill.revision() != 0 || kill.can_open()) {
        return fail("A new store did not start latched.");
    }
    if (kill.reset_persistent(kill.revision(), []() noexcept { return true; }) !=
            hy::PersistentResetResult::Applied ||
        kill.state() != hy::KillState::Normal) {
        return fail("The persisted reset did not publish Normal.");
    }
    if (kill.open_revision() != kNormalRevision) return fail("The persisted reset did not publish Normal.");

    if (phase == "exit-before-persist") {
        kill.attach_persistent({&exit_before_persistence, &kill}, kill.snapshot(), hy::KillPersistenceHealth::Ready);
        kill.trigger();
        return fail("The test persistence callback returned.");
    }
    kill.trigger();
    if (kill.state() != hy::KillState::Triggered || kill.revision() != kRestrictionRevision ||
        kill.persistence_health() != hy::KillPersistenceHealth::Ready || !final_claim_is_blocked(kill)) {
        return fail("The restriction did not become durable.");
    }
    terminate_process("Restricted and persisted the new record.");
}
