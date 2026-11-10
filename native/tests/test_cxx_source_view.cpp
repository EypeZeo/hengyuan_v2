// test_helpers/cxx_source_view.hpp: the scanner behind every source-scanning guard (audit F-B-001 / F-B-010 / F-D-007).
//
// Two halves. (1) The behaviour the eight guard copies had stays exactly as it was: a reference copy of the OLD algorithm lives here, and
// the shared one must agree with it, in both views, over every file of the native/ tree that holds no raw string literal. (2) What the old
// copies got wrong: a raw string literal, above all one with an odd number of double quotes in it, no longer swallows the rest of the file.
// The negative controls build the exact input the audit reported and show that the OLD algorithm loses the code after it while the new one
// keeps it.
#include <gtest/gtest.h>

#include "test_helpers/cxx_source_view.hpp"

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef HY_NATIVE_SOURCE_DIR
#error "HY_NATIVE_SOURCE_DIR must name the native/ directory (see native/CMakeLists.txt)"
#endif

namespace {

using hy::test_helpers::code_only;
using hy::test_helpers::Literals;
using hy::test_helpers::strip_cxx_source;

// The scanner as the guards carried it before: one `String` state, no notion of a raw string. Kept ONLY as the reference the shared one is
// compared with, and as the negative control that shows the blind spot.
std::string legacy_strip(const std::string& s, Literals literals) {
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
                    out.push_back(c);
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
                    if (literals == Literals::Keep) {
                        out.push_back(c);
                        if (i + 1 < s.size()) out.push_back(s[i + 1]);
                    }
                    ++i;
                } else if (c == quote) {
                    st = State::Code;
                    out.push_back(c);
                } else if (literals == Literals::Keep) {
                    out.push_back(c);
                }
                break;
            }
        }
    }
    return out;
}

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// The exact reproduction of the audit: a raw string with ONE double quote in it, then real code.
const std::string kOddQuoteThenCode =
    "auto s = R\"(abc\")\";\n"
    "void f() { ctx.readiness = nullptr; drain_user_data_events(a, b); }\n";

}  // namespace

// ---- the blind spot, and that it is closed ---------------------------------------------------------------------------------------

TEST(CxxSourceView, TheOldScannerLosesTheCodeAfterARawStringWithAnOddNumberOfQuotes) {
    const std::string old_view = legacy_strip(kOddQuoteThenCode, Literals::Blank);
    EXPECT_FALSE(has(old_view, "ctx.readiness = nullptr")) << "the control: the old scanner really is blind here";
    EXPECT_FALSE(has(old_view, "drain_user_data_events"));
}

TEST(CxxSourceView, TheSharedScannerKeepsTheCodeAfterARawStringWithAnOddNumberOfQuotes) {
    const std::string view = code_only(kOddQuoteThenCode);
    EXPECT_TRUE(has(view, "ctx.readiness = nullptr"));
    EXPECT_TRUE(has(view, "drain_user_data_events(a, b)"));
    EXPECT_FALSE(has(view, "abc")) << "and the raw string's own content is blanked, like any literal's";
}

TEST(CxxSourceView, RawStringsWithCustomDelimitersAndPrefixes) {
    // The delimiter lets the content hold `)"`; the literal ends only at `)x"`.
    const std::string custom = "auto a = R\"x(he said )\" and left)x\"; call_after_custom();\n";
    EXPECT_TRUE(has(code_only(custom), "call_after_custom()"));
    EXPECT_FALSE(has(code_only(custom), "he said"));
    EXPECT_TRUE(has(strip_cxx_source(custom, Literals::Keep), "he said )\" and left")) << "the literal view keeps the content";

    // Every spelling of the prefix, each with ONE quote in the content: read as an ordinary string the literal would end at that quote and
    // the next one would swallow the rest of the line, so `after_prefixed()` only survives if the prefix is recognised.
    for (const char* prefix : {"R", "u8R", "uR", "UR", "LR"}) {
        const std::string text = std::string("auto a = ") + prefix + "\"(a\"b)\"; after_prefixed();\n";
        EXPECT_TRUE(has(code_only(text), "after_prefixed()")) << prefix;
        EXPECT_FALSE(has(code_only(text), "a\"b")) << prefix;
    }
}

TEST(CxxSourceView, CommentMarkersAndEscapesInsideARawStringAreContentNotSyntax) {
    const std::string text =
        "auto a = R\"(// not a comment /* nor this \\ no escape \")\"; real_call();\n"
        "auto b = 1; /* a real comment */ still_code();\n";
    const std::string view = code_only(text);
    EXPECT_TRUE(has(view, "real_call()"));
    EXPECT_TRUE(has(view, "still_code()"));
    EXPECT_FALSE(has(view, "a real comment"));
    EXPECT_FALSE(has(view, "not a comment"));
}

TEST(CxxSourceView, ARawStringMayRunOverLinesAndTheLineStructureSurvives) {
    const std::string text = "auto j = R\"(\n{\"a\": 1,\n \"b\": 2}\n)\";\nvoid g() { after_multiline(); }\n";
    const std::string view = code_only(text);
    EXPECT_TRUE(has(view, "after_multiline()"));
    std::size_t newlines_in = 0;
    std::size_t newlines_out = 0;
    for (char c : text) newlines_in += c == '\n' ? 1u : 0u;
    for (char c : view) newlines_out += c == '\n' ? 1u : 0u;
    EXPECT_EQ(newlines_in, newlines_out) << "newlines inside the literal are kept, so line numbers still mean something";
}

TEST(CxxSourceView, AnIdentifierThatEndsInRIsNotARawStringPrefix) {
    // FOOR"..." is the identifier FOOR followed by an ordinary string: the string ends at its own closing quote.
    const std::string text = "auto a = FOOR\"x\"; visible_after();\n";
    EXPECT_TRUE(has(code_only(text), "visible_after()"));
    // and an ordinary string with an escaped quote is still read as an ordinary string
    const std::string escaped = "auto b = \"a\\\"b\"; visible_after_escaped();\n";
    EXPECT_TRUE(has(code_only(escaped), "visible_after_escaped()"));
}

TEST(CxxSourceView, AMalformedRawOpenerFallsBackToAnOrdinaryString) {
    // A delimiter longer than 16 characters is not a raw string opener: read as an ordinary string, which then ends at the next quote.
    const std::string text = "auto a = R\"0123456789abcdefg(x\"; after_bad_opener();\n";
    EXPECT_TRUE(has(code_only(text), "after_bad_opener()"));
}

TEST(CxxSourceView, AnUnterminatedRawStringEndsTheScanWithoutCrashing) {
    const std::string text = "before(); auto a = R\"(never closed ... \n more();\n";
    const std::string view = code_only(text);
    EXPECT_TRUE(has(view, "before()"));
    EXPECT_FALSE(has(view, "more()"));
}

TEST(CxxSourceView, TheOrdinaryBehaviourIsUnchanged) {
    const std::string text =
        "int a = 1'000; // call_in_comment();\n"
        "const char* s = \"call_in_string(); \\\" still string\"; char c = '\\'';\n"
        "/* call_in_block(); */ real_call(1'000);\n";
    const std::string view = code_only(text);
    EXPECT_FALSE(has(view, "call_in_comment"));
    EXPECT_FALSE(has(view, "call_in_string"));
    EXPECT_FALSE(has(view, "call_in_block"));
    EXPECT_TRUE(has(view, "real_call(1'000)"));
    EXPECT_EQ(view, legacy_strip(text, Literals::Blank));
    EXPECT_EQ(strip_cxx_source(text, Literals::Keep), legacy_strip(text, Literals::Keep));
}

// ---- (1) nothing but raw strings changed: the old and the shared scanner agree over the whole tree ---------------------------------

TEST(CxxSourceView, AgreesWithTheOldScannerOnEveryFileOfTheTreeThatHoldsNoRawString) {
    namespace fs = std::filesystem;
    const fs::path root(HY_NATIVE_SOURCE_DIR);
    std::size_t compared = 0;
    for (const char* dir : {"include", "src", "tests"}) {
        const fs::path base = root / dir;
        if (!fs::exists(base)) continue;
        for (const auto& entry : fs::recursive_directory_iterator(base)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".hpp" && ext != ".cpp" && ext != ".h") continue;
            std::ifstream in(entry.path(), std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            const std::string text = ss.str();
            if (text.find("R\"") != std::string::npos) continue;  // raw strings are where the two are SUPPOSED to differ
            ASSERT_EQ(strip_cxx_source(text, Literals::Blank), legacy_strip(text, Literals::Blank)) << entry.path().string();
            ASSERT_EQ(strip_cxx_source(text, Literals::Keep), legacy_strip(text, Literals::Keep)) << entry.path().string();
            ++compared;
        }
    }
    EXPECT_GT(compared, 200u) << "the tree walk found almost nothing: HY_NATIVE_SOURCE_DIR is wrong or the filter is";
}

TEST(CxxSourceView, TheTwoRawStringsInTheProductionTreeAreReadWholeAndTheirCodeSurvives) {
    namespace fs = std::filesystem;
    std::ifstream in(fs::path(HY_NATIVE_SOURCE_DIR) / "src" / "latency_bench.cpp", std::ios::binary);
    ASSERT_TRUE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const std::string view = code_only(text);
    // The file's own code after its raw string literals is still there.
    EXPECT_TRUE(has(view, "int main") || has(view, "main("));
    EXPECT_FALSE(has(view, "\"e\"")) << "the JSON examples inside the raw strings are blanked";
}
