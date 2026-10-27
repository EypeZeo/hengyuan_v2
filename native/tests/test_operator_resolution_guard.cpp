// SPDX-License-Identifier: proprietary
// test_operator_resolution_guard.cpp -- D5 slice R1 guard (docs/OPERATOR_RESOLUTION_DESIGN.md 9, "R-guard").
//
// R1 teaches the recovery reader to accept ONE new kind of frame, OrderOperatorResolved, written by an offline operator
// tool (slices R3-0, R3a and R3b). What keeps that acceptance safe is not the reader alone but the absence of anything
// else that could produce such a frame, or open a store the way the tool does. Behaviour cannot see an absence, so this
// test reads the sources:
//
//   1. AuditEventType::OrderOperatorResolved is NAMED only where it is defined, where the codec decides which
//      version may carry it, where the reader recognises it, and (R3a) in the tool's flow, which builds the one frame
//      it may write. A further file naming it is a producer (or a new reader) somebody has to look at. Each pin carries
//      the exact number of mentions.
//   2. OperatorOpen -- the sink mode that tolerates the capacity bounds -- is named by no source but the sink header
//      and (R3a) the tool's flow, which refuses any other mode. The trading process must never open a store that way;
//      the day the command line exists (R3b), its translation unit is added here with the reason.
//   3. validate_operator_resolution() is called by the reader and nothing else. (That validate_transition() still
//      refuses every edge out of EscalatedToOperator is a behaviour, pinned in test_order_lifecycle.cpp.)
//   4. The sink's write version is assigned exactly once, by its recovery scan, and the only production call of
//      encode_order_event_frame() passes it as the version. So no code path writes the first v6 frame of a store.
//   5. Format version 6 is named only by the codec and the sink; every control-plane record is still written and
//      read at kFrameFormatVersion (5).
//
// The scan strips comments and the contents of string/char literals first, so prose that mentions a name does not
// count and a string that contains one cannot hide a real use. (The helpers are copies of the ones in
// test_reconcile_durable_guard.cpp; the guards share no header on purpose, so neither can break the other.)

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef HY_NATIVE_SOURCE_DIR
#error "HY_NATIVE_SOURCE_DIR must name the native/ directory (see native/CMakeLists.txt)"
#endif

namespace fs = std::filesystem;

namespace {

// Source text with comments removed and the contents of string and char literals blanked.
std::string code_only(const std::string& s) {
    enum class State { Code, LineComment, BlockComment, String, Char };
    std::string out;
    out.reserve(s.size());
    State st = State::Code;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const char next = i + 1 < s.size() ? s[i + 1] : '\0';
        switch (st) {
            case State::Code:
                if (c == '/' && next == '/') {
                    st = State::LineComment;
                    ++i;
                } else if (c == '/' && next == '*') {
                    st = State::BlockComment;
                    ++i;
                } else if (c == '"') {
                    st = State::String;
                    out.push_back(c);
                } else if (c == '\'' && !out.empty() &&
                           (std::isalnum(static_cast<unsigned char>(out.back())) != 0 || out.back() == '_')) {
                    out.push_back(c);  // a digit separator (1'000) or a literal prefix, not a char literal
                } else if (c == '\'') {
                    st = State::Char;
                    out.push_back(c);
                } else {
                    out.push_back(c);
                }
                break;
            case State::LineComment:
                if (c == '\n') {
                    st = State::Code;
                    out.push_back(c);
                }
                break;
            case State::BlockComment:
                if (c == '*' && next == '/') {
                    st = State::Code;
                    ++i;
                } else if (c == '\n') {
                    out.push_back(c);
                }
                break;
            case State::String:
            case State::Char: {
                const char quote = st == State::String ? '"' : '\'';
                if (c == '\\') {
                    ++i;  // skip the escaped character
                } else if (c == quote) {
                    st = State::Code;
                    out.push_back(c);
                }
                break;
            }
        }
    }
    return out;
}

bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

std::string squeeze(const std::string& s) {
    // Leading/trailing whitespace dropped, every inner run of whitespace one space: the text of an argument,
    // independent of how the call was wrapped.
    std::string out;
    bool pending_space = false;
    for (const char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            pending_space = !out.empty();
        } else {
            if (pending_space) out.push_back(' ');
            pending_space = false;
            out.push_back(c);
        }
    }
    return out;
}

// How many times `name` appears as a whole identifier (not as part of a longer one) in `code`.
std::size_t count_identifier(const std::string& code, std::string_view name) {
    std::size_t n = 0;
    for (std::size_t pos = code.find(name); pos != std::string::npos; pos = code.find(name, pos + name.size())) {
        if (pos > 0 && is_ident_char(code[pos - 1])) continue;
        const std::size_t end = pos + name.size();
        if (end < code.size() && is_ident_char(code[end])) continue;
        ++n;
    }
    return n;
}

// How many times `name` is the target of a plain assignment (`name = ...`, not `==`, `<=`, `!=`, `+=` ...).
std::size_t count_assignments(const std::string& code, std::string_view name) {
    std::size_t n = 0;
    for (std::size_t pos = code.find(name); pos != std::string::npos; pos = code.find(name, pos + name.size())) {
        if (pos > 0 && is_ident_char(code[pos - 1])) continue;
        std::size_t i = pos + name.size();
        if (i < code.size() && is_ident_char(code[i])) continue;
        while (i < code.size() && std::isspace(static_cast<unsigned char>(code[i])) != 0) ++i;
        if (i < code.size() && code[i] == '=' && (i + 1 >= code.size() || code[i + 1] != '=')) ++n;
    }
    return n;
}

// The arguments (as squeezed text) of every CALL of `name` in `code`. A definition or declaration -- the name directly
// preceded by a type name -- is not a call; a call is preceded by a statement boundary, `->`, `.`, `::`, an operator,
// or one of the words `return`, `else`, `do`.
std::vector<std::vector<std::string>> call_args(const std::string& code, std::string_view name) {
    std::vector<std::vector<std::string>> calls;
    for (std::size_t pos = code.find(name); pos != std::string::npos; pos = code.find(name, pos + name.size())) {
        if (pos > 0 && is_ident_char(code[pos - 1])) continue;  // part of a longer identifier
        std::size_t open = pos + name.size();
        while (open < code.size() && std::isspace(static_cast<unsigned char>(code[open])) != 0) ++open;
        if (open >= code.size() || code[open] != '(') continue;  // not a call expression

        std::size_t before = pos;
        while (before > 0 && std::isspace(static_cast<unsigned char>(code[before - 1])) != 0) --before;
        if (before > 0 && is_ident_char(code[before - 1])) {
            std::size_t word_start = before;
            while (word_start > 0 && is_ident_char(code[word_start - 1])) --word_start;
            const std::string_view word(code.data() + word_start, before - word_start);
            if (word != "return" && word != "else" && word != "do") continue;  // a definition
        }

        int depth = 0;
        std::vector<std::string> args;
        std::string current;
        for (std::size_t i = open; i < code.size(); ++i) {
            const char c = code[i];
            if (c == '(' || c == '[' || c == '{') {
                if (depth >= 1) current.push_back(c);
                ++depth;
            } else if (c == ')' || c == ']' || c == '}') {
                --depth;
                if (depth == 0) {
                    const std::string last = squeeze(current);
                    if (!last.empty() || !args.empty()) args.push_back(last);
                    break;
                }
                current.push_back(c);
            } else if (depth == 1 && c == ',') {
                args.push_back(squeeze(current));
                current.clear();
            } else if (depth >= 1) {
                current.push_back(c);
            }
        }
        calls.push_back(std::move(args));
    }
    return calls;
}

struct SourceFile {
    std::string rel;   // relative to native/, forward slashes
    std::string code;  // code_only() of the file
};

// Everything the production binaries are built from: include/ and src/.
const std::vector<SourceFile>& production_sources() {
    static const std::vector<SourceFile> files = [] {
        std::vector<SourceFile> v;
        const fs::path root(HY_NATIVE_SOURCE_DIR);
        for (const char* dir : {"include", "src"}) {
            const fs::path base = root / dir;
            if (!fs::exists(base)) continue;
            for (const auto& entry : fs::recursive_directory_iterator(base)) {
                if (!entry.is_regular_file()) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".hpp" && ext != ".cpp" && ext != ".h") continue;
                std::ifstream in(entry.path(), std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                v.push_back({fs::relative(entry.path(), root).generic_string(), code_only(text)});
            }
        }
        return v;
    }();
    return files;
}

constexpr const char* kCodec = "include/hengyuan/durable_frame_codec.hpp";
constexpr const char* kSink = "include/hengyuan/durable_audit_sink.hpp";
constexpr const char* kTrail = "include/hengyuan/audit_trail.hpp";
constexpr const char* kLifecycle = "include/hengyuan/order_lifecycle.hpp";
constexpr const char* kFrame = "include/hengyuan/operator_resolution_frame.hpp";
constexpr const char* kRules = "include/hengyuan/operator_resolution_rules.hpp";
constexpr const char* kEvidence = "include/hengyuan/operator_resolution_evidence.hpp";
constexpr const char* kResponse = "include/hengyuan/operator_resolution_response.hpp";
constexpr const char* kConfirmation = "include/hengyuan/operator_resolution_confirmation.hpp";
constexpr const char* kPorts = "include/hengyuan/operator_resolution_ports.hpp";
constexpr const char* kBundle = "include/hengyuan/operator_resolution_bundle.hpp";
constexpr const char* kFlow = "include/hengyuan/operator_resolution_flow.hpp";

const SourceFile* find_source(std::string_view rel) {
    for (const SourceFile& f : production_sources()) {
        if (f.rel == rel) return &f;
    }
    return nullptr;
}

// The relative paths of the production files that name `name` as an identifier, in scan order.
std::vector<std::string> files_naming(std::string_view name) {
    std::vector<std::string> out;
    for (const SourceFile& f : production_sources()) {
        if (count_identifier(f.code, name) != 0) out.push_back(f.rel);
    }
    return out;
}

// The text between the braces of the body of the function `name` (its first occurrence followed by a parameter list and a
// brace), braces matched. Empty if there is none.
std::string function_body(const std::string& code, std::string_view name) {
    for (std::size_t pos = code.find(name); pos != std::string::npos; pos = code.find(name, pos + name.size())) {
        if (pos > 0 && is_ident_char(code[pos - 1])) continue;
        std::size_t i = pos + name.size();
        while (i < code.size() && std::isspace(static_cast<unsigned char>(code[i])) != 0) ++i;
        if (i >= code.size() || code[i] != '(') continue;
        int paren = 0;
        for (; i < code.size(); ++i) {  // to the end of the parameter list
            if (code[i] == '(') ++paren;
            if (code[i] == ')' && --paren == 0) break;
        }
        while (i < code.size() && code[i] != '{' && code[i] != ';') ++i;  // qualifiers, then the body or the end of a declaration
        if (i >= code.size() || code[i] != '{') continue;
        const std::size_t begin = i + 1;
        int depth = 1;
        for (++i; i < code.size(); ++i) {
            if (code[i] == '{') ++depth;
            if (code[i] == '}' && --depth == 0) return code.substr(begin, i - begin);
        }
    }
    return {};
}

// The `#include` lines of a file, squeezed, in order.
std::vector<std::string> include_lines(const std::string& code) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < code.size()) {
        std::size_t end = code.find('\n', pos);
        if (end == std::string::npos) end = code.size();
        const std::string line = squeeze(code.substr(pos, end - pos));
        if (line.rfind("#include", 0) == 0) out.push_back(line);
        pos = end + 1;
    }
    return out;
}

struct Pin {
    const char* file;
    std::size_t mentions;
    const char* why;
};

// Fails for every file that names `name` and is not in `pins`, for every pin whose count differs, and for a pinned
// file that no longer names it at all.
void expect_named_only_in(std::string_view name, const std::vector<Pin>& pins) {
    std::vector<std::string> problems;
    for (const SourceFile& f : production_sources()) {
        const std::size_t n = count_identifier(f.code, name);
        if (n == 0) continue;
        const Pin* pin = nullptr;
        for (const Pin& p : pins) {
            if (f.rel == p.file) pin = &p;
        }
        if (pin == nullptr) {
            problems.push_back(f.rel + " names " + std::string(name) + " " + std::to_string(n) +
                               " time(s) and is not pinned: a new user of it is a new producer or reader, and "
                               "somebody has to look at it before it is added here with the reason");
        } else if (pin->mentions != n) {
            problems.push_back(f.rel + " names " + std::string(name) + " " + std::to_string(n) + " time(s), pinned " +
                               std::to_string(pin->mentions) + " (" + pin->why + ")");
        }
    }
    for (const Pin& p : pins) {
        bool found = false;
        for (const SourceFile& f : production_sources()) {
            if (f.rel == p.file && count_identifier(f.code, name) != 0) found = true;
        }
        if (!found) problems.push_back(std::string(p.file) + ": pinned for " + std::string(name) + " but no longer names it");
    }
    for (const std::string& s : problems) ADD_FAILURE() << s;
    EXPECT_TRUE(problems.empty());
}

}  // namespace

TEST(OperatorResolutionGuard, TheScanSeesRealSources) {
    // A guard that scans an empty tree passes for the wrong reason.
    std::size_t files = 0;
    bool saw_sink = false;
    bool saw_codec = false;
    for (const SourceFile& f : production_sources()) {
        ++files;
        if (f.rel == kSink) saw_sink = true;
        if (f.rel == kCodec) saw_codec = true;
    }
    EXPECT_GT(files, 50u);
    EXPECT_TRUE(saw_sink);
    EXPECT_TRUE(saw_codec);
}

TEST(OperatorResolutionGuard, TheScannerHelpersHaveKnownAnswers) {
    // The negative controls of the guard itself: the helpers the pins use, on text with known answers.
    EXPECT_EQ(count_identifier(code_only("a(X); b(X);"), "X"), 2u);
    EXPECT_EQ(count_identifier(code_only("a(MyX); b(XY); c(X_1);"), "X"), 0u) << "part of a longer identifier";
    EXPECT_EQ(count_identifier(code_only("// X\n/* X */ const char* s = \"X\"; char c = 'X';"), "X"), 0u)
        << "prose and literals do not count";
    EXPECT_EQ(count_identifier(code_only("E::X == v; E::X"), "X"), 2u) << "a qualified name is the identifier";

    EXPECT_EQ(count_assignments(code_only("w_ = 5; w_ == 5; w_ <= 5; w_ != 5; if (a) w_ = 6;"), "w_"), 2u);
    EXPECT_EQ(count_assignments(code_only("w_(5); xw_ = 5; w_x = 5; // w_ = 7\n"), "w_"), 0u);

    using Args = std::vector<std::string>;
    EXPECT_EQ(call_args(code_only("void f() { enc(a, g(b, c), {1, 2}, w_); }"), "enc").at(0),
              (Args{"a", "g(b, c)", "{1, 2}", "w_"}));
    EXPECT_TRUE(call_args(code_only("inline std::size_t enc(A a, B b) noexcept {}"), "enc").empty())
        << "a definition is not a call";
    EXPECT_TRUE(call_args(code_only("// enc(a, b)\nconst char* s = \"enc(a, b)\";"), "enc").empty());
}

TEST(OperatorResolutionGuard, TheOperatorEventIsNamedOnlyByItsDefinitionTheCodecAndTheReader) {
    expect_named_only_in("OrderOperatorResolved",
                         {{kTrail, 2, "the enumerator and its name in audit_event_name()"},
                          {kCodec, 1, "is_legal_audit_event_type_for_version(): the one version that may carry it"},
                          {kSink, 2, "the recovery scan recognising the frame, and append_operator_resolution() recognising "
                                     "the one record it may write (R3-0) -- neither constructs one"},
                          {kFlow, 1, "the one frame the tool builds, from a checkpoint and a conclusion (R3a)"}});
}

TEST(OperatorResolutionGuard, OperatorOpenIsNamedByTheSinkHeaderAndTheToolsFlowAlone) {
    // The trading process must never open a store in the mode that tolerates the capacity bounds. Count only the
    // fact of naming here: the sink header uses it in several places (the enum, the constructor, the three bounds); the
    // tool's flow (R3a) names it to REFUSE any sink that was not opened that way -- it opens nothing itself.
    std::vector<std::string> elsewhere;
    std::size_t in_sink = 0;
    std::size_t in_flow = 0;
    for (const SourceFile& f : production_sources()) {
        const std::size_t n = count_identifier(f.code, "OperatorOpen");
        if (f.rel == kSink) {
            in_sink = n;
        } else if (f.rel == kFlow) {
            in_flow = n;
        } else if (n != 0) {
            elsewhere.push_back(f.rel);
        }
    }
    for (const std::string& f : elsewhere) {
        ADD_FAILURE() << f << " names OperatorOpen -- only the offline operator tool may, and it is added to this "
                              "guard (with its file) in the slice that introduces it";
    }
    EXPECT_GT(in_sink, 3u) << "the sink header defines the mode and applies it to the three bounds";
    EXPECT_EQ(in_flow, 2u) << "the flow refuses a sink in any other mode, when a session begins and when an order is resolved";
    // ...and it only compares: the flow builds no sink
    const SourceFile* flow = find_source(kFlow);
    ASSERT_NE(flow, nullptr);
    EXPECT_EQ(call_args(flow->code, "DurableAuditSink").size(), 0u);
    EXPECT_EQ(count_identifier(flow->code, "make_unique"), 0u);
}

TEST(OperatorResolutionGuard, TheOperatorEdgeFunctionIsCalledByTheSharedResolutionRuleAndNothingElse) {
    // R3-0 moved the rule out of the sink: check_resolution() applies it for the reader AND the writer.
    expect_named_only_in("validate_operator_resolution",
                         {{kLifecycle, 1, "the definition"}, {kRules, 1, "check_resolution(): the one place the edge is applied"}});
}

TEST(OperatorResolutionGuard, TheSharedResolutionRuleIsAppliedByTheReaderAndTheWriterAlone) {
    // One rule, two callers, both in the sink: the recovery scan's accepts_operator_resolution() and the writer's
    // append_operator_resolution() -- so a frame the writer lets through is a frame the next start accepts.
    expect_named_only_in("check_resolution", {{kRules, 1, "the definition"},
                                              {kSink, 2, "accepts_operator_resolution() (the reader) and "
                                                         "append_operator_resolution() (the writer's pre-check)"}});
    const SourceFile* sink = find_source(kSink);
    ASSERT_NE(sink, nullptr);
    EXPECT_EQ(count_identifier(function_body(sink->code, "accepts_operator_resolution"), "check_resolution"), 1u);
    EXPECT_EQ(count_identifier(function_body(sink->code, "append_operator_resolution"), "check_resolution"), 1u);
}

TEST(OperatorResolutionGuard, TheOperatorAppendIsCalledByTheToolsFlowAlone) {
    // R3-0 built the writer; R3a is the offline tool that calls it, once, at the end of resolve_order(). Nothing else may.
    expect_named_only_in("append_operator_resolution", {{kSink, 1, "the definition"}, {kFlow, 1, "the one call"}});
    const SourceFile* flow = find_source(kFlow);
    ASSERT_NE(flow, nullptr);
    const std::vector<std::vector<std::string>> calls = call_args(flow->code, "append_operator_resolution");
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0].size(), 2u);
    EXPECT_EQ(calls[0][0], "rec");
    EXPECT_EQ(calls[0][1], "append_ref->count()") << "stamped with the exchange time read just before, not a local clock";
    const std::string body = function_body(flow->code, "resolve_order");
    ASSERT_FALSE(body.empty());
    EXPECT_EQ(count_identifier(body, "append_operator_resolution"), 1u);
}

TEST(OperatorResolutionGuard, TheWriteVersionIsAssignedByTheRecoveryScanAndByTheAckedOperatorFrameOnly) {
    std::size_t assignments = 0;
    std::vector<std::string> elsewhere;
    const SourceFile* sink = find_source(kSink);
    ASSERT_NE(sink, nullptr);
    for (const SourceFile& f : production_sources()) {
        const std::size_t any = count_identifier(f.code, "write_version_");
        if (f.rel == kSink) {
            assignments = count_assignments(f.code, "write_version_");
        } else if (any != 0) {
            elsewhere.push_back(f.rel);
        }
    }
    for (const std::string& f : elsewhere) ADD_FAILURE() << f << " names write_version_";
    EXPECT_EQ(assignments, 2u)
        << "write_version_ is set exactly twice: from the highest version the recovery scan saw, and to 6 right after "
           "append_operator_resolution() has had an operator frame acknowledged. A third assignment is a way to write "
           "the first v6 frame of a store without a v6 frame ever having been there (or to go back to v5 after one)";
    const std::string scan = function_body(sink->code, "run_recovery_scan_impl");
    ASSERT_FALSE(scan.empty());
    EXPECT_EQ(count_assignments(scan, "write_version_"), 1u) << "the first assignment belongs to the recovery scan";
    const std::string append = function_body(sink->code, "append_operator_resolution");
    ASSERT_FALSE(append.empty());
    EXPECT_EQ(count_assignments(append, "write_version_"), 1u) << "the second belongs to append_operator_resolution()";
    // ...and it comes AFTER the append was acknowledged, never before: a refused or failed append leaves the version alone.
    const std::size_t acked_at = append.find("acked()");
    const std::size_t assigned_at = append.find("write_version_ =");
    ASSERT_NE(acked_at, std::string::npos);
    ASSERT_NE(assigned_at, std::string::npos);
    EXPECT_LT(acked_at, assigned_at);
}

TEST(OperatorResolutionGuard, TheOnlyProductionEncoderCallPassesItsOwnVersionAndHasTwoCallers) {
    std::size_t total_calls = 0;
    for (const SourceFile& f : production_sources()) {
        const std::vector<std::vector<std::string>> calls = call_args(f.code, "encode_order_event_frame");
        total_calls += calls.size();
        if (calls.empty()) continue;
        EXPECT_EQ(f.rel, kSink) << "a second production caller of encode_order_event_frame(): its version argument "
                                   "decides what ends up in a log";
        for (const std::vector<std::string>& args : calls) {
            ASSERT_EQ(args.size(), 9u) << f.rel << ": (buf, key_id, seq, time_kind, now, record, prev_mac, key, version)";
            EXPECT_EQ(args.back(), "version") << f.rel << ": the version of THIS frame, handed in by append_impl()'s caller";
        }
    }
    EXPECT_EQ(total_calls, 1u);

    // append_impl() is where that version comes from: the store's write version for append_durable(), 6 for the operator
    // frame -- and the 6 is passed only from inside append_operator_resolution().
    const SourceFile* sink = find_source(kSink);
    ASSERT_NE(sink, nullptr);
    const std::vector<std::vector<std::string>> impl_calls = call_args(sink->code, "append_impl");
    ASSERT_EQ(impl_calls.size(), 2u);
    std::size_t with_write_version = 0;
    std::size_t with_v6 = 0;
    for (const std::vector<std::string>& args : impl_calls) {
        ASSERT_EQ(args.size(), 3u) << "(record, now_ms, version)";
        if (args.back() == "write_version_") ++with_write_version;
        if (args.back() == "kFrameFormatVersionV6") ++with_v6;
    }
    EXPECT_EQ(with_write_version, 1u);
    EXPECT_EQ(with_v6, 1u);
    EXPECT_EQ(count_identifier(function_body(sink->code, "append_operator_resolution"), "kFrameFormatVersionV6"), 2u)
        << "the append call and the assignment after it";
    EXPECT_EQ(count_identifier(function_body(sink->code, "append_durable"), "write_version_"), 1u);
}

TEST(OperatorResolutionGuard, FormatVersionSixIsNamedOnlyByTheCodecAndTheSink) {
    std::vector<std::string> problems;
    for (const SourceFile& f : production_sources()) {
        if (count_identifier(f.code, "kFrameFormatVersionV6") == 0) continue;
        if (f.rel != kCodec && f.rel != kSink) problems.push_back(f.rel);
    }
    for (const std::string& f : problems) {
        ADD_FAILURE() << f << " names kFrameFormatVersionV6 -- only the codec (which reads it) and the sink (which "
                              "adopts it from the store's tip) may in R1; the tool of slice R3 is added with its file";
    }
    // ...and the control-plane records stay at 5: they are written and read strictly against kFrameFormatVersion.
    bool saw_control_plane = false;
    for (const SourceFile& f : production_sources()) {
        if (f.rel != "include/hengyuan/control_plane_frame_codec.hpp") continue;
        saw_control_plane = true;
        EXPECT_GT(count_identifier(f.code, "kFrameFormatVersion"), 0u);
        EXPECT_EQ(count_identifier(f.code, "kFrameFormatVersionV6"), 0u);
    }
    EXPECT_TRUE(saw_control_plane);
}

// --- D5 slice R2: the evidence primitives -----------------------------------------------------------------------------
//
//   6. The two evidence headers are for the offline operator tool (slice R3) and the tests. No production file includes
//      either, except that the response parser includes the decision header it feeds -- so neither can end up in the
//      trading process, and the reconciliation path keeps folding every answer that is not a positive match into
//      QueryOutcome::Inconclusive.
//   7. The decision header allocates nothing and parses nothing: its includes are an exact list, and none of the names that
//      allocate, throw or parse JSON appears in it. (The parser is the other header, and it allocates by design.)
//   8. The frame's machine field is built in one place. The six claim bits are named only by the frame header (which defines
//      them and checks them) and the decision header, and detail::make_absent_detail_code() -- the only builder -- is
//      called by conclude_absent() and nothing else, so no code can write "the fence was satisfied" without conclude_absent()
//      having looked at the evidence.

TEST(OperatorResolutionGuard, TheFunctionBodyHelperHasKnownAnswers) {
    const std::string code = code_only("int f() noexcept { if (x) { y(); } return 1; }\nint g() { z(); }\nint h();\n");
    const std::string f = function_body(code, "f");
    EXPECT_NE(f.find("y()"), std::string::npos);
    EXPECT_NE(f.find("return 1"), std::string::npos);
    EXPECT_EQ(f.find("z()"), std::string::npos) << "the body ends at the matching brace";
    EXPECT_NE(function_body(code, "g").find("z()"), std::string::npos);
    EXPECT_TRUE(function_body(code, "h").empty()) << "a declaration has no body";
    EXPECT_TRUE(function_body(code, "nope").empty());
    EXPECT_EQ(include_lines(code_only("#include <a>\n  #include   <b/c.hpp>\n// #include <d>\nint x;\n")),
              (std::vector<std::string>{"#include <a>", "#include <b/c.hpp>"}));
}

TEST(OperatorResolutionGuard, TheEvidenceHeadersAreIncludedByTheOfflineToolsHeadersAlone) {
    // R2 pinned "nothing includes them but each other"; R3a's tool headers are the offline tool, so they may -- and they are
    // the only ones.
    expect_named_only_in("operator_resolution_evidence",
                         {{kResponse, 1, "the parser produces the verdicts the decision header consumes"},
                          {kPorts, 1, "ExchangeMs, the type of every exchange time the ports speak"},
                          {kBundle, 1, "the verdicts, the fence parameters and the times the evidence file records"},
                          {kFlow, 1, "the fence, the windows and the conclusion"}});
    expect_named_only_in("operator_resolution_response", {{kFlow, 1, "the classifiers of the exchange's answers"}});
    ASSERT_NE(find_source(kEvidence), nullptr);
    ASSERT_NE(find_source(kResponse), nullptr);
}

TEST(OperatorResolutionGuard, TheDecisionHeaderAllocatesNothingAndParsesNothing) {
    const SourceFile* f = find_source(kEvidence);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(include_lines(f->code),
              (std::vector<std::string>{"#include <array>", "#include <cstddef>", "#include <cstdint>", "#include <limits>",
                                        "#include <optional>", "#include <span>", "#include <string_view>",
                                        "#include <hengyuan/operator_resolution_frame.hpp>"}))
        << "an include added here must be argued for: this header is the part of the evidence path that allocates nothing";
    for (const char* word :
         {"simdjson", "vector", "string", "unique_ptr", "shared_ptr", "make_unique", "make_shared", "new", "delete", "malloc",
          "calloc", "realloc", "free", "map", "unordered_map", "set", "list", "deque", "function", "ostream", "istream",
          "iostream", "fstream", "sstream", "printf", "snprintf", "throw", "try", "catch", "exception"}) {
        EXPECT_EQ(count_identifier(f->code, word), 0u) << word;
    }
    // the response header is where the allocating parser lives, and it says so
    const SourceFile* r = find_source(kResponse);
    ASSERT_NE(r, nullptr);
    EXPECT_GT(count_identifier(r->code, "simdjson"), 0u);
}

TEST(OperatorResolutionGuard, TheMachineFieldBuilderIsCalledByTheConclusionAlone) {
    expect_named_only_in("make_absent_detail_code",
                         {{kEvidence, 2, "the definition, and the one call inside conclude_absent()"}});
    const SourceFile* f = find_source(kEvidence);
    ASSERT_NE(f, nullptr);
    const std::string body = function_body(f->code, "conclude_absent");
    ASSERT_FALSE(body.empty());
    EXPECT_EQ(count_identifier(body, "make_absent_detail_code"), 1u) << "the call must be inside conclude_absent()";
    EXPECT_EQ(call_args(f->code, "make_absent_detail_code").size(), 1u);
}

TEST(OperatorResolutionGuard, TheClaimBitsAreNamedOnlyByTheFrameHeaderAndTheDecisionHeader) {
    for (const char* bit : {"kAbsentRequiredBits", "kFenceSatisfied", "kPointQueryUnknownOrder", "kAllOrdersAbsent",
                            "kOpenOrdersAbsent", "kOperatorConfirmed", "kLockHeld", "kWaitedSecondsShift", "kToolVersionShift"}) {
        const std::vector<std::string> files = files_naming(bit);
        for (const std::string& file : files) {
            EXPECT_TRUE(file == kFrame || file == kEvidence)
                << file << " names " << bit << ": a detail_code is built by conclude_absent() and read by "
                           "is_valid_absent_detail_code(), nowhere else";
        }
        EXPECT_FALSE(files.empty()) << bit << " is not named anywhere any more";
    }
}

// --- D5 slice R3a: the offline tool's procedure -------------------------------------------------------------------------------
//
//   9. The tool's headers belong to the tool. Nothing in the trading process includes the flow, the ports or the bundle, and
//      the confirmation header (which holds the grammar the typed phrase is made of) is included only by the tool's headers.
//  10. The shared rule is asked in the right order: the flow asks about the ORDER (check_order_resolvable) before it waits and
//      before it queries; only the writer and the reader ask about a FRAME (check_resolution / check_frame).
//  11. The one frame the tool writes is built from the checkpoint and the conclusion: its machine field is assigned once, from
//      conclude_absent()'s result, and conclude_absent() is asked exactly twice (without the operator's word, then with it).
//  12. The confirmation header is the allocation-free part of the tool: its includes are an exact list.

TEST(OperatorResolutionGuard, TheToolsHeadersAreIncludedByTheToolAlone) {
    expect_named_only_in("operator_resolution_flow", {});
    expect_named_only_in("operator_resolution_ports", {{kFlow, 1, "the flow reaches the world through the ports"}});
    expect_named_only_in("operator_resolution_bundle", {{kFlow, 1, "the flow builds the evidence file"}});
    expect_named_only_in("operator_resolution_confirmation",
                         {{kPorts, 1, "OperatorLineResult, what the console port returns"},
                          {kBundle, 1, "environment_name() and the digest grammar"},
                          {kFlow, 1, "the phrase and its judge"}});
    for (const char* f : {kFlow, kPorts, kBundle, kConfirmation}) ASSERT_NE(find_source(f), nullptr) << f;
}

TEST(OperatorResolutionGuard, TheFlowAsksAboutTheOrderAndOnlyTheWriterAndTheReaderAskAboutTheFrame) {
    expect_named_only_in("check_order_resolvable", {{kRules, 2, "the definition and its one call inside check_resolution()"},
                                                    {kFlow, 2, "resolve_order() before it waits, and list_unresolved_orders()"}});
    expect_named_only_in("check_frame", {{kRules, 2, "the definition and its one call inside check_resolution()"}});
    const SourceFile* flow = find_source(kFlow);
    ASSERT_NE(flow, nullptr);
    const std::string body = function_body(flow->code, "resolve_order");
    ASSERT_FALSE(body.empty());
    // the order is judged before the first exchange request of any kind, and before the first sleep
    const std::size_t asked = body.find("check_order_resolvable");
    const std::size_t first_read = body.find("exchange.");
    const std::size_t first_sleep = body.find("sleep_for");
    ASSERT_NE(asked, std::string::npos);
    ASSERT_NE(first_read, std::string::npos);
    ASSERT_NE(first_sleep, std::string::npos);
    EXPECT_LT(asked, first_read);
    EXPECT_LT(asked, first_sleep);
}

TEST(OperatorResolutionGuard, TheFrameTheToolWritesIsMadeOfTheCheckpointAndTheConclusionAlone) {
    const SourceFile* flow = find_source(kFlow);
    ASSERT_NE(flow, nullptr);
    const std::string body = function_body(flow->code, "resolve_order");
    ASSERT_FALSE(body.empty());
    const std::string squeezed = squeeze(body);
    EXPECT_EQ(count_identifier(body, "conclude_absent"), 2u) << "once without the operator's word (to know whether to ask), once with";
    EXPECT_EQ(count_assignments(body, "rec.detail_code"), 1u);
    EXPECT_NE(squeezed.find("rec.detail_code = fin.detail_code;"), std::string::npos)
        << "the machine field is the conclusion's, never built by the flow";
    // the identity of the frame is the checkpoint's, field by field
    for (const char* assignment : {"rec.mode = cp.mode;", "rec.symbol_id = cp.symbol_id;", "rec.price_ticks = cp.intended_price_ticks;",
                                   "rec.qty_ticks = cp.intended_qty_ticks;", "rec.side = cp.side;"}) {
        EXPECT_NE(squeezed.find(assignment), std::string::npos) << assignment;
    }
    // the operator's word reaches the conclusion only after the judge said Confirmed
    EXPECT_EQ(count_assignments(body, "in.operator_confirmed"), 2u) << "false before the prompt, true after the judge";
    const std::size_t judged = body.find("ConfirmationVerdict::Confirmed");
    const std::size_t set_true = body.find("in.operator_confirmed = true");
    ASSERT_NE(judged, std::string::npos);
    ASSERT_NE(set_true, std::string::npos);
    EXPECT_LT(judged, set_true);
    // nothing in the flow builds a machine field of its own
    EXPECT_EQ(count_identifier(flow->code, "make_absent_detail_code"), 0u);
    EXPECT_EQ(count_identifier(flow->code, "kAbsentRequiredBits"), 0u);
}

TEST(OperatorResolutionGuard, TheConfirmationHeaderAllocatesNothing) {
    const SourceFile* f = find_source(kConfirmation);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(include_lines(f->code),
              (std::vector<std::string>{"#include <array>", "#include <chrono>", "#include <cstddef>", "#include <cstdint>",
                                        "#include <string_view>", "#include <hengyuan/account_truth.hpp>",
                                        "#include <hengyuan/operator_confirmation_gate.hpp>",
                                        "#include <hengyuan/operator_input_reader.hpp>", "#include <hengyuan/order_lifecycle.hpp>"}))
        << "an include added here must be argued for: this header is the part of the tool that allocates nothing";
    // `= delete;` declares a function away (OperatorLineReader's copy operations); it is not the expression that frees memory.
    const std::string squeezed = squeeze(f->code);
    std::size_t deleted_functions = 0;
    for (std::size_t at = squeezed.find("= delete;"); at != std::string::npos; at = squeezed.find("= delete;", at + 1)) {
        ++deleted_functions;
    }
    EXPECT_EQ(deleted_functions, 2u);
    for (const char* word : {"vector", "string", "unique_ptr", "shared_ptr", "make_unique", "make_shared", "new", "delete", "malloc",
                             "calloc", "realloc", "free", "map", "unordered_map", "set", "list", "deque", "function", "ostream",
                             "istream", "iostream", "fstream", "sstream", "printf", "snprintf", "throw", "try", "catch", "exception"}) {
        const std::size_t allowed = std::string_view(word) == "delete" ? deleted_functions : 0u;
        EXPECT_EQ(count_identifier(f->code, word), allowed) << word;
    }
}
