// SPDX-License-Identifier: proprietary
// test_listenkey_retirement_guard.cpp -- private-stream migration M5b-2: the listenKey stack stays retired.
//
// The exchange removed the REST listenKey endpoints (POST/PUT/DELETE /api/v3/userDataStream) and the WebSocket API methods userDataStream.start, .ping and
// .stop on 2026-02-20 (blueprint 5.7 P-07; the Testnet earlier, P-32). M5b-2 deleted everything in this tree that spoke them: the listenKey publisher, the
// keepalive scheduler, the listenKey WebSocket session and its supervisor, the three REST calls with their parser and constants, the H6 demo. A path that talked
// to those endpoints could only fail against the real exchange -- and could pass every unit test against a loopback server in the meantime -- so bringing one
// back is a review item, not a refactor. This test is where that review is forced.
//
// What it scans: the production tree (include/ and src/), in two views of every file.
//   * the CODE view: comments removed and the contents of string/char literals blanked, so an identifier is found where it is code and prose does not count;
//   * the LITERAL view: comments removed, string literals KEPT -- an endpoint path or a method name only ever appears inside a string literal.
// What it pins:
//   1. the retired files do not exist;
//   2. no identifier or include of the retired stack is left in the code view: anything with "listenkey" / "listen_key" in it (any case), and the names of the
//      listenKey session, its supervisor, its parser and their config/stat types;
//   3. no string literal names a retired endpoint or method, or the listenKey itself ("userDataStream.subscribe.signature" and "userDataStream.unsubscribe", the
//      live WebSocket API methods, are not matched);
//   4. the one surviving trace is the reserved enumerator UserDataEventKind::ListenKeyExpired = 3, exactly once, in binance_user_data_event.hpp: it keeps its
//      number so the kinds after it do not move, and nothing in production code produces or consumes it.
// The scanner has negative controls of its own below: a guard that silently finds nothing would be worse than none.

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

enum class Literals { Blank, Keep };

// Source text with comments removed; the contents of string and char literals either blanked (the CODE view) or kept (the LITERAL view).
std::string strip(const std::string& s, Literals literals) {
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
                } else if (c == '\'' && !out.empty() && (std::isalnum(static_cast<unsigned char>(out.back())) != 0 || out.back() == '_')) {
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
                    if (literals == Literals::Keep) {
                        out.push_back(c);
                        if (i + 1 < s.size()) out.push_back(s[i + 1]);
                    }
                    ++i;  // skip the escaped character
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

std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::size_t count_of(const std::string& hay, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// `word` as a whole identifier (not a piece of a longer one).
std::size_t count_word(const std::string& hay, std::string_view word) {
    std::size_t n = 0;
    for (std::size_t pos = hay.find(word); pos != std::string::npos; pos = hay.find(word, pos + word.size())) {
        const bool left_ok = pos == 0 || !is_ident_char(hay[pos - 1]);
        const std::size_t end = pos + word.size();
        const bool right_ok = end >= hay.size() || !is_ident_char(hay[end]);
        if (left_ok && right_ok) ++n;
    }
    return n;
}

constexpr const char* kEventHeader = "include/hengyuan/binance_user_data_event.hpp";
constexpr std::string_view kReservedEnumerator = "ListenKeyExpired = 3,";

// The retired files: gone for good.
const char* const kRetiredFiles[] = {
    "include/hengyuan/binance_listen_key_publisher.hpp",
    "include/hengyuan/binance_listen_key_keepalive.hpp",
    "include/hengyuan/binance_user_data_ws_session.hpp",
    "include/hengyuan/binance_user_data_ws_supervisor.hpp",
    "src/live_submit_real_credentials_demo.cpp",
};

// Whole identifiers of the retired stack that do not contain "listenkey" themselves.
const char* const kRetiredNames[] = {
    "BinanceUserDataWsSession", "UserDataWsSessionSupervisor", "UserDataWsReconnectPolicy", "UserDataWsSupervisorStats",
    "UserDataWsSessionConfig",  "UserDataWsSessionStats",      "UserDataJsonParser",        "UserDataParseResult",
    "ws_reconnect_backoff_delay_ms",
};

// Pieces of the retired headers' names, wherever they appear in the CODE view (an `#include <...>` is not inside quotes).
const char* const kRetiredPathPieces[] = {"binance_user_data_ws_session", "binance_user_data_ws_supervisor", "binance_listen_key"};

// In the LITERAL view: the retired REST path and WebSocket API methods, the listenKey itself, and quoted include paths of the retired headers.
const char* const kRetiredLiterals[] = {
    "/api/v3/userDataStream", "userDataStream.start", "userDataStream.ping", "userDataStream.stop", "listenKey",
    "binance_user_data_ws_session", "binance_user_data_ws_supervisor", "binance_listen_key",
};

// What the CODE view of `rel` still says about the retired stack. The reserved enumerator is exempt in its one home, once.
std::vector<std::string> retired_code_hits(const std::string& rel, std::string code) {
    std::vector<std::string> hits;
    if (rel == kEventHeader) {
        const std::size_t at = code.find(kReservedEnumerator);
        if (at != std::string::npos) code.erase(at, kReservedEnumerator.size());
    }
    const std::string lower = lowered(code);
    if (lower.find("listenkey") != std::string::npos || lower.find("listen_key") != std::string::npos) {
        hits.push_back(rel + ": an identifier with listenKey / listen_key in it");
    }
    for (const char* name : kRetiredNames) {
        if (count_word(code, name) != 0) hits.push_back(rel + ": " + name);
    }
    for (const char* piece : kRetiredPathPieces) {
        if (code.find(piece) != std::string::npos) hits.push_back(rel + ": " + piece);
    }
    return hits;
}

std::vector<std::string> retired_literal_hits(const std::string& rel, const std::string& literal_view) {
    std::vector<std::string> hits;
    for (const char* lit : kRetiredLiterals) {
        if (literal_view.find(lit) != std::string::npos) hits.push_back(rel + ": a string literal names " + lit);
    }
    return hits;
}

struct SourceFile {
    std::string rel;      // relative to native/, forward slashes
    std::string code;     // the CODE view
    std::string literal;  // the LITERAL view
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
                v.push_back({fs::relative(entry.path(), root).generic_string(), strip(text, Literals::Blank), strip(text, Literals::Keep)});
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

}  // namespace

// --- the scanner itself: a guard that silently counts nothing would be worse than none ---------------------------------------------------------

TEST(ListenKeyRetirementScanner, TheViewsDifferExactlyInWhatTheyKeep) {
    const std::string src =
        "int a = 1;  // create_listen_key in a line comment\n"
        "/* ListenKeyPublisher in a\n   block comment */\n"
        "const char* s = \"/api/v3/userDataStream?listenKey=\";\n"
        "char c = '\\'';\n"
        "int big = 1'000'000;\n";
    const std::string code = strip(src, Literals::Blank);
    const std::string lit = strip(src, Literals::Keep);
    EXPECT_EQ(code.find("create_listen_key"), std::string::npos) << "a comment is not code";
    EXPECT_EQ(code.find("ListenKeyPublisher"), std::string::npos) << "a block comment is not code";
    EXPECT_EQ(code.find("listenKey"), std::string::npos) << "the CODE view blanks a literal";
    EXPECT_NE(code.find("const char* s = \"\";"), std::string::npos);
    EXPECT_EQ(lit.find("create_listen_key"), std::string::npos);
    EXPECT_EQ(lit.find("ListenKeyPublisher"), std::string::npos);
    EXPECT_NE(lit.find("\"/api/v3/userDataStream?listenKey=\""), std::string::npos) << "the LITERAL view keeps it";
    EXPECT_NE(code.find("1'000'000"), std::string::npos) << "a digit separator must not open a char literal";
    EXPECT_NE(lit.find("1'000'000"), std::string::npos);
}

TEST(ListenKeyRetirementScanner, CodeHitsAreFoundWhereTheyAreCodeAndNowhereElse) {
    const auto hits = [](const std::string& src, const char* rel = "include/hengyuan/x.hpp") {
        return retired_code_hits(rel, strip(src, Literals::Blank));
    };
    EXPECT_FALSE(hits("inline int create_listen_key();").empty());
    EXPECT_FALSE(hits("inline int keepalive_listen_key(int);").empty());
    EXPECT_FALSE(hits("class ListenKeyPublisher {};").empty());
    EXPECT_FALSE(hits("constexpr int kListenKeyLen = 128;").empty());
    EXPECT_FALSE(hits("class BinanceUserDataWsSession {};").empty());
    EXPECT_FALSE(hits("UserDataJsonParser parser;").empty());
    EXPECT_FALSE(hits("#include <hengyuan/binance_user_data_ws_session.hpp>\n").empty());
    EXPECT_FALSE(hits("#include <hengyuan/binance_listen_key_publisher.hpp>\n").empty());
    EXPECT_TRUE(hits("// create_listen_key and ListenKeyPublisher, in prose\n").empty());
    EXPECT_TRUE(hits("const char* s = \"create_listen_key\";").empty()) << "a literal is the other view's business";
    EXPECT_TRUE(hits("class BinanceWsApiUserDataSession {};").empty()) << "the live session is not the retired one";
    EXPECT_TRUE(hits("#include <hengyuan/binance_ws_api_user_data_session.hpp>\n").empty());
    EXPECT_TRUE(hits("class MyUserDataJsonParserLike {};").empty()) << "a whole identifier, not a piece of a longer one";
    EXPECT_TRUE(hits("class MyUserDataJsonParser {};").empty()) << "a longer identifier on the left";
    EXPECT_TRUE(hits("class UserDataJsonParserLike {};").empty()) << "a longer identifier on the right";
}

TEST(ListenKeyRetirementScanner, LiteralHitsNameTheRetiredEndpointsAndNotTheLiveOnes) {
    const auto hits = [](const std::string& src) { return retired_literal_hits("include/hengyuan/x.hpp", strip(src, Literals::Keep)); };
    EXPECT_FALSE(hits("target = \"/api/v3/userDataStream\";").empty());
    EXPECT_FALSE(hits("target = \"/api/v3/userDataStream?listenKey=\";").empty());
    EXPECT_FALSE(hits("method = \"userDataStream.start\";").empty());
    EXPECT_FALSE(hits("method = \"userDataStream.ping\";").empty());
    EXPECT_FALSE(hits("method = \"userDataStream.stop\";").empty());
    EXPECT_FALSE(hits("auto k = doc[\"listenKey\"];").empty());
    EXPECT_FALSE(hits("#include \"hengyuan/binance_user_data_ws_session.hpp\"\n").empty());
    EXPECT_TRUE(hits("method = \"userDataStream.subscribe.signature\";").empty()) << "the live signed subscription";
    EXPECT_TRUE(hits("method = \"userDataStream.unsubscribe\";").empty());
    EXPECT_TRUE(hits("method = \"session.subscriptions\";").empty());
    EXPECT_TRUE(hits("target = \"/api/v3/account\";").empty());
    EXPECT_TRUE(hits("// target = \"/api/v3/userDataStream\"; in a comment\n").empty());
}

TEST(ListenKeyRetirementScanner, TheReservedEnumeratorIsExemptOnlyInItsHomeAndOnlyOnce) {
    const std::string def = strip("    ListenKeyExpired = 3,  // reserved\n", Literals::Blank);
    EXPECT_TRUE(retired_code_hits(kEventHeader, def).empty()) << "the enumerator, in the event header";
    EXPECT_FALSE(retired_code_hits("include/hengyuan/other.hpp", def).empty()) << "anywhere else it is a hit";
    EXPECT_FALSE(retired_code_hits(kEventHeader, def + "    case UserDataEventKind::ListenKeyExpired: break;\n").empty())
        << "a use of it is a hit even in the header";
    EXPECT_FALSE(retired_code_hits(kEventHeader, def + def).empty()) << "a second definition is a hit";
    EXPECT_FALSE(retired_code_hits(kEventHeader, strip("    ListenKeyExpired = 4,\n", Literals::Blank)).empty()) << "it keeps its number";
}

// --- the production tree ----------------------------------------------------------------------------------------------------------------

TEST(ListenKeyRetirementGuard, TheScanSeesRealSources) {
    // A guard that scans an empty tree passes for the wrong reason.
    EXPECT_GT(production_sources().size(), 50u) << "HY_NATIVE_SOURCE_DIR=" << HY_NATIVE_SOURCE_DIR;
    EXPECT_NE(find_source("include/hengyuan/binance_private_rest.hpp"), nullptr);
    EXPECT_NE(find_source("include/hengyuan/binance_ws_api_user_data_session.hpp"), nullptr);
    EXPECT_NE(find_source(kEventHeader), nullptr);
    EXPECT_NE(find_source("src/live_submit_preflight_harness.cpp"), nullptr);
    // The two views are wired the right way round: the live WebSocket API method name is a string literal, so only the LITERAL view has it.
    const SourceFile* signing = find_source("include/hengyuan/binance_ws_api_signing.hpp");
    ASSERT_NE(signing, nullptr);
    EXPECT_NE(signing->literal.find("userDataStream.subscribe.signature"), std::string::npos) << "the LITERAL view keeps string literals";
    EXPECT_EQ(signing->code.find("userDataStream.subscribe.signature"), std::string::npos) << "the CODE view blanks them";
}

TEST(ListenKeyRetirementGuard, TheRetiredFilesDoNotExist) {
    const fs::path root(HY_NATIVE_SOURCE_DIR);
    for (const char* rel : kRetiredFiles) {
        EXPECT_FALSE(fs::exists(root / rel)) << rel << " is back: the listenKey stack was retired in M5b-2 (the exchange removed its endpoints on 2026-02-20)";
    }
}

TEST(ListenKeyRetirementGuard, NoRetiredNameOrEndpointIsLeftInProductionCode) {
    std::vector<std::string> problems;
    for (const SourceFile& f : production_sources()) {
        for (std::string& s : retired_code_hits(f.rel, f.code)) problems.push_back(std::move(s));
        for (std::string& s : retired_literal_hits(f.rel, f.literal)) problems.push_back(std::move(s));
    }
    for (const std::string& s : problems) ADD_FAILURE() << s;
    EXPECT_TRUE(problems.empty());
}

TEST(ListenKeyRetirementGuard, TheOneSurvivorIsTheReservedEnumeratorWithItsNumber) {
    std::size_t mentions = 0;
    for (const SourceFile& f : production_sources()) {
        const std::size_t n = count_of(f.code, "ListenKeyExpired");
        mentions += n;
        if (n != 0) {
            EXPECT_EQ(f.rel, kEventHeader) << "the reserved enumerator is named outside its one home: something produces or consumes it";
        }
    }
    EXPECT_EQ(mentions, 1u) << "exactly one: the enumerator's definition";
    const SourceFile* header = find_source(kEventHeader);
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(count_of(header->code, kReservedEnumerator), 1u) << "it keeps its number (3), so the kinds after it do not move";
}
