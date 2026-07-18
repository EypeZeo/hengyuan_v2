// P2-INFRA-HB-01: SHM heartbeat control block + writer tests.
// Portable (no shm_open/mmap) — tests the data structures and logic.
#include <gtest/gtest.h>
#include <hengyuan/shm_heartbeat.hpp>
#include <cstring>

using hy::ShmControlBlock;
using hy::ShmHeartbeatWriter;
using hy::kShmMagic;
using hy::kShmVersion;
using hy::shm_compute_checksum;
using hy::shm_crc32;
using hy::shm_verify;

TEST(ShmHeartbeat, ControlBlockIs64Bytes) {
    EXPECT_EQ(sizeof(ShmControlBlock), 64u);
    EXPECT_EQ(alignof(ShmControlBlock), 64u);
}

TEST(ShmHeartbeat, CRC32KnownVector) {
    const char* data = "123456789";
    auto crc = shm_crc32(data, 9);
    EXPECT_EQ(crc, 0xCBF43926u);
}

TEST(ShmHeartbeat, CRC32EmptyIsConsistent) {
    auto crc1 = shm_crc32("", 0);
    auto crc2 = shm_crc32("", 0);
    EXPECT_EQ(crc1, crc2);
}

TEST(ShmHeartbeat, InitSetsFieldsCorrectly) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(12345);

    EXPECT_EQ(blk.magic, kShmMagic);
    EXPECT_EQ(blk.version, kShmVersion);
    EXPECT_EQ(blk.main_pid, 12345u);
    EXPECT_EQ(blk.loop_counter, 0u);
    EXPECT_EQ(blk.last_incoming_ns, 0u);
    EXPECT_EQ(blk.last_outgoing_ns, 0u);
    EXPECT_EQ(blk.kill_armed, 0u);
    EXPECT_TRUE(shm_verify(blk));
}

TEST(ShmHeartbeat, TickIncrementsCounter) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);

    EXPECT_EQ(blk.loop_counter, 0u);
    writer.tick();
    EXPECT_EQ(blk.loop_counter, 1u);
    writer.tick();
    writer.tick();
    EXPECT_EQ(blk.loop_counter, 3u);
    EXPECT_TRUE(shm_verify(blk));
}

TEST(ShmHeartbeat, TimestampsUpdate) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);

    writer.set_incoming(1000000000);
    EXPECT_EQ(blk.last_incoming_ns, 1000000000u);
    EXPECT_TRUE(shm_verify(blk));

    writer.set_outgoing(2000000000);
    EXPECT_EQ(blk.last_outgoing_ns, 2000000000u);
    EXPECT_TRUE(shm_verify(blk));
}

TEST(ShmHeartbeat, ChecksumDetectsCorruption) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);
    EXPECT_TRUE(shm_verify(blk));

    // Corrupt loop_counter without updating checksum
    blk.loop_counter = 999;
    EXPECT_FALSE(shm_verify(blk));
}

TEST(ShmHeartbeat, BadMagicFails) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);

    blk.magic = 0xDEADBEEF;
    blk.checksum = shm_compute_checksum(blk);
    EXPECT_FALSE(shm_verify(blk));
}

TEST(ShmHeartbeat, BadVersionFails) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);

    blk.version = 99;
    blk.checksum = shm_compute_checksum(blk);
    EXPECT_FALSE(shm_verify(blk));
}

TEST(ShmHeartbeat, KillRequestDetected) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(1);

    EXPECT_FALSE(writer.kill_requested());
    blk.kill_armed = 1;
    EXPECT_TRUE(writer.kill_requested());
}

TEST(ShmHeartbeat, NullWriterSafe) {
    ShmHeartbeatWriter writer(nullptr);
    writer.init(1);       // no crash
    writer.tick();         // no crash
    writer.set_incoming(0);
    writer.set_outgoing(0);
    EXPECT_FALSE(writer.kill_requested());
    EXPECT_EQ(writer.block(), nullptr);
}

TEST(ShmHeartbeat, ChecksumStableAcrossMultipleTicks) {
    alignas(64) ShmControlBlock blk{};
    ShmHeartbeatWriter writer(&blk);
    writer.init(42);

    for (int i = 0; i < 10000; ++i) {
        writer.tick();
        writer.set_incoming(static_cast<std::uint64_t>(i) * 1000);
        EXPECT_TRUE(shm_verify(blk)) << "Failed at tick " << i;
    }
    EXPECT_EQ(blk.loop_counter, 10000u);
}
