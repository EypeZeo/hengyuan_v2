// SPDX-License-Identifier: proprietary
// operator_resolution_cli.hpp -- the command line of the offline operator tool hy_operator_resolve, parsed (D5 slice R3b-2).
//
// Governance: offline tool only. Pure: a list of strings in, ToolOptions (or the reason there are none) out -- no file is opened, no
// environment variable is read, no clock, no console. docs/OPERATOR_RESOLUTION_DESIGN.md 6 is the procedure; this header is only the
// question "what did the operator ask for", answered strictly:
//   * long options only, `--name value`, each at most once. An unknown option, a missing value, a value that looks like an option, a
//     repeated option and every value outside its grammar is a refusal. There is no default for anything that matters: not a path,
//     not a key id, not the environment, not the account uid.
//   * two modes. `--list` shows the unresolved orders of the two logs and needs nothing else (no credentials, no network). Without it
//     the tool resolves ONE order, and then the order, the environment, the account uid, the operator's label and the credentials
//     file are all required. Mixing the two is a refusal, not a guess.
//   * numbers are plain decimal digits (no sign, no space, no leading zero, no overflow): `--uid 0123` is a typo, not a number.
//   * two arguments that name the same file as a string are a refusal (the KEK file in particular must not be the credentials file:
//     kek_loader.hpp). The tool checks the same on the file system before it opens anything.
//
// Paths come from the command line here, which kek_loader.hpp and env_loader.hpp say theirs must not (a compiled-in or
// caller-determined constant). That rule protects a process that runs unattended. This tool is run by an operator, who is the trust
// root of what it reads, and every check those loaders make on the file itself -- owner-only permissions, no symlink, exact size, locked
// memory, secure wipe -- is made on whatever path he gives. (The earlier offline tool, durable_audit_log_export, takes the KEK as
// hex on the command line, where it would show up in the process list and the shell history; a file path does not.)

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <hengyuan/operator_resolution_confirmation.hpp>

namespace hy::operator_resolution {

enum class ToolMode : std::uint8_t {
    None = 0,  // the zero value is not a mode
    List = 1,
    Resolve = 2,
};

// A path argument is at most this long and holds no control character (it is printed back to the operator).
inline constexpr std::size_t kMaxPathChars = 4096;

struct ToolOptions {
    ToolMode mode{ToolMode::None};
    // --- both modes ---
    std::string audit_log;  // --audit-log: DurableAuditSink base path of the stopped trading process
    std::string control_log;  // --control-log: its ControlPlaneLogSink base path
    std::string kek_file;  // --kek-file: the 32-byte KEK (KekLoader)
    std::string key_file;  // --key-file: the HYKEYS01 wrapped keys the logs are signed with (load_audit_key_file)
    std::uint32_t audit_key_id{0};  // --audit-key-id: the key the trading process signs the audit log with
    std::uint32_t control_key_id{0};  // --control-key-id: ... the control-plane log
    // --- resolve mode only ---
    std::string client_order_id;  // --coid
    OperatorEnvironment environment{static_cast<OperatorEnvironment>(255)};  // --env testnet|production
    std::uint64_t account_uid{0};  // --uid: what the operator says the API key's account is (the tool checks it with the exchange)
    std::string operator_label;  // --operator: goes into the frame as "op=<label>"
    std::string credentials_file;  // --credentials-file: the .env file holding the READ-ONLY API key
};

enum class ParseStatus : std::uint8_t {
    NotParsed = 0,  // the zero value is not a parse
    Ok = 1,
    Help = 2,  // --help: not an error, but nothing to run
    UnknownArgument = 3,
    MissingValue = 4,  // the option is the last argument, or the next argument looks like an option
    DuplicateArgument = 5,
    BadValue = 6,
    MissingArgument = 7,
    NotForThisMode = 8,  // an argument of the resolve mode together with --list
    SamePath = 9,  // two arguments name the same file
};

struct ParseResult {
    ParseStatus status{ParseStatus::NotParsed};
    ToolOptions options;  // meaningful only when status == Ok
    std::string argument;  // the option the status is about ("--uid"); empty for Ok and Help
};

// --- numbers and grammars ------------------------------------------------------------------------------------------------------

// Plain decimal digits: non-empty, no sign, no space, no leading zero (except "0" itself), at most the largest uint64.
constexpr std::optional<std::uint64_t> parse_decimal_u64(std::string_view s) noexcept {
    if (s.empty() || s.size() > 20) return std::nullopt;
    if (s.size() > 1 && s.front() == '0') return std::nullopt;
    std::uint64_t value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return std::nullopt;  // value * 10 + digit would wrap
        value = value * 10 + digit;
    }
    return value;
}

constexpr std::optional<std::uint32_t> parse_decimal_u32(std::string_view s) noexcept {
    const std::optional<std::uint64_t> v = parse_decimal_u64(s);
    if (!v || *v > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return static_cast<std::uint32_t>(*v);
}

constexpr bool is_path_argument(std::string_view s) noexcept {
    if (s.empty() || s.size() > kMaxPathChars) return false;
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) return false;
    }
    return true;
}

namespace detail {

// The options in the order the tool reports a missing one. kOptions[i].name is the only place an option's spelling is written;
// the usage text below is checked against this table by test_operator_resolution_cli.cpp.
enum class Opt : std::size_t {
    AuditLog = 0,
    ControlLog,
    KekFile,
    KeyFile,
    AuditKeyId,
    ControlKeyId,
    List,
    Coid,
    Env,
    Uid,
    Operator,
    CredentialsFile,
};

struct OptSpec {
    std::string_view name;
    bool takes_value;
    bool resolve_only;  // required with the resolve mode, refused with --list
};

inline constexpr std::size_t kOptCount = 12;

inline constexpr std::array<OptSpec, kOptCount> kOptions{{
    {"--audit-log", true, false},
    {"--control-log", true, false},
    {"--kek-file", true, false},
    {"--key-file", true, false},
    {"--audit-key-id", true, false},
    {"--control-key-id", true, false},
    {"--list", false, false},
    {"--coid", true, true},
    {"--env", true, true},
    {"--uid", true, true},
    {"--operator", true, true},
    {"--credentials-file", true, true},
}};

static_assert(kOptions[static_cast<std::size_t>(Opt::List)].name == "--list" && !kOptions[static_cast<std::size_t>(Opt::List)].takes_value);
static_assert(kOptions[static_cast<std::size_t>(Opt::CredentialsFile)].name == "--credentials-file");

constexpr std::size_t find_option(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kOptCount; ++i) {
        if (kOptions[i].name == name) return i;
    }
    return kOptCount;
}

constexpr bool looks_like_option(std::string_view s) noexcept { return s.size() >= 2 && s[0] == '-' && s[1] == '-'; }

// Stores `value` in the field `which` of `o`; false if the value is outside that option's grammar.
inline bool assign_value(Opt which, std::string_view value, ToolOptions& o) {
    switch (which) {
        case Opt::AuditLog:
            if (!is_path_argument(value)) return false;
            o.audit_log = std::string(value);
            return true;
        case Opt::ControlLog:
            if (!is_path_argument(value)) return false;
            o.control_log = std::string(value);
            return true;
        case Opt::KekFile:
            if (!is_path_argument(value)) return false;
            o.kek_file = std::string(value);
            return true;
        case Opt::KeyFile:
            if (!is_path_argument(value)) return false;
            o.key_file = std::string(value);
            return true;
        case Opt::AuditKeyId: {
            const std::optional<std::uint32_t> v = parse_decimal_u32(value);
            if (!v) return false;
            o.audit_key_id = *v;
            return true;
        }
        case Opt::ControlKeyId: {
            const std::optional<std::uint32_t> v = parse_decimal_u32(value);
            if (!v) return false;
            o.control_key_id = *v;
            return true;
        }
        case Opt::List: return false;  // takes no value: unreachable through parse_tool_args()
        case Opt::Coid:
            if (!is_exchange_client_order_id(value)) return false;
            o.client_order_id = std::string(value);
            return true;
        case Opt::Env:
            if (value == "testnet") {
                o.environment = OperatorEnvironment::Testnet;
            } else if (value == "production") {
                o.environment = OperatorEnvironment::Production;
            } else {
                return false;
            }
            return true;
        case Opt::Uid: {
            const std::optional<std::uint64_t> v = parse_decimal_u64(value);
            if (!v || *v == 0) return false;  // an account uid is a positive integer
            o.account_uid = *v;
            return true;
        }
        case Opt::Operator:
            if (!is_operator_label(value)) return false;
            o.operator_label = std::string(value);
            return true;
        case Opt::CredentialsFile:
            if (!is_path_argument(value)) return false;
            o.credentials_file = std::string(value);
            return true;
    }
    return false;
}

}  // namespace detail

// `args` are the command line without the program name. The first problem found wins; Ok means every rule above holds.
inline ParseResult parse_tool_args(std::span<const std::string_view> args) {
    ParseResult r;
    std::array<bool, detail::kOptCount> seen{};
    const auto fail = [&r](ParseStatus status, std::string_view argument) {
        r.status = status;
        r.options = ToolOptions{};
        r.argument = std::string(argument);
        return r;
    };

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view name = args[i];
        if (name == "--help" || name == "-h" || name == "--usage") {
            ParseResult help;
            help.status = ParseStatus::Help;
            return help;
        }
        const std::size_t index = detail::find_option(name);
        if (index == detail::kOptCount) return fail(ParseStatus::UnknownArgument, name);
        if (seen[index]) return fail(ParseStatus::DuplicateArgument, name);
        seen[index] = true;
        if (!detail::kOptions[index].takes_value) continue;  // --list
        if (i + 1 >= args.size() || detail::looks_like_option(args[i + 1])) return fail(ParseStatus::MissingValue, name);
        const std::string_view value = args[++i];
        if (!detail::assign_value(static_cast<detail::Opt>(index), value, r.options)) return fail(ParseStatus::BadValue, name);
    }

    const bool list = seen[static_cast<std::size_t>(detail::Opt::List)];
    for (std::size_t i = 0; i < detail::kOptCount; ++i) {
        const detail::OptSpec& spec = detail::kOptions[i];
        if (list && spec.resolve_only && seen[i]) return fail(ParseStatus::NotForThisMode, spec.name);
    }
    for (std::size_t i = 0; i < detail::kOptCount; ++i) {
        const detail::OptSpec& spec = detail::kOptions[i];
        if (i == static_cast<std::size_t>(detail::Opt::List)) continue;
        const bool required = !list || !spec.resolve_only;
        if (required && !seen[i]) return fail(ParseStatus::MissingArgument, spec.name);
    }
    r.options.mode = list ? ToolMode::List : ToolMode::Resolve;

    const ToolOptions& o = r.options;
    if (o.audit_log == o.control_log) return fail(ParseStatus::SamePath, "--control-log");
    if (o.kek_file == o.key_file) return fail(ParseStatus::SamePath, "--key-file");
    if (!list) {
        if (o.credentials_file == o.kek_file) return fail(ParseStatus::SamePath, "--credentials-file");
        if (o.credentials_file == o.key_file) return fail(ParseStatus::SamePath, "--credentials-file");
    }
    r.status = ParseStatus::Ok;
    return r;
}

// One line saying what is wrong with the command line. Never contains a value the operator gave, only the option it was given to
// (an unknown option is shown, cut at 64 characters: it is the thing that is wrong).
inline std::string parse_failure_message(const ParseResult& r) {
    std::string option = r.argument.size() > 64 ? r.argument.substr(0, 64) + "..." : r.argument;
    switch (r.status) {
        case ParseStatus::UnknownArgument: return "unknown argument " + option;
        case ParseStatus::MissingValue: return option + " needs a value";
        case ParseStatus::DuplicateArgument: return option + " is given more than once";
        case ParseStatus::BadValue: return "the value of " + option + " is not acceptable";
        case ParseStatus::MissingArgument: return option + " is required";
        case ParseStatus::NotForThisMode: return option + " belongs to resolving one order and cannot be combined with --list";
        case ParseStatus::SamePath: return option + " names the same file as another argument";
        case ParseStatus::NotParsed:
        case ParseStatus::Ok:
        case ParseStatus::Help: break;
    }
    return "the command line was not understood";
}

// The help text, one line per element (the console writes lines).
inline constexpr std::string_view kToolUsage[] = {
    "hy_operator_resolve -- close an order the log cannot close, after evidence that the exchange has no such order.",
    "",
    "Stop the trading process first: this tool takes the exclusive locks of both logs, and nothing else may send orders with",
    "the same API credentials while it runs. It waits out a fence of 4.5 minutes of EXCHANGE time before it reads the evidence.",
    "",
    "  hy_operator_resolve --list    <log and key arguments>",
    "      shows the unresolved orders of the logs. Needs no credentials and sends nothing.",
    "  hy_operator_resolve --coid <id> --env testnet|production --uid <n> --operator <label> --credentials-file <path>",
    "                      <log and key arguments>",
    "      resolves ONE order (run it again for the next one).",
    "",
    "log and key arguments (all required, in both modes):",
    "  --audit-log <base path>      the DurableAuditSink log of the stopped process, without suffix (e.g. state/audit.log)",
    "  --control-log <base path>    its ControlPlaneLogSink log",
    "  --kek-file <path>            the 32-byte KEK file (owner-only permissions), not the credentials file",
    "  --key-file <path>            the HYKEYS01 wrapped-key file the logs were signed with, wrapped under that KEK",
    "  --audit-key-id <n>           the key id the trading process signs the audit log with",
    "  --control-key-id <n>         the key id it signs the control-plane log with",
    "  (A log opens only under the key id it was written with: a wrong id is a refusal, never a write.)",
    "",
    "resolve arguments:",
    "  --coid <id>                  the client order id, as --list shows it",
    "  --env testnet|production     the exchange environment the order was sent to (you state it; the log cannot confirm it)",
    "  --uid <n>                    the uid of the exchange account the order was sent from; the tool asks the exchange which",
    "                               account the API key belongs to and refuses unless it is this one",
    "  --operator <label>           your tag in the log (1 to 24 of letters, digits . _ -)",
    "  --credentials-file <path>    a .env file with the tool's own READ-ONLY API key and nothing else:",
    "                               HENGYUAN_OPERATOR_TESTNET_API_KEY / HENGYUAN_OPERATOR_TESTNET_API_SECRET, or",
    "                               HENGYUAN_OPERATOR_LIVE_API_KEY / HENGYUAN_OPERATOR_LIVE_API_SECRET. The trading key is refused.",
    "",
    "Evidence files go to <audit log>.operator-evidence/ and are never overwritten.",
    "Exit codes: 0 the order was resolved and the frame is durable. 2 the command line. 3 the KEK or key file. 4 a log. 5 the",
    "credentials or the exchange client. 6 the session could not start. 70 an unexpected error. 40 + n: the procedure stopped with",
    "status n (55 evidence does not show absence, 57 not confirmed, 59 the frame could not be appended). Every code but 0 means no",
    "frame was written, except possibly 59 and 70: run --list to see whether the order is still listed.",
};

}  // namespace hy::operator_resolution
