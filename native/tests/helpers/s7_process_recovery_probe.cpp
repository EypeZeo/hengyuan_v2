// Subprocess helper for S7 migration crash-window tests. Each action uses a
// fresh fixture directory containing audit.log and its ordinary sidecars.
#include <hengyuan/durable_audit_archive.hpp>
#include <hengyuan/durable_audit_sink.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace hy;
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kKeyId = 17;
constexpr std::int64_t kCreatedUtcMs = 1'800'000'000'000;
constexpr std::int64_t kFrameTimeMs = 1'800'000'000'100;
constexpr char kOrderA[] = "HY-S7-RECOVERY-A";
constexpr char kOrderB[] = "HY-S7-RECOVERY-B";

bool fail(std::string_view action, std::string_view reason) {
    std::cerr << action << ": " << reason << '\n';
    return false;
}

std::array<std::byte, kKekSize> make_kek() {
    std::array<std::byte, kKekSize> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::byte>(0x31u + i);
    return bytes;
}

std::array<std::byte, 32> make_plaintext_key() {
    std::array<std::byte, 32> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::byte>(0x71u + i);
    return bytes;
}

struct TestKeys {
    std::array<std::byte, kKekSize> kek = make_kek();
    KeyRing ring{std::span<const std::byte, kKekSize>(kek.data(), kek.size())};
    bool ready{false};

    TestKeys() {
        const auto plaintext = make_plaintext_key();
        WrappedKeyRecord wrapped{};
        ready = ring.add_key(kKeyId, std::span<const std::byte>(plaintext.data(), plaintext.size()), wrapped) ==
                KeyRingAddStatus::Ok;
    }
};

struct Fixture {
    fs::path directory;
    fs::path log;

    explicit Fixture(const char* path) : directory(path), log(directory / "audit.log") {}

    fs::path anchor() const { return fs::path(log.string() + ".tip"); }
    fs::path identity() const { return fs::path(log.string() + ".storeid.tip"); }
    fs::path temporary_anchor() const { return fs::path(anchor().string() + ".tmp"); }
    fs::path migration_archive() const { return fs::path(log.string() + ".s7.archive"); }
};

AuditRecord make_intent(const char* coid, std::int64_t time_ms = kFrameTimeMs) {
    AuditRecord record{};
    record.timestamp_ms = time_ms;
    record.event_type = AuditEventType::OrderIntentCreated;
    record.mode = ExecutionMode::Live;
    record.symbol_id = 7;
    record.set_client_order_id(coid);
    record.price_ticks = 500'000;
    record.qty_ticks = 100;
    record.resulting_state = OrderState::Intent;
    record.side = OrderSide::Buy;
    record.set_detail("synthetic S7 process-recovery fixture");
    return record;
}

AuditRecord make_prepared() {
    AuditRecord record = make_intent(kOrderA, kFrameTimeMs + 1);
    record.event_type = AuditEventType::OrderSubmitPrepared;
    record.resulting_state = OrderState::Submitting;
    return record;
}

bool read_bytes(const fs::path& path, std::vector<std::byte>& out) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::vector<char> raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) return false;
    out.resize(raw.size());
    if (!raw.empty()) std::memcpy(out.data(), raw.data(), raw.size());
    return true;
}

bool source_verification(const Fixture& fixture, const KeyRing& ring, AuditLogVerification& verification,
                         bool require_complete) {
    std::vector<std::byte> log;
    std::vector<std::byte> anchor;
    std::vector<std::byte> identity;
    if (!read_bytes(fixture.log, log) || !read_bytes(fixture.anchor(), anchor) || !read_bytes(fixture.identity(), identity)) {
        return false;
    }
    ExportOptions options{};
    options.require_complete_source = require_complete;
    verification = verify_audit_log(std::span<const std::byte>(log), ring, true, std::span<const std::byte>(anchor),
                                    true, std::span<const std::byte>(identity), options);
    return verification.ok();
}

bool archive_verification(const Fixture& fixture, const KeyRing& ring, std::uint64_t expected_frames,
                          std::string_view action) {
    const ExportFileResult result = verify_audit_archive_file(fixture.migration_archive().string(), ring);
    if (result.status != ExportStatus::Ok) {
        return fail(action, std::string("migration archive verification failed: ") + export_status_name(result.status));
    }
    if (result.verification.frames != expected_frames || result.verification.first_v6_sequence.has_value()) {
        return fail(action, "migration archive no longer describes the original v5 source");
    }
    return true;
}

bool seed_fixture(const Fixture& fixture, const KeyRing& ring, std::string_view action) {
    if (!fs::is_regular_file(fixture.log) || !fs::is_regular_file(fixture.migration_archive())) {
        return fail(action, "seed fixture files are missing");
    }
    if (!archive_verification(fixture, ring, 1, action)) return false;
    AuditLogVerification verification{};
    if (!source_verification(fixture, ring, verification, true)) {
        return fail(action, std::string("seed source verification failed: ") + export_status_name(verification.status));
    }
    std::vector<std::byte> log;
    if (!read_bytes(fixture.log, log) || log.size() != kOrderEventFrameSize ||
        static_cast<std::uint8_t>(log.front()) != kFrameFormatVersion || verification.frames != 1 ||
        verification.max_frame_version != kFrameFormatVersion || !verification.tip_anchor_sequence.has_value() ||
        verification.tip_anchor_sequence.value() != 0) {
        return fail(action, "fixture is not exactly one anchored v5 Intent frame");
    }
    return true;
}

std::unique_ptr<DurableAuditSink> open_required(const Fixture& fixture, KeyRing& ring) {
    return std::make_unique<DurableAuditSink>(fixture.log.string(), ring, kKeyId, kMaxIntentOrphans,
                                              AuditSinkOpenMode::Trading, NeverSentWrites::Disabled,
                                              kMaxDurableLogBytes / kOrderEventFrameSize,
                                              AuditMigrationPolicy::RequireArchive);
}

bool current_file_size(const fs::path& path, std::uintmax_t& size) {
    std::error_code error;
    size = fs::file_size(path, error);
    return !error;
}

bool make_fresh_directory(const Fixture& fixture) {
    std::error_code error;
    if (!fs::exists(fixture.directory, error)) {
        if (error || !fs::create_directories(fixture.directory, error) || error) {
            return fail("seed", "could not create the fixture directory");
        }
    } else if (error || !fs::is_directory(fixture.directory, error) || error) {
        return fail("seed", "fixture path exists and is not a directory");
    }
    const fs::directory_iterator end;
    if (fs::directory_iterator(fixture.directory, error) != end || error) {
        return fail("seed", "fixture directory must be empty");
    }
    return true;
}

int seed(const Fixture& fixture, TestKeys& keys) {
    if (!make_fresh_directory(fixture)) return 1;
    if (!keys.ready) return fail("seed", "could not initialize the deterministic test key"), 1;
    {
        DurableAuditSink sink(fixture.log.string(), keys.ring, kKeyId);
        if (!sink.is_open() || sink.fenced()) return fail("seed", "could not open a fresh audit sink"), 1;
        if (!sink.append_durable(make_intent(kOrderA), kFrameTimeMs).acked()) {
            return fail("seed", "could not append the v5 Intent frame"), 1;
        }
    }
    const ExportFileResult archived = prepare_audit_migration(fixture.log.string(), keys.ring, kCreatedUtcMs);
    if (archived.status != ExportStatus::Ok) {
        return fail("seed", std::string("S7 archive preparation failed: ") + export_status_name(archived.status)), 1;
    }
    if (!seed_fixture(fixture, keys.ring, "seed")) return 1;
    std::cout << "seed: one v5 Intent and its S7 archive verified\n";
    return 0;
}

bool encode_first_v6_prepared(const Fixture& fixture, TestKeys& keys,
                              std::array<std::byte, kOrderEventFrameSize>& frame) {
    std::vector<std::byte> log;
    if (!read_bytes(fixture.log, log) || log.size() != kOrderEventFrameSize) return false;
    std::array<std::byte, kMacLen> previous_mac{};
    std::copy_n(log.begin() + static_cast<std::ptrdiff_t>(kOrderEventFrameSize - kMacLen), kMacLen,
                previous_mac.begin());
    std::array<std::byte, kKeyBlockSize> key_block{};
    if (!keys.ring.active_key(kKeyId, key_block)) return false;
    const std::size_t written = encode_order_event_frame(
        std::span<std::byte>(frame), kKeyId, 1, FrameTimeKind::ServerCorrectedUtc, kFrameTimeMs + 1,
        make_prepared(), std::span<const std::byte, kMacLen>(previous_mac),
        std::span<const std::byte>(key_block.data(), key_block.size()), kFrameFormatVersionV6);
    return written == kOrderEventFrameSize;
}

int crash_partial(const Fixture& fixture, TestKeys& keys) {
    if (!keys.ready || !seed_fixture(fixture, keys.ring, "crash-partial")) return 1;
    if (fs::exists(fixture.temporary_anchor())) return fail("crash-partial", "temporary anchor path already exists"), 1;
    std::array<std::byte, kOrderEventFrameSize> frame{};
    if (!encode_first_v6_prepared(fixture, keys, frame)) return fail("crash-partial", "could not encode the first v6 frame"), 1;

    DurableLogStore store(fixture.log.string(), fixture.log.string() + ".lock", fixture.log.string() + ".tip");
    if (!store.acquire_lock()) return fail("crash-partial", "could not acquire the audit log lock"), 1;
    if (!store.open_log()) return fail("crash-partial", "could not open the audit log"), 1;
    if (!store.append_and_fsync(std::span<const std::byte>(frame.data(), 2))) {
        return fail("crash-partial", "could not fsync the two-byte v6 prefix"), 1;
    }
    std::cout << "crash-partial: fsynced two bytes of the first v6 frame; exiting with the sink lock held\n" << std::flush;
    std::_Exit(73);
}

int crash_stale_anchor(const Fixture& fixture, TestKeys& keys) {
    if (!keys.ready || !seed_fixture(fixture, keys.ring, "crash-stale-anchor")) return 1;
    if (fs::exists(fixture.temporary_anchor())) return fail("crash-stale-anchor", "temporary anchor path already exists"), 1;
    std::uintmax_t size_before = 0;
    if (!current_file_size(fixture.log, size_before) || size_before != kOrderEventFrameSize) {
        return fail("crash-stale-anchor", "seed log size is not one complete frame"), 1;
    }
    std::error_code error;
    if (!fs::create_directory(fixture.temporary_anchor(), error) || error) {
        return fail("crash-stale-anchor", "could not create the empty temporary-anchor directory"), 1;
    }

    const auto sink = open_required(fixture, keys.ring);
    if (!sink->is_open() || sink->fenced() || sink->write_format_version() != kFrameFormatVersionV6) {
        return fail("crash-stale-anchor", "valid S7 migration did not open as a v6 writer"), 1;
    }
    const AuditAppendResult append = sink->append_durable(make_intent(kOrderB, kFrameTimeMs + 2), kFrameTimeMs + 2);
    if (append.status != AuditAppendResult::Status::Failed || append.acked() || !sink->fenced()) {
        return fail("crash-stale-anchor", "the blocked anchor update did not fail and fence the writer"), 1;
    }
    std::uintmax_t size_after = 0;
    if (!current_file_size(fixture.log, size_after) || size_after != size_before + kOrderEventFrameSize) {
        return fail("crash-stale-anchor", "the failed append did not leave exactly one complete v6 frame in the log"), 1;
    }
    std::cout << "crash-stale-anchor: v6 frame fsynced, anchor update failed, writer fenced; exiting with it open\n"
              << std::flush;
    std::_Exit(73);
}

int recover_partial(const Fixture& fixture, TestKeys& keys) {
    if (!keys.ready || !archive_verification(fixture, keys.ring, 1, "recover-partial")) return 1;
    AuditLogVerification before{};
    if (!source_verification(fixture, keys.ring, before, false) || before.frames != 1 || before.torn_tail_bytes != 2) {
        return fail("recover-partial", "fixture does not contain one v5 frame followed by a two-byte torn v6 prefix"), 1;
    }

    {
        const auto sink = open_required(fixture, keys.ring);
        if (!sink->is_open() || sink->fenced() || sink->recovered_torn_tail_bytes() != 2 ||
            sink->write_format_version() != kFrameFormatVersionV6) {
            return fail("recover-partial", "restart did not cut the prefix and select v6"), 1;
        }
    }
    std::vector<std::byte> cut_log;
    if (!read_bytes(fixture.log, cut_log) || cut_log.size() != kOrderEventFrameSize ||
        static_cast<std::uint8_t>(cut_log.front()) != kFrameFormatVersion) {
        return fail("recover-partial", "recovery did not physically trim the log to its one v5 frame"), 1;
    }
    {
        const auto sink = open_required(fixture, keys.ring);
        if (!sink->is_open() || sink->fenced() || sink->write_format_version() != kFrameFormatVersionV6 ||
            sink->recovered_torn_tail_bytes() != 0) {
            return fail("recover-partial", "post-trim restart did not retain v6 as the write version"), 1;
        }
        if (!sink->append_durable(make_intent(kOrderB, kFrameTimeMs + 2), kFrameTimeMs + 2).acked()) {
            return fail("recover-partial", "the first post-recovery v6 append was not acknowledged"), 1;
        }
    }

    std::vector<std::byte> recovered_log;
    if (!read_bytes(fixture.log, recovered_log) || recovered_log.size() != 2 * kOrderEventFrameSize ||
        static_cast<std::uint8_t>(recovered_log[kOrderEventFrameSize]) != kFrameFormatVersionV6) {
        return fail("recover-partial", "recovery did not leave one v5 frame followed by one v6 frame"), 1;
    }
    AuditLogVerification recovered{};
    if (!source_verification(fixture, keys.ring, recovered, true) || recovered.frames != 2 ||
        recovered.torn_tail_bytes != 0 || !recovered.first_v6_sequence.has_value() ||
        recovered.first_v6_sequence.value() != 1 || !recovered.tip_anchor_sequence.has_value() ||
        recovered.tip_anchor_sequence.value() != 1) {
        return fail("recover-partial", std::string("strict source verification failed: ") + export_status_name(recovered.status)), 1;
    }
    if (!archive_verification(fixture, keys.ring, 1, "recover-partial")) return 1;
    std::vector<std::byte> stable_anchor;
    if (!read_bytes(fixture.anchor(), stable_anchor)) return fail("recover-partial", "could not read the repaired anchor"), 1;

    {
        const auto sink = open_required(fixture, keys.ring);
        if (!sink->is_open() || sink->fenced() || sink->recovered_torn_tail_bytes() != 0 ||
            sink->write_format_version() != kFrameFormatVersionV6) {
            return fail("recover-partial", "second restart did not preserve the recovered v6 store"), 1;
        }
    }
    std::vector<std::byte> second_log;
    std::vector<std::byte> second_anchor;
    if (!read_bytes(fixture.log, second_log) || !read_bytes(fixture.anchor(), second_anchor) ||
        second_log != recovered_log || second_anchor != stable_anchor) {
        return fail("recover-partial", "second restart changed the recovered log or anchor"), 1;
    }
    AuditLogVerification second{};
    if (!source_verification(fixture, keys.ring, second, true) || second.frames != 2 ||
        !archive_verification(fixture, keys.ring, 1, "recover-partial")) {
        return fail("recover-partial", "strict verification failed after the second restart"), 1;
    }
    std::cout << "recover-partial: trimmed two bytes, wrote and verified one v6 frame; second restart was stable\n";
    return 0;
}

int recover_stale_anchor(const Fixture& fixture, TestKeys& keys) {
    if (!keys.ready || !archive_verification(fixture, keys.ring, 1, "recover-stale-anchor")) return 1;
    std::error_code error;
    if (!fs::is_directory(fixture.temporary_anchor(), error) || error || !fs::is_empty(fixture.temporary_anchor(), error) ||
        error || !fs::remove(fixture.temporary_anchor(), error) || error) {
        return fail("recover-stale-anchor", "could not remove the injected empty temporary-anchor directory"), 1;
    }
    std::uintmax_t log_size = 0;
    if (!current_file_size(fixture.log, log_size) || log_size != 2 * kOrderEventFrameSize) {
        return fail("recover-stale-anchor", "fixture does not contain exactly two complete frames"), 1;
    }

    {
        const auto sink = open_required(fixture, keys.ring);
        if (!sink->is_open() || sink->fenced() || sink->recovered_torn_tail_bytes() != 0 ||
            sink->write_format_version() != kFrameFormatVersionV6) {
            return fail("recover-stale-anchor", "restart did not recover the complete v6 frame and repair its anchor"), 1;
        }
    }

    AuditLogVerification repaired{};
    if (!source_verification(fixture, keys.ring, repaired, true) || repaired.frames != 2 ||
        repaired.max_frame_version != kFrameFormatVersionV6 || !repaired.first_v6_sequence.has_value() ||
        repaired.first_v6_sequence.value() != 1 || !repaired.tip_anchor_sequence.has_value() ||
        repaired.tip_anchor_sequence.value() != 1) {
        return fail("recover-stale-anchor", std::string("repaired source verification failed: ") + export_status_name(repaired.status)), 1;
    }
    if (!archive_verification(fixture, keys.ring, 1, "recover-stale-anchor")) return 1;
    std::vector<std::byte> recovered_log;
    std::vector<std::byte> repaired_anchor;
    if (!read_bytes(fixture.log, recovered_log) || !read_bytes(fixture.anchor(), repaired_anchor)) {
        return fail("recover-stale-anchor", "could not read the recovered source files");
    }

    {
        const auto sink = open_required(fixture, keys.ring);
        if (!sink->is_open() || sink->fenced() || sink->recovered_torn_tail_bytes() != 0 ||
            sink->write_format_version() != kFrameFormatVersionV6) {
            return fail("recover-stale-anchor", "second restart did not preserve the repaired v6 store"), 1;
        }
    }
    std::vector<std::byte> second_log;
    std::vector<std::byte> second_anchor;
    if (!read_bytes(fixture.log, second_log) || !read_bytes(fixture.anchor(), second_anchor) ||
        second_log != recovered_log || second_anchor != repaired_anchor) {
        return fail("recover-stale-anchor", "second restart changed the recovered log or anchor"), 1;
    }
    AuditLogVerification second{};
    if (!source_verification(fixture, keys.ring, second, true) || second.frames != 2 ||
        !second.tip_anchor_sequence.has_value() || second.tip_anchor_sequence.value() != 1 ||
        !archive_verification(fixture, keys.ring, 1, "recover-stale-anchor")) {
        return fail("recover-stale-anchor", "strict verification failed after the second restart"), 1;
    }
    std::cout << "recover-stale-anchor: repaired the anchor for two frames; both source and archive verify across restart\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: s7_process_recovery_probe <seed|crash-partial|crash-stale-anchor|recover-partial|recover-stale-anchor> <fresh-fixture-dir>\n";
        return 2;
    }
    Fixture fixture(argv[2]);
    TestKeys keys;
    const std::string_view action(argv[1]);
    if (action == "seed") return seed(fixture, keys);
    if (action == "crash-partial") return crash_partial(fixture, keys);
    if (action == "crash-stale-anchor") return crash_stale_anchor(fixture, keys);
    if (action == "recover-partial") return recover_partial(fixture, keys);
    if (action == "recover-stale-anchor") return recover_stale_anchor(fixture, keys);
    std::cerr << "unknown action: " << action << '\n';
    return 2;
}
