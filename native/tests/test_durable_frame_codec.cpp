// Pure in-memory round-trip and corruption-detection tests for
// durable_frame_codec.hpp. No file I/O anywhere in this file -- that's
// durable_audit_sink.hpp's job (test_durable_audit_sink.cpp).
#include <gtest/gtest.h>
#include <hengyuan/durable_frame_codec.hpp>

#include <array>
#include <cstring>
#include <vector>

using namespace hy;

namespace {

std::vector<std::byte> as_key(std::string_view s) {
    std::vector<std::byte> k(s.size());
    std::memcpy(k.data(), s.data(), s.size());
    return k;
}

AuditRecord make_sample_record() {
    AuditRecord rec{};
    rec.timestamp_ms = 1'700'000'000'123;
    rec.event_type = AuditEventType::OrderReconciled;
    rec.mode = ExecutionMode::Live;
    rec.symbol_id = 42;
    rec.set_client_order_id("HY-1700000000000-7-42");
    rec.exchange_order_id = 998877;
    rec.price_ticks = 500000;
    rec.qty_ticks = 100;
    rec.detail_code = -1013;
    rec.set_detail("reconciled via poll");
    rec.resulting_state = OrderState::Filled;
    rec.filled_qty_ticks = 100;
    rec.avg_fill_price_ticks = 499950;
    return rec;
}

constexpr std::array<std::byte, kMacLen> kZeroMac{};

}  // namespace

// --- encode_audit_record / decode_audit_record (payload-only round trip) ---

TEST(AuditRecordCodec, RoundTripPreservesEveryField) {
    AuditRecord rec = make_sample_record();
    std::array<std::byte, kAuditRecordWireSize> buf{};
    encode_audit_record(buf, rec);

    AuditRecord decoded{};
    ASSERT_TRUE(decode_audit_record(buf, decoded));

    EXPECT_EQ(decoded.timestamp_ms, rec.timestamp_ms);
    EXPECT_EQ(decoded.event_type, rec.event_type);
    EXPECT_EQ(decoded.mode, rec.mode);
    EXPECT_EQ(decoded.symbol_id, rec.symbol_id);
    EXPECT_STREQ(decoded.client_order_id, rec.client_order_id);
    EXPECT_EQ(decoded.exchange_order_id, rec.exchange_order_id);
    EXPECT_EQ(decoded.price_ticks, rec.price_ticks);
    EXPECT_EQ(decoded.qty_ticks, rec.qty_ticks);
    EXPECT_EQ(decoded.detail_code, rec.detail_code);
    EXPECT_STREQ(decoded.detail_msg, rec.detail_msg);
    EXPECT_EQ(decoded.resulting_state, rec.resulting_state);
    EXPECT_EQ(decoded.filled_qty_ticks, rec.filled_qty_ticks);
    EXPECT_EQ(decoded.avg_fill_price_ticks, rec.avg_fill_price_ticks);
}

TEST(AuditRecordCodec, DefaultConstructedRecordRoundTrips) {
    AuditRecord rec{};
    std::array<std::byte, kAuditRecordWireSize> buf{};
    encode_audit_record(buf, rec);
    AuditRecord decoded{};
    ASSERT_TRUE(decode_audit_record(buf, decoded));
    EXPECT_EQ(decoded.event_type, AuditEventType::OrderIntentCreated);
    EXPECT_EQ(decoded.resulting_state, OrderState::Intent);
}

TEST(AuditRecordCodec, OutOfRangeEventTypeRejected) {
    AuditRecord rec = make_sample_record();
    std::array<std::byte, kAuditRecordWireSize> buf{};
    encode_audit_record(buf, rec);
    buf[8] = std::byte{200};  // event_type is byte offset 8 (after 8-byte timestamp_ms)
    AuditRecord decoded{};
    EXPECT_FALSE(decode_audit_record(buf, decoded));
}

TEST(AuditRecordCodec, OutOfRangeResultingStateRejected) {
    AuditRecord rec = make_sample_record();
    std::array<std::byte, kAuditRecordWireSize> buf{};
    encode_audit_record(buf, rec);
    // resulting_state is the last single byte before the two trailing i64
    // fields: offset = kAuditRecordWireSize - 8 - 8 - 1.
    const std::size_t offset = kAuditRecordWireSize - 8 - 8 - 1;
    buf[offset] = std::byte{200};
    AuditRecord decoded{};
    EXPECT_FALSE(decode_audit_record(buf, decoded));
}

// --- Full frame encode/decode ---

TEST(OrderEventFrameCodec, RoundTripAtSequenceZero) {
    AuditRecord rec = make_sample_record();
    auto key = as_key("test-hmac-key");
    std::vector<std::byte> buf(kOrderEventFrameSize);

    auto n = encode_order_event_frame(buf, /*sequence_number=*/0, FrameTimeKind::ServerCorrectedUtc,
                                       1'700'000'000'000, rec, kZeroMac, key);
    ASSERT_EQ(n, kOrderEventFrameSize);

    DecodedOrderFrame decoded{};
    std::size_t frame_size = 0;
    auto status = decode_order_event_frame(buf, key, decoded, frame_size);
    ASSERT_EQ(status, FrameDecodeStatus::Ok);
    EXPECT_EQ(frame_size, kOrderEventFrameSize);
    EXPECT_EQ(decoded.record_type, DurableRecordType::OrderEvent);
    EXPECT_EQ(decoded.sequence_number, 0u);
    EXPECT_EQ(decoded.time_kind, FrameTimeKind::ServerCorrectedUtc);
    EXPECT_EQ(decoded.recorded_utc_ms, 1'700'000'000'000);
    EXPECT_EQ(decoded.prev_mac, kZeroMac);
    EXPECT_STREQ(decoded.record.client_order_id, rec.client_order_id);
    EXPECT_EQ(decoded.record.resulting_state, rec.resulting_state);
}

TEST(OrderEventFrameCodec, ChainedFramesLinkViaPrevMac) {
    AuditRecord rec1 = make_sample_record();
    AuditRecord rec2 = make_sample_record();
    rec2.set_client_order_id("HY-1700000000001-8-42");
    auto key = as_key("test-hmac-key");

    std::vector<std::byte> buf1(kOrderEventFrameSize);
    auto n1 = encode_order_event_frame(buf1, 0, FrameTimeKind::ServerCorrectedUtc, 1000, rec1, kZeroMac, key);
    ASSERT_EQ(n1, kOrderEventFrameSize);

    DecodedOrderFrame d1{};
    std::size_t sz1 = 0;
    ASSERT_EQ(decode_order_event_frame(buf1, key, d1, sz1), FrameDecodeStatus::Ok);

    std::vector<std::byte> buf2(kOrderEventFrameSize);
    auto n2 = encode_order_event_frame(buf2, 1, FrameTimeKind::ServerCorrectedUtc, 2000, rec2, d1.mac, key);
    ASSERT_EQ(n2, kOrderEventFrameSize);

    DecodedOrderFrame d2{};
    std::size_t sz2 = 0;
    ASSERT_EQ(decode_order_event_frame(buf2, key, d2, sz2), FrameDecodeStatus::Ok);
    EXPECT_EQ(d2.prev_mac, d1.mac) << "second frame's prev_mac must equal the first frame's own mac";
    EXPECT_NE(d1.mac, d2.mac) << "different sequence/content must produce a different mac";
}

TEST(OrderEventFrameCodec, TooSmallBufferForHeaderIsTruncated) {
    AuditRecord rec = make_sample_record();
    auto key = as_key("k");
    std::vector<std::byte> buf(kOrderEventFrameSize);
    encode_order_event_frame(buf, 0, FrameTimeKind::ServerCorrectedUtc, 1000, rec, kZeroMac, key);

    DecodedOrderFrame decoded{};
    std::size_t frame_size = 0;
    auto status = decode_order_event_frame(std::span(buf).first(10), key, decoded, frame_size);
    EXPECT_EQ(status, FrameDecodeStatus::Truncated);
    EXPECT_EQ(frame_size, 0u);
}

TEST(OrderEventFrameCodec, TruncatedMidPayloadIsTruncatedNotCorrupt) {
    AuditRecord rec = make_sample_record();
    auto key = as_key("k");
    std::vector<std::byte> buf(kOrderEventFrameSize);
    encode_order_event_frame(buf, 0, FrameTimeKind::ServerCorrectedUtc, 1000, rec, kZeroMac, key);

    // Header (23 bytes) fully present, but the frame is cut off partway
    // through the payload -- this is exactly the "torn tail write" scenario
    // recovery_scan() must discard without treating as Corrupt.
    DecodedOrderFrame decoded{};
    std::size_t frame_size = 0;
    auto status = decode_order_event_frame(std::span(buf).first(50), key, decoded, frame_size);
    EXPECT_EQ(status, FrameDecodeStatus::Truncated);
    EXPECT_EQ(frame_size, kOrderEventFrameSize)
        << "the frame's own length fields were readable, so the needed size is known even "
           "though the bytes aren't all present";
}

TEST(OrderEventFrameCodec, SingleByteCorruptionInPayloadIsChecksumMismatch) {
    AuditRecord rec = make_sample_record();
    auto key = as_key("k");
    std::vector<std::byte> buf(kOrderEventFrameSize);
    encode_order_event_frame(buf, 0, FrameTimeKind::ServerCorrectedUtc, 1000, rec, kZeroMac, key);

    // Flip a byte inside the payload region (well past the 23-byte header).
    buf[40] ^= std::byte{0x01};

    DecodedOrderFrame decoded{};
    std::size_t frame_size = 0;
    auto status = decode_order_event_frame(buf, key, decoded, frame_size);
    EXPECT_EQ(status, FrameDecodeStatus::ChecksumMismatch);
    EXPECT_EQ(frame_size, kOrderEventFrameSize);
}

TEST(OrderEventFrameCodec, SingleByteCorruptionOnLastByteIsAlsoChecksumMismatchNotTruncated) {
    // The round-6-P0 distinction this whole design exists to enforce: a
    // complete-but-corrupt frame at the PHYSICAL END of the buffer must never
    // be treated like a torn write.
    AuditRecord rec = make_sample_record();
    auto key = as_key("k");
    std::vector<std::byte> buf(kOrderEventFrameSize);
    encode_order_event_frame(buf, 0, FrameTimeKind::ServerCorrectedUtc, 1000, rec, kZeroMac, key);

    buf[kOrderEventFrameSize - 1] ^= std::byte{0x01};  // last byte of the mac itself

    DecodedOrderFrame decoded{};
    std::size_t frame_size = 0;
    auto status = decode_order_event_frame(buf, key, decoded, frame_size);
    EXPECT_EQ(status, FrameDecodeStatus::ChecksumMismatch);
}

TEST(OrderEventFrameCodec, CorruptedPrevMacIsChecksumMismatch) {
    // prev_mac is covered BY the mac computation -- tampering with it (even
    // though it's "just" a field, not the payload) must be caught.
    AuditRecord rec = make_sample_record();
    auto key = as_key("k");
    std::vector<std::byte> buf(kOrderEventFrameSize);
    encode_order_event_frame(buf, 0, FrameTimeKind::ServerCorrectedUtc, 1000, rec, kZeroMac, key);

    const std::size_t prev_mac_offset = kOrderEventFrameSize - kMacLen - kMacLen;
    buf[prev_mac_offset] ^= std::byte{0x01};

    DecodedOrderFrame decoded{};
    std::size_t frame_size = 0;
    EXPECT_EQ(decode_order_event_frame(buf, key, decoded, frame_size), FrameDecodeStatus::ChecksumMismatch);
}

TEST(OrderEventFrameCodec, WrongKeyFailsChecksum) {
    AuditRecord rec = make_sample_record();
    std::vector<std::byte> buf(kOrderEventFrameSize);
    encode_order_event_frame(buf, 0, FrameTimeKind::ServerCorrectedUtc, 1000, rec, kZeroMac, as_key("key-a"));

    DecodedOrderFrame decoded{};
    std::size_t frame_size = 0;
    EXPECT_EQ(decode_order_event_frame(buf, as_key("key-b"), decoded, frame_size),
              FrameDecodeStatus::ChecksumMismatch);
}

TEST(OrderEventFrameCodec, WrongFormatVersionRejected) {
    AuditRecord rec = make_sample_record();
    auto key = as_key("k");
    std::vector<std::byte> buf(kOrderEventFrameSize);
    encode_order_event_frame(buf, 0, FrameTimeKind::ServerCorrectedUtc, 1000, rec, kZeroMac, key);
    buf[0] = std::byte{4};  // valid-looking but not kFrameFormatVersion(3)

    DecodedOrderFrame decoded{};
    std::size_t frame_size = 0;
    EXPECT_EQ(decode_order_event_frame(buf, key, decoded, frame_size), FrameDecodeStatus::UnknownVersion);
}

TEST(OrderEventFrameCodec, OutOfRangeRecordTypeRejected) {
    AuditRecord rec = make_sample_record();
    auto key = as_key("k");
    std::vector<std::byte> buf(kOrderEventFrameSize);
    encode_order_event_frame(buf, 0, FrameTimeKind::ServerCorrectedUtc, 1000, rec, kZeroMac, key);
    buf[1] = std::byte{255};  // record_type byte

    DecodedOrderFrame decoded{};
    std::size_t frame_size = 0;
    EXPECT_EQ(decode_order_event_frame(buf, key, decoded, frame_size), FrameDecodeStatus::MalformedEnum);
}

TEST(OrderEventFrameCodec, OutOfRangeTimeKindRejected) {
    AuditRecord rec = make_sample_record();
    auto key = as_key("k");
    std::vector<std::byte> buf(kOrderEventFrameSize);
    encode_order_event_frame(buf, 0, FrameTimeKind::ServerCorrectedUtc, 1000, rec, kZeroMac, key);
    buf[10] = std::byte{99};  // time_kind byte (offset 1+1+8=10)

    DecodedOrderFrame decoded{};
    std::size_t frame_size = 0;
    EXPECT_EQ(decode_order_event_frame(buf, key, decoded, frame_size), FrameDecodeStatus::MalformedEnum);
}

TEST(OrderEventFrameCodec, WrongPayloadLengthRejected) {
    AuditRecord rec = make_sample_record();
    auto key = as_key("k");
    std::vector<std::byte> buf(kOrderEventFrameSize);
    encode_order_event_frame(buf, 0, FrameTimeKind::ServerCorrectedUtc, 1000, rec, kZeroMac, key);

    // payload_length is bytes [19,23) LE u32. Corrupt just the low byte.
    buf[19] ^= std::byte{0x01};

    DecodedOrderFrame decoded{};
    std::size_t frame_size = 0;
    EXPECT_EQ(decode_order_event_frame(buf, key, decoded, frame_size), FrameDecodeStatus::PayloadLengthInvalid);
}

TEST(OrderEventFrameCodec, MalformedPayloadEnumRejected) {
    // event_type inside the payload is corrupted -- distinct failure path
    // from the frame-header enum checks above.
    AuditRecord rec = make_sample_record();
    auto key = as_key("k");
    std::vector<std::byte> buf(kOrderEventFrameSize);
    encode_order_event_frame(buf, 0, FrameTimeKind::ServerCorrectedUtc, 1000, rec, kZeroMac, key);

    constexpr std::size_t kHeaderSize = 23;
    buf[kHeaderSize + 8] = std::byte{200};  // event_type is payload offset 8

    DecodedOrderFrame decoded{};
    std::size_t frame_size = 0;
    EXPECT_EQ(decode_order_event_frame(buf, key, decoded, frame_size), FrameDecodeStatus::MalformedEnum);
}

TEST(OrderEventFrameCodec, EncodeFailsOnBufferTooSmall) {
    AuditRecord rec = make_sample_record();
    auto key = as_key("k");
    std::vector<std::byte> buf(kOrderEventFrameSize - 1);
    EXPECT_EQ(encode_order_event_frame(buf, 0, FrameTimeKind::ServerCorrectedUtc, 1000, rec, kZeroMac, key), 0u);
}
