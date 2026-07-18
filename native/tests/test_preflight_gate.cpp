// D3-LIVE preflight gate tests.
#include <gtest/gtest.h>
#include <hengyuan/preflight_gate.hpp>

using hy::DepthManager;
using hy::DepthSnapshot;
using hy::KillSwitch;
using hy::PreflightStatus;
using hy::ShmHeartbeatWriter;
using hy::ShmControlBlock;

TEST(PreflightGate, AllFailByDefault) {
    PreflightStatus s;
    EXPECT_FALSE(s.all_pass());
    EXPECT_EQ(s.pass_count(), 0);
}

TEST(PreflightGate, AllPassWhenSet) {
    PreflightStatus s;
    s.kill_switch_normal = true;
    s.risk_gate_configured = true;
    s.depth_synced = true;
    s.heartbeat_active = true;
    s.signer_ready = true;
    s.operator_confirmed = true;
    s.regression_passed = true;
    EXPECT_TRUE(s.all_pass());
    EXPECT_EQ(s.pass_count(), 7);
}

TEST(PreflightGate, SingleFailBlocksAll) {
    PreflightStatus s;
    s.kill_switch_normal = true;
    s.risk_gate_configured = true;
    s.depth_synced = true;
    s.heartbeat_active = true;
    s.signer_ready = true;
    s.operator_confirmed = true;
    s.regression_passed = false;  // one missing
    EXPECT_FALSE(s.all_pass());
    EXPECT_EQ(s.pass_count(), 6);
}

TEST(PreflightGate, CheckPreflightAutomated) {
    KillSwitch ks;
    DepthManager dm;
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter hb(&blk);
    hb.init(1);

    // Sync depth
    DepthSnapshot snap;
    snap.last_update_id = 100;
    snap.bids[0] = {99, 50};
    snap.bid_count = 1;
    snap.asks[0] = {101, 30};
    snap.ask_count = 1;
    dm.apply_snapshot(snap);

    auto s = hy::check_preflight(ks, true, dm, &hb, true);
    EXPECT_TRUE(s.kill_switch_normal);
    EXPECT_TRUE(s.risk_gate_configured);
    EXPECT_TRUE(s.depth_synced);
    EXPECT_TRUE(s.heartbeat_active);
    EXPECT_TRUE(s.signer_ready);
    EXPECT_FALSE(s.operator_confirmed);  // not set by automated check
    EXPECT_FALSE(s.regression_passed);   // not set by automated check
    EXPECT_EQ(s.pass_count(), 5);
}

TEST(PreflightGate, ArmedKillSwitchFails) {
    KillSwitch ks;
    ks.arm();
    DepthManager dm;

    auto s = hy::check_preflight(ks, true, dm, nullptr, true);
    EXPECT_FALSE(s.kill_switch_normal);
    EXPECT_FALSE(s.heartbeat_active);
}

TEST(PreflightGate, NullHeartbeatFails) {
    KillSwitch ks;
    DepthManager dm;
    auto s = hy::check_preflight(ks, true, dm, nullptr, true);
    EXPECT_FALSE(s.heartbeat_active);
}

TEST(PreflightGate, UnsyncedDepthFails) {
    KillSwitch ks;
    DepthManager dm;  // still in Buffering state
    auto s = hy::check_preflight(ks, true, dm, nullptr, false);
    EXPECT_FALSE(s.depth_synced);
}
