// SPDX-License-Identifier: proprietary
// test_operator_resolution_cli.cpp -- D5 slice R3b-2: the command line of the offline operator tool (operator_resolution_cli.hpp) and its real
// console and evidence directory (operator_resolution_terminal.hpp).
//
// Two layers:
//   1. the argument grammar: every option, what makes each value acceptable, what is required together, what is refused together, and the
//      usage text agreeing with the option table. The parser opens no file, so these tests touch none.
//   2. the real ports the flow talks to the operator and the disk through: the console writes printable ASCII only and reads a line through
//      whatever byte source it is given (the native stdin adapter in production); the evidence directory creates its file exclusively, never
//      touches an existing one, and leaves nothing behind when it refuses.

#include <gtest/gtest.h>
#include <hengyuan/operator_resolution_cli.hpp>
#include <hengyuan/operator_resolution_terminal.hpp>

#include "test_helpers/temp_directory.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace hy;
using namespace hy::operator_resolution;

namespace fs = std::filesystem;
namespace cli_detail = hy::operator_resolution::detail;

namespace {

using Args = std::vector<std::string_view>;

Args list_args() {
    return {"--audit-log",    "state/audit.log", "--control-log",    "state/cp.log", "--kek-file", "keys/kek.bin",
            "--key-file",     "keys/keys.bin",   "--audit-key-id",   "1",            "--control-key-id", "2",
            "--list"};
}

Args resolve_args() {
    Args v = list_args();
    v.pop_back();  // --list
    for (const std::string_view s : {"--coid", "HY-1790000000000-7-0A1B", "--env", "testnet", "--uid", "123456789", "--operator", "alice",
                                     "--credentials-file", "keys/operator.env"}) {
        v.push_back(s);
    }
    return v;
}

ParseResult parse(const Args& v) { return parse_tool_args(std::span<const std::string_view>(v)); }

// `v` without the option `name` and its value.
Args without(const Args& v, std::string_view name) {
    Args out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == name) {
            const std::size_t at = cli_detail::find_option(name);
            if (at != cli_detail::kOptCount && cli_detail::kOptions[at].takes_value) ++i;
            continue;
        }
        out.push_back(v[i]);
    }
    return out;
}

// `v` with the value of option `name` replaced.
Args with_value(Args v, std::string_view name, std::string_view value) {
    for (std::size_t i = 0; i + 1 < v.size(); ++i) {
        if (v[i] == name) {
            v[i + 1] = value;
            return v;
        }
    }
    ADD_FAILURE() << name << " is not in the arguments";
    return v;
}

using TempDir = hy::test_helpers::TempDirectory;

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> directory_listing(const fs::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace

// ============================================================ 1. the grammar ========================================================================

static_assert(parse_decimal_u64("0") == 0u);
static_assert(parse_decimal_u64("18446744073709551615") == 18446744073709551615ull);
static_assert(!parse_decimal_u64("18446744073709551616"));
static_assert(!parse_decimal_u64("00"));
static_assert(!parse_decimal_u64(""));

TEST(ToolArgs, ADecimalNumberIsDigitsAndNothingElse) {
    EXPECT_EQ(parse_decimal_u64("7"), 7u);
    EXPECT_EQ(parse_decimal_u64("10"), 10u);
    EXPECT_EQ(parse_decimal_u64("4294967295"), 4294967295u);
    EXPECT_EQ(parse_decimal_u64("9223372036854775808"), 9223372036854775808ull);
    for (const std::string_view bad : {"", " ", "-1", "+1", "1 ", " 1", "1.0", "1e3", "0x10", "01", "007", "abc", "1a", "१२", "18446744073709551616",
                                       "99999999999999999999", "184467440737095516150"}) {
        EXPECT_FALSE(parse_decimal_u64(bad).has_value()) << bad;
    }
    // the neighbours of the overflow edge: the largest value whose last digit still fits, and one more
    EXPECT_EQ(parse_decimal_u64("18446744073709551614"), 18446744073709551614ull);
    EXPECT_FALSE(parse_decimal_u64("18446744073709551620").has_value());
    EXPECT_EQ(parse_decimal_u32("4294967295"), 4294967295u);
    EXPECT_FALSE(parse_decimal_u32("4294967296").has_value());
    EXPECT_EQ(parse_decimal_u32("0"), 0u);
}

TEST(ToolArgs, AListCommandLineNeedsTheLogAndKeyArgumentsAndNothingElse) {
    const ParseResult r = parse(list_args());
    ASSERT_EQ(r.status, ParseStatus::Ok) << parse_failure_message(r);
    EXPECT_EQ(r.options.mode, ToolMode::List);
    EXPECT_EQ(r.options.audit_log, "state/audit.log");
    EXPECT_EQ(r.options.control_log, "state/cp.log");
    EXPECT_EQ(r.options.kek_file, "keys/kek.bin");
    EXPECT_EQ(r.options.key_file, "keys/keys.bin");
    EXPECT_EQ(r.options.audit_key_id, 1u);
    EXPECT_EQ(r.options.control_key_id, 2u);
    EXPECT_TRUE(r.argument.empty());
    // the order of the arguments does not matter: the same pairs, the other way round
    const Args base = list_args();
    Args reordered;
    for (std::size_t i = 0; i + 1 < base.size(); i += 2) reordered.insert(reordered.begin(), base.begin() + static_cast<std::ptrdiff_t>(i), base.begin() + static_cast<std::ptrdiff_t>(i + 2));
    reordered.insert(reordered.begin(), "--list");
    const ParseResult again = parse(reordered);
    ASSERT_EQ(again.status, ParseStatus::Ok) << parse_failure_message(again);
    EXPECT_EQ(again.options.audit_log, "state/audit.log");
    EXPECT_EQ(again.options.control_key_id, 2u);
}

TEST(ToolArgs, AResolveCommandLineCarriesEveryFieldOfTheRequest) {
    const ParseResult r = parse(resolve_args());
    ASSERT_EQ(r.status, ParseStatus::Ok) << parse_failure_message(r);
    EXPECT_EQ(r.options.mode, ToolMode::Resolve);
    EXPECT_EQ(r.options.client_order_id, "HY-1790000000000-7-0A1B");
    EXPECT_EQ(r.options.environment, OperatorEnvironment::Testnet);
    EXPECT_EQ(r.options.account_uid, 123456789u);
    EXPECT_EQ(r.options.operator_label, "alice");
    EXPECT_EQ(r.options.credentials_file, "keys/operator.env");
    EXPECT_EQ(parse(with_value(resolve_args(), "--env", "production")).options.environment, OperatorEnvironment::Production);
}

TEST(ToolArgs, EveryRequiredArgumentIsRequiredInBothModesItBelongsTo) {
    for (const cli_detail::OptSpec& spec : cli_detail::kOptions) {
        if (spec.name == "--list") continue;
        const ParseResult r = parse(without(resolve_args(), spec.name));
        EXPECT_EQ(r.status, ParseStatus::MissingArgument) << spec.name;
        EXPECT_EQ(r.argument, spec.name);
        if (!spec.resolve_only) {
            const ParseResult l = parse(without(list_args(), spec.name));
            EXPECT_EQ(l.status, ParseStatus::MissingArgument) << spec.name;
            EXPECT_EQ(l.argument, spec.name);
        }
    }
    // no arguments at all: the first option of the table is the first one missing
    const ParseResult none = parse(Args{});
    EXPECT_EQ(none.status, ParseStatus::MissingArgument);
    EXPECT_EQ(none.argument, "--audit-log");
    // dropping --list from a list command line makes it a resolve command line, which is then missing the order
    const ParseResult no_list = parse(without(list_args(), "--list"));
    EXPECT_EQ(no_list.status, ParseStatus::MissingArgument);
    EXPECT_EQ(no_list.argument, "--coid");
}

TEST(ToolArgs, ListAndTheArgumentsOfResolvingOneOrderAreNeverMixed) {
    for (const cli_detail::OptSpec& spec : cli_detail::kOptions) {
        if (!spec.resolve_only) continue;
        Args v = list_args();
        v.push_back(spec.name);
        v.push_back("x");  // (an acceptable value is not needed: the mix is refused after the grammar of each value is checked)
        const ParseResult r = parse(v);
        EXPECT_NE(r.status, ParseStatus::Ok) << spec.name;
    }
    Args v = list_args();
    for (const std::string_view s : {"--coid", "HY-1-2-AB"}) v.push_back(s);
    const ParseResult r = parse(v);
    EXPECT_EQ(r.status, ParseStatus::NotForThisMode);
    EXPECT_EQ(r.argument, "--coid");
    EXPECT_NE(parse_failure_message(r).find("--list"), std::string::npos);
    // every resolve-only option with an acceptable value is refused as NotForThisMode, not accepted and not ignored
    const std::vector<std::pair<std::string_view, std::string_view>> acceptable = {
        {"--coid", "HY-1-2-AB"}, {"--env", "testnet"}, {"--uid", "5"}, {"--operator", "bob"}, {"--credentials-file", "x.env"}};
    for (const auto& [name, value] : acceptable) {
        Args w = list_args();
        w.push_back(name);
        w.push_back(value);
        const ParseResult m = parse(w);
        EXPECT_EQ(m.status, ParseStatus::NotForThisMode) << name;
        EXPECT_EQ(m.argument, name);
    }
}

TEST(ToolArgs, AnUnknownRepeatedOrValuelessOptionIsRefused) {
    {
        Args v = resolve_args();
        v.push_back("--bogus");
        const ParseResult r = parse(v);
        EXPECT_EQ(r.status, ParseStatus::UnknownArgument);
        EXPECT_EQ(r.argument, "--bogus");
        EXPECT_EQ(parse_failure_message(r), "unknown argument --bogus");
    }
    for (const std::string_view stray : {"positional", "-x", "--audit-log=state/a.log", "-audit-log", "", "--audit-l", "--audit-logs", "--LIST", "--"}) {
        Args v = resolve_args();
        v.push_back(stray);
        EXPECT_EQ(parse(v).status, ParseStatus::UnknownArgument) << stray;
    }
    for (const cli_detail::OptSpec& spec : cli_detail::kOptions) {
        Args v = spec.name == "--list" ? list_args() : resolve_args();
        const std::size_t at = static_cast<std::size_t>(std::find(v.begin(), v.end(), spec.name) - v.begin());
        ASSERT_LT(at, v.size()) << spec.name;
        const std::size_t width = spec.takes_value ? 2 : 1;
        const Args again(v.begin() + static_cast<std::ptrdiff_t>(at), v.begin() + static_cast<std::ptrdiff_t>(at + width));
        v.insert(v.end(), again.begin(), again.end());
        const ParseResult r = parse(v);
        EXPECT_EQ(r.status, ParseStatus::DuplicateArgument) << spec.name;
        EXPECT_EQ(r.argument, spec.name);
    }
    // a refusal hands back nothing it had collected on the way (a caller that forgot to look at the status gets an empty command line, not half of one)
    for (const Args& bad : {Args{"--audit-log", "state/a.log", "--bogus"}, Args{"--audit-log", "state/a.log", "--audit-log", "x"},
                            Args{"--audit-log", "state/a.log", "--uid", "0"}, Args{"--audit-log", "state/a.log", "--kek-file"}}) {
        const ParseResult r = parse(bad);
        EXPECT_NE(r.status, ParseStatus::Ok);
        EXPECT_EQ(r.options.mode, ToolMode::None);
        EXPECT_TRUE(r.options.audit_log.empty());
        EXPECT_TRUE(r.options.kek_file.empty());
        EXPECT_EQ(r.options.account_uid, 0u);
    }
    // an option that is the last argument has no value; a value that looks like an option is not one
    {
        Args v = resolve_args();
        v.pop_back();  // the value of --credentials-file
        const ParseResult last = parse(v);
        EXPECT_EQ(last.status, ParseStatus::MissingValue);
        EXPECT_EQ(last.argument, "--credentials-file");
        const ParseResult r = parse(Args{"--audit-log"});
        EXPECT_EQ(r.status, ParseStatus::MissingValue);
        EXPECT_EQ(r.argument, "--audit-log");
        const ParseResult looks = parse(Args{"--audit-log", "--control-log", "x"});
        EXPECT_EQ(looks.status, ParseStatus::MissingValue);
        EXPECT_EQ(looks.argument, "--audit-log");
        EXPECT_EQ(parse(with_value(resolve_args(), "--operator", "--env")).status, ParseStatus::MissingValue);
        EXPECT_EQ(parse(with_value(resolve_args(), "--operator", "-")).status, ParseStatus::Ok) << "a single dash is a value (and a fine label)";
    }
}

TEST(ToolArgs, TheAccountUidAndTheKeyIdsAreCheckedAsNumbers) {
    for (const std::string_view bad : {"0", "-1", "+5", "5 ", "0123", "abc", "18446744073709551616", "1.5", ""}) {
        const ParseResult r = parse(with_value(resolve_args(), "--uid", bad));
        EXPECT_EQ(r.status, ParseStatus::BadValue) << bad;
        EXPECT_EQ(r.argument, "--uid");
    }
    EXPECT_EQ(parse(with_value(resolve_args(), "--uid", "18446744073709551615")).options.account_uid, 18446744073709551615ull);
    EXPECT_EQ(parse(with_value(resolve_args(), "--uid", "1")).options.account_uid, 1u);
    for (const char* option : {"--audit-key-id", "--control-key-id"}) {
        for (const std::string_view bad : {"-1", "4294967296", "1x", "01", " 1", ""}) {
            const ParseResult r = parse(with_value(list_args(), option, bad));
            EXPECT_EQ(r.status, ParseStatus::BadValue) << option << " " << bad;
            EXPECT_EQ(r.argument, option);
        }
        EXPECT_EQ(parse(with_value(list_args(), option, "4294967295")).status, ParseStatus::Ok);
        EXPECT_EQ(parse(with_value(list_args(), option, "0")).status, ParseStatus::Ok) << "the log, not the grammar, decides which ids exist";
    }
    // the two ids are read into their own fields
    const ParseResult r = parse(with_value(with_value(list_args(), "--audit-key-id", "11"), "--control-key-id", "22"));
    EXPECT_EQ(r.options.audit_key_id, 11u);
    EXPECT_EQ(r.options.control_key_id, 22u);
}

TEST(ToolArgs, TheEnvironmentIsExactlyTestnetOrProduction) {
    for (const std::string_view bad : {"TESTNET", "Testnet", "PRODUCTION", "prod", "live", "mainnet", "testnet ", " testnet", ""}) {
        EXPECT_EQ(parse(with_value(resolve_args(), "--env", bad)).status, ParseStatus::BadValue) << bad;
    }
    EXPECT_EQ(parse(with_value(resolve_args(), "--env", "testnet")).options.environment, OperatorEnvironment::Testnet);
    EXPECT_EQ(parse(with_value(resolve_args(), "--env", "production")).options.environment, OperatorEnvironment::Production);
}

TEST(ToolArgs, TheOrderIdAndTheOperatorLabelFollowTheirGrammars) {
    const std::string longest(kClientOrderIdLen, 'A');
    const std::string too_long(kClientOrderIdLen + 1, 'A');
    EXPECT_EQ(parse(with_value(resolve_args(), "--coid", longest)).status, ParseStatus::Ok);
    for (const std::string_view bad : {"", "HY 1", "HY-1\n", "HY-1;rm", "HY-1,2", "HY-\x01", "ID\xc3\xa9"}) {
        EXPECT_EQ(parse(with_value(resolve_args(), "--coid", bad)).status, ParseStatus::BadValue) << bad;
    }
    EXPECT_EQ(parse(with_value(resolve_args(), "--coid", too_long)).status, ParseStatus::BadValue);
    for (const std::string_view ok : {"a", "HY-1790000000000-7-0A1B", "a.b_c-d:e/f"}) {
        EXPECT_EQ(parse(with_value(resolve_args(), "--coid", ok)).status, ParseStatus::Ok) << ok;
    }
    const std::string longest_label(kOperatorLabelMaxChars, 'x');
    const std::string too_long_label(kOperatorLabelMaxChars + 1, 'x');
    EXPECT_EQ(parse(with_value(resolve_args(), "--operator", longest_label)).status, ParseStatus::Ok);
    EXPECT_EQ(parse(with_value(resolve_args(), "--operator", too_long_label)).status, ParseStatus::BadValue);
    for (const std::string_view bad : {"", "a b", "a;b", "a=b", "a/b", "a:b", "al\xc3\xaf" "ce"}) {
        EXPECT_EQ(parse(with_value(resolve_args(), "--operator", bad)).status, ParseStatus::BadValue) << bad;
    }
    for (const std::string_view ok : {"alice", "a.b_c-d", "OPS-1"}) {
        EXPECT_EQ(parse(with_value(resolve_args(), "--operator", ok)).status, ParseStatus::Ok) << ok;
    }
}

TEST(ToolArgs, APathIsNonEmptyBoundedAndFreeOfControlCharacters) {
    const std::string longest(kMaxPathChars, 'p');
    const std::string too_long(kMaxPathChars + 1, 'p');
    for (const char* option : {"--audit-log", "--control-log", "--kek-file", "--key-file"}) {
        EXPECT_EQ(parse(with_value(list_args(), option, longest)).status, ParseStatus::Ok) << option;
        EXPECT_EQ(parse(with_value(list_args(), option, too_long)).status, ParseStatus::BadValue) << option;
        EXPECT_EQ(parse(with_value(list_args(), option, "")).status, ParseStatus::BadValue) << option;
        EXPECT_EQ(parse(with_value(list_args(), option, "a\nb")).status, ParseStatus::BadValue) << option;
        EXPECT_EQ(parse(with_value(list_args(), option, "a\x1b[2Jb")).status, ParseStatus::BadValue) << option;
        EXPECT_EQ(parse(with_value(list_args(), option, "a\x7f")).status, ParseStatus::BadValue) << option;
        EXPECT_EQ(parse(with_value(list_args(), option, "a\x1f")).status, ParseStatus::BadValue) << option << ": the last control character";
        EXPECT_EQ(parse(with_value(list_args(), option, " ~ ")).status, ParseStatus::Ok) << option << ": the first and the last printable ones";
        EXPECT_EQ(parse(with_value(list_args(), option, "C:\\state dir\\audit log")).status, ParseStatus::Ok) << option << ": spaces are fine";
    }
    EXPECT_EQ(parse(with_value(resolve_args(), "--credentials-file", too_long)).status, ParseStatus::BadValue);
    EXPECT_EQ(parse(with_value(resolve_args(), "--credentials-file", "a\tb")).status, ParseStatus::BadValue);
}

TEST(ToolArgs, TwoArgumentsThatNameTheSameFileAreRefused) {
    {
        const ParseResult r = parse(with_value(list_args(), "--control-log", "state/audit.log"));
        EXPECT_EQ(r.status, ParseStatus::SamePath);
        EXPECT_EQ(r.argument, "--control-log");
    }
    {
        const ParseResult r = parse(with_value(list_args(), "--key-file", "keys/kek.bin"));
        EXPECT_EQ(r.status, ParseStatus::SamePath);
        EXPECT_EQ(r.argument, "--key-file");
    }
    {
        const ParseResult r = parse(with_value(resolve_args(), "--credentials-file", "keys/kek.bin"));
        EXPECT_EQ(r.status, ParseStatus::SamePath) << "the KEK file must not be the credentials file";
        EXPECT_EQ(r.argument, "--credentials-file");
    }
    {
        const ParseResult r = parse(with_value(resolve_args(), "--credentials-file", "keys/keys.bin"));
        EXPECT_EQ(r.status, ParseStatus::SamePath);
        EXPECT_EQ(r.argument, "--credentials-file");
    }
    // a different spelling of the same file is not caught here (the parser opens nothing): the tool checks it on disk
    EXPECT_EQ(parse(with_value(list_args(), "--control-log", "./state/audit.log")).status, ParseStatus::Ok);
}

TEST(ToolArgs, HelpIsNotAnErrorAndTheFirstProblemWins) {
    for (const std::string_view h : {"--help", "-h", "--usage"}) {
        EXPECT_EQ(parse(Args{h}).status, ParseStatus::Help) << h;
        Args v = resolve_args();
        v.push_back(h);
        EXPECT_EQ(parse(v).status, ParseStatus::Help) << h << " after a complete command line";
        EXPECT_EQ(parse(Args{h, "--bogus"}).status, ParseStatus::Help) << h << " before a bad argument";
        EXPECT_EQ(parse(Args{"--bogus", h}).status, ParseStatus::UnknownArgument) << "a bad argument before " << h;
    }
    // the first problem found is the one reported
    EXPECT_EQ(parse(Args{"--uid", "x", "--bogus"}).status, ParseStatus::BadValue);
    EXPECT_EQ(parse(Args{"--bogus", "--uid", "x"}).status, ParseStatus::UnknownArgument);
}

TEST(ToolArgs, AFailureMessageNamesTheOptionAndNeverTheValue) {
    ParseResult bad = parse(with_value(resolve_args(), "--kek-file", "a\nSECRETVALUE"));
    ASSERT_EQ(bad.status, ParseStatus::BadValue);
    const std::string message = parse_failure_message(bad);
    EXPECT_NE(message.find("--kek-file"), std::string::npos);
    EXPECT_EQ(message.find("SECRETVALUE"), std::string::npos);

    // an unknown option IS the thing that is wrong, so it is shown -- cut, so a megabyte of it is not echoed
    const std::string huge = "--" + std::string(5000, 'z');
    Args v = resolve_args();
    v.push_back(huge);
    const ParseResult unknown = parse(v);
    ASSERT_EQ(unknown.status, ParseStatus::UnknownArgument);
    EXPECT_LT(parse_failure_message(unknown).size(), 100u);

    // the exact words, one per status
    const std::vector<std::pair<ParseStatus, std::string>> exact = {
        {ParseStatus::UnknownArgument, "unknown argument --uid"},
        {ParseStatus::MissingValue, "--uid needs a value"},
        {ParseStatus::DuplicateArgument, "--uid is given more than once"},
        {ParseStatus::BadValue, "the value of --uid is not acceptable"},
        {ParseStatus::MissingArgument, "--uid is required"},
        {ParseStatus::NotForThisMode, "--uid belongs to resolving one order and cannot be combined with --list"},
        {ParseStatus::SamePath, "--uid names the same file as another argument"},
    };
    for (const auto& [status, words] : exact) {
        ParseResult r;
        r.status = status;
        r.argument = "--uid";
        EXPECT_EQ(parse_failure_message(r), words) << static_cast<int>(status);
    }

    ParseResult ok = parse(resolve_args());
    EXPECT_EQ(parse_failure_message(ok), "the command line was not understood") << "a message for a result that is not a failure says nothing specific";
    for (const ParseStatus s : {ParseStatus::MissingValue, ParseStatus::DuplicateArgument, ParseStatus::BadValue, ParseStatus::MissingArgument,
                                ParseStatus::NotForThisMode, ParseStatus::SamePath, ParseStatus::UnknownArgument}) {
        ParseResult r;
        r.status = s;
        r.argument = "--uid";
        EXPECT_NE(parse_failure_message(r).find("--uid"), std::string::npos) << static_cast<int>(s);
    }
}

TEST(ToolArgs, TheUsageTextNamesEveryOptionAndTheExitCodesThatMatter) {
    std::string usage;
    for (const std::string_view line : kToolUsage) {
        usage += line;
        usage += '\n';
    }
    for (const cli_detail::OptSpec& spec : cli_detail::kOptions) EXPECT_NE(usage.find(spec.name), std::string::npos) << spec.name << " is not in the usage text";
    // the credentials the tool reads, by name, and the fact that the trading key is refused
    for (const char* name : {"HENGYUAN_OPERATOR_TESTNET_API_KEY", "HENGYUAN_OPERATOR_TESTNET_API_SECRET", "HENGYUAN_OPERATOR_LIVE_API_KEY",
                             "HENGYUAN_OPERATOR_LIVE_API_SECRET"}) {
        EXPECT_NE(usage.find(name), std::string::npos) << name;
    }
    EXPECT_NE(usage.find("trading key is refused"), std::string::npos);
    // the exit codes the operator has to know: the one that may have left a frame behind, and the unexpected one
    EXPECT_NE(usage.find("59 the frame could not be appended"), std::string::npos);
    EXPECT_NE(usage.find("70 an unexpected error"), std::string::npos);
    EXPECT_NE(usage.find(".operator-evidence"), std::string::npos);
    // every line is printable ASCII (the console would replace anything else) and short enough for a terminal
    for (const std::string_view line : kToolUsage) {
        EXPECT_LE(line.size(), 140u) << line;
        for (const char c : line) EXPECT_TRUE(StdConsole::is_printable_ascii(c)) << line;
    }
}

// ============================================================ 2. the console ========================================================================

namespace {

struct ScriptedBytes {
    enum class End : std::uint8_t { EndOfInput, Timeout, Error };
    std::string data;
    std::size_t next{0};
    End end{End::EndOfInput};
    int calls{0};
};

OperatorByteResult scripted_byte(void* user, std::chrono::milliseconds) noexcept {
    auto* s = static_cast<ScriptedBytes*>(user);
    ++s->calls;
    if (s->next < s->data.size()) return {OperatorByteStatus::Byte, s->data[s->next++]};
    switch (s->end) {
        case ScriptedBytes::End::EndOfInput: return {OperatorByteStatus::EndOfInput};
        case ScriptedBytes::End::Timeout: return {OperatorByteStatus::Timeout};
        case ScriptedBytes::End::Error: return {OperatorByteStatus::Error};
    }
    return {OperatorByteStatus::Error};
}

}  // namespace

TEST(StdConsole, WritesOnePrintableAsciiLinePerCallAndReplacesEverythingElse) {
    TempDir dir;
    const std::string path = dir.file("console.txt");
    std::FILE* f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    {
        StdConsole console(f);
        console.write_line("plain line");
        console.write_line(std::string("escape \x1b[31mred"));  // an escape sequence must not reach a terminal
        console.write_line("tab\there");
        console.write_line("caf\xc3\xa9");
        console.write_line("");
        console.write_line(std::string("nul\0inside", 10));
        console.write_line("delete\x7f");
        console.write_line("unit\x1f" "separator");  // the last control character
        console.write_line("\x1f");
        console.write_line("two\nlines");  // a newline inside a line would let the flow's text forge a second line
        console.write_line("carriage\rreturn");
        console.write_line(" ~ edges ");
    }
    std::fclose(f);
    EXPECT_EQ(read_file(path),
              "plain line\n"
              "escape ?[31mred\n"
              "tab?here\n"
              "caf??\n"
              "\n"
              "nul?inside\n"
              "delete?\n"
              "unit?separator\n"
              "?\n"
              "two?lines\n"
              "carriage?return\n"
              " ~ edges \n");
}

TEST(StdConsole, ALineIsFlushedWhenItIsWritten) {
    TempDir dir;
    const std::string path = dir.file("flushed.txt");
    std::FILE* f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    StdConsole console(f);
    console.write_line("the phrase to type");
    // the stream is still open and nothing else was done to it: a reader of the file already sees the line (an operator waiting at a
    // terminal sees it before he is asked to type)
    EXPECT_EQ(read_file(path), "the phrase to type\n");
    std::fclose(f);
}

TEST(StdConsole, AConsoleWithoutAStreamWritesNothingAndDoesNotCrash) {
    StdConsole console(nullptr);
    console.write_line("nobody reads this");
    ScriptedBytes bytes;
    bytes.data = "x\n";
    StdConsole reading(nullptr, &scripted_byte, &bytes);
    EXPECT_EQ(reading.read_line(std::chrono::seconds{1}).status, OperatorInputStatus::Line) << "reading does not need the output stream";
}

TEST(StdConsole, ReadsALineThroughTheByteSourceItWasGiven) {
    ScriptedBytes bytes;
    bytes.data = "ABSENT HY-1 BTCUSDT TESTNET 5 0a1b2c3d\nnext line\n";
    StdConsole console(nullptr, &scripted_byte, &bytes);
    OperatorLineResult first = console.read_line(std::chrono::seconds{30});
    ASSERT_EQ(first.status, OperatorInputStatus::Line);
    EXPECT_EQ(first.line(), "ABSENT HY-1 BTCUSDT TESTNET 5 0a1b2c3d");
    // each call is a new read attempt (one OperatorLineReader is one attempt): the next line is read by the next call
    OperatorLineResult second = console.read_line(std::chrono::seconds{30});
    ASSERT_EQ(second.status, OperatorInputStatus::Line);
    EXPECT_EQ(second.line(), "next line");
    EXPECT_EQ(console.read_line(std::chrono::seconds{30}).status, OperatorInputStatus::EndOfInput);
}

TEST(StdConsole, ACarriageReturnEndsTheLineAndEveryOtherWayOfNotGettingOneIsReported) {
    {
        ScriptedBytes bytes;
        bytes.data = "abc\r\n";
        StdConsole console(nullptr, &scripted_byte, &bytes);
        const OperatorLineResult r = console.read_line(std::chrono::seconds{1});
        EXPECT_EQ(r.status, OperatorInputStatus::Line);
        EXPECT_EQ(r.line(), "abc");
    }
    {
        ScriptedBytes bytes;
        bytes.data = "no end of line";
        bytes.end = ScriptedBytes::End::EndOfInput;
        StdConsole console(nullptr, &scripted_byte, &bytes);
        EXPECT_EQ(console.read_line(std::chrono::seconds{1}).status, OperatorInputStatus::EndOfInput) << "a half line is not an answer";
    }
    {
        ScriptedBytes bytes;
        bytes.data = "typing";
        bytes.end = ScriptedBytes::End::Timeout;
        StdConsole console(nullptr, &scripted_byte, &bytes);
        EXPECT_EQ(console.read_line(std::chrono::seconds{1}).status, OperatorInputStatus::Timeout);
    }
    {
        ScriptedBytes bytes;
        bytes.end = ScriptedBytes::End::Error;
        StdConsole console(nullptr, &scripted_byte, &bytes);
        EXPECT_EQ(console.read_line(std::chrono::seconds{1}).status, OperatorInputStatus::Error);
    }
    {
        ScriptedBytes bytes;
        bytes.data = std::string(kConfirmationLineMaxBytes + 40, 'x') + "\nok\n";
        StdConsole console(nullptr, &scripted_byte, &bytes);
        EXPECT_EQ(console.read_line(std::chrono::seconds{1}).status, OperatorInputStatus::TooLong);
        const OperatorLineResult after = console.read_line(std::chrono::seconds{1});
        EXPECT_EQ(after.status, OperatorInputStatus::Line) << "the tail of the overlong line was drained, it is not the start of the next answer";
        EXPECT_EQ(after.line(), "ok");
    }
    {
        ScriptedBytes bytes;
        bytes.data = "x\n";
        StdConsole console(nullptr, &scripted_byte, &bytes);
        EXPECT_EQ(console.read_line(std::chrono::milliseconds{0}).status, OperatorInputStatus::Timeout);
        EXPECT_EQ(bytes.calls, 0) << "a timeout of nothing reads nothing";
        EXPECT_EQ(console.read_line(std::chrono::minutes{6}).status, OperatorInputStatus::Error) << "the reader caps a human's wait at five minutes";
    }
}

// ============================================================ 3. the evidence directory ==============================================================

TEST(EvidenceStem, IsLettersDigitsDotUnderscoreAndHyphenAndNothingThatMovesAround) {
    for (const std::string_view ok : {"a", "HY-1790000000000-7-0A1B-1790000270000", "a.b_c-d", "..x", "-x", "x.", "0"}) {
        EXPECT_TRUE(is_evidence_stem(ok)) << ok;
    }
    for (const std::string_view bad : {"", "a/b", "a\\b", "../x", "a:b", "a b", "a\n", "a\x01", "a%2e", "caf\xc3\xa9", "a*", "a?", "a|b"}) {
        EXPECT_FALSE(is_evidence_stem(bad)) << bad;
    }
    EXPECT_TRUE(is_evidence_stem(std::string(kEvidenceStemMaxChars, 'a')));
    EXPECT_FALSE(is_evidence_stem(std::string(kEvidenceStemMaxChars + 1, 'a')));
}

TEST(DirectoryEvidenceStore, CreatesTheDirectoryAndTheFileAndWritesTheBytesExactly) {
    TempDir dir;
    const std::string evidence_dir = dir.file("audit.log.operator-evidence");
    DirectoryEvidenceStore store(evidence_dir);
    EXPECT_EQ(store.directory(), evidence_dir);
    std::string contents = "{\"format\":\"hy-operator-evidence/1\"}\r\n";
    contents.push_back('\0');
    contents += "trailing";
    contents.push_back(static_cast<char>(0xff));
    contents.push_back('\0');
    ASSERT_TRUE(store.store("HY-1-2-AB-1790000270000", contents));
    const std::string path = store.path_for("HY-1-2-AB-1790000270000");
    EXPECT_EQ(path, (fs::path(evidence_dir) / "HY-1-2-AB-1790000270000.json").string());
    EXPECT_EQ(read_file(path), contents) << "byte for byte: no newline translation, no truncation at a NUL";
    EXPECT_EQ(directory_listing(evidence_dir), (std::vector<std::string>{"HY-1-2-AB-1790000270000.json"})) << "and nothing else is left in it";
#ifndef _WIN32
    const fs::perms dir_perms = fs::status(evidence_dir).permissions();
    EXPECT_EQ(dir_perms & (fs::perms::group_all | fs::perms::others_all), fs::perms::none) << "owner-only";
    const fs::perms file_perms = fs::status(path).permissions();
    EXPECT_EQ(file_perms & (fs::perms::group_all | fs::perms::others_all), fs::perms::none);
#endif
}

TEST(DirectoryEvidenceStore, ABigFileGoesToDiskWhole) {
    TempDir dir;
    DirectoryEvidenceStore store(dir.file("ev"));
    std::string contents(3u * 1024u * 1024u + 17u, 'x');
    for (std::size_t i = 0; i < contents.size(); i += 4099) contents[i] = static_cast<char>('a' + (i / 4099) % 26);
    ASSERT_TRUE(store.store("big-1", contents));
    EXPECT_EQ(read_file(store.path_for("big-1")), contents);
}

TEST(DirectoryEvidenceStore, AnExistingFileIsNeverTouched) {
    TempDir dir;
    DirectoryEvidenceStore store(dir.file("ev"));
    ASSERT_TRUE(store.store("a-1", "first"));
    EXPECT_FALSE(store.store("a-1", "second, and longer than the first"));
    EXPECT_EQ(read_file(store.path_for("a-1")), "first") << "the file that was there is exactly as it was";
    EXPECT_EQ(directory_listing(dir.file("ev")), (std::vector<std::string>{"a-1.json"}));
    // a file somebody else put there is just as safe
    {
        std::ofstream f(store.path_for("b-1"), std::ios::binary);
        f << "not ours";
    }
    EXPECT_FALSE(store.store("b-1", "ours"));
    EXPECT_EQ(read_file(store.path_for("b-1")), "not ours");
    // and a different stem still works
    EXPECT_TRUE(store.store("a-2", "second"));
    EXPECT_EQ(directory_listing(dir.file("ev")), (std::vector<std::string>{"a-1.json", "a-2.json", "b-1.json"}));
}

TEST(DirectoryEvidenceStore, AStemThatIsNotAStemIsRefusedBeforeAnythingIsCreated) {
    TempDir dir;
    const std::string evidence_dir = dir.file("ev");
    DirectoryEvidenceStore store(evidence_dir);
    for (const std::string_view bad : {"", "../escape", "a/b", "a\\b", "..\\x", "a:b", "sub/../x"}) {
        EXPECT_FALSE(store.store(bad, "x")) << bad;
        EXPECT_EQ(store.path_for(bad), "") << bad;
    }
    EXPECT_FALSE(fs::exists(evidence_dir)) << "not even the directory";
    EXPECT_FALSE(fs::exists(dir.file("escape.json")));
    EXPECT_FALSE(fs::exists(dir.path / ".." / "escape.json"));
    DirectoryEvidenceStore nowhere("");
    EXPECT_FALSE(nowhere.store("a-1", "x"));
    EXPECT_EQ(nowhere.path_for("a-1"), "");
}

TEST(DirectoryEvidenceStore, ADirectoryThatCannotBeUsedIsAFailureAndLeavesNothingBehind) {
    TempDir dir;
    // a file where the directory should be
    {
        std::ofstream f(dir.file("ev"), std::ios::binary);
        f << "i am a file";
    }
    DirectoryEvidenceStore in_the_way(dir.file("ev"));
    EXPECT_FALSE(in_the_way.store("a-1", "x"));
    EXPECT_EQ(read_file(dir.file("ev")), "i am a file");
    // a directory whose parent does not exist: one level is created, not a tree
    DirectoryEvidenceStore deep(dir.file("missing/parent/ev"));
    EXPECT_FALSE(deep.store("a-1", "x"));
    EXPECT_FALSE(fs::exists(dir.file("missing")));
}

TEST(DirectoryEvidenceStore, TheSameDirectoryTakesManyFilesOneAfterTheOther) {
    TempDir dir;
    DirectoryEvidenceStore store(dir.file("ev"));
    std::vector<std::string> expected;
    for (int i = 0; i < 20; ++i) {
        const std::string stem = "coid-" + std::to_string(1000 + i);
        ASSERT_TRUE(store.store(stem, "n=" + std::to_string(i)));
        expected.push_back(stem + ".json");
    }
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(directory_listing(dir.file("ev")), expected);
    EXPECT_EQ(read_file(store.path_for("coid-1007")), "n=7");
}
