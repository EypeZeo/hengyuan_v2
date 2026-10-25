// SPDX-License-Identifier: proprietary
// test_operator_resolution_guard.cpp -- D5 slice R1 guard (docs/OPERATOR_RESOLUTION_DESIGN.md 9, "R-guard").
//
// R1 teaches the recovery reader to accept ONE new kind of frame, OrderOperatorResolved, written by an offline operator
// tool that does not exist yet (slice R3). What keeps that acceptance safe is not the reader alone but the absence of
// anything else that could produce such a frame, or open a store the way the tool will. Behaviour cannot see an
// absence, so this test reads the sources:
//
//   1. AuditEventType::OrderOperatorResolved is NAMED only where it is defined, where the codec decides which
//      version may carry it, and where the reader recognises it. A fourth file naming it is a producer (or a new
//      reader) somebody has to look at. Each pin carries the exact number of mentions.
//   2. OperatorOpen -- the sink mode that tolerates the capacity bounds -- is named by no source but the sink
//      header. The trading process must never open a store that way; the day the tool exists, its translation unit
//      is added here with the reason.
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
                          {kSink, 1, "the recovery scan recognising the frame (never constructing one)"}});
}

TEST(OperatorResolutionGuard, OperatorOpenIsNamedByNothingButTheSinkHeader) {
    // The trading process must never open a store in the mode that tolerates the capacity bounds. Count only the
    // fact of naming here: the header uses it in several places (the enum, the constructor, the three bounds).
    std::vector<std::string> elsewhere;
    std::size_t in_sink = 0;
    for (const SourceFile& f : production_sources()) {
        const std::size_t n = count_identifier(f.code, "OperatorOpen");
        if (f.rel == kSink) {
            in_sink = n;
        } else if (n != 0) {
            elsewhere.push_back(f.rel);
        }
    }
    for (const std::string& f : elsewhere) {
        ADD_FAILURE() << f << " names OperatorOpen -- only the offline operator tool may, and it is added to this "
                              "guard (with its file) in the slice that introduces it";
    }
    EXPECT_GT(in_sink, 3u) << "the sink header defines the mode and applies it to the three bounds";
}

TEST(OperatorResolutionGuard, TheOperatorEdgeFunctionIsCalledByTheReaderAndNothingElse) {
    expect_named_only_in("validate_operator_resolution",
                         {{kLifecycle, 1, "the definition"}, {kSink, 1, "accepts_operator_resolution()"}});
}

TEST(OperatorResolutionGuard, TheWriteVersionIsAssignedOnceByTheRecoveryScan) {
    std::size_t assignments = 0;
    std::vector<std::string> elsewhere;
    for (const SourceFile& f : production_sources()) {
        const std::size_t any = count_identifier(f.code, "write_version_");
        if (f.rel == kSink) {
            assignments = count_assignments(f.code, "write_version_");
        } else if (any != 0) {
            elsewhere.push_back(f.rel);
        }
    }
    for (const std::string& f : elsewhere) ADD_FAILURE() << f << " names write_version_";
    EXPECT_EQ(assignments, 1u)
        << "write_version_ may be set exactly once, from the highest version the recovery scan saw; a second "
           "assignment is a way to write the first v6 frame of a store without a v6 frame ever having been there";
}

TEST(OperatorResolutionGuard, TheOnlyProductionEncoderCallPassesTheWriteVersion) {
    std::size_t total_calls = 0;
    for (const SourceFile& f : production_sources()) {
        const std::vector<std::vector<std::string>> calls = call_args(f.code, "encode_order_event_frame");
        total_calls += calls.size();
        if (calls.empty()) continue;
        EXPECT_EQ(f.rel, kSink) << "a second production caller of encode_order_event_frame(): its version argument "
                                   "decides what ends up in a log";
        for (const std::vector<std::string>& args : calls) {
            ASSERT_EQ(args.size(), 9u) << f.rel << ": (buf, key_id, seq, time_kind, now, record, prev_mac, key, version)";
            EXPECT_EQ(args.back(), "write_version_") << f.rel;
        }
    }
    EXPECT_EQ(total_calls, 1u);
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
