// 批次 6 6c: offline export of a durable audit log. Every test writes a REAL DurableAuditSink log (never a
// hand-built frame stream) so the export's understanding of the wire format is proven against the actual writer,
// not against this test's own assumptions about it. Corruption is then injected byte-for-byte into that real log.

#include <gtest/gtest.h>
#include <hengyuan/durable_audit_export.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __linux__
#include <unistd.h>
#endif

using namespace hy;

namespace {

std::string scratch_prefix() {
    static int counter = 0;
    ++counter;
#ifdef _WIN32
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + "hy_audit_export_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter);
#else
    return "/tmp/hy_audit_export_" + std::to_string(getpid()) + "_" + std::to_string(counter);
#endif
}

void remove_sink_files(const std::string& base) {
    for (const char* suffix : {"", ".lock", ".tip", ".tip.tmp", ".keyrotations", ".keyrotations.lock",
                               ".keyrotations.tip", ".keyrotations.tip.tmp", ".storeid", ".storeid.lock",
                               ".storeid.tip", ".storeid.tip.tmp"}) {
        std::remove((base + suffix).c_str());
    }
}

// One real KEK, one real KeyRing, one real DurableAuditSink -- the same fixture shape as
// test_verified_dry_run_evidence.cpp's DrillRig, so a log this fixture writes is exactly what production writes.
struct SinkFixture {
    std::string path;
    std::array<std::byte, kKekSize> kek{};
    std::unique_ptr<KeyRing> ring;
    std::unique_ptr<DurableAuditSink> sink;

    explicit SinkFixture(std::uint32_t key_id = 1) : path(scratch_prefix()) {
        remove_sink_files(path);
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x40 + i);
        ring = std::make_unique<KeyRing>(std::span<const std::byte, kKekSize>(kek.data(), kek.size()));
        const std::array<std::byte, 32> plaintext = make_key_bytes(key_id);
        WrappedKeyRecord rec{};
        EXPECT_EQ(ring->add_key(key_id, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
                  KeyRingAddStatus::Ok);
        sink = std::make_unique<DurableAuditSink>(path, *ring, key_id);
        EXPECT_TRUE(sink->is_open());
        EXPECT_FALSE(sink->fenced());
    }

    ~SinkFixture() {
        sink.reset();
        ring.reset();
        remove_sink_files(path);
        std::remove(out_path().c_str());
    }

    SinkFixture(const SinkFixture&) = delete;
    SinkFixture& operator=(const SinkFixture&) = delete;

    static std::array<std::byte, 32> make_key_bytes(std::uint32_t key_id) {
        std::array<std::byte, 32> k{};
        for (std::size_t i = 0; i < k.size(); ++i) {
            k[i] = static_cast<std::byte>(static_cast<std::uint8_t>(0x10 + i + key_id * 7));
        }
        return k;
    }

    std::string out_path() const { return path + ".export.ndjson"; }

    void append_one(std::int64_t now_ms, const char* coid, OrderState state = OrderState::Accepted,
                    std::int64_t filled = 0) {
        AuditRecord ar{};
        ar.timestamp_ms = now_ms;
        ar.event_type = (state == OrderState::Intent)   ? AuditEventType::OrderIntentCreated
                        : (state == OrderState::Rejected) ? AuditEventType::OrderRejected
                                                          : AuditEventType::OrderAccepted;
        ar.mode = ExecutionMode::DryRun;
        ar.symbol_id = 1;
        ar.set_client_order_id(coid);
        ar.exchange_order_id = 42;
        ar.price_ticks = 6'000'000;
        ar.qty_ticks = 100;
        ar.resulting_state = state;
        ar.filled_qty_ticks = filled;
        ar.avg_fill_price_ticks = filled > 0 ? 6'000'000 : 0;
        ar.side = OrderSide::Buy;
        ar.set_detail("t");
        const AuditAppendResult r = sink->append_durable(ar, now_ms);
        ASSERT_TRUE(r.acked()) << "fixture append must succeed for the test to mean anything";
    }

    // A first appearance for coid MUST be resulting_state Intent (recovery_scan()'s own per-COID rule) --
    // convenience for tests that need a legal multi-record chain for one order.
    void append_intent(std::int64_t now_ms, const char* coid) { append_one(now_ms, coid, OrderState::Intent); }

    // The sink opens its log AND its lock file with zero sharing (durable_log_store.hpp): while it is alive, not
    // even a same-process std::ifstream can open the log for reading, and export_audit_log_file() correctly reads
    // the still-held lock as WriterLive. Every test must call this -- ending the "writer" phase -- before it reads
    // the log's raw bytes or calls export_audit_log_file(): this is not a workaround, it is this tool's actual
    // contract (OFFLINE ONLY, see the header), so the tests naturally have the same two phases production does.
    void close_writer() { sink.reset(); }
};

std::vector<std::byte> read_whole(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    std::vector<std::byte> out(size > 0 ? static_cast<std::size_t>(size) : 0);
    if (size > 0) {
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(out.data()), size);
    }
    return out;
}

void write_whole(const std::string& path, std::span<const std::byte> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::string> split_lines(std::span<const std::byte> bytes) {
    std::vector<std::string> lines;
    std::string cur;
    for (const std::byte b : bytes) {
        const char c = static_cast<char>(b);
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

bool contains(const std::string& haystack, std::string_view needle) { return haystack.find(needle) != std::string::npos; }

}  // namespace

// --- the happy path ------------------------------------------------------------------------------------------------

TEST(DurableAuditExport, ExportsAWrittenLogWithAManifestAndOneLinePerFrame) {
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.append_one(1001, "COID-0000000000000001", OrderState::Accepted, 0);
    fx.append_intent(1002, "COID-0000000000000002");
    fx.append_one(1002, "COID-0000000000000002", OrderState::Rejected, 0);
    fx.close_writer();

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);

    const ExportFileResult r = export_audit_log_file(fx.path, fx.out_path(), reader_ring);
    ASSERT_EQ(r.status, ExportStatus::Ok) << export_status_name(r.status);
    EXPECT_EQ(r.verification.frames, 4U);
    EXPECT_EQ(r.verification.torn_tail_bytes, 0U);
    EXPECT_EQ(r.verification.anchor, AnchorState::Verified);
    EXPECT_TRUE(std::filesystem::exists(fx.out_path()));

    const std::vector<std::byte> raw = read_whole(fx.out_path());
    const std::vector<std::string> lines = split_lines(raw);
    ASSERT_EQ(lines.size(), 5U) << "1 manifest + 4 frames";
    EXPECT_TRUE(contains(lines[0], "\"type\":\"manifest\""));
    EXPECT_TRUE(contains(lines[0], "\"frames\":4"));
    EXPECT_TRUE(contains(lines[0], "\"tail_seq\":3"));
    EXPECT_TRUE(contains(lines[0], "\"tip_anchor\":\"verified\""));
    EXPECT_TRUE(contains(lines[0], "\"key_ids\":[1]"));

    EXPECT_TRUE(contains(lines[1], "\"seq\":0"));
    EXPECT_TRUE(contains(lines[1], "\"event\":\"ORDER_INTENT_CREATED\""))
        << lines[1];  // Intent's event_type is OrderIntentCreated per audit_event_name(); confirms real names, not raw ints
    EXPECT_TRUE(contains(lines[2], "\"seq\":1"));
    EXPECT_TRUE(contains(lines[2], "\"event\":\"ORDER_ACCEPTED\""));
    EXPECT_TRUE(contains(lines[2], "\"resulting_state\":\"Accepted\""));
    EXPECT_TRUE(contains(lines[2], "\"side\":\"BUY\""));
    EXPECT_TRUE(contains(lines[2], "\"client_order_id\":\"COID-0000000000000001\""));
    // qty_ticks (100) and filled_qty_ticks (0, the fixture's default) differ, so this also proves the two fields
    // aren't swapped -- "qty_ticks":100 alone would still be present even if filled_qty_ticks wrongly rendered it.
    EXPECT_TRUE(contains(lines[2], "\"qty_ticks\":100")) << lines[2];
    EXPECT_TRUE(contains(lines[2], "\"filled_qty_ticks\":0")) << lines[2];
    EXPECT_FALSE(contains(lines[2], "\"mac\":\"\"")) << "a real 32-byte mac, not an empty one: " << lines[2];
    {
        const std::size_t key = lines[2].find("\"mac\":\"");
        ASSERT_NE(key, std::string::npos);
        const std::size_t value_start = key + 7;  // strlen("\"mac\":\"")
        const std::size_t value_end = lines[2].find('"', value_start);
        ASSERT_NE(value_end, std::string::npos);
        EXPECT_EQ(value_end - value_start, kMacLen * 2) << "hex-encoded 32-byte mac: " << lines[2];
    }
    EXPECT_TRUE(contains(lines[4], "\"resulting_state\":\"Rejected\""));

    for (const std::string& line : lines) {
        EXPECT_EQ(std::count(line.begin(), line.end(), '{'), std::count(line.begin(), line.end(), '}')) << line;
    }
}

TEST(DurableAuditExport, AnEmptyLogExportsJustTheManifest) {
    SinkFixture fx;  // no appends
    ASSERT_EQ(fx.sink->recovery_status(), RecoveryScanStatus::Clean);
    fx.close_writer();

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);

    const ExportFileResult r = export_audit_log_file(fx.path, fx.out_path(), reader_ring);
    ASSERT_EQ(r.status, ExportStatus::Ok) << export_status_name(r.status);
    EXPECT_EQ(r.verification.frames, 0U);
    EXPECT_EQ(r.verification.anchor, AnchorState::EmptyLog);
    const std::vector<std::string> lines = split_lines(read_whole(fx.out_path()));
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_TRUE(contains(lines[0], "\"frames\":0"));
    EXPECT_TRUE(contains(lines[0], "\"first_seq\":null"));
}

TEST(DurableAuditExport, StoreIdentityIsCarriedIntoTheManifestWhenPresent) {
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    ASSERT_NE(fx.sink->store_uuid_lo(), 0U) << "establish_store_identity() ran on construction";
    const std::uint64_t expected_lo = fx.sink->store_uuid_lo();
    const std::uint64_t expected_hi = fx.sink->store_uuid_hi();
    fx.close_writer();

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    const ExportFileResult r = export_audit_log_file(fx.path, fx.out_path(), reader_ring);
    ASSERT_EQ(r.status, ExportStatus::Ok) << export_status_name(r.status);
    EXPECT_EQ(r.verification.identity, IdentityState::Verified);
    EXPECT_EQ(r.verification.store_uuid_lo, expected_lo);
    EXPECT_EQ(r.verification.store_uuid_hi, expected_hi);
    const std::string manifest = split_lines(read_whole(fx.out_path()))[0];
    EXPECT_FALSE(contains(manifest, "\"store_uuid\":null"));
}

// --- torn tail vs. real corruption --------------------------------------------------------------------------------

TEST(DurableAuditExport, ATornTailExportsEverythingBeforeItAndSaysSo) {
    SinkFixture fx2;
    fx2.append_intent(1000, "COID-0000000000000001");
    fx2.append_one(1001, "COID-0000000000000001", OrderState::Accepted, 0);
    fx2.close_writer();  // the anchor is now fsynced for the full 2-frame log
    const std::vector<std::byte> full_log = read_whole(fx2.path);
    ASSERT_FALSE(full_log.empty());

    // Cut the ON-DISK log short by 10 bytes -- a plausible torn tail (a crash mid-append never leaves a frame
    // boundary) -- while the already-fsynced anchor still claims the full, untruncated tip.
    const std::vector<std::byte> torn(full_log.begin(), full_log.end() - 10);
    write_whole(fx2.path, torn);

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx2.kek.data(), fx2.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    ExportOptions opts;
    opts.allow_missing_anchor = false;
    const ExportFileResult r = export_audit_log_file(fx2.path, fx2.out_path(), reader_ring, opts);
    // The anchor (seq 1, full log's mac) no longer matches what the truncated log's chain can prove (it now has
    // only frame 0 complete, frame 1 torn) -- AnchorAheadOfLog is the correct, honest classification: the anchor
    // vouches for a tip the log no longer physically has. This is NOT the "torn tail is fine" path; a torn tail is
    // only legal when the anchor does NOT outrun what remains.
    EXPECT_EQ(r.status, ExportStatus::AnchorAheadOfLog) << export_status_name(r.status);
    EXPECT_FALSE(std::filesystem::exists(fx2.out_path())) << "a hard error must produce no output file";
}

TEST(DurableAuditExport, ATornTailWithAConsistentAnchorExportsTheCompletePrefix) {
    // One complete, anchored frame, then 4 raw junk bytes appended directly to the log file -- never through the
    // sink, so no anchor was ever fsynced for them: a genuine torn tail whose anchor matches the last COMPLETE
    // frame exactly (unlike the previous test, where the anchor outruns what remains).
    //
    // Exactly 4 bytes, not some other small number: peek_frame_key_id() needs >= 6 bytes to even attempt a key
    // lookup (durable_frame_codec.hpp's own kMinBytes). Below that, verify_audit_log() -- mirroring
    // run_recovery_scan()'s own documented reasoning -- never touches the ring at all and falls straight through
    // to decode_order_event_frame()'s length check, which is Truncated for anything under its 20-byte header. At
    // 6..19 bytes the peek WOULD succeed and read 4 garbage bytes as a key_id; if those do not happen to name a
    // real key (overwhelmingly likely for real torn garbage) the result is UnknownKeyId, not Truncated -- a real,
    // narrow, and INHERITED edge case (peek_frame_key_id() is used exactly this way, with exactly this trade-off
    // accepted, in the real recovery scan), not something to paper over with a fragile choice of bytes here.
    SinkFixture fx2;
    fx2.append_intent(2000, "COID-0000000000000003");
    fx2.close_writer();
    const std::vector<std::byte> log = read_whole(fx2.path);
    ASSERT_FALSE(log.empty());
    std::vector<std::byte> with_junk = log;
    const std::array<std::byte, 4> junk = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    with_junk.insert(with_junk.end(), junk.begin(), junk.end());
    write_whole(fx2.path, with_junk);

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx2.kek.data(), fx2.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    const ExportFileResult r = export_audit_log_file(fx2.path, fx2.out_path(), reader_ring);
    ASSERT_EQ(r.status, ExportStatus::Ok) << export_status_name(r.status);
    EXPECT_EQ(r.verification.frames, 1U);
    EXPECT_EQ(r.verification.torn_tail_bytes, 4U);
    EXPECT_EQ(r.verification.anchor, AnchorState::Verified) << "the anchor matches the one complete frame";
    const std::vector<std::string> lines = split_lines(read_whole(fx2.out_path()));
    ASSERT_EQ(lines.size(), 2U) << "manifest + the one complete frame; the torn 4 bytes are not a frame";
    EXPECT_TRUE(contains(lines[0], "\"torn_tail_bytes\":4"));
}

// --- byte-level corruption: every kind is a hard error, none produce output ---------------------------------------

namespace {

struct CorruptionCase {
    const char* name;
    ExportStatus expected;
};

}  // namespace

TEST(DurableAuditExport, FlippingAnyByteOfAFrameIsChecksumMismatchAndProducesNoOutput) {
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.append_one(1001, "COID-0000000000000001", OrderState::Accepted, 0);
    fx.close_writer();
    const std::vector<std::byte> good = read_whole(fx.path);
    ASSERT_GT(good.size(), 100U);

    // Flip one bit in the middle of the frame payload (well inside the header/payload, before the macs at the
    // tail) -- exhaustively flipping every byte would make this test disproportionately slow; the frame codec
    // itself has its own exhaustive corruption tests (test_durable_frame_codec.cpp). This proves the EXPORT layer
    // reacts correctly to the kind of damage that layer will actually see.
    for (const std::size_t offset : {std::size_t{20}, good.size() / 2, good.size() - 40}) {
        SCOPED_TRACE(offset);
        std::vector<std::byte> corrupt = good;
        corrupt[offset] ^= std::byte{0x01};
        SinkFixture victim;
        victim.close_writer();  // only its empty directory/path is wanted; the sink must let go of the file first
        write_whole(victim.path, corrupt);
        KeyRing reader_ring(std::span<const std::byte, kKekSize>(victim.kek.data(), victim.kek.size()));
        WrappedKeyRecord rec{};
        const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
        ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
                  KeyRingAddStatus::Ok);
        // The anchor still claims the original (now-invalid) tip.
        const ExportFileResult r = export_audit_log_file(victim.path, victim.out_path(), reader_ring);
        EXPECT_NE(r.status, ExportStatus::Ok);
        EXPECT_FALSE(std::filesystem::exists(victim.out_path()));
    }
}

TEST(DurableAuditExport, AnUnknownKeyIdIsAHardError) {
    SinkFixture fx(/*key_id=*/5);
    fx.append_intent(1000, "COID-0000000000000001");
    fx.close_writer();
    // The anchor is signed under the same unresolvable key 5 and is checked FIRST (its sequence number is what
    // the frame scan needs to remember which frame to cross-check) -- with it present, a ring that knows nothing
    // would fail on the anchor (AnchorInvalid) before ever reaching a single frame, which is a real and correctly
    // ordered outcome but not the one THIS test isolates (see the dedicated test right below). Removing it (with
    // allow_missing_anchor) means the only thing left to fail on is the frame's own key id.
    std::remove((fx.path + ".tip").c_str());

    // A ring that never learned key id 5.
    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    ExportOptions opts;
    opts.allow_missing_anchor = true;
    const ExportFileResult r = export_audit_log_file(fx.path, fx.out_path(), reader_ring, opts);
    EXPECT_EQ(r.status, ExportStatus::UnknownKeyId) << export_status_name(r.status);
    EXPECT_EQ(r.verification.error_offset, 0U) << "the one frame this log has starts at offset 0";
    EXPECT_EQ(r.verification.error_sequence, 0U);
    EXPECT_FALSE(std::filesystem::exists(fx.out_path()));
}

TEST(DurableAuditExport, AnAnchorSignedUnderAnUnresolvableKeyIsAnchorInvalid) {
    SinkFixture fx(/*key_id=*/5);
    fx.append_intent(1000, "COID-0000000000000001");
    fx.close_writer();

    // The anchor is present this time (the default): a ring that knows nothing fails there first, exactly as
    // described above, and never reaches the frame loop at all.
    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    const ExportFileResult r = export_audit_log_file(fx.path, fx.out_path(), reader_ring);
    EXPECT_EQ(r.status, ExportStatus::AnchorInvalid) << export_status_name(r.status);
    EXPECT_FALSE(std::filesystem::exists(fx.out_path()));
}

TEST(DurableAuditExport, AChecksumMismatchRecordsWhereItWasFound) {
    // A single-frame log, corrupted well inside the frame (past the header, before the trailing macs) -- exactly
    // where this landed, unlike the multi-offset test above, is unambiguous: sequence 0, offset 0.
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.close_writer();
    std::vector<std::byte> corrupt = read_whole(fx.path);
    ASSERT_GT(corrupt.size(), 100U);
    corrupt[50] ^= std::byte{0x01};

    SinkFixture victim;
    victim.close_writer();
    write_whole(victim.path, corrupt);
    KeyRing reader_ring(std::span<const std::byte, kKekSize>(victim.kek.data(), victim.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    const ExportFileResult r = export_audit_log_file(victim.path, victim.out_path(), reader_ring);
    ASSERT_EQ(r.status, ExportStatus::ChecksumMismatch) << export_status_name(r.status);
    EXPECT_EQ(r.verification.error_offset, 0U);
    EXPECT_EQ(r.verification.error_sequence, 0U);
}

TEST(DurableAuditExport, ASequenceGapIsAHardError) {
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.append_one(1001, "COID-0000000000000001", OrderState::Accepted, 0);
    fx.close_writer();
    std::vector<std::byte> log = read_whole(fx.path);
    // Duplicate frame 0's bytes in place of frame 1 -- decodes fine on its own (valid MAC under its own content)
    // but its sequence_number (0) is not the expected next one (1).
    const std::size_t half = log.size() / 2;  // both frames the same nominal size makes this a fair split
    std::vector<std::byte> mutated(log.begin(), log.begin() + static_cast<long>(half));
    mutated.insert(mutated.end(), log.begin(), log.begin() + static_cast<long>(half));

    SinkFixture victim;
    victim.close_writer();
    write_whole(victim.path, mutated);
    KeyRing reader_ring(std::span<const std::byte, kKekSize>(victim.kek.data(), victim.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    const ExportFileResult r = export_audit_log_file(victim.path, victim.out_path(), reader_ring);
    // Either SequenceGap (if the duplicate frame decodes but its seq is wrong) or ChecksumMismatch/ChainBroken (if
    // the split point does not land on a frame boundary) is an acceptable hard-error outcome here -- what matters
    // is that it is NEVER Ok and NEVER produces output; the exact classification is pinned by the two narrower
    // tests below instead.
    EXPECT_NE(r.status, ExportStatus::Ok) << export_status_name(r.status);
    EXPECT_FALSE(std::filesystem::exists(victim.out_path()));
}

namespace {

// Overwrites frame 1's mutable header field at `field_offset` (relative to the frame's own start, which is
// `kOrderEventFrameSize` since frame 0 comes first) with `new_value`, then recomputes frame 1's OWN trailing mac
// over its now-modified content -- exactly what encode_order_event_frame() itself covers (everything from the
// frame's start through prev_mac) -- so decode_order_event_frame() accepts the frame on its own merits and the
// ONLY thing wrong with it is the field this helper changed. Without the recompute, changing any header byte
// would fail on ChecksumMismatch before the field-specific check below it is ever reached -- proving nothing
// about that specific check.
std::vector<std::byte> log_with_frame1_field_overwritten(const std::vector<std::byte>& two_frame_log,
                                                          std::size_t field_offset, std::uint64_t new_value,
                                                          const KeyRing& ring, std::uint32_t key_id) {
    std::vector<std::byte> mutated = two_frame_log;
    std::byte* field = mutated.data() + kOrderEventFrameSize + field_offset;
    detail::write_u64_le(field, new_value);

    std::array<std::byte, kKeyBlockSize> key_block{};
    if (!ring.active_key(key_id, key_block)) return {};
    const std::size_t frame1_start = kOrderEventFrameSize;
    const std::size_t content_len = kOrderEventFrameSize - kMacLen;
    const auto mac = crypto::hmac_sha256(std::span<const std::byte>(key_block.data(), key_block.size()),
                                         std::span<const std::byte>(mutated.data() + frame1_start, content_len));
    const std::span<const std::byte> mac_bytes = mac.as_bytes();
    std::copy(mac_bytes.begin(), mac_bytes.end(), mutated.begin() + static_cast<long>(frame1_start + content_len));
    return mutated;
}

}  // namespace

TEST(DurableAuditExport, ASequenceNumberMismatchWithAnOtherwiseIntactChainIsSequenceGapNotChainBroken) {
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.append_one(1001, "COID-0000000000000001", OrderState::Accepted, 0);
    fx.close_writer();
    const std::vector<std::byte> log = read_whole(fx.path);

    SinkFixture victim;
    victim.close_writer();
    KeyRing reader_ring(std::span<const std::byte, kKekSize>(victim.kek.data(), victim.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    // sequence_number is 8 bytes at header offset 6 (format_version + record_type + key_id). Frame 1's prev_mac
    // (unaffected by this) still correctly names frame 0's real mac -- the chain itself is intact, only the
    // sequence number is wrong, so this cannot also be caught as ChainBroken.
    const std::vector<std::byte> mutated = log_with_frame1_field_overwritten(log, 6, 5, reader_ring, 1);
    ASSERT_FALSE(mutated.empty());
    write_whole(victim.path, mutated);

    const ExportFileResult r = export_audit_log_file(victim.path, victim.out_path(), reader_ring);
    EXPECT_EQ(r.status, ExportStatus::SequenceGap) << export_status_name(r.status);
    EXPECT_EQ(r.verification.error_offset, kOrderEventFrameSize);
    EXPECT_EQ(r.verification.error_sequence, 1U);
    EXPECT_FALSE(std::filesystem::exists(victim.out_path()));
}

TEST(DurableAuditExport, APrevMacMismatchWithACorrectSequenceNumberIsChainBrokenNotSequenceGap) {
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.append_one(1001, "COID-0000000000000001", OrderState::Accepted, 0);
    fx.close_writer();
    const std::vector<std::byte> log = read_whole(fx.path);

    SinkFixture victim;
    victim.close_writer();
    KeyRing reader_ring(std::span<const std::byte, kKekSize>(victim.kek.data(), victim.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    // prev_mac is the 32 bytes right after the payload, at header(27) + kAuditRecordWireSize. Overwrite its first
    // 8 bytes -- frame 1's own sequence_number (correctly 1) is untouched, so this cannot also read as SequenceGap.
    const std::size_t prev_mac_offset = 27 + kAuditRecordWireSize;
    const std::vector<std::byte> mutated =
        log_with_frame1_field_overwritten(log, prev_mac_offset, 0xDEADBEEFULL, reader_ring, 1);
    ASSERT_FALSE(mutated.empty());
    write_whole(victim.path, mutated);

    const ExportFileResult r = export_audit_log_file(victim.path, victim.out_path(), reader_ring);
    EXPECT_EQ(r.status, ExportStatus::ChainBroken) << export_status_name(r.status);
    EXPECT_EQ(r.verification.error_offset, kOrderEventFrameSize);
    EXPECT_EQ(r.verification.error_sequence, 1U);
    EXPECT_FALSE(std::filesystem::exists(victim.out_path()));
}

TEST(DurableAuditExport, AnUnknownKeyIdOnANonFirstFrameRecordsItsOwnOffsetAndSequence) {
    // AnUnknownKeyIdIsAHardError puts the bad frame at sequence 0 / offset 0, where error_offset/error_sequence
    // staying at their struct default (0) is indistinguishable from actually being recorded. Put the unresolvable
    // key on frame 1 instead so a real assignment is the only way these come out non-zero.
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.append_one(1001, "COID-0000000000000001", OrderState::Accepted, 0);
    fx.close_writer();
    const std::vector<std::byte> log = read_whole(fx.path);

    SinkFixture victim;
    victim.close_writer();
    KeyRing reader_ring(std::span<const std::byte, kKekSize>(victim.kek.data(), victim.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    // key_id is 4 bytes at header offset 2; the helper's 8-byte write also clobbers the low 4 bytes of the
    // following sequence_number field, which is harmless here -- an unknown key id is refused before
    // sequence_number (or the mac this helper recomputes under the still-known key 1) is ever inspected.
    const std::vector<std::byte> mutated = log_with_frame1_field_overwritten(log, 2, 0xFFFFFFFFULL, reader_ring, 1);
    ASSERT_FALSE(mutated.empty());
    write_whole(victim.path, mutated);

    const ExportFileResult r = export_audit_log_file(victim.path, victim.out_path(), reader_ring);
    EXPECT_EQ(r.status, ExportStatus::UnknownKeyId) << export_status_name(r.status);
    EXPECT_EQ(r.verification.error_offset, kOrderEventFrameSize);
    EXPECT_EQ(r.verification.error_sequence, 1U);
    EXPECT_FALSE(std::filesystem::exists(victim.out_path()));
}

TEST(DurableAuditExport, AChecksumMismatchOnANonFirstFrameRecordsItsOwnSequence) {
    // AChecksumMismatchRecordsWhereItWasFound corrupts the only frame in a single-frame log (sequence 0), where
    // error_sequence staying at its struct default (0) is indistinguishable from actually being recorded.
    // Corrupt frame 1 of a two-frame log instead.
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.append_one(1001, "COID-0000000000000001", OrderState::Accepted, 0);
    fx.close_writer();
    std::vector<std::byte> corrupt = read_whole(fx.path);
    ASSERT_GE(corrupt.size(), 2 * kOrderEventFrameSize);
    corrupt[kOrderEventFrameSize + 50] ^= std::byte{0x01};  // inside frame 1's payload, past its header

    SinkFixture victim;
    victim.close_writer();
    write_whole(victim.path, corrupt);
    KeyRing reader_ring(std::span<const std::byte, kKekSize>(victim.kek.data(), victim.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    const ExportFileResult r = export_audit_log_file(victim.path, victim.out_path(), reader_ring);
    ASSERT_EQ(r.status, ExportStatus::ChecksumMismatch) << export_status_name(r.status);
    EXPECT_EQ(r.verification.error_offset, kOrderEventFrameSize);
    EXPECT_EQ(r.verification.error_sequence, 1U);
    EXPECT_FALSE(std::filesystem::exists(victim.out_path()));
}

TEST(DurableAuditExport, AnUnexpectedRecordTypeIsAHardError) {
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.close_writer();
    std::vector<std::byte> log = read_whole(fx.path);
    ASSERT_GE(log.size(), kMacLen) << "the fixture's own log must have been read successfully for this test to mean anything";
    // record_type is the second byte of the frame (format_version, record_type, ...). OrderEvent == 0; flip it to
    // an adjacent legal-but-wrong DurableRecordType value. Recomputing the MAC is not needed to prove the check
    // fires (a wrong record_type is rejected before -- in program order, AFTER -- the MAC check in this file's own
    // logic; either way the outcome must be a hard error), but to isolate record_type specifically (not just any
    // corruption) the MAC must still match, so recompute it over the mutated header.
    ASSERT_EQ(static_cast<std::uint8_t>(log[1]), 0U) << "OrderEvent must be 0 for this test to target the right byte";
    log[1] = static_cast<std::byte>(1);  // any other legal DurableRecordType enumerator
    // Recompute the frame's own mac so ChecksumMismatch cannot mask UnexpectedRecordType: the mac covers the
    // header through prev_mac, so locate it (frame is fixed-size for OrderEvent) and rewrite the trailing 32 bytes.
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    KeyRing recompute_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    ASSERT_EQ(recompute_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    std::array<std::byte, kKeyBlockSize> key_block{};
    ASSERT_TRUE(recompute_ring.active_key(1, key_block));
    const std::size_t content_len = log.size() - kMacLen;
    const auto mac = crypto::hmac_sha256(std::span<const std::byte>(key_block.data(), key_block.size()),
                                         std::span<const std::byte>(log.data(), content_len));
    const std::span<const std::byte> mac_bytes = mac.as_bytes();
    std::copy(mac_bytes.begin(), mac_bytes.end(), log.end() - static_cast<long>(kMacLen));

    SinkFixture victim;
    victim.close_writer();
    write_whole(victim.path, log);
    KeyRing reader_ring(std::span<const std::byte, kKekSize>(victim.kek.data(), victim.kek.size()));
    WrappedKeyRecord rec2{};
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec2),
              KeyRingAddStatus::Ok);
    const ExportFileResult r = export_audit_log_file(victim.path, victim.out_path(), reader_ring);
    EXPECT_EQ(r.status, ExportStatus::UnexpectedRecordType) << export_status_name(r.status);
    EXPECT_FALSE(std::filesystem::exists(victim.out_path()));
}

// --- the anchor and the writer-live probe -----------------------------------------------------------------------------

TEST(DurableAuditExport, AMissingAnchorRefusesByDefaultAndExportsWhenExplicitlyAllowed) {
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.close_writer();
    std::remove((fx.path + ".tip").c_str());  // the anchor is gone; the log is not

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);

    const ExportFileResult refused = export_audit_log_file(fx.path, fx.out_path(), reader_ring);
    EXPECT_EQ(refused.status, ExportStatus::AnchorMissing);
    EXPECT_FALSE(std::filesystem::exists(fx.out_path()));

    ExportOptions opts;
    opts.allow_missing_anchor = true;
    const ExportFileResult allowed = export_audit_log_file(fx.path, fx.out_path(), reader_ring, opts);
    ASSERT_EQ(allowed.status, ExportStatus::Ok);
    EXPECT_EQ(allowed.verification.anchor, AnchorState::AbsentAllowed);
    const std::string manifest = split_lines(read_whole(fx.out_path()))[0];
    EXPECT_TRUE(contains(manifest, "\"tip_anchor\":\"absent-allowed\""));
}

TEST(DurableAuditExport, AnAnchorAheadOfTheLogIsRollbackAndRefused) {
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.append_one(1001, "COID-0000000000000001", OrderState::Accepted, 0);
    fx.close_writer();
    const std::vector<std::byte> anchor_at_seq1 = read_whole(fx.path + ".tip");
    // Truncate the log back to just frame 0's bytes, but KEEP the anchor that was fsynced for seq 1 -- a tail
    // deletion (or a restore from an older log snapshot under a newer anchor) the export must refuse, not silently
    // accept as if it were an ordinary torn tail.
    const std::vector<std::byte> log = read_whole(fx.path);
    const std::size_t half = log.size() / 2;
    write_whole(fx.path, std::span<const std::byte>(log.data(), half));
    write_whole(fx.path + ".tip", anchor_at_seq1);

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    const ExportFileResult r = export_audit_log_file(fx.path, fx.out_path(), reader_ring);
    EXPECT_EQ(r.status, ExportStatus::AnchorAheadOfLog) << export_status_name(r.status);
    EXPECT_FALSE(std::filesystem::exists(fx.out_path()));
}

TEST(DurableAuditExport, AnAnchorThatIsItselfAuthenticButNamesTheWrongMacIsAnchorMismatch) {
    // A genuinely, honestly-signed anchor (its own anchor_mac verifies) for sequence 1 -- but the mac field IT
    // CARRIES is not frame 1's real mac. This is not "the anchor is malformed" (AnchorInvalid, checked earlier and
    // separately): the anchor decodes fine on its own; the log's real frame 1 simply does not agree with what it
    // claims. Isolates the one line that compares mac_at_anchor against anchor.mac.
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.append_one(1001, "COID-0000000000000001", OrderState::Accepted, 0);
    fx.close_writer();

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    std::array<std::byte, kKeyBlockSize> key_block{};
    ASSERT_TRUE(reader_ring.active_key(1, key_block));

    std::array<std::byte, kMacLen> wrong_mac{};
    wrong_mac.fill(std::byte{0xAB});  // not frame 1's real mac, by construction
    std::array<std::byte, kTipAnchorSize> fake_anchor{};
    detail::encode_tip_anchor(fake_anchor, /*sequence_number=*/1, wrong_mac, /*key_id=*/1,
                              std::span<const std::byte>(key_block.data(), key_block.size()));
    write_whole(fx.path + ".tip", std::span<const std::byte>(fake_anchor.data(), fake_anchor.size()));

    const ExportFileResult r = export_audit_log_file(fx.path, fx.out_path(), reader_ring);
    EXPECT_EQ(r.status, ExportStatus::AnchorMismatch) << export_status_name(r.status);
    EXPECT_FALSE(std::filesystem::exists(fx.out_path()));
}

TEST(DurableAuditExport, AWriterHoldingTheLockRefusesTheExport) {
    SinkFixture fx;  // the sink's constructor acquires the lock and keeps it for the fixture's lifetime
    fx.append_intent(1000, "COID-0000000000000001");

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    const ExportFileResult r = export_audit_log_file(fx.path, fx.out_path(), reader_ring);
    EXPECT_EQ(r.status, ExportStatus::WriterLive);
    EXPECT_FALSE(std::filesystem::exists(fx.out_path()));
}

TEST(DurableAuditExport, OnceTheWriterClosesTheExportProceeds) {
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.sink.reset();  // releases the lock (destructor)

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    const ExportFileResult r = export_audit_log_file(fx.path, fx.out_path(), reader_ring);
    EXPECT_EQ(r.status, ExportStatus::Ok) << export_status_name(r.status);
}

TEST(DurableAuditExport, ProbeWriterLockReportsNotHeldWhenThereIsNoLockFileAtAll) {
    const std::string missing = scratch_prefix();
    EXPECT_EQ(probe_writer_lock(missing + ".lock"), WriterProbe::NotHeld);
}

TEST(DurableAuditExport, ReadFileBoundedRefusesAFileOverItsOwnCap) {
    const std::string path = scratch_prefix() + ".bin";
    {
        std::ofstream out(path, std::ios::binary);
        out << "0123456789";  // 10 bytes
    }
    std::vector<std::byte> out;
    EXPECT_EQ(read_file_bounded(path, out, /*max_bytes=*/10), ReadFileStatus::Ok) << "exactly at the cap";
    EXPECT_EQ(read_file_bounded(path, out, /*max_bytes=*/9), ReadFileStatus::TooLarge) << "one byte over";
    std::remove(path.c_str());
}

TEST(DurableAuditExport, AnOversizedLogFileIsLogTooLarge) {
    // A sparse file just past kMaxDurableLogBytes -- its logical size is what read_file_bounded()'s cap check
    // sees, without this test actually writing (or this repo's CI actually storing) a real multi-gigabyte file.
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.close_writer();
    std::error_code ec;
    std::filesystem::resize_file(fx.path, kMaxDurableLogBytes + 1, ec);
    ASSERT_FALSE(ec) << ec.message();

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    const ExportFileResult r = export_audit_log_file(fx.path, fx.out_path(), reader_ring);
    EXPECT_EQ(r.status, ExportStatus::LogTooLarge) << export_status_name(r.status);
    EXPECT_FALSE(std::filesystem::exists(fx.out_path()));
}

TEST(DurableAuditExport, AnOversizedAnchorFileIsAnchorInvalidNotSilentlyAbsent) {
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.close_writer();
    {
        std::ofstream out(fx.path + ".tip", std::ios::binary | std::ios::trunc);
        out << std::string(4097, 'x');  // a real tip anchor is 77 bytes; this is not merely malformed, it is huge
    }

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    ExportOptions opts;
    opts.allow_missing_anchor = true;  // proves this is NOT read as "absent"; it would export under this option if it were
    const ExportFileResult r = export_audit_log_file(fx.path, fx.out_path(), reader_ring, opts);
    EXPECT_EQ(r.status, ExportStatus::AnchorInvalid) << export_status_name(r.status);
    EXPECT_FALSE(std::filesystem::exists(fx.out_path()));
}

// --- output file discipline: CREATE_NEW, and no half-written export survives a failure -------------------------------

TEST(DurableAuditExport, AnExistingOutputPathIsNeverOverwritten) {
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.close_writer();
    {
        std::ofstream pre(fx.out_path(), std::ios::binary);
        pre << "not a real export";
    }
    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    const ExportFileResult r = export_audit_log_file(fx.path, fx.out_path(), reader_ring);
    EXPECT_EQ(r.status, ExportStatus::OutputExists);
    const std::vector<std::byte> still_there = read_whole(fx.out_path());
    const std::string as_text(reinterpret_cast<const char*>(still_there.data()), still_there.size());
    EXPECT_EQ(as_text, "not a real export") << "the pre-existing file must be untouched";
}

TEST(DurableAuditExport, ExclusiveOutputFileRemovesWhatItCreatedOnAbandon) {
    const std::string path = scratch_prefix() + ".ndjson";
    {
        ExclusiveOutputFile out;
        ASSERT_EQ(out.create(path), ExclusiveOutputFile::OpenStatus::Ok);
        ASSERT_TRUE(out.write("partial"));
        // No commit(): goes out of scope and must remove the file it created.
    }
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(DurableAuditExport, ExclusiveOutputFileKeepsWhatItCommitted) {
    const std::string path = scratch_prefix() + ".ndjson";
    {
        ExclusiveOutputFile out;
        ASSERT_EQ(out.create(path), ExclusiveOutputFile::OpenStatus::Ok);
        ASSERT_TRUE(out.write("full\n"));
        ASSERT_TRUE(out.commit());
    }
    EXPECT_TRUE(std::filesystem::exists(path));
    const std::vector<std::byte> bytes = read_whole(path);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), "full\n");
    std::remove(path.c_str());
}

TEST(DurableAuditExport, ExclusiveOutputFileSecondCreateOnTheSamePathSeesExists) {
    const std::string path = scratch_prefix() + ".ndjson";
    ExclusiveOutputFile first;
    ASSERT_EQ(first.create(path), ExclusiveOutputFile::OpenStatus::Ok);
    ASSERT_TRUE(first.commit());
    ExclusiveOutputFile second;
    EXPECT_EQ(second.create(path), ExclusiveOutputFile::OpenStatus::Exists);
    std::remove(path.c_str());
}

TEST(DurableAuditExport, ExclusiveOutputFileRefusesWriteAndCommitBeforeACreateOrAfterOne) {
    // Never created at all.
    {
        ExclusiveOutputFile out;
        EXPECT_FALSE(out.write("x"));
        EXPECT_FALSE(out.commit());
    }
    // Created, then committed once -- write()/a second commit() must both see it as no longer open, not silently
    // "succeed" against a handle that no longer exists.
    const std::string path = scratch_prefix() + ".ndjson";
    ExclusiveOutputFile out;
    ASSERT_EQ(out.create(path), ExclusiveOutputFile::OpenStatus::Ok);
    ASSERT_TRUE(out.commit());
    EXPECT_FALSE(out.write("late"));
    EXPECT_FALSE(out.commit());
    const std::vector<std::byte> bytes = read_whole(path);
    EXPECT_EQ(bytes.size(), 0U) << "the refused write() must not have reached the file";
    std::remove(path.c_str());
}

// --- JSON string encoding ------------------------------------------------------------------------------------------

TEST(DurableAuditExport, JsonStringEscapesQuotesBackslashesAndControlBytesButNotPlainAscii) {
    std::string out;
    detail::export_json_string(out, "hello", 5);
    EXPECT_EQ(out, "\"hello\"");

    out.clear();
    const char with_quote_and_backslash[] = "a\"b\\c";
    detail::export_json_string(out, with_quote_and_backslash, sizeof(with_quote_and_backslash) - 1);
    EXPECT_EQ(out, "\"a\\\"b\\\\c\"");

    out.clear();
    const char with_control[] = "a\x01" "b";
    detail::export_json_string(out, with_control, 3);
    EXPECT_EQ(out, "\"a\\u0001b\"");
}

TEST(DurableAuditExport, JsonCstrStopsAtTheFirstNulAndIgnoresTrailingBufferBytes) {
    char buf[8] = {'h', 'i', '\0', 'X', 'X', 'X', 'X', 'X'};
    std::string out;
    detail::export_json_cstr(out, buf, sizeof(buf));
    EXPECT_EQ(out, "\"hi\"");

    char no_terminator[4] = {'a', 'b', 'c', 'd'};  // never hits NUL within cap
    out.clear();
    detail::export_json_cstr(out, no_terminator, sizeof(no_terminator));
    EXPECT_EQ(out, "\"abcd\"");
}

// DurableAuditSink itself only ever writes FrameTimeKind::ServerCorrectedUtc (every encode_*_frame call site in
// durable_audit_sink.hpp hardcodes it), so no SinkFixture-written log can ever produce an UnknownBootstrap frame to
// exercise the other branch of this word table; test the pure rendering functions directly instead.
TEST(DurableAuditExport, ModeWordAndTimeKindWordRenderEachEnumValueDistinctly) {
    EXPECT_STREQ(detail::export_mode_word(ExecutionMode::Live), "LIVE");
    EXPECT_STREQ(detail::export_mode_word(ExecutionMode::DryRun), "DRY_RUN");
    EXPECT_STREQ(detail::export_time_kind_word(FrameTimeKind::ServerCorrectedUtc), "SERVER_CORRECTED_UTC");
    EXPECT_STREQ(detail::export_time_kind_word(FrameTimeKind::UnknownBootstrap), "UNKNOWN_BOOTSTRAP");
}

// --- the key file (this tool's own input contract) ------------------------------------------------------------------

TEST(DurableAuditExport, KeyFileRoundTripsThroughEncodeAndLoad) {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x77 + i);
    KeyRing writer_ring(std::span<const std::byte, kKekSize>(kek.data(), kek.size()));
    std::vector<WrappedKeyRecord> records;
    for (std::uint32_t id : {1U, 2U, 3U}) {
        const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(id);
        WrappedKeyRecord rec{};
        ASSERT_EQ(writer_ring.add_key(id, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
                  KeyRingAddStatus::Ok);
        records.push_back(rec);
    }
    const std::vector<std::byte> file_bytes = encode_audit_key_file(records);

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(kek.data(), kek.size()));
    EXPECT_EQ(load_audit_key_file(file_bytes, reader_ring), AuditKeyFileStatus::Ok);
    for (std::uint32_t id : {1U, 2U, 3U}) {
        std::array<std::byte, kKeyBlockSize> got{};
        ASSERT_TRUE(reader_ring.active_key(id, got));
    }
}

TEST(DurableAuditExport, KeyFileRejectsBadMagicWrongLengthAndWrongKek) {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x99 + i);
    KeyRing writer_ring(std::span<const std::byte, kKekSize>(kek.data(), kek.size()));
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    WrappedKeyRecord rec{};
    ASSERT_EQ(writer_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    const std::vector<std::byte> good = encode_audit_key_file({&rec, 1});

    {
        std::vector<std::byte> bad_magic = good;
        bad_magic[0] = std::byte{'X'};
        KeyRing ring(std::span<const std::byte, kKekSize>(kek.data(), kek.size()));
        EXPECT_EQ(load_audit_key_file(bad_magic, ring), AuditKeyFileStatus::BadMagic);
    }
    {
        std::vector<std::byte> truncated(good.begin(), good.end() - 1);
        KeyRing ring(std::span<const std::byte, kKekSize>(kek.data(), kek.size()));
        EXPECT_EQ(load_audit_key_file(truncated, ring), AuditKeyFileStatus::BadLength);
    }
    {
        std::array<std::byte, kKekSize> wrong_kek{};
        for (std::size_t i = 0; i < wrong_kek.size(); ++i) wrong_kek[i] = std::byte{0};
        KeyRing ring(std::span<const std::byte, kKekSize>(wrong_kek.data(), wrong_kek.size()));
        EXPECT_EQ(load_audit_key_file(good, ring), AuditKeyFileStatus::TagMismatch);
    }
}

TEST(DurableAuditExport, KeyFileRejectsMoreRecordsThanTheRingCanHold) {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x22 + i);
    std::vector<WrappedKeyRecord> records;
    // One more than kMaxLiveKeys, every record genuinely valid on its own (a real, correctly-sized, correctly
    // declared file) -- so only the count-vs-capacity guard itself can be what refuses it, not a length mismatch.
    // A fresh ring per key (same KEK throughout, so every wrap is mutually compatible): the WRITER side must
    // produce more than kMaxLiveKeys records too, and one ring can never itself hold more than that many.
    for (std::uint32_t id = 1; id <= kMaxLiveKeys + 1; ++id) {
        KeyRing writer_ring(std::span<const std::byte, kKekSize>(kek.data(), kek.size()));
        const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(id);
        WrappedKeyRecord rec{};
        ASSERT_EQ(writer_ring.add_key(id, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
                  KeyRingAddStatus::Ok);
        records.push_back(rec);
    }
    const std::vector<std::byte> file_bytes = encode_audit_key_file(records);

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(kek.data(), kek.size()));
    EXPECT_EQ(load_audit_key_file(file_bytes, reader_ring), AuditKeyFileStatus::TooManyKeys);
    // The file-level count-vs-capacity guard must refuse before the loop ever touches the ring -- not rely on
    // the ring's own per-key TableFull to eventually catch the (kMaxLiveKeys+1)th record after the loop has
    // already loaded the first kMaxLiveKeys of them into it. Confirm the ring is untouched: without this guard,
    // the loop would run and key id 1 (the file's first record) would already be active.
    std::array<std::byte, kKeyBlockSize> probe{};
    EXPECT_FALSE(reader_ring.active_key(1, probe))
        << "the count check must fire before any record is loaded, not after loading kMaxLiveKeys of them";
}

TEST(DurableAuditExport, KeyFileRejectsADuplicateKeyId) {
    std::array<std::byte, kKekSize> kek{};
    for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x55 + i);
    // Two independently-wrapped records that both claim key id 1 -- each individually valid (own fresh writer
    // ring, same KEK, so both wraps verify), the only thing wrong is the file assigns the same id twice.
    std::vector<WrappedKeyRecord> records;
    for (int copy = 0; copy < 2; ++copy) {
        KeyRing writer_ring(std::span<const std::byte, kKekSize>(kek.data(), kek.size()));
        const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
        WrappedKeyRecord rec{};
        ASSERT_EQ(writer_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
                  KeyRingAddStatus::Ok);
        records.push_back(rec);
    }
    const std::vector<std::byte> file_bytes = encode_audit_key_file(records);

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(kek.data(), kek.size()));
    EXPECT_EQ(load_audit_key_file(file_bytes, reader_ring), AuditKeyFileStatus::DuplicateKeyId);
}

// --- verify_audit_log() directly, without file I/O: negative controls on the pure function --------------------------

TEST(DurableAuditExportPure, AnAnchorOverAnEmptyLogIsRejectedByThePureFunctionDirectly) {
    // A minimal, deliberately empty log with an anchor that claims a nonexistent tip -- proves verify_audit_log()
    // itself (not just the file wrapper) rejects an anchor that outruns an empty log.
    std::array<std::byte, kKekSize> kek{};
    KeyRing real_ring(std::span<const std::byte, kKekSize>(kek.data(), kek.size()));
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    WrappedKeyRecord rec{};
    ASSERT_EQ(real_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);

    std::array<std::byte, kKeyBlockSize> key_block{};
    ASSERT_TRUE(real_ring.active_key(1, key_block));
    std::array<std::byte, kTipAnchorSize> anchor_buf{};
    std::array<std::byte, kMacLen> some_mac{};
    detail::encode_tip_anchor(anchor_buf, /*sequence_number=*/0, some_mac, /*key_id=*/1,
                              std::span<const std::byte>(key_block.data(), key_block.size()));

    const AuditLogVerification v = verify_audit_log(/*log=*/{}, real_ring, /*anchor_present=*/true,
                                                     std::span<const std::byte>(anchor_buf.data(), anchor_buf.size()),
                                                     /*identity_present=*/false, {});
    EXPECT_EQ(v.status, ExportStatus::AnchorAheadOfLog);
}

TEST(DurableAuditExportPure, EmitStopsWhenTheCallbackReturnsFalse) {
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.append_one(1001, "COID-0000000000000001", OrderState::Accepted, 0);
    fx.close_writer();
    const std::vector<std::byte> log = read_whole(fx.path);
    const std::vector<std::byte> anchor = read_whole(fx.path + ".tip");

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    const AuditLogVerification v =
        verify_audit_log(log, reader_ring, true, anchor, false, {});
    ASSERT_TRUE(v.ok());

    int calls = 0;
    const bool completed = emit_audit_export(log, reader_ring, v, [&](std::string_view) {
        ++calls;
        return calls < 2;  // stop right after the manifest line
    });
    EXPECT_FALSE(completed);
    EXPECT_EQ(calls, 2);
}

TEST(DurableAuditExportPure, EmitStopsOnTheManifestLineItselfWithoutEmittingAnyFrame) {
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.close_writer();
    const std::vector<std::byte> log = read_whole(fx.path);
    const std::vector<std::byte> anchor = read_whole(fx.path + ".tip");

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);
    const AuditLogVerification v = verify_audit_log(log, reader_ring, true, anchor, false, {});
    ASSERT_TRUE(v.ok());
    ASSERT_EQ(v.frames, 1U);

    int calls = 0;
    const bool completed = emit_audit_export(log, reader_ring, v, [&](std::string_view) {
        ++calls;
        return false;  // refuse even the manifest line
    });
    EXPECT_FALSE(completed);
    EXPECT_EQ(calls, 1) << "the frame must never be attempted once the manifest line itself was refused";
}

TEST(DurableAuditExportPure, EmitRefusesWhenTheLogNoLongerMatchesTheVerificationItWasGivenFor) {
    // emit_audit_export()'s contract is that `verification` is the ok() result of verify_audit_log() over the
    // SAME `log` -- export_audit_log_file() always calls it that way (both read the one in-memory buffer, nothing
    // between the two calls can change it), so this defends a caller discipline invariant, not a reachable
    // production state. Proven here by breaking that discipline on purpose: a hand-built verification claims 2
    // frames, but the log passed in only physically contains ONE real frame repeated twice -- so its "second
    // frame" decodes fine on its own (a real, self-consistent frame) but carries sequence_number 0, not the 1 the
    // loop expects at that position.
    SinkFixture fx;
    fx.append_intent(1000, "COID-0000000000000001");
    fx.close_writer();
    const std::vector<std::byte> one_frame = read_whole(fx.path);
    ASSERT_EQ(one_frame.size(), kOrderEventFrameSize);
    std::vector<std::byte> duplicated = one_frame;
    duplicated.insert(duplicated.end(), one_frame.begin(), one_frame.end());

    KeyRing reader_ring(std::span<const std::byte, kKekSize>(fx.kek.data(), fx.kek.size()));
    WrappedKeyRecord rec{};
    const std::array<std::byte, 32> plaintext = SinkFixture::make_key_bytes(1);
    ASSERT_EQ(reader_ring.add_key(1, std::span<const std::byte>(plaintext.data(), plaintext.size()), rec),
              KeyRingAddStatus::Ok);

    AuditLogVerification fake;
    fake.status = ExportStatus::Ok;
    fake.frames = 2;  // a lie relative to `duplicated`'s real content

    int calls = 0;
    const bool completed = emit_audit_export(duplicated, reader_ring, fake, [&](std::string_view) {
        ++calls;
        return true;
    });
    EXPECT_FALSE(completed);
    EXPECT_EQ(calls, 2) << "the manifest, then the genuinely-valid frame 0 -- refused only once it reaches the "
                           "mislabeled second copy";
}

TEST(DurableAuditExportPure, EmitRefusesAVerificationThatWasNotOk) {
    AuditLogVerification bad;
    bad.status = ExportStatus::ChecksumMismatch;
    std::array<std::byte, kKekSize> kek{};
    KeyRing ring(std::span<const std::byte, kKekSize>(kek.data(), kek.size()));
    int calls = 0;
    const bool completed = emit_audit_export({}, ring, bad, [&](std::string_view) {
        ++calls;
        return true;
    });
    EXPECT_FALSE(completed);
    EXPECT_EQ(calls, 0) << "must not even write the manifest for a failed verification";
}

TEST(DurableAuditExportPure, StatusNamesAreAllDistinctAndUnknownIsAQuestionMark) {
    const ExportStatus all[] = {
        ExportStatus::Ok,           ExportStatus::IoError,           ExportStatus::LogTooLarge,
        ExportStatus::WriterLive,   ExportStatus::WriterUnknown,     ExportStatus::UnknownKeyId,
        ExportStatus::UnknownVersion, ExportStatus::MalformedFrame,  ExportStatus::ChecksumMismatch,
        ExportStatus::SequenceGap, ExportStatus::ChainBroken,        ExportStatus::UnexpectedRecordType,
        ExportStatus::AnchorMissing, ExportStatus::AnchorInvalid,    ExportStatus::AnchorAheadOfLog,
        ExportStatus::AnchorMismatch, ExportStatus::StoreIdentityInvalid, ExportStatus::OutputExists,
        ExportStatus::OutputError,
    };
    std::vector<std::string> names;
    for (const ExportStatus s : all) names.emplace_back(export_status_name(s));
    std::vector<std::string> unique_names = names;
    std::sort(unique_names.begin(), unique_names.end());
    unique_names.erase(std::unique(unique_names.begin(), unique_names.end()), unique_names.end());
    EXPECT_EQ(names.size(), unique_names.size()) << "two enumerators share a name";
    EXPECT_STREQ(export_status_name(static_cast<ExportStatus>(250)), "?");
}
