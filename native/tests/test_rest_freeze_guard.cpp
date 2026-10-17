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
// Slice 5 does the same for two more bounds: the response size (the permit carries the parser limits of the kind
// of answer a request expects, and no REST client has a literal limit of its own) and the signing timestamp (a
// ClockReadyToken, issued by one function from a fresh clock snapshot, is the only thing build_signed_query()
// accepts).
//
// Slice 4 adds the same two mechanisms for the local rate-limit budget (spot_rate_limit_budget.hpp): the
// private client's three network coroutines also take a RateLimitPermit, so a request that nobody counted does
// not compile either; and the only two ways to make that permit without the tracker -- "the order
// orchestrator already counted this POST" and "this client has no tracker" -- have exactly one call site
// each, pinned below.
//
// Private-stream migration M2 does the same for the WebSocket API user-data session: its one connection point is pinned as a WsEndpointPermit
// session (host and frame cap out of the permit, no host member of its own), and the three steps that make a generation -- issue the endpoint
// permit, sign the subscribe request, hand it to the session -- each have exactly one production caller, the factory
// (binance_ws_api_session_factory.hpp), so a second path that connects without the allowlist, the budget or a fresh signature is a review item.
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
    // WebSocket API session (private-stream migration M2): takes a WsEndpointPermit and a RateLimitPermit instead of a host.
    WsEndpointPermit,
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
    {"include/hengyuan/binance_ws_api_user_data_session.hpp", 1, 1, Kind::WsEndpointPermit},
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

// --- slice 4: every request is counted against the budget, or says why it is not -------------------------

TEST(RestFreezeGuardBudget, EveryPrivateRestCoroutineNamesItsBudgetPermit) {
    // The compile-time pin lives in test_rest_config_surface.cpp; this is the textual cross-check, one budget
    // permit per connection the private client opens.
    const SourceFile* f = find_source("include/hengyuan/binance_private_rest.hpp");
    ASSERT_NE(f, nullptr);
    const Pinned* pinned = find_pinned(f->rel);
    ASSERT_NE(pinned, nullptr);
    EXPECT_GE(count_of(f->code, "RateLimitPermit budget"), pinned->resolves)
        << f->rel << ": fewer budget-taking coroutines than connections";
}

TEST(RestFreezeGuardBudget, TheTwoWaysToSkipTheLocalBudgetEachHaveExactlyOneCallSite) {
    // reserved_by_caller(): POST /api/v3/order, which the order orchestrator reserves under the Strategy lane
    // at Gate 12c before it reaches the submit port. unaccounted(): a client built without a tracker. A third
    // request path that reaches for either one is a request the budget never sees.
    constexpr std::string_view kOwner = "include/hengyuan/binance_private_rest.hpp";
    std::size_t unaccounted = 0;
    std::size_t by_caller = 0;
    for (const SourceFile& f : production_sources()) {
        const std::size_t u = count_of(f.code, "RateLimitPermit::unaccounted(");
        const std::size_t b = count_of(f.code, "RateLimitPermit::reserved_by_caller(");
        unaccounted += u;
        by_caller += b;
        if (f.rel != kOwner) {
            EXPECT_EQ(u, 0u) << f.rel << " makes an unaccounted permit: only " << kOwner << " may";
            EXPECT_EQ(b, 0u) << f.rel << " makes a reserved-by-caller permit: only " << kOwner << " may";
        }
    }
    EXPECT_EQ(unaccounted, 1u) << "RateLimitPermit::unaccounted( must be called from exactly one place";
    EXPECT_EQ(by_caller, 1u) << "RateLimitPermit::reserved_by_caller( must be called from exactly one place";
}

TEST(RestFreezeGuardBudget, TheNoTrackerPermitIsOnlyHandedOutByTheTwoReservationHelpers) {
    // With a tracker configured, every request is reserved through reserve_budget()/reserve_budget_explicit().
    // The no-tracker shortcut is reachable only from those two (plus its own definition); a method that calls
    // it directly would skip the accounting even when a tracker is there.
    const SourceFile* f = find_source("include/hengyuan/binance_private_rest.hpp");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(count_of(f->code, "no_tracker_permit("), 3u)
        << "one definition and the two reservation helpers: a new caller needs a reviewer";
}

// --- slice 5: nothing reads more than its permit allows, nothing signs without a fresh clock -------------

TEST(RestFreezeGuardLimits, EveryParserLimitInTheRestClientsComesOutOfThePermit) {
    // One parser per connection, each configured from the permit before the first byte is read. A literal here is
    // the pre-slice-5 shape; a limit anywhere outside the three pinned REST clients is a new HTTP client nobody has
    // put under the response bound yet.
    for (const SourceFile& f : production_sources()) {
        const std::size_t bodies = count_of(f.code, "body_limit(");
        const std::size_t headers = count_of(f.code, "header_limit(");
        const Pinned* pinned = find_pinned(f.rel);
        if (pinned == nullptr || pinned->kind != Kind::EndpointPermit) {
            EXPECT_EQ(bodies, 0u) << f.rel << " configures a parser body limit but is not a pinned REST client";
            EXPECT_EQ(headers, 0u) << f.rel << " configures a parser header limit but is not a pinned REST client";
            continue;
        }
        EXPECT_EQ(bodies, pinned->resolves) << f.rel << ": one body limit per connection";
        EXPECT_EQ(count_of(f.code, "body_limit(permit.max_body_bytes())"), pinned->resolves)
            << f.rel << ": a body limit that does not come out of the permit";
        EXPECT_EQ(headers, pinned->resolves) << f.rel << ": one header limit per connection";
        EXPECT_EQ(count_of(f.code, "header_limit(permit.max_header_bytes())"), pinned->resolves)
            << f.rel << ": a header limit that does not come out of the permit";
    }
}

TEST(RestFreezeGuardClockToken, TheSigningTimestampIsOnlyEverTakenThroughTheTokenIssuer) {
    // try_get_signing_timestamp_ms() has its definition and its one caller, issue_clock_ready_token(), in the clock
    // header -- nowhere else may take a raw signing timestamp out of the clock gate.
    constexpr std::string_view kOwner = "include/hengyuan/binance_clock_sync.hpp";
    std::size_t total = 0;
    for (const SourceFile& f : production_sources()) {
        const std::size_t n = count_of(f.code, "try_get_signing_timestamp_ms(");
        total += n;
        if (f.rel != kOwner) {
            EXPECT_EQ(n, 0u) << f.rel << " takes a raw signing timestamp: go through issue_clock_ready_token()";
        }
    }
    EXPECT_EQ(total, 2u) << "the definition and the one call inside issue_clock_ready_token()";
}

TEST(RestFreezeGuardClockToken, TheTokenIsMintedOnceAndItsTestHookIsNamedOnlyByTheFriendDeclaration) {
    constexpr std::string_view kOwner = "include/hengyuan/binance_clock_sync.hpp";
    for (const SourceFile& f : production_sources()) {
        const std::size_t minted = count_of(f.code, "ClockReadyToken(");
        const std::size_t hooks = count_of(f.code, "ClockReadyTokenTestHooks");
        if (f.rel == kOwner) {
            EXPECT_EQ(minted, 2u) << f.rel << ": the private constructor and the one mint in issue_clock_ready_token()";
            EXPECT_EQ(hooks, 1u) << f.rel << ": the friend declaration, and nothing else";
        } else {
            EXPECT_EQ(minted, 0u) << f.rel << " constructs a ClockReadyToken";
            EXPECT_EQ(hooks, 0u) << f.rel << " names ClockReadyTokenTestHooks: it may be defined only under native/tests/";
        }
    }
}

TEST(RestFreezeGuardClockToken, EveryTimestampASignedRequestIsSignedWithWasIssuedByTheClientItself) {
    // The private client signs in three places (account, order query, order submit); each issues its own token
    // just before. A fourth signing site that skipped the issue would not compile (build_signed_query() takes only a
    // token), and this keeps the two counts moving together.
    const SourceFile* f = find_source("include/hengyuan/binance_private_rest.hpp");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(count_of(f->code, "build_signed_query("), 3u);
    EXPECT_EQ(count_of(f->code, "issue_clock_ready_token("), 3u);
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

// --- private-stream migration M2: the WebSocket API session ------------------------------------------------

TEST(RestFreezeGuardWsApi, TheWsApiSessionsAreExactlyThePermitTakingOnes) {
    std::vector<std::string> sessions;
    for (const Pinned& p : kPinned) {
        if (p.kind == Kind::WsEndpointPermit) sessions.emplace_back(p.file);
    }
    EXPECT_EQ(sessions, (std::vector<std::string>{"include/hengyuan/binance_ws_api_user_data_session.hpp"}));
}

TEST(RestFreezeGuardWsApi, TheWsApiSessionTakesItsHostAndItsFrameCapOutOfThePermit) {
    for (const Pinned& p : kPinned) {
        if (p.kind != Kind::WsEndpointPermit) continue;
        const SourceFile* f = find_source(p.file);
        ASSERT_NE(f, nullptr) << p.file;
        EXPECT_GE(count_of(f->code, "WsEndpointPermit& permit"), p.resolves) << p.file << ": fewer permit-taking constructors than connections";
        EXPECT_EQ(count_of(f->code, "host_(permit.host())"), 1u) << p.file << ": the host must come out of the permit, once";
        EXPECT_EQ(count_of(f->code, "max_message_bytes_(permit.max_message_bytes())"), 1u)
            << p.file << ": the frame cap must come out of the permit, once";
        EXPECT_EQ(count_of(f->code, "configure_ws_stream("), 1u) << p.file << ": one stream configuration per connection";
        EXPECT_EQ(count_of(f->code, "static_cast<std::size_t>(max_message_bytes_)"), 2u)
            << p.file << ": the stream's read limit and the buffer reservation, and no literal";
        EXPECT_EQ(count_of(f->code, "std::string host;"), 0u) << p.file << ": a member that could name another host";
        EXPECT_EQ(count_of(f->code, "RateLimitPermit connect_permit,"), 1u) << p.file << ": the connection must be counted against the budget";
    }
}

TEST(RestFreezeGuardWsApi, EachStepThatMakesAGenerationHasExactlyOneProductionCaller) {
    constexpr std::string_view kFactory = "include/hengyuan/binance_ws_api_session_factory.hpp";
    struct Step {
        std::string_view symbol;
        std::string_view definition_file;  // where it is declared and defined (these files may name it more than once)
        std::size_t definition_mentions;
    };
    constexpr Step kSteps[] = {
        // the friend declaration and the definition
        {"issue_ws_endpoint_permit(", "include/hengyuan/transport_policy.hpp", 2},
        // the friend declaration inside WsApiSignedRequest and the definition
        {"build_subscribe_signature_request(", "include/hengyuan/binance_ws_api_signing.hpp", 2},
        // the definition only
        {"deposit_subscribe(", "include/hengyuan/binance_ws_api_user_data_session.hpp", 1},
    };
    for (const Step& step : kSteps) {
        for (const SourceFile& f : production_sources()) {
            const std::size_t n = count_of(f.code, step.symbol);
            if (f.rel == step.definition_file) {
                EXPECT_EQ(n, step.definition_mentions) << f.rel << ": " << step.symbol << " is declared/defined here and nowhere else in this file";
            } else if (f.rel == kFactory) {
                EXPECT_EQ(n, 1u) << f.rel << ": the factory is the one caller of " << step.symbol;
            } else {
                EXPECT_EQ(n, 0u) << f.rel << " names " << step.symbol << ": only " << kFactory
                                 << " may -- a generation that skips the allowlist, the budget or a fresh signature is a review item";
            }
        }
    }
}
