// SAFE-04: real sidecar recovery, failure ordering and concurrent snapshots.
#include <gtest/gtest.h>
#include <hengyuan/durable_kill_switch.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::uint64_t process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

struct Notification {
    void signal() {
        {
            std::lock_guard lock(mutex);
            signaled = true;
        }
        changed.notify_all();
    }

    bool wait_for(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return signaled; });
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool signaled{false};
};

struct BlockingPersistenceProbe {
    static bool persist(void* user_data, const hy::KillSwitchSnapshot&) noexcept {
        auto& probe = *static_cast<BlockingPersistenceProbe*>(user_data);
        std::unique_lock lock(probe.mutex);
        const int call = ++probe.calls;
        probe.changed.notify_all();
        probe.changed.wait(lock, [&] { return probe.released >= call; });
        return true;
    }

    bool wait_until_entered(int call) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(2), [&] { return calls >= call; });
    }

    void unblock(int call) {
        {
            std::lock_guard lock(mutex);
            released = call;
        }
        changed.notify_all();
    }

    std::mutex mutex;
    std::condition_variable changed;
    int calls{0};
    int released{0};
};

class DurableKillSwitchTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<std::uint64_t> sequence{0};
        directory_ = std::filesystem::temp_directory_path() /
                     ("hy-kill-switch-" + std::to_string(process_id()) + "-" + std::to_string(++sequence));
        std::filesystem::create_directories(directory_);

        std::array<std::byte, hy::kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x20 + i);
        keys_ = std::make_unique<hy::KeyRing>(kek);
        std::array<std::byte, hy::kKeyBlockSize> key{};
        for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::byte>(0x51 + i);
        hy::WrappedKeyRecord wrapped{};
        ASSERT_EQ(keys_->add_key(1, key, wrapped), hy::KeyRingAddStatus::Ok);
    }

    void TearDown() override {
        keys_.reset();
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
    }

    std::string audit_path() const { return (directory_ / "audit.log").string(); }
    std::string tip_path() const { return hy::DurableKillSwitchStore::tip_path(audit_path()); }
    std::string tmp_path() const { return hy::DurableKillSwitchStore::tip_tmp_path(audit_path()); }

    hy::DurableKillSwitchConfig config() const {
        return hy::DurableKillSwitchConfig{audit_path(), hy::StoreEnvironment::Testnet,
                                           7'654'321, 0x1122334455667788, 0x99AABBCCDDEEFF01, 1,
                                           true};
    }

    std::unique_ptr<hy::DurableKillSwitchStore> make_store() const {
        return std::make_unique<hy::DurableKillSwitchStore>(config(), *keys_);
    }

    static void write_text(const std::string& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        ASSERT_TRUE(out.good());
    }

    static std::string read_text(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    std::filesystem::path directory_;
    std::unique_ptr<hy::KeyRing> keys_;
};

TEST_F(DurableKillSwitchTest, MissingPersistentStateDefaultsToLatchedWithoutNormalStartPermission) {
    auto config = this->config();
    config.allow_normal_start = false;
    hy::DurableKillSwitchStore store(config, *keys_);
    hy::KillSwitch kill;

    ASSERT_TRUE(store.recover(kill));
    EXPECT_EQ(kill.state(), hy::KillState::Latched);
    EXPECT_EQ(kill.persistence_health(), hy::KillPersistenceHealth::Ready);
    EXPECT_FALSE(kill.can_open());
    EXPECT_TRUE(std::filesystem::exists(tip_path()));
}

TEST_F(DurableKillSwitchTest, PreviouslyPersistedNormalStateIsLatchedOnDefaultRestart) {
    {
        auto store = make_store();
        hy::KillSwitch kill;
        ASSERT_TRUE(store->recover(kill));
        ASSERT_EQ(kill.state(), hy::KillState::Normal);
        ASSERT_TRUE(kill.can_open());
    }

    auto config = this->config();
    config.allow_normal_start = false;
    hy::DurableKillSwitchStore store(config, *keys_);
    hy::KillSwitch kill;
    ASSERT_TRUE(store.recover(kill));
    EXPECT_EQ(kill.state(), hy::KillState::Latched);
    EXPECT_EQ(kill.revision(), 1u);
    EXPECT_FALSE(kill.can_open());
    EXPECT_EQ(kill.persistence_health(), hy::KillPersistenceHealth::Ready);
}

TEST_F(DurableKillSwitchTest, TipDirectoryFailsClosedWithoutReplacingTheUnsafeEntry) {
    ASSERT_TRUE(std::filesystem::create_directory(tip_path()));
    auto store = make_store();
    hy::KillSwitch kill;

    EXPECT_FALSE(store->recover(kill));
    EXPECT_EQ(kill.state(), hy::KillState::Latched);
    EXPECT_EQ(kill.persistence_health(), hy::KillPersistenceHealth::Corrupt);
    EXPECT_TRUE(std::filesystem::is_directory(tip_path()));

    EXPECT_EQ(kill.reset_persistent(0, []() noexcept { return true; }), hy::PersistentResetResult::Unavailable);
    EXPECT_EQ(kill.state(), hy::KillState::Latched);
    EXPECT_TRUE(std::filesystem::is_directory(tip_path()));
}

TEST_F(DurableKillSwitchTest, NonNormalStateAndRevisionSurviveRestartWithoutAutomaticReset) {
    {
        auto store = make_store();
        hy::KillSwitch kill;
        ASSERT_TRUE(store->recover(kill));
        EXPECT_EQ(kill.state(), hy::KillState::Normal);
        EXPECT_EQ(kill.persistence_health(), hy::KillPersistenceHealth::Ready);

        kill.trigger();
        EXPECT_EQ(kill.state(), hy::KillState::Triggered);
        EXPECT_EQ(kill.revision(), 1u);
        kill.latch();
        EXPECT_EQ(kill.state(), hy::KillState::Latched);
        EXPECT_EQ(kill.revision(), 2u);
    }
    {
        auto store = make_store();
        hy::KillSwitch kill;
        ASSERT_TRUE(store->recover(kill));
        EXPECT_EQ(kill.state(), hy::KillState::Latched);
        EXPECT_EQ(kill.revision(), 2u);
        EXPECT_EQ(kill.trigger_count(), 1u);
        EXPECT_FALSE(kill.can_open());
    }
}

TEST_F(DurableKillSwitchTest, OnlyExplicitOperatorResetPublishesNormalAndKeepsRevisionMonotonic) {
    {
        auto store = make_store();
        hy::KillSwitch kill;
        ASSERT_TRUE(store->recover(kill));
        kill.trigger();
        EXPECT_EQ(kill.reset_persistent(kill.revision(), []() noexcept { return true; }),
                  hy::PersistentResetResult::Applied);
        EXPECT_EQ(kill.state(), hy::KillState::Normal);
        EXPECT_EQ(kill.revision(), 2u);
    }
    {
        auto store = make_store();
        hy::KillSwitch kill;
        ASSERT_TRUE(store->recover(kill));
        EXPECT_EQ(kill.state(), hy::KillState::Normal);
        EXPECT_EQ(kill.revision(), 2u);
        kill.arm();
        EXPECT_EQ(kill.state(), hy::KillState::Armed);
        EXPECT_EQ(kill.revision(), 3u);
    }
    {
        auto store = make_store();
        hy::KillSwitch kill;
        ASSERT_TRUE(store->recover(kill));
        EXPECT_EQ(kill.state(), hy::KillState::Armed);
        EXPECT_EQ(kill.revision(), 3u);
        EXPECT_EQ(kill.arm_count(), 1u);
    }
}

TEST_F(DurableKillSwitchTest, WriteFailureLatchesUntilAnExplicitDurableReset) {
    {
        auto store = make_store();
        hy::KillSwitch kill;
        ASSERT_TRUE(store->recover(kill));

        std::filesystem::remove(tip_path());
        std::filesystem::create_directory(tip_path());  // rename(temp -> tip) must now fail
        kill.trigger();
        EXPECT_EQ(kill.state(), hy::KillState::Latched);
        EXPECT_EQ(kill.persistence_health(), hy::KillPersistenceHealth::Degraded);

        std::filesystem::remove_all(tip_path());
        EXPECT_EQ(kill.reset_persistent(kill.revision(), []() noexcept { return true; }),
                  hy::PersistentResetResult::Applied);
        EXPECT_EQ(kill.state(), hy::KillState::Normal);
        EXPECT_EQ(kill.persistence_health(), hy::KillPersistenceHealth::Ready);
        EXPECT_EQ(kill.revision(), 2u);
    }
    {
        auto store = make_store();
        hy::KillSwitch kill;
        ASSERT_TRUE(store->recover(kill));
        EXPECT_EQ(kill.state(), hy::KillState::Normal);
        EXPECT_EQ(kill.revision(), 2u);
    }
}

TEST_F(DurableKillSwitchTest, CorruptRecoveryNeverOverwritesEvidence) {
    write_text(tip_path(), "not a valid kill-switch record");
    const auto before = read_text(tip_path());
    auto store = make_store();
    hy::KillSwitch kill;
    EXPECT_FALSE(store->recover(kill));
    EXPECT_EQ(kill.state(), hy::KillState::Latched);
    EXPECT_EQ(kill.persistence_health(), hy::KillPersistenceHealth::Corrupt);

    EXPECT_EQ(kill.reset_persistent(0, []() noexcept { return true; }), hy::PersistentResetResult::Unavailable);
    EXPECT_EQ(kill.state(), hy::KillState::Latched);
    EXPECT_EQ(kill.persistence_health(), hy::KillPersistenceHealth::Corrupt);
    EXPECT_EQ(store->status(), hy::KillPersistenceHealth::Corrupt);
    EXPECT_EQ(read_text(tip_path()), before);
}

TEST_F(DurableKillSwitchTest, StaleTemporaryFileBlocksFreshNormalInitialization) {
    write_text(tmp_path(), "uncertain previous migration");
    auto store = make_store();
    hy::KillSwitch kill;
    EXPECT_FALSE(store->recover(kill));
    EXPECT_EQ(kill.state(), hy::KillState::Latched);
    EXPECT_EQ(kill.persistence_health(), hy::KillPersistenceHealth::Corrupt);

    EXPECT_EQ(kill.reset_persistent(0, []() noexcept { return true; }), hy::PersistentResetResult::Unavailable);
    EXPECT_EQ(kill.state(), hy::KillState::Latched);
    EXPECT_FALSE(std::filesystem::exists(tip_path()));
    EXPECT_EQ(read_text(tmp_path()), "uncertain previous migration");
}

TEST_F(DurableKillSwitchTest, AnotherAccountsStoreCannotBeRecoveredOrReset) {
    {
        auto store = make_store();
        hy::KillSwitch kill;
        ASSERT_TRUE(store->recover(kill));
        kill.trigger();
    }
    auto config = this->config();
    ++config.account_uid;
    hy::DurableKillSwitchStore other(config, *keys_);
    hy::KillSwitch kill;
    EXPECT_FALSE(other.recover(kill));
    EXPECT_EQ(kill.state(), hy::KillState::Latched);
    EXPECT_EQ(kill.reset_persistent(0, []() noexcept { return true; }), hy::PersistentResetResult::Unavailable);
    EXPECT_EQ(kill.state(), hy::KillState::Latched);
    EXPECT_EQ(other.status(), hy::KillPersistenceHealth::Corrupt);
}

TEST_F(DurableKillSwitchTest, SecondRecoveryOwnerFailsClosed) {
    auto first = make_store();
    hy::KillSwitch first_kill;
    ASSERT_TRUE(first->recover(first_kill));
    first_kill.trigger();
    ASSERT_EQ(first_kill.state(), hy::KillState::Triggered);
    const auto first_revision = first_kill.revision();

    auto second = make_store();
    hy::KillSwitch second_kill;
    EXPECT_FALSE(second->recover(second_kill));
    EXPECT_EQ(second_kill.state(), hy::KillState::Latched);
    EXPECT_EQ(second_kill.persistence_health(), hy::KillPersistenceHealth::Corrupt);
    EXPECT_EQ(first_kill.state(), hy::KillState::Triggered);
    EXPECT_EQ(first_kill.revision(), first_revision);
}

TEST(KillSwitchPersistencePort, PublishesOnlyAfterThePortAcknowledges) {
    struct Probe {
        hy::KillSwitchSnapshot offered{};
        int calls{0};
    } probe;
    hy::KillSwitch kill;
    kill.attach_persistent(
        {[](void* user_data, const hy::KillSwitchSnapshot& offered) noexcept {
             auto* p = static_cast<Probe*>(user_data);
             ++p->calls;
             p->offered = offered;
             return false;
         }, &probe},
        {}, hy::KillPersistenceHealth::Ready);

    kill.trigger();
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(probe.offered.state, hy::KillState::Triggered);
    EXPECT_EQ(kill.state(), hy::KillState::Latched) << "the failed Triggered write cannot publish a weaker state";
    EXPECT_EQ(kill.persistence_health(), hy::KillPersistenceHealth::Degraded);
}

TEST(KillSwitchPersistencePort, PersistentResetRejectsStaleAndUnauthorizedRequestsBeforePersisting) {
    struct Probe {
        int persistence_calls{0};
        int authorization_calls{0};
    } probe;
    hy::KillSwitch kill;
    kill.attach_persistent(
        {[](void* user_data, const hy::KillSwitchSnapshot&) noexcept {
             ++static_cast<Probe*>(user_data)->persistence_calls;
             return true;
         },
         &probe},
        {}, hy::KillPersistenceHealth::Ready);

    kill.trigger();
    ASSERT_EQ(kill.state(), hy::KillState::Triggered);
    ASSERT_EQ(kill.revision(), 1u);
    ASSERT_EQ(probe.persistence_calls, 1);

    EXPECT_EQ(kill.reset_persistent(0, [&]() noexcept {
                      ++probe.authorization_calls;
                      return true;
                  }),
              hy::PersistentResetResult::StateChanged);
    EXPECT_EQ(probe.authorization_calls, 0);
    EXPECT_EQ(probe.persistence_calls, 1);

    EXPECT_EQ(kill.reset_persistent(1, [&]() noexcept {
                      ++probe.authorization_calls;
                      return false;
                  }),
              hy::PersistentResetResult::Refused);
    EXPECT_EQ(probe.authorization_calls, 1);
    EXPECT_EQ(probe.persistence_calls, 1);
    EXPECT_EQ(kill.state(), hy::KillState::Triggered);
    EXPECT_EQ(kill.revision(), 1u);

    EXPECT_EQ(kill.reset_persistent(1, [&]() noexcept {
                      ++probe.authorization_calls;
                      return true;
                  }),
              hy::PersistentResetResult::Applied);
    EXPECT_EQ(probe.authorization_calls, 2);
    EXPECT_EQ(probe.persistence_calls, 2);
    EXPECT_EQ(kill.state(), hy::KillState::Normal);
    EXPECT_EQ(kill.revision(), 2u);
}

TEST(KillSwitchPersistencePort, LegacyOperatorResetCannotClearPersistentLatch) {
    struct Probe {
        int persistence_calls{0};
    } probe;
    hy::KillSwitch kill;
    kill.attach_persistent(
        {[](void* user_data, const hy::KillSwitchSnapshot&) noexcept {
             ++static_cast<Probe*>(user_data)->persistence_calls;
             return true;
         },
         &probe},
        {hy::KillState::Latched, 7, 0, 1}, hy::KillPersistenceHealth::Ready);

    kill.operator_reset();

    EXPECT_EQ(kill.state(), hy::KillState::Latched);
    EXPECT_EQ(kill.revision(), 7u);
    EXPECT_EQ(kill.persistence_health(), hy::KillPersistenceHealth::Ready);
    EXPECT_EQ(probe.persistence_calls, 0);
}

TEST(KillSwitchPersistencePort, FailedPersistentResetConsumesThePriorRevision) {
    struct Probe {
        int persistence_calls{0};
    } probe;
    hy::KillSwitch kill;
    kill.attach_persistent(
        {[](void* user_data, const hy::KillSwitchSnapshot&) noexcept {
             ++static_cast<Probe*>(user_data)->persistence_calls;
             return false;
         },
         &probe},
        {hy::KillState::Triggered, 4, 0, 1}, hy::KillPersistenceHealth::Ready);

    EXPECT_EQ(kill.reset_persistent(4, []() noexcept { return true; }),
              hy::PersistentResetResult::WriteFailed);
    EXPECT_EQ(kill.state(), hy::KillState::Latched);
    EXPECT_EQ(kill.revision(), 5u);
    EXPECT_EQ(kill.persistence_health(), hy::KillPersistenceHealth::Degraded);
    EXPECT_EQ(probe.persistence_calls, 1);

    EXPECT_EQ(kill.reset_persistent(4, []() noexcept { return true; }),
              hy::PersistentResetResult::StateChanged);
    EXPECT_EQ(probe.persistence_calls, 1);
}

TEST(KillSwitchConcurrency, RestrictiveWriteKeepsReadersAvailableAndBlocksNormalClaims) {
    BlockingPersistenceProbe probe;
    hy::KillSwitch kill;
    kill.attach_persistent({&BlockingPersistenceProbe::persist, &probe}, {},
                           hy::KillPersistenceHealth::Ready);

    const auto check_pending = [&](int call, auto transition) {
        std::thread writer(transition);
        if (!probe.wait_until_entered(call)) {
            probe.unblock(call);
            writer.join();
            ADD_FAILURE() << "the persistence callback did not start";
            return;
        }

        Notification read_done;
        hy::KillSwitchSnapshot snapshot{};
        hy::KillPersistenceHealth health{};
        bool can_open = true;
        bool claim_called = false;
        bool claim_succeeded = true;
        std::thread reader([&] {
            snapshot = kill.snapshot();
            health = kill.persistence_health();
            can_open = kill.can_open();
            claim_succeeded = kill.claim_normal(snapshot.revision, [&]() noexcept {
                claim_called = true;
                return true;
            });
            read_done.signal();
        });
        const bool read_while_writing = read_done.wait_for(std::chrono::seconds(2));
        probe.unblock(call);
        writer.join();
        reader.join();

        ASSERT_TRUE(read_while_writing) << "the read or claim waited for persistence";
        EXPECT_EQ(snapshot.state, hy::KillState::Triggered);
        EXPECT_EQ(snapshot.revision, static_cast<std::uint64_t>(call));
        EXPECT_EQ(health, hy::KillPersistenceHealth::Writing);
        EXPECT_FALSE(can_open);
        EXPECT_FALSE(claim_succeeded);
        EXPECT_FALSE(claim_called);
    };

    check_pending(1, [&] { kill.trigger(); });
    hy::PersistentResetResult reset_result = hy::PersistentResetResult::StateChanged;
    check_pending(2, [&] {
        reset_result = kill.reset_persistent(1, []() noexcept { return true; });
    });
    EXPECT_EQ(reset_result, hy::PersistentResetResult::Applied);
    EXPECT_EQ(probe.calls, 2);
    EXPECT_EQ(kill.state(), hy::KillState::Normal);
    EXPECT_EQ(kill.revision(), 2u);
    EXPECT_EQ(kill.persistence_health(), hy::KillPersistenceHealth::Ready);
}

TEST(KillSwitchConcurrency, SnapshotsRemainConsistentWhileTheSwitchEscalates) {
    hy::KillSwitch kill;
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<bool> valid{true};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {}
            while (!done.load(std::memory_order_acquire)) {
                const auto snapshot = kill.snapshot();
                if (snapshot.arm_count > snapshot.revision || snapshot.trigger_count > snapshot.revision ||
                    (snapshot.state != hy::KillState::Normal && snapshot.state != hy::KillState::Armed &&
                     snapshot.state != hy::KillState::Triggered && snapshot.state != hy::KillState::Latched)) {
                    valid.store(false, std::memory_order_release);
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (int i = 0; i < 1000; ++i) kill.trigger();
    kill.latch();
    done.store(true, std::memory_order_release);
    for (auto& reader : readers) reader.join();

    EXPECT_TRUE(valid.load(std::memory_order_acquire));
    EXPECT_EQ(kill.state(), hy::KillState::Latched);
    EXPECT_EQ(kill.revision(), 1001u);
    EXPECT_EQ(kill.trigger_count(), 1000u);
}

}  // namespace
