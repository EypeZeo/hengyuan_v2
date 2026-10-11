// SPDX-License-Identifier: proprietary
// test_rest_freeze_guard.cpp -- SAFE-01 freeze guard (audit P1-001 / P2-001; fault cases FI-032, FI-037).
//
// "Adding any REST request method must come with proof that it passes the endpoint check first."
// Two mechanisms carry that proof:
//   1. the type system: the network coroutines of every REST client (the private client, the depth
//      snapshot fetcher, the klines fetcher) take an EndpointPermit (transport_policy.hpp) instead of a
//      host, so a request path without the check does not compile (tests/test_rest_config_surface.cpp
//      pins those signatures, and that no REST config can name a host any more);
//   2. this test: every place under include/ and src/ that opens a connection is pinned in kPinned.
//      A new one, or a pinned one that gains or loses a connection, fails the test until a reviewer has
//      looked at it and updated the table -- and the table says what kind of entry it has to be.
//
// It also pins the containment of the test seam (rest_test_seam.hpp): the builder that can fill a
// RestTestSeam is named only by that header's friend declaration, and nothing under include/ or src/ may
// declare a connect-target override or an extra-trust-anchor member again.
//
// The scan strips comments and the contents of string/char literals first, so prose that mentions a
// primitive does not count and a string that contains one cannot hide a real use.

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

const SourceFile* find_source(std::string_view rel) {
    for (const SourceFile& f : production_sources()) {
        if (f.rel == rel) return &f;
    }
    return nullptr;
}

enum class Kind {
    // REST path whose coroutines take an EndpointPermit: the check is a compile-time property.
    EndpointPermit,
    // WebSocket session: connects to the binding's stream host, a separate mechanism from REST policy.
    WebSocket,
};

struct Pinned {
    const char* file;
    std::size_t resolves;  // async_resolve( call sites
    std::size_t connects;  // async_connect( call sites
    Kind kind;
};

// Every file under include/ and src/ that resolves a name or opens a socket. To change a row you must
// have looked at the connection it adds, moves or removes -- which, for a REST path, means giving it an
// EndpointPermit (transport_policy.hpp) before it resolves anything.
constexpr Pinned kPinned[] = {
    {"include/hengyuan/binance_private_rest.hpp", 3, 3, Kind::EndpointPermit},
    {"include/hengyuan/binance_rest_snapshot.hpp", 1, 1, Kind::EndpointPermit},
    {"include/hengyuan/binance_klines_rest.hpp", 1, 1, Kind::EndpointPermit},
    {"include/hengyuan/binance_ws_session.hpp", 1, 1, Kind::WebSocket},
    {"include/hengyuan/binance_kline_ws_session.hpp", 1, 1, Kind::WebSocket},
    {"include/hengyuan/binance_user_data_ws_session.hpp", 1, 1, Kind::WebSocket},
};

const Pinned* find_pinned(std::string_view rel) {
    for (const Pinned& p : kPinned) {
        if (rel == p.file) return &p;
    }
    return nullptr;
}

constexpr std::string_view kResolve = "async_resolve(";
constexpr std::string_view kConnect = "async_connect(";

}  // namespace

// --- the scanner itself: a guard that silently counts nothing would be worse than none ---------------

TEST(RestFreezeGuardScanner, IgnoresCommentsAndLiteralsButSeesRealCode) {
    const std::string src =
        "// async_resolve( in a line comment\n"
        "/* async_connect( in a\n   block comment */\n"
        "const char* s = \"async_resolve( in a string\";\n"
        "char c = '\\'';\n"
        "int big = 1'000'000;\n"
        "resolver.async_resolve(host, port, use_awaitable);\n"
        "stream.async_connect(results, use_awaitable);\n";
    const std::string code = code_only(src);
    EXPECT_EQ(count_of(code, kResolve), 1u);
    EXPECT_EQ(count_of(code, kConnect), 1u);
    EXPECT_NE(code.find("1'000'000"), std::string::npos) << "a digit separator must not open a char literal";
}

TEST(RestFreezeGuardScanner, FindsTheProductionSourceTree) {
    EXPECT_FALSE(production_sources().empty()) << "HY_NATIVE_SOURCE_DIR=" << HY_NATIVE_SOURCE_DIR;
    std::size_t resolves = 0;
    for (const SourceFile& f : production_sources()) resolves += count_of(f.code, kResolve);
    EXPECT_GT(resolves, 0u) << "the scan found no connection at all: the guard would pass vacuously";
}

// --- every connection is pinned -----------------------------------------------------------------------

TEST(RestFreezeGuard, EveryConnectionUnderIncludeAndSrcIsPinnedWithItsExactCount) {
    for (const SourceFile& f : production_sources()) {
        const std::size_t resolves = count_of(f.code, kResolve);
        const std::size_t connects = count_of(f.code, kConnect);
        if (resolves == 0 && connects == 0) continue;
        const Pinned* pinned = find_pinned(f.rel);
        if (pinned == nullptr) {
            ADD_FAILURE() << f.rel << " opens a connection (" << resolves << " x async_resolve, " << connects
                          << " x async_connect) and is not pinned. A new REST request path must take an "
                             "EndpointPermit (transport_policy.hpp) before it resolves anything; add it to kPinned "
                             "only after that is true and a reviewer has looked at it.";
            continue;
        }
        EXPECT_EQ(resolves, pinned->resolves) << f.rel << ": the number of async_resolve call sites changed";
        EXPECT_EQ(connects, pinned->connects) << f.rel << ": the number of async_connect call sites changed";
    }
}

TEST(RestFreezeGuard, NoOtherWayToResolveANameOrOpenASocketExistsUnderIncludeAndSrc) {
    // The two pinned primitives are how every connection here is made today. A client written with the
    // synchronous API (or the C library) would not show up in the counts above, so those spellings must
    // not exist at all until a reviewer adds a row for them.
    constexpr std::string_view kOtherPrimitives[] = {
        ".resolve(", "->resolve(", "net::connect(", "asio::connect(",
        "::connect(", "getaddrinfo", "gethostbyname", "WSAConnect",
    };
    for (const SourceFile& f : production_sources()) {
        for (const std::string_view primitive : kOtherPrimitives) {
            EXPECT_EQ(count_of(f.code, primitive), 0u)
                << f.rel << " uses '" << primitive
                << "': a connection opened outside the pinned async_resolve/async_connect sites";
        }
    }
}

TEST(RestFreezeGuard, EveryPinnedFileStillExists) {
    for (const Pinned& p : kPinned) {
        EXPECT_NE(find_source(p.file), nullptr) << p.file << " was pinned but is gone: renamed or deleted?";
    }
}

TEST(RestFreezeGuard, EveryRestCoroutineNamesItsPermit) {
    // The compile-time pin lives in test_rest_config_surface.cpp; this is the textual cross-check that
    // each of the file's connections is opened by a coroutine that takes the permit as its first parameter.
    for (const Pinned& p : kPinned) {
        if (p.kind != Kind::EndpointPermit) continue;
        const SourceFile* f = find_source(p.file);
        ASSERT_NE(f, nullptr) << p.file;
        EXPECT_GE(count_of(f->code, "EndpointPermit permit"), p.resolves)
            << p.file << ": fewer permit-taking coroutines than connections";
    }
}

TEST(RestFreezeGuard, TheRestClientsAreExactlyThePermitTakingOnes) {
    // Every REST client takes its host and policy from an EnvironmentBinding and its coroutines take the
    // permit. A fourth REST client has to be added here on purpose (and arrive with its permit), and a
    // client must not drop out of this list while it still opens connections.
    std::vector<std::string> rest;
    for (const Pinned& p : kPinned) {
        if (p.kind == Kind::EndpointPermit) rest.emplace_back(p.file);
    }
    EXPECT_EQ(rest, (std::vector<std::string>{"include/hengyuan/binance_private_rest.hpp",
                                               "include/hengyuan/binance_rest_snapshot.hpp",
                                               "include/hengyuan/binance_klines_rest.hpp"}));
}

// --- the test seam stays a test seam -----------------------------------------------------------------

TEST(RestFreezeGuardSeam, TheBuilderIsOnlyEverNamedByTheSeamHeadersFriendDeclaration) {
    for (const SourceFile& f : production_sources()) {
        const std::size_t n = count_of(f.code, "RestTestSeamBuilder");
        if (f.rel == "include/hengyuan/rest_test_seam.hpp") {
            EXPECT_EQ(n, 1u) << f.rel << " must name the builder exactly once: its friend declaration";
        } else {
            EXPECT_EQ(n, 0u) << f.rel << " names RestTestSeamBuilder: the class that can fill a RestTestSeam "
                                         "may be defined only under native/tests/";
        }
    }
}

TEST(RestFreezeGuardSeam, NoProductionTypeDeclaresAConnectTargetOrATrustAnchorMemberAgain) {
    for (const SourceFile& f : production_sources()) {
        EXPECT_EQ(count_of(f.code, "std::string connect_host_override"), 0u) << f.rel;
        EXPECT_EQ(count_of(f.code, "std::string extra_trusted_ca_pem_path"), 0u) << f.rel;
    }
}
