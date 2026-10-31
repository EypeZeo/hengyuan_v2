// SPDX-License-Identifier: proprietary
// operator_resolution_tool.hpp -- the offline operator tool from the command line to the exit code (D5 slice R3b-2).
//
// Governance: offline tool only (nothing in the trading process includes it -- test_operator_resolution_guard.cpp pins that). The flow
// (operator_resolution_flow.hpp) is the procedure over injected ports; this header is everything around it that is not a port: the key
// material, the two logs, the order of events, and what each way of stopping exits with. Boost-free on purpose: the exchange comes in
// through an ExchangeProvider, so the whole tool runs in the default build against fakes, and only operator_resolution_real_exchange.hpp
// (credentials, TLS client) needs Boost and OpenSSL.
//
// The order of events, and why (docs/OPERATOR_RESOLUTION_DESIGN.md 6):
//   1. parse the command line (operator_resolution_cli.hpp); --help prints the usage and exits 0;
//   2. check on the file system that the files the command line names as different files are different files;
//   3. resolve mode only: ask the provider for the exchange -- credentials loaded, client built -- BEFORE any log is touched, so a
//      missing or wrong credentials file ends the run with both logs byte for byte as they were and no lock ever held;
//   4. the KEK is read from its file into a KeyRing and wiped at once; the wrapped-key file is loaded into the ring; both key ids must be
//      in it. A wrong KEK, a damaged file or a missing key is a refusal here, before a log is opened;
//   5. both logs must already exist (the sinks would create a missing one, and a mistyped path would then "list" an empty store);
//   6. the audit log is opened in OperatorOpen mode -- THE EXCLUSIVE LOCK, so the trading process must be stopped -- and the control-plane
//      log next. Both are open, and checked, BEFORE begin_operator_session(): the fence is counted from the first serverTime reading
//      after the lock, so a reading taken before the lock would be a fence that protects nothing;
//   7. --list prints the unresolved orders and exits; otherwise the session begins and resolve_order() runs, once.
// Nothing is written to a log before the flow's last step, and the only thing the flow writes there is the one frame.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <hengyuan/control_plane_log_sink.hpp>
#include <hengyuan/durable_audit_export.hpp>
#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/kek_loader.hpp>
#include <hengyuan/key_ring.hpp>
#include <hengyuan/operator_resolution_cli.hpp>
#include <hengyuan/operator_resolution_flow.hpp>
#include <hengyuan/operator_resolution_ports.hpp>
#include <hengyuan/operator_resolution_terminal.hpp>
#include <hengyuan/secure_wipe.hpp>

namespace hy::operator_resolution {

// --- what the tool exits with ------------------------------------------------------------------------------------------------------

enum class ToolExit : int {
    Ok = 0,           // the order was resolved (or --list / --help ran): the frame is durable
    Usage = 2,        // the command line
    KeyMaterial = 3,  // the KEK file, the key file, a key id
    Logs = 4,         // a log is missing, locked, fenced or did not recover cleanly
    Exchange = 5,     // the credentials or the exchange client
    Session = 6,      // begin_operator_session() refused
    Internal = 70,    // an exception nobody expected
};

// resolve_order() stopped with this status: 0 for Resolved, 40 + the status number for every other one. (55 EvidenceNotAbsent, 57
// NotConfirmed, 59 AppendFailed.) Of those only AppendFailed may have left a frame behind.
inline constexpr int kResolveExitBase = 40;

constexpr int exit_code_of(ToolExit e) noexcept { return static_cast<int>(e); }

constexpr int exit_code_of(ResolveStatus s) noexcept {
    return s == ResolveStatus::Resolved ? 0 : kResolveExitBase + static_cast<int>(s);
}

// --- the world the tool reaches that is not a log -------------------------------------------------------------------------------------

// The real one reads the credentials file and builds the TLS client (operator_resolution_real_exchange.hpp); a test hands in a fake.
class ExchangeProvider {
public:
    virtual ~ExchangeProvider() = default;
    // The exchange for `options`, or nullptr after saying why on `err`. Asked in resolve mode only, and before any log is touched. The
    // port stays valid for as long as the provider does.
    virtual ExchangeReadPort* open(const ToolOptions& options, SleepPort& sleeper, ConsolePort& err) = 0;
};

struct ToolEnvironment {
    ConsolePort& out;  // the dialogue: the list, what the flow shows, the phrase to type, the answer
    ConsolePort& err;  // the tool's own diagnostics
    SleepPort& sleeper;
    ExchangeProvider& exchange;
    FenceParams fence{};  // the recommended parameters, which are also the floors: they can be raised, never lowered
    std::chrono::milliseconds confirm_timeout{kDefaultConfirmTimeout};
};

// --- names (for the operator's eyes) -----------------------------------------------------------------------------------------------

constexpr std::string_view recovery_status_name(RecoveryScanStatus s) noexcept {
    switch (s) {
        case RecoveryScanStatus::Clean: return "Clean";
        case RecoveryScanStatus::Recovered: return "Recovered";
        case RecoveryScanStatus::Corrupt: return "Corrupt";
        case RecoveryScanStatus::CapacityExceeded: return "CapacityExceeded";
        case RecoveryScanStatus::IoError: return "IoError";
        case RecoveryScanStatus::ExternalAnchorUnavailable: return "ExternalAnchorUnavailable";
    }
    return "Unknown";
}

constexpr std::string_view session_status_name(SessionStatus s) noexcept {
    switch (s) {
        case SessionStatus::NotStarted: return "NotStarted";
        case SessionStatus::Ready: return "Ready";
        case SessionStatus::BadFenceParams: return "BadFenceParams";
        case SessionStatus::AuditNotUsable: return "AuditNotUsable";
        case SessionStatus::ServerTimeUnavailable: return "ServerTimeUnavailable";
    }
    return "Unknown";
}

constexpr std::string_view resolution_check_name(ResolutionCheck c) noexcept {
    switch (c) {
        case ResolutionCheck::NotChecked: return "NotChecked";
        case ResolutionCheck::Ok: return "Ok";
        case ResolutionCheck::NotInAResolvableState: return "NotInAResolvableState";
        case ResolutionCheck::OrderShowsExistence: return "OrderShowsExistence";
        case ResolutionCheck::FrameNotRejected: return "FrameNotRejected";
        case ResolutionCheck::FrameClaimsExistence: return "FrameClaimsExistence";
        case ResolutionCheck::NotTheSameOrder: return "NotTheSameOrder";
        case ResolutionCheck::BadDetailCode: return "BadDetailCode";
    }
    return "Unknown";
}

constexpr std::string_view key_file_status_text(AuditKeyFileStatus s) noexcept {
    switch (s) {
        case AuditKeyFileStatus::Ok: return "Ok";
        case AuditKeyFileStatus::BadMagic: return "not a HYKEYS01 wrapped-key file";
        case AuditKeyFileStatus::BadLength: return "the file is not the length its own key count says";
        case AuditKeyFileStatus::TooManyKeys: return "more keys than the key ring holds";
        case AuditKeyFileStatus::TagMismatch: return "a key does not unwrap under this KEK (wrong KEK, or a damaged file)";
        case AuditKeyFileStatus::DuplicateKeyId: return "the same key id twice";
    }
    return "unknown";
}

constexpr std::string_view kek_status_text(KekLoadStatus s) noexcept {
    switch (s) {
        case KekLoadStatus::Ok: return "Ok";
        case KekLoadStatus::FileNotFound: return "no such file";
        case KekLoadStatus::PermissionTooWide: return "group or other can read it: make it owner-only (chmod 600)";
        case KekLoadStatus::IsSymlink: return "it is a symbolic link";
        case KekLoadStatus::SizeMismatch: return "it is not exactly 32 bytes";
        case KekLoadStatus::ReadError: return "it could not be read";
        case KekLoadStatus::LockFailed: return "its memory could not be locked";
    }
    return "unknown";
}

namespace detail {

inline constexpr std::uint64_t kMaxKeyFileBytes = 64 * 1024;  // sixteen records are under 2 KiB: anything bigger is not a key file

inline void say(ConsolePort& console, const std::string& text) { console.write_line(text); }

// Do these two paths, which the command line gave as different strings, name one file? (Only if both exist.)
inline bool same_file(const std::string& a, const std::string& b) {
    std::error_code ec;
    if (!std::filesystem::exists(a, ec) || ec) return false;
    if (!std::filesystem::exists(b, ec) || ec) return false;
    const bool same = std::filesystem::equivalent(a, b, ec);
    return same && !ec;
}

inline bool is_existing_file(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

// When the control-plane log first named a symbol, for a refusal message: the time as the frame recorded it, or why there is none.
inline std::string named_since_text(std::int64_t named_since_ms) {
    return named_since_ms != 0 ? std::to_string(named_since_ms) : std::string("an unknown time");
}

// Both logs, as they are on disk, for --list and for resolving.
inline void print_unresolved_orders(const DurableAuditSink& audit, const ControlPlaneLogSink& names, ConsolePort& out) {
    const std::vector<UnresolvedOrderSummary> orders = list_unresolved_orders(audit, names);
    out.write_line("unresolved orders: " + std::to_string(orders.size()));
    for (const UnresolvedOrderSummary& s : orders) {
        std::string line = "[" + std::to_string(s.checkpoint_index + 1) + "] " + s.client_order_id + "  state " + order_state_name(s.state) +
                           "  " + (s.mode == ExecutionMode::Live ? "Live" : "DryRun") + "  ";
        line += s.symbol_status == SymbolNameStatus::Known ? s.symbol : "symbol unknown (id " + std::to_string(s.symbol_id) + ")";
        line += std::string("  ") + (s.side == OrderSide::Buy ? "Buy" : "Sell") + "  quantity " + std::to_string(s.qty_ticks) +
                " ticks  price " + std::to_string(s.price_ticks) + " ticks  send started " +
                (s.send_started_utc_ms != 0 ? std::to_string(s.send_started_utc_ms) : std::string("unknown"));
        if (s.mode != ExecutionMode::Live) {
            line += "  -> cannot be resolved: a dry-run order never reached the exchange";
        } else if (s.order_check != ResolutionCheck::Ok) {
            line += "  -> cannot be resolved: " + std::string(resolution_check_name(s.order_check));
        } else if (s.symbol_status == SymbolNameStatus::NotNamedAtOrderTime) {
            line += "  -> cannot be resolved: the control-plane log does not show that it named this symbol before this order was sent "
                    "(first named at " + named_since_text(s.symbol_named_since_ms) + ")";
        } else if (s.symbol_status != SymbolNameStatus::Known) {
            line += "  -> cannot be resolved: the control-plane log does not name this symbol";
        } else {
            line += "  -> can be resolved";
        }
        out.write_line(line);
    }
    out.write_line("intent-only orders (nothing was ever sent for them; not listed): " + std::to_string(audit.recovered_intent_orphans()));
    out.write_line("orders an operator has resolved before: " + std::to_string(audit.recovered_operator_resolved_total()));
    const RecoveryOverflow& over = audit.recovery_overflow();
    if (over.any()) {
        out.write_line("WARNING: the log holds more than the tool can list: " + std::to_string(over.checkpoints) +
                       " unresolved order(s), " + std::to_string(over.intent_orphans) + " intent-only order(s) and " +
                       std::to_string(over.operator_resolved) + " operator-resolved order(s) beyond the tables. Resolve those by hand.");
    }
}

inline int run_parsed(const ToolOptions& options, ToolEnvironment& env) {
    ConsolePort& out = env.out;
    ConsolePort& err = env.err;
    const bool resolve = options.mode == ToolMode::Resolve;

    // 2. the files that must be different files
    const auto distinct = [&](const std::string& a, const std::string& b, std::string_view what) {
        if (!same_file(a, b)) return true;
        say(err, "error: " + std::string(what) + " name the same file");
        return false;
    };
    if (!distinct(options.audit_log, options.control_log, "--audit-log and --control-log") ||
        !distinct(options.kek_file, options.key_file, "--kek-file and --key-file")) {
        return exit_code_of(ToolExit::Usage);
    }
    if (resolve && (!distinct(options.credentials_file, options.kek_file, "--credentials-file and --kek-file") ||
                    !distinct(options.credentials_file, options.key_file, "--credentials-file and --key-file"))) {
        return exit_code_of(ToolExit::Usage);
    }

    // 3. the exchange, before any log is touched
    ExchangeReadPort* exchange = nullptr;
    if (resolve) {
        exchange = env.exchange.open(options, env.sleeper, err);
        if (exchange == nullptr) return exit_code_of(ToolExit::Exchange);
    }

    // 4. the key material
    KekLoader kek;
    const KekLoadStatus kek_status = kek.load(options.kek_file.c_str());
    if (kek_status != KekLoadStatus::Ok) {
        say(err, "error: --kek-file: " + std::string(kek_status_text(kek_status)));
        return exit_code_of(ToolExit::KeyMaterial);
    }
    auto ring = std::make_unique<KeyRing>(kek.get());
    kek.wipe();  // the ring holds its own locked copy; the loader's is not needed for another moment
    std::vector<std::byte> key_bytes;
    switch (read_file_bounded(options.key_file, key_bytes, kMaxKeyFileBytes)) {
        case ReadFileStatus::Ok: break;
        case ReadFileStatus::NotFound:
            say(err, "error: --key-file: no such file");
            return exit_code_of(ToolExit::KeyMaterial);
        case ReadFileStatus::TooLarge:
            say(err, "error: --key-file: too large to be a wrapped-key file");
            return exit_code_of(ToolExit::KeyMaterial);
        case ReadFileStatus::Error:
            say(err, "error: --key-file: it could not be read");
            return exit_code_of(ToolExit::KeyMaterial);
    }
    const AuditKeyFileStatus key_status = load_audit_key_file(key_bytes, *ring);
    if (key_status != AuditKeyFileStatus::Ok) {
        say(err, "error: --key-file: " + std::string(key_file_status_text(key_status)));
        return exit_code_of(ToolExit::KeyMaterial);
    }
    const auto has_key = [&ring](std::uint32_t key_id) {
        std::array<std::byte, kKeyBlockSize> block{};
        const bool found = ring->active_key(key_id, block);
        secure_wipe(block.data(), block.size());
        return found;
    };
    if (!has_key(options.audit_key_id)) {
        say(err, "error: --key-file does not hold the key --audit-key-id names");
        return exit_code_of(ToolExit::KeyMaterial);
    }
    if (!has_key(options.control_key_id)) {
        say(err, "error: --key-file does not hold the key --control-key-id names");
        return exit_code_of(ToolExit::KeyMaterial);
    }

    // 5. the logs exist
    if (!is_existing_file(options.audit_log)) {
        say(err, "error: --audit-log: there is no log file at that path (a mistyped path would be created empty)");
        return exit_code_of(ToolExit::Logs);
    }
    if (!is_existing_file(options.control_log)) {
        say(err, "error: --control-log: there is no log file at that path (a mistyped path would be created empty)");
        return exit_code_of(ToolExit::Logs);
    }

    // 6. both logs open -- the audit log's OperatorOpen IS the exclusive lock -- and checked, before the session begins
    DurableAuditSink audit(options.audit_log, *ring, options.audit_key_id, kMaxIntentOrphans, AuditSinkOpenMode::OperatorOpen);
    if (!audit.is_open()) {
        say(err, "error: the audit log could not be opened. Is the trading process still running? It holds the log's lock, which this "
                 "tool needs, and must be stopped.");
        return exit_code_of(ToolExit::Logs);
    }
    if (audit.fenced() || (audit.recovery_status() != RecoveryScanStatus::Clean && audit.recovery_status() != RecoveryScanStatus::Recovered)) {
        say(err, "error: the audit log did not recover cleanly (" + std::string(recovery_status_name(audit.recovery_status())) +
                     "). Nothing was written.");
        return exit_code_of(ToolExit::Logs);
    }
    ControlPlaneLogSink names(options.control_log, *ring, options.control_key_id);
    if (!names.is_open()) {
        say(err, "error: the control-plane log could not be opened (is the trading process still running?)");
        return exit_code_of(ToolExit::Logs);
    }
    if (names.fenced() || (names.recovery_status() != RecoveryScanStatus::Clean && names.recovery_status() != RecoveryScanStatus::Recovered)) {
        say(err, "error: the control-plane log did not recover cleanly (" + std::string(recovery_status_name(names.recovery_status())) +
                     "). Nothing was written.");
        return exit_code_of(ToolExit::Logs);
    }

    // 7. the list, or the one order
    if (!resolve) {
        print_unresolved_orders(audit, names, out);
        return exit_code_of(ToolExit::Ok);
    }
    DirectoryEvidenceStore evidence(options.audit_log + std::string(kEvidenceDirectorySuffix));
    const OperatorPorts ports{*exchange, env.sleeper, out, evidence};
    OperatorSession session = begin_operator_session(audit, names, ports, env.fence);
    if (session.status != SessionStatus::Ready) {
        say(err, "error: the session could not start: " + std::string(session_status_name(session.status)) +
                     ". Nothing was written.");
        return exit_code_of(ToolExit::Session);
    }
    OperatorResolutionRequest request;
    request.client_order_id = options.client_order_id;
    request.environment = options.environment;
    request.account_uid = options.account_uid;
    request.operator_label = options.operator_label;
    request.confirm_timeout = env.confirm_timeout;
    const OperatorResolutionOutcome outcome = resolve_order(session, request);
    const int code = exit_code_of(outcome.status);
    out.write_line("result: " + std::string(resolve_status_name(outcome.status)) + " (exit code " + std::to_string(code) + ")");
    if (outcome.status == ResolveStatus::AppendFailed) {
        out.write_line("the frame may or may not have been written: run --list to see whether the order is still listed.");
    }
    if (outcome.status == ResolveStatus::SymbolNameNotKnown && outcome.symbol_status == SymbolNameStatus::NotNamedAtOrderTime) {
        out.write_line("the control-plane log does not show that it named this order's symbol before the order was sent (first named at " +
                       named_since_text(outcome.symbol_named_since_ms) +
                       "): it may not be the log that was in use then. Nothing was written.");
    }
    return code;
}

}  // namespace detail

// The whole tool. `args` are the command line without the program name. Returns the exit code.
inline int run_operator_tool(std::span<const std::string_view> args, ToolEnvironment& env) {
    const ParseResult parsed = parse_tool_args(args);
    if (parsed.status == ParseStatus::Help) {
        for (const std::string_view line : kToolUsage) env.out.write_line(line);
        return exit_code_of(ToolExit::Ok);
    }
    if (parsed.status != ParseStatus::Ok) {
        env.err.write_line("error: " + parse_failure_message(parsed));
        env.err.write_line("run with --help for the arguments");
        return exit_code_of(ToolExit::Usage);
    }
    try {
        return detail::run_parsed(parsed.options, env);
    } catch (...) {
        // The last line of defence (an allocation failure, an exception out of a library). Nothing here knows whether the frame was
        // written, so the message says to look.
        env.err.write_line("error: an unexpected exception stopped the tool. Run --list to see whether the order is still listed.");
        return exit_code_of(ToolExit::Internal);
    }
}

}  // namespace hy::operator_resolution
