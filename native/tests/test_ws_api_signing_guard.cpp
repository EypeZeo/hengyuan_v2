// SPDX-License-Identifier: proprietary
// test_ws_api_signing_guard.cpp -- private-stream migration M1: the one place a WebSocket API request is signed stays the one place.
//
// detail::build_ws_api_signed_request() (binance_ws_api_signing.hpp) signs a frame for ANY well-formed method name, because the official
// signing examples ("order.place") have to be checked against it. That generality is deliberate, and it is also why a second caller would
// matter: the day something outside build_subscribe_signature_request() calls it, a signed WS API request exists that nobody typed. So every
// mention of it under include/ and src/ is pinned here -- the declarations, the two friend declarations that let it reach the credentials' private
// sign(), the definition, and its two calls inside build_subscribe_signature_request(). A new caller, or a moved one, fails this test until a
// reviewer has looked at what it signs and has updated the table.
//
// The scan strips comments and the contents of string/char literals first (same scanner as test_rest_freeze_guard.cpp), so prose that mentions
// the function does not count and a string that contains it cannot hide a real use.

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
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

std::size_t count_of(const std::string& hay, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
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

constexpr std::string_view kSigner = "build_ws_api_signed_request";

struct Pinned {
    const char* file;
    std::size_t mentions;
};

// Every file under include/ and src/ that names the signer, and how many times:
//   binance_ws_api_signing.hpp   the declaration in detail::, the friend declaration in WsApiSignedRequest, the definition, and the two calls
//                                inside build_subscribe_signature_request() (with and without recvWindow);
//   binance_environment.hpp      the forward declaration and the friend declaration that give it BoundHmacCredentials::sign().
constexpr Pinned kPinned[] = {
    {"include/hengyuan/binance_ws_api_signing.hpp", 5},
    {"include/hengyuan/binance_environment.hpp", 2},
};

}  // namespace

// --- the scanner itself: a guard that silently counts nothing would be worse than none ---------------

TEST(WsApiSigningGuardScanner, IgnoresCommentsAndLiteralsButSeesRealCode) {
    const std::string src =
        "// build_ws_api_signed_request in a line comment\n"
        "/* build_ws_api_signed_request in a\n   block comment */\n"
        "const char* s = \"build_ws_api_signed_request in a string\";\n"
        "char c = '\\'';\n"
        "int big = 1'000'000;\n"
        "auto r = detail::build_ws_api_signed_request(creds, m, id, p, t);\n";
    EXPECT_EQ(count_of(code_only(src), kSigner), 1u);
}

TEST(WsApiSigningGuardScanner, ReadsTheProductionTreeAndFindsTheSigner) {
    // If the tree could not be read, every count below would be 0 and the pin could never fail.
    ASSERT_GT(production_sources().size(), 50u) << "HY_NATIVE_SOURCE_DIR=" << HY_NATIVE_SOURCE_DIR;
    std::size_t total = 0;
    for (const SourceFile& f : production_sources()) total += count_of(f.code, kSigner);
    EXPECT_GT(total, 0u);
}

// --- the pin ---------------------------------------------------------------------------------------------

TEST(WsApiSigningGuard, TheSignerIsNamedInExactlyTheFilesAndTheNumbersPinned) {
    for (const SourceFile& f : production_sources()) {
        std::size_t expected = 0;
        for (const Pinned& p : kPinned) {
            if (f.rel == p.file) expected = p.mentions;
        }
        EXPECT_EQ(count_of(f.code, kSigner), expected)
            << f.rel << ": detail::build_ws_api_signed_request signs a frame for any method name; a new or moved caller has to be looked at "
                        "and the table in test_ws_api_signing_guard.cpp updated";
    }
    for (const Pinned& p : kPinned) {
        bool found = false;
        for (const SourceFile& f : production_sources()) {
            if (f.rel == p.file) found = true;
        }
        EXPECT_TRUE(found) << p.file << " is pinned but no longer exists";
    }
}

TEST(WsApiSigningGuard, OnlyTheTypedSubscribeEntryPointCallsTheSignerInsideItsHeader) {
    // Inside binance_ws_api_signing.hpp the two calls must both be in build_subscribe_signature_request(): from the start of that function's
    // definition to the end of the file the signer is named exactly twice (its two calls), and before it exactly three times (the declaration, the
    // friend declaration and the signer's own definition).
    const std::string* code = nullptr;
    for (const SourceFile& f : production_sources()) {
        if (f.rel == "include/hengyuan/binance_ws_api_signing.hpp") code = &f.code;
    }
    ASSERT_NE(code, nullptr);
    const std::size_t entry = code->find("inline std::pair<WsApiSigningError, WsApiSignedRequest> build_subscribe_signature_request(");
    ASSERT_NE(entry, std::string::npos) << "the typed entry point's definition moved or was renamed";
    EXPECT_EQ(count_of(code->substr(entry), kSigner), 2u) << "both calls live in the typed entry point";
    EXPECT_EQ(count_of(code->substr(0, entry), kSigner), 3u) << "before it: the declaration, the friend declaration and the definition";
}
