// Q23 S6: class A closure append failures and the opt-in Q19 cost benchmark.
#include <gtest/gtest.h>

#include "test_helpers/never_sent_fixture.hpp"
#include "test_helpers/temp_directory.hpp"

#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/live_submit_orchestrator.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace hy;

namespace {

constexpr std::uint32_t kKeyId = 1;
constexpr std::size_t kSmallOrphanBudget = 1;
constexpr std::size_t kBenchmarkAttempts = 32;

std::array<std::byte, kKekSize> make_kek() {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) {
        kek[i] = static_cast<std::byte>(0x30U + static_cast<unsigned int>(i));
    }
    return kek;
}

std::array<std::byte, 24> make_plaintext_key() {
    std::array<std::byte, 24> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::byte>(0x60U + static_cast<unsigned int>(i));
    }
    return key;
}

enum class ClosureFault : std::uint8_t {
    None = 0,
    ActiveKeyUnavailable = 1,
    TipTemporaryPathBlocked = 2,
};

struct ClosureFaultState {
    DurableAuditSink* sink{nullptr};
    KeyRing* key_ring{nullptr};
    std::array<std::byte, 24> plaintext_key{};
    std::string tip_temporary_path;
    ClosureFault fault{ClosureFault::None};
    RetireStatus retire_status{RetireStatus::NotFound};
    KeyRingAddStatus restore_status{KeyRingAddStatus::TableFull};
    bool tip_obstacle_created{false};
};

AuditAppendResult append_to_real_sink(void* user_data, const AuditRecord& rec, std::int64_t now_ms) noexcept {
    auto& state = *static_cast<ClosureFaultState*>(user_data);
    return state.sink->append_durable(rec, now_ms);
}

bool real_sink_is_fenced(void* user_data) noexcept {
    return static_cast<ClosureFaultState*>(user_data)->sink->fenced();
}

ClosureAppendResult append_to_real_sink_with_fault(void* user_data, const never_sent::ClosureTarget& target,
                                                   const AuditRecord& rec, std::int64_t now_ms) noexcept {
    auto& state = *static_cast<ClosureFaultState*>(user_data);
    if (state.fault == ClosureFault::ActiveKeyUnavailable) {
        state.retire_status = state.key_ring->retire(kKeyId);
    } else if (state.fault == ClosureFault::TipTemporaryPathBlocked) {
        std::error_code ec;
        state.tip_obstacle_created = std::filesystem::create_directory(state.tip_temporary_path, ec);
    }

    const NeverSentAppend appended = state.sink->append_never_sent(target, rec, now_ms);

    if (state.fault == ClosureFault::ActiveKeyUnavailable && state.retire_status == RetireStatus::Retired) {
        WrappedKeyRecord wrapped{};
        state.restore_status = state.key_ring->add_key(kKeyId, state.plaintext_key, wrapped);
    }

    ClosureAppendResult result{};
    result.append = appended.append;
    switch (appended.status) {
        case NeverSentAppendStatus::Appended:
            result.status = ClosureAppendStatus::Appended;
            break;
        case NeverSentAppendStatus::AppendFailed:
            result.status = ClosureAppendStatus::Failed;
            break;
        case NeverSentAppendStatus::NotEnabled:
        case NeverSentAppendStatus::NotTradingOpen:
            result.status = ClosureAppendStatus::NotEnabled;
            break;
        default:
            result.status = ClosureAppendStatus::Refused;
            break;
    }
    return result;
}

struct CostSample {
    double nanoseconds_per_refusal{0.0};
    std::uintmax_t bytes_per_refusal{0};
    std::uintmax_t bytes_written{0};
    bool outcomes_match{true};
};

}  // namespace

class NeverSentBudgetTest : public test_helpers::never_sent::NeverSentOrchestratorFixture {
protected:
    test_helpers::TempDirectory scratch_;
    std::unique_ptr<KeyRing> key_ring_;
    std::unique_ptr<DurableAuditSink> sink_;
    ClosureFaultState fault_state_{};
    std::array<std::byte, 24> plaintext_key_{make_plaintext_key()};

    void SetUp() override {
        test_helpers::never_sent::NeverSentOrchestratorFixture::SetUp();
        const auto kek = make_kek();
        key_ring_ = std::make_unique<KeyRing>(kek);
        WrappedKeyRecord wrapped{};
        ASSERT_EQ(key_ring_->add_key(kKeyId, plaintext_key_, wrapped), KeyRingAddStatus::Ok);
        fault_state_.key_ring = key_ring_.get();
        fault_state_.plaintext_key = plaintext_key_;
    }

    std::string log_path(const char* name) const { return scratch_.file(name); }

    void open_sink(const std::string& path, NeverSentWrites writes = NeverSentWrites::Enabled,
                   std::size_t max_intent_orphans = kSmallOrphanBudget) {
        sink_ = std::make_unique<DurableAuditSink>(path, *key_ring_, kKeyId, max_intent_orphans,
                                                   AuditSinkOpenMode::Trading, writes);
        ASSERT_TRUE(sink_->is_open());
        ASSERT_FALSE(sink_->fenced());
        fault_state_.sink = sink_.get();
        ctx_.durable_audit = DurableOrderAuditPort{&append_to_real_sink, &real_sink_is_fenced, &fault_state_,
                                                   &append_to_real_sink_with_fault};
    }

    void prepare_class_a_refusal() noexcept { ctx_.confirmation.confirmed = false; }

    CostSample run_cost_group(const std::string& path, NeverSentWrites writes, std::int64_t group_time) {
        open_sink(path, writes);
        fault_state_.fault = ClosureFault::None;
        const std::uintmax_t before = std::filesystem::file_size(path);
        bool outcomes_match = true;
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < kBenchmarkAttempts; ++i) {
            ctx_.now_ms = group_time;
            ctx_.sequence = 1U + static_cast<std::uint32_t>(i);
            const OrchestratorResult result = orchestrate_submit(ctx_);
            const ClosureAppendStatus expected_closure = writes == NeverSentWrites::Enabled
                                                             ? ClosureAppendStatus::Appended
                                                             : ClosureAppendStatus::NotEnabled;
            outcomes_match = outcomes_match && result.gate == OrchestratorGate::OperatorNotConfirmed &&
                             result.closure == expected_closure;
        }
        const auto stop = std::chrono::steady_clock::now();
        const std::uintmax_t after = std::filesystem::file_size(path);
        const std::uintmax_t written = after - before;
        const double elapsed_ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
        CostSample sample{};
        sample.nanoseconds_per_refusal = elapsed_ns / static_cast<double>(kBenchmarkAttempts);
        sample.bytes_per_refusal = written / static_cast<std::uintmax_t>(kBenchmarkAttempts);
        sample.bytes_written = written;
        sample.outcomes_match = outcomes_match;
        sink_.reset();
        fault_state_.sink = nullptr;
        return sample;
    }
};

TEST_F(NeverSentBudgetTest, FailedClosureWithoutAFrameRestoresTheIntentOrphanOnRestart) {
    const std::string path = log_path("closure-no-frame.log");
    open_sink(path);
    prepare_class_a_refusal();
    fault_state_.fault = ClosureFault::ActiveKeyUnavailable;

    const OrchestratorResult result = orchestrate_submit(ctx_);
    EXPECT_EQ(result.gate, OrchestratorGate::OperatorNotConfirmed);
    EXPECT_EQ(result.closure, ClosureAppendStatus::Failed);
    EXPECT_EQ(result.order.state, OrderState::Intent);
    EXPECT_EQ(fault_state_.retire_status, RetireStatus::Retired);
    EXPECT_EQ(fault_state_.restore_status, KeyRingAddStatus::Ok);
    EXPECT_FALSE(sink_->fenced());
    ASSERT_EQ(std::filesystem::file_size(path), kOrderEventFrameSize)
        << "the failed closure left the acknowledged Intent frame only";

    sink_.reset();
    open_sink(path, NeverSentWrites::Disabled);
    EXPECT_EQ(sink_->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(sink_->recovered_never_sent(), 0u);
    EXPECT_EQ(sink_->recovered_intent_orphans(), 1u);
    EXPECT_TRUE(sink_->recovered_checkpoints().empty());
}

TEST_F(NeverSentBudgetTest, FailedClosureWhoseFrameLandedClosesTheOrderOnRestart) {
    const std::string path = log_path("closure-frame-landed.log");
    open_sink(path);
    prepare_class_a_refusal();
    fault_state_.fault = ClosureFault::TipTemporaryPathBlocked;
    fault_state_.tip_temporary_path = path + ".tip.tmp";

    const OrchestratorResult result = orchestrate_submit(ctx_);
    EXPECT_EQ(result.gate, OrchestratorGate::OperatorNotConfirmed);
    EXPECT_EQ(result.closure, ClosureAppendStatus::Failed);
    EXPECT_EQ(result.order.state, OrderState::Intent);
    EXPECT_TRUE(fault_state_.tip_obstacle_created);
    EXPECT_TRUE(sink_->fenced());
    EXPECT_EQ(sink_->write_format_version(), kFrameFormatVersion);
    ASSERT_EQ(std::filesystem::file_size(path), 2 * kOrderEventFrameSize)
        << "the closure frame reached the log before the tip-anchor write failed";

    sink_.reset();
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::remove(fault_state_.tip_temporary_path, ec)) << ec.message();
    open_sink(path, NeverSentWrites::Disabled);
    EXPECT_EQ(sink_->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(sink_->recovered_never_sent(), 1u);
    EXPECT_EQ(sink_->recovered_intent_orphans(), 0u);
    EXPECT_TRUE(sink_->recovered_checkpoints().empty());
}

TEST_F(NeverSentBudgetTest, DISABLED_ClassARefusalCostBenchmark) {
    prepare_class_a_refusal();
    const CostSample disabled = run_cost_group(log_path("cost-disabled.log"), NeverSentWrites::Disabled, 10000);
    const CostSample enabled = run_cost_group(log_path("cost-enabled.log"), NeverSentWrites::Enabled, 20000);

    EXPECT_TRUE(disabled.outcomes_match);
    EXPECT_TRUE(enabled.outcomes_match);
    EXPECT_EQ(disabled.bytes_per_refusal, kOrderEventFrameSize);
    EXPECT_EQ(enabled.bytes_per_refusal, 2 * kOrderEventFrameSize);

    RecordProperty("disabled_mean_refusal_ns", std::to_string(disabled.nanoseconds_per_refusal));
    RecordProperty("disabled_log_bytes_per_refusal", std::to_string(disabled.bytes_per_refusal));
    RecordProperty("enabled_mean_refusal_ns", std::to_string(enabled.nanoseconds_per_refusal));
    RecordProperty("enabled_log_bytes_per_refusal", std::to_string(enabled.bytes_per_refusal));
    std::printf("Q19 refusal cost: disabled %.1f ns/call, %llu log bytes/call; "
                "enabled %.1f ns/call, %llu log bytes/call\n",
                disabled.nanoseconds_per_refusal, static_cast<unsigned long long>(disabled.bytes_per_refusal),
                enabled.nanoseconds_per_refusal, static_cast<unsigned long long>(enabled.bytes_per_refusal));
}
