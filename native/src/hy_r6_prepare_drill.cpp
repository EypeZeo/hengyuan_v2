// SPDX-License-Identifier: proprietary
// D5 R6: create a standalone, synthetic Testnet store at the Prepared boundary.
#include <hengyuan/control_plane_log_sink.hpp>
#include <hengyuan/durable_audit_export.hpp>
#include <hengyuan/store_binding_writer.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace {

constexpr std::uint32_t kDrillKeyId = 1;
constexpr std::string_view kDrillSymbol = "BTCUSDT";
constexpr std::int64_t kMinDrillTimeMs = 1'577'836'800'000;  // 2020-01-01 UTC
constexpr std::int64_t kMaxDrillTimeMs = 4'102'444'800'000;  // 2100-01-01 UTC

void usage(std::FILE* out) {
    std::fputs("Usage: hy_r6_prepare_drill --output-dir <new-directory> --uid <Testnet-account-uid> [--empty]\n"
               "Creates a synthetic Prepared BTCUSDT order and local keys for the R6 Testnet drill.\n"
               "Use --empty to create a bound store without a synthetic order.\n"
               "Use a private parent directory. No credentials or exchange connection are used.\n", out);
}

int failure(const char* message) {
    std::fprintf(stderr, "ERROR: %s\n", message);
    return 1;
}

bool write_file(const std::string& path, std::string_view bytes) {
    hy::ExclusiveOutputFile file;
    return file.create(path) == hy::ExclusiveOutputFile::OpenStatus::Ok &&
           file.write(bytes) && file.commit();
}

bool write_bytes(const std::string& path, std::span<const std::byte> bytes) {
    return write_file(path, {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

int create_drill(const std::filesystem::path& directory, std::uint64_t uid, bool empty) {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (now_ms < kMinDrillTimeMs + 1'000 || now_ms > kMaxDrillTimeMs) {
        return failure("Set the system clock to the current time before creating the drill.");
    }

    if (!std::filesystem::create_directory(directory)) {
        return failure("The output directory already exists. Choose a new directory.");
    }
#ifndef _WIN32
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
#endif
    const auto audit_path = (directory / "audit.log").string();
    const auto control_path = (directory / "control.log").string();
    const auto kek_path = (directory / "operator.kek").string();
    const auto key_path = (directory / "audit.keys").string();

    auto ring = [&]() -> std::unique_ptr<hy::KeyRing> {
        std::array<std::byte, hy::kKekSize> kek{};
        hy::ScopedSecureWipe wipe_kek(kek);
        std::array<std::byte, 32> key{};
        hy::ScopedSecureWipe wipe_key(key);
        if (!hy::detail::fill_random_bytes(kek) || !hy::detail::fill_random_bytes(key)) return {};

        auto keys = std::make_unique<hy::KeyRing>(kek);
        hy::WrappedKeyRecord record{};
        if (keys->add_key(kDrillKeyId, key, record) != hy::KeyRingAddStatus::Ok) return {};
        const auto encoded = hy::encode_audit_key_file(std::span(&record, 1));
        if (encoded.empty() || !write_bytes(kek_path, kek) || !write_bytes(key_path, encoded)) return {};
        return keys;
    }();
    if (!ring) return failure("Could not create local drill keys.");

    std::uint32_t sequence{};
    if (!hy::detail::fill_random_bytes(std::as_writable_bytes(std::span(&sequence, 1)))) {
        return failure("Could not generate a drill order ID.");
    }
    constexpr std::uint32_t kSymbolId = 0;
    const auto coid = hy::make_client_order_id(now_ms, sequence, kSymbolId);
    {
        auto audit = std::make_unique<hy::DurableAuditSink>(audit_path, *ring, kDrillKeyId);
        auto control = std::make_unique<hy::ControlPlaneLogSink>(control_path, *ring, kDrillKeyId);
        if (!audit->is_open() || audit->fenced() || !control->is_open() || control->fenced()) {
            return failure("Could not open both drill logs.");
        }
        if (hy::ensure_store_binding(*audit, audit_path, *ring, hy::StoreEnvironment::Testnet, uid, now_ms) !=
            hy::EnsureBindingStatus::BoundNew) {
            return failure("Could not bind the new drill store to the Testnet account.");
        }

        hy::SymbolRules rules{};
        kDrillSymbol.copy(rules.symbol, kDrillSymbol.size());
        rules.is_trading = true;
        rules.rules_version = 1;
        hy::SymbolRegistrySnapshotPayload snapshot{};
        snapshot.timestamp_ms = now_ms - 1'000;
        snapshot.rules_version = rules.rules_version;
        snapshot.symbol_count = 1;
        if (!control->append_snapshot(snapshot, std::span(&rules, 1)).acked()) {
            return failure("The symbol snapshot was not acknowledged.");
        }

        if (empty) {
            std::puts("Empty Testnet store created. audit_key_id=1 control_key_id=1");
            return 0;
        }
        hy::AuditRecord order{};
        order.timestamp_ms = now_ms;
        // The resolver treats this as a possible exchange order. Storage and network
        // environments are independent of the order-event mode.
        order.mode = hy::ExecutionMode::Live;
        order.symbol_id = kSymbolId;
        order.price_ticks = 500'000;
        order.qty_ticks = 100;
        order.side = hy::OrderSide::Buy;
        order.set_client_order_id(coid.view());
        order.event_type = hy::AuditEventType::OrderIntentCreated;
        order.resulting_state = hy::OrderState::Intent;
        if (!audit->append_durable(order, now_ms).acked()) return failure("Intent was not acknowledged.");

        order.event_type = hy::AuditEventType::OrderSubmitPrepared;
        order.resulting_state = hy::OrderState::Submitting;
        if (!audit->append_durable(order, now_ms).acked()) return failure("Prepared was not acknowledged.");
    }

    const std::string report = "R6 synthetic Testnet drill. No order was sent.\ncoid=" + std::string(coid.view()) + "\n";
    if (!write_file((directory / "r6-drill.txt").string(), report)) {
        return failure("Could not write the drill completion record.");
    }
    std::fputs(report.c_str(), stdout);
    std::printf("audit_key_id=%u control_key_id=%u\n", kDrillKeyId, kDrillKeyId);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage(stdout);
        return 0;
    }
    const bool empty = argc == 6 && std::string_view(argv[5]) == "--empty";
    if ((argc != 5 && !empty) || std::string_view(argv[1]) != "--output-dir" ||
        argv[2][0] == '\0' || std::string_view(argv[3]) != "--uid") {
        usage(stderr);
        return 2;
    }
    const std::string_view uid_text(argv[4]);
    std::uint64_t uid{};
    const auto end = uid_text.data() + uid_text.size();
    const auto parsed = std::from_chars(uid_text.data(), end, uid);
    if (parsed.ec != std::errc{} || parsed.ptr != end || uid == 0) {
        std::fputs("ERROR: --uid must be a nonzero decimal Testnet account UID.\n", stderr);
        return 2;
    }
    try {
        return create_drill(argv[2], uid, empty);
    } catch (const std::exception& e) {
        return failure(e.what());
    }
}
