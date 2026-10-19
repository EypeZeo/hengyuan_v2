// SPDX-License-Identifier: proprietary
// test_submit_readiness_guard.cpp -- private-stream migration M3a guard (design 5.4; fault cases FI-009, FI-011, FI-020, FI-043).
//
// "No order leaves this process without a SubmitReadyToken" is carried by two things:
//   1. behaviour: test_live_submit_orchestrator.cpp (Gate 2b: where it sits, what a refusal leaves behind, that an unwired readiness is a
//      refusal), test_submit_readiness.cpp (each condition, the priority of the reasons, a real supervisor end to end) and
//      test_reconnect_barrier.cpp (the state machine);
//   2. this test, for what behaviour cannot see. OrchestratorContext::readiness defaults to nullptr and an unset one is a REFUSAL -- the
//      reverse of every nullable field around it -- so a call site that builds a context and forgets it is not a silent pass-through, it is
//      an order that can never go out; still, "never goes out" in a harness that is meant to send one is a bug somebody should see at review
//      time and not at the first run, and "goes out because somebody wired a readiness that is not the real thing" is worse. So:
//        * every place under include/ and src/ that CONSTRUCTS an OrchestratorContext is pinned with its count and the exact text it assigns
//          to .readiness (an explicit `nullptr` there would be a context that can never submit; anything but the pinned text is a readiness
//          nobody reviewed);
//        * SyntheticReadiness -- the real barrier and startup recovery driven by scripted observations, a stand-in for a stream that does not
//          exist -- may be used only by the listed drills and harnesses, each of which says so in its banner; a new use is a review item;
//        * SubmitReadyToken has exactly one mint site (inside SubmitReadiness::authorize) and exactly one friend;
//        * every production call of drain_user_data_events() is pinned with its argument count, and for a call that passes a barrier, the
//          exact text of that argument: a call that leaves it out still compiles and quietly merges events of a generation the sweep
//          already settled, and never learns that a serverShutdown ended its generation. The memory-only calls are on the retired listenKey
//          stream, which stamps no generation and has no serverShutdown; M5 removes them together with that stream.
//
// The scan strips comments and the contents of string/char literals first, so prose that mentions a call does not count and a string that
// contains one cannot hide a real use. (The stripping helpers are copies of the ones in test_reconcile_durable_guard.cpp; the guards share no
// header on purpose, so neither can break the other.)

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
    // Leading/trailing whitespace dropped, every inner run of whitespace one space: the text of an argument, independent of how it was wrapped.
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

// The word that ends right before `pos` (skipping whitespace), or "" if the previous significant character is not an identifier character.
std::string_view previous_word(const std::string& code, std::size_t pos) {
    std::size_t before = pos;
    while (before > 0 && std::isspace(static_cast<unsigned char>(code[before - 1])) != 0) --before;
    if (before == 0 || !is_ident_char(code[before - 1])) return {};
    std::size_t word_start = before;
    while (word_start > 0 && is_ident_char(code[word_start - 1])) --word_start;
    return std::string_view(code.data() + word_start, before - word_start);
}

// The arguments (as squeezed text) of every CALL of `name` in `code`. A definition or declaration -- the name directly preceded by a type
// name, as in `inline void name(` -- is not a call; a call is preceded by a statement boundary, `->`, `.`, `::`, an operator, or one of the
// words `return`, `else`, `do`.
std::vector<std::vector<std::string>> call_args(const std::string& code, std::string_view name) {
    std::vector<std::vector<std::string>> calls;
    for (std::size_t pos = code.find(name); pos != std::string::npos; pos = code.find(name, pos + name.size())) {
        if (pos > 0 && is_ident_char(code[pos - 1])) continue;  // part of a longer identifier
        std::size_t open = pos + name.size();
        while (open < code.size() && std::isspace(static_cast<unsigned char>(code[open])) != 0) ++open;
        if (open >= code.size() || code[open] != '(') continue;  // not a call expression

        const std::string_view word = previous_word(code, pos);
        if (!word.empty() && word != "return" && word != "else" && word != "do") continue;  // a definition

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

// How many times `type` is CONSTRUCTED in `code`: every use of the identifier except its definition (`struct T {`), a reference or pointer
// declarator (`T& c`, `const T* p`) and a qualified name (`T::member`). A variable (`T ctx{};`), a temporary (`T{}`), a template argument
// (`std::make_unique<T>()`) all count.
std::size_t constructions_of(const std::string& code, std::string_view type) {
    std::size_t n = 0;
    for (std::size_t pos = code.find(type); pos != std::string::npos; pos = code.find(type, pos + type.size())) {
        if (pos > 0 && is_ident_char(code[pos - 1])) continue;
        const std::size_t end = pos + type.size();
        if (end < code.size() && is_ident_char(code[end])) continue;
        const std::string_view word = previous_word(code, pos);
        if (word == "struct" || word == "class") continue;  // the definition (or a forward declaration)
        std::size_t next = end;
        while (next < code.size() && std::isspace(static_cast<unsigned char>(code[next])) != 0) ++next;
        if (next < code.size() && (code[next] == '&' || code[next] == '*')) continue;
        if (next + 1 < code.size() && code[next] == ':' && code[next + 1] == ':') continue;
        ++n;
    }
    return n;
}

// The right-hand side (squeezed) of every ASSIGNMENT to a member named `member` through `.` or `->`: `x.member = rhs;`. A comparison
// (`==`, `!=`), a call (`x.member->f()`), a declaration (`T* member{nullptr};`) and a longer identifier are not assignments.
std::vector<std::string> member_assignments(const std::string& code, std::string_view member) {
    std::vector<std::string> out;
    for (std::size_t pos = code.find(member); pos != std::string::npos; pos = code.find(member, pos + member.size())) {
        if (pos > 0 && is_ident_char(code[pos - 1])) continue;
        const std::size_t end = pos + member.size();
        if (end < code.size() && is_ident_char(code[end])) continue;
        const bool through_dot = pos >= 1 && code[pos - 1] == '.';
        const bool through_arrow = pos >= 2 && code[pos - 1] == '>' && code[pos - 2] == '-';
        if (!through_dot && !through_arrow) continue;
        std::size_t eq = end;
        while (eq < code.size() && std::isspace(static_cast<unsigned char>(code[eq])) != 0) ++eq;
        if (eq >= code.size() || code[eq] != '=') continue;
        if (eq + 1 < code.size() && code[eq + 1] == '=') continue;  // ==
        std::string rhs;
        int depth = 0;
        for (std::size_t i = eq + 1; i < code.size(); ++i) {
            const char c = code[i];
            if (c == '(' || c == '[' || c == '{') ++depth;
            if (c == ')' || c == ']' || c == '}') --depth;
            if (c == ';' && depth <= 0) break;
            rhs.push_back(c);
        }
        out.push_back(squeeze(rhs));
    }
    return out;
}

std::size_t count_substring(const std::string& haystack, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + needle.size())) ++n;
    return n;
}

struct SourceFile {
    std::string rel;   // relative to native/, forward slashes
    std::string raw;   // the file as it is
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
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::string code = code_only(text);
                v.push_back({fs::relative(entry.path(), root).generic_string(), std::move(text), std::move(code)});
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

// --- the pins ------------------------------------------------------------------------------------------------------------------------

struct ContextPin {
    const char* file;
    std::size_t contexts;       // constructions of OrchestratorContext in the file
    const char* readiness_rhs;  // the exact text assigned to .readiness at each of them
    const char* why;
};

constexpr const char* kSyntheticRhs = "&synthetic_readiness.readiness()";

// Every production place that builds an OrchestratorContext, and what it gives it as a readiness. All three have no private stream (the
// listenKey one is retired and was never wired to the orchestrator's barrier) and say so in their banner or comment.
const ContextPin kContexts[] = {
    {"include/hengyuan/verified_dry_run_evidence.hpp", 1, kSyntheticRhs,
     "DrillRig: the verified dry-run drills run with no private stream; the readiness is the synthetic one (real barrier + startup recovery)"},
    {"src/live_submit_evidence_harness.cpp", 1, kSyntheticRhs,
     "the evidence harness is a mock-submit dry run; its banner says the readiness is synthetic"},
    {"src/live_submit_preflight_harness.cpp", 1, kSyntheticRhs,
     "the preflight harness still runs on the retired listenKey stream; its banner says the readiness is synthetic and M5 wires the real one"},
};

// The only files allowed to use SyntheticReadiness (by name or by including its header).
const char* const kSyntheticUsers[] = {
    "include/hengyuan/verified_dry_run_evidence.hpp",
    "src/live_submit_evidence_harness.cpp",
    "src/live_submit_preflight_harness.cpp",
};

struct DrainPin {
    const char* file;
    std::size_t calls;
    std::size_t args;
    const char* barrier_arg;  // the exact text of the seventh argument; "" for a call that passes none
    const char* why;
};

constexpr std::size_t kBarrierArgCount = 7;
constexpr std::size_t kNoBarrierArgCount = 6;

// Every production call of drain_user_data_events(), and what kind it is.
const DrainPin kUserDataDrains[] = {
    {"include/hengyuan/live_submit_orchestrator.hpp", 1, kBarrierArgCount, "ctx.readiness != nullptr ? &ctx.readiness->barrier() : nullptr",
     "orchestrate_submit(): the barrier of the readiness Gate 2b consults (no readiness: no barrier, and Gate 2b refuses anyway)"},
    {"src/live_submit_preflight_harness.cpp", 3, kNoBarrierArgCount, "",
     "NO BARRIER: the listenKey stream stamps no generation (0 = always merged) and has no serverShutdown; retired with that stream in M5"},
    {"src/live_submit_real_credentials_demo.cpp", 3, kNoBarrierArgCount, "",
     "NO BARRIER: the listenKey stream stamps no generation and has no serverShutdown; the SubmitPort is assembled but never invoked"},
    {"src/live_submit_reconcile_harness_demo.cpp", 2, kNoBarrierArgCount, "",
     "NO BARRIER: the listenKey stream stamps no generation and has no serverShutdown; nothing is ever submitted"},
};

}  // namespace

// --- the scan itself ---------------------------------------------------------------------------------------------------------------------

TEST(SubmitReadinessGuard, TheScanSeesRealSources) {
    // A guard that scans an empty tree passes for the wrong reason.
    EXPECT_GT(production_sources().size(), 50u);
    EXPECT_NE(find_source("include/hengyuan/submit_readiness.hpp"), nullptr);
    EXPECT_NE(find_source("include/hengyuan/live_submit_orchestrator.hpp"), nullptr);
    EXPECT_NE(find_source("include/hengyuan/reconnect_barrier.hpp"), nullptr);
}

TEST(SubmitReadinessGuard, TheExtractorsKnowWhatTheyAreLookingFor) {
    // The negative controls of the guard itself: the same helpers the pins use, on text with known answers.
    const auto stripped = [](const char* t) { return code_only(t); };

    EXPECT_EQ(constructions_of(stripped("OrchestratorContext ctx{};"), "OrchestratorContext"), 1u);
    EXPECT_EQ(constructions_of(stripped("auto p = std::make_unique<OrchestratorContext>();"), "OrchestratorContext"), 1u);
    EXPECT_EQ(constructions_of(stripped("use(OrchestratorContext{});"), "OrchestratorContext"), 1u);
    EXPECT_EQ(constructions_of(stripped("OrchestratorContext a{}; OrchestratorContext b{};"), "OrchestratorContext"), 2u);
    EXPECT_EQ(constructions_of(stripped("struct OrchestratorContext {\n int x;\n};"), "OrchestratorContext"), 0u) << "the definition";
    EXPECT_EQ(constructions_of(stripped("OrchestratorResult f(OrchestratorContext& ctx);"), "OrchestratorContext"), 0u) << "a reference";
    EXPECT_EQ(constructions_of(stripped("void g(const OrchestratorContext* ctx);"), "OrchestratorContext"), 0u) << "a pointer";
    EXPECT_EQ(constructions_of(stripped("auto x = OrchestratorContext::kThing;"), "OrchestratorContext"), 0u) << "a qualified name";
    EXPECT_EQ(constructions_of(stripped("MyOrchestratorContext x; OrchestratorContextX y;"), "OrchestratorContext"), 0u) << "longer identifiers";
    EXPECT_EQ(constructions_of(stripped("// OrchestratorContext ctx{};\nconst char* s = \"OrchestratorContext ctx{};\";"), "OrchestratorContext"), 0u)
        << "prose and string literals";

    using Rhs = std::vector<std::string>;
    EXPECT_EQ(member_assignments(stripped("ctx.readiness = &r.readiness();"), "readiness"), (Rhs{"&r.readiness()"}));
    EXPECT_EQ(member_assignments(stripped("ctx.readiness = nullptr;"), "readiness"), (Rhs{"nullptr"})) << "an explicit nullptr is seen as such";
    EXPECT_EQ(member_assignments(stripped("p->readiness =\n   &x . readiness ( ) ;"), "readiness"), (Rhs{"&x . readiness ( )"}));
    EXPECT_EQ(member_assignments(stripped("a.readiness = f(1, 2); b.readiness = &c;"), "readiness"), (Rhs{"f(1, 2)", "&c"}));
    EXPECT_TRUE(member_assignments(stripped("if (ctx.readiness == nullptr) {}"), "readiness").empty()) << "a comparison";
    EXPECT_TRUE(member_assignments(stripped("if (ctx.readiness != nullptr) {}"), "readiness").empty()) << "a comparison";
    EXPECT_TRUE(member_assignments(stripped("ctx.readiness->authorize(now);"), "readiness").empty()) << "a call through it";
    EXPECT_TRUE(member_assignments(stripped("SubmitReadiness* readiness{nullptr};"), "readiness").empty()) << "a member declaration";
    EXPECT_TRUE(member_assignments(stripped("x.readiness_ = 1; y.my_readiness = 2;"), "readiness").empty()) << "longer identifiers";
    EXPECT_TRUE(member_assignments(stripped("// ctx.readiness = nullptr;\n"), "readiness").empty()) << "prose";

    const std::string name = "drain_user_data_events";
    using Args = std::vector<std::string>;
    const auto first_call = [&](const char* text) {
        const auto calls = call_args(code_only(text), name);
        return calls.empty() ? Args{"<no call found>"} : calls[0];
    };
    EXPECT_EQ(first_call("void f() { drain_user_data_events(a, &b, c, d, &e, &f, g); }"), (Args{"a", "&b", "c", "d", "&e", "&f", "g"}));
    EXPECT_EQ(first_call("drain_user_data_events(a, &b, c, d, &e, &f);").size(), 6u);
    EXPECT_EQ(first_call("drain_user_data_events(a, b, c, d, e, f, nullptr);").back(), "nullptr")
        << "an explicit nullptr is seven arguments, and visibly not a barrier";
    EXPECT_EQ(first_call("drain_user_data_events(a, b, c, d, e, f, x ? &y->z() : nullptr);").back(), "x ? &y->z() : nullptr");
    EXPECT_TRUE(call_args(code_only("inline void drain_user_data_events(A& a, B* b) noexcept {}"), name).empty()) << "a definition";
    EXPECT_TRUE(call_args(code_only("// drain_user_data_events(a, b)\n"), name).empty()) << "prose";
    EXPECT_EQ(count_substring("abcabcab", "abc"), 2u);
}

// --- every OrchestratorContext sets a reviewed readiness --------------------------------------------------------------------------------

TEST(SubmitReadinessGuard, EveryProductionOrchestratorContextIsPinnedWithItsReadiness) {
    std::vector<std::string> problems;
    for (const SourceFile& f : production_sources()) {
        const std::size_t built = constructions_of(f.code, "OrchestratorContext");
        if (built == 0) continue;

        const ContextPin* pin = nullptr;
        for (const ContextPin& p : kContexts) {
            if (f.rel == p.file) pin = &p;
        }
        if (pin == nullptr) {
            problems.push_back(f.rel + ": builds " + std::to_string(built) +
                               " OrchestratorContext(s) that are not pinned in kContexts -- a new one must set .readiness to a reviewed "
                               "readiness (never left unset: an unset readiness refuses every order) and be pinned, with the reason");
            continue;
        }
        if (built != pin->contexts) {
            problems.push_back(f.rel + ": expected " + std::to_string(pin->contexts) + " OrchestratorContext(s), found " + std::to_string(built));
        }
        const std::vector<std::string> rhs = member_assignments(f.code, "readiness");
        if (rhs.size() != pin->contexts) {
            problems.push_back(f.rel + ": " + std::to_string(rhs.size()) + " assignment(s) to .readiness for " +
                               std::to_string(pin->contexts) + " context(s) (" + pin->why + ")");
        }
        for (const std::string& text : rhs) {
            if (text != pin->readiness_rhs) {
                problems.push_back(f.rel + ": .readiness is assigned `" + text + "`, expected `" + pin->readiness_rhs +
                                   "` -- anything else (an explicit nullptr above all) is a context that can never submit, or a readiness "
                                   "nobody reviewed");
            }
        }
    }
    // ...and every pin must still be there (a deleted construction site is a change somebody should look at too).
    for (const ContextPin& p : kContexts) {
        const SourceFile* f = find_source(p.file);
        if (f == nullptr || constructions_of(f->code, "OrchestratorContext") == 0) {
            problems.push_back(std::string(p.file) + ": pinned in kContexts but builds no OrchestratorContext");
        }
    }
    for (const std::string& s : problems) ADD_FAILURE() << s;
    EXPECT_TRUE(problems.empty());
}

// --- the synthetic readiness has a short list of users ------------------------------------------------------------------------------------

TEST(SubmitReadinessGuard, SyntheticReadinessIsUsedOnlyByTheListedDrillsAndHarnesses) {
    std::vector<std::string> problems;
    const auto mentions_synthetic = [](const SourceFile& f) {
        if (constructions_of(f.code, "SyntheticReadiness") != 0) return true;
        // the include line itself (a quoted include is blanked in `code`, so look at the raw text)
        std::size_t pos = 0;
        while ((pos = f.raw.find("synthetic_readiness.hpp", pos)) != std::string::npos) {
            const std::size_t line_start = f.raw.rfind('\n', pos);
            const std::size_t from = line_start == std::string::npos ? 0 : line_start + 1;
            std::string_view line(f.raw.data() + from, pos - from);
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())) != 0) line.remove_prefix(1);
            if (line.rfind("#include", 0) == 0) return true;
            pos += 1;
        }
        return false;
    };
    for (const SourceFile& f : production_sources()) {
        if (f.rel == "include/hengyuan/synthetic_readiness.hpp") continue;  // the definition
        if (!mentions_synthetic(f)) continue;
        bool listed = false;
        for (const char* user : kSyntheticUsers) listed = listed || f.rel == user;
        if (!listed) {
            problems.push_back(f.rel + ": uses SyntheticReadiness but is not in kSyntheticUsers -- it claims a private stream that does not "
                               "exist; only a drill or a harness whose banner says so may, and a new one is a review item");
        }
    }
    for (const char* user : kSyntheticUsers) {
        const SourceFile* f = find_source(user);
        if (f == nullptr || !mentions_synthetic(*f)) problems.push_back(std::string(user) + ": listed in kSyntheticUsers but does not use it");
    }
    for (const std::string& s : problems) ADD_FAILURE() << s;
    EXPECT_TRUE(problems.empty());
}

// --- one mint site for the token -------------------------------------------------------------------------------------------------------------

TEST(SubmitReadinessGuard, TheTokenHasOneMintSiteAndOneFriend) {
    const SourceFile* header = find_source("include/hengyuan/submit_readiness.hpp");
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(count_substring(header->code, "SubmitReadyToken(feed.generation, now_ms)"), 1u)
        << "exactly one expression builds a token, inside SubmitReadiness::authorize()";
    // ... and no other two-argument construction (the private constructor takes the generation and the issue time) and no brace construction.
    std::size_t two_argument_constructions = 0;
    for (const std::vector<std::string>& args : call_args(header->code, "SubmitReadyToken")) {
        if (args.size() == 2) ++two_argument_constructions;
    }
    EXPECT_EQ(two_argument_constructions, 1u);
    EXPECT_EQ(count_substring(header->code, "SubmitReadyToken{"), 0u);
    EXPECT_EQ(count_substring(header->code, "friend"), 1u);
    EXPECT_EQ(count_substring(header->code, "friend class SubmitReadiness;"), 1u) << "the one friend is the issuer";

    for (const SourceFile& f : production_sources()) {
        if (f.rel == header->rel) continue;
        EXPECT_TRUE(call_args(f.code, "SubmitReadyToken").empty())
            << f.rel << " builds or declares a SubmitReadyToken by call syntax; the only issuer is SubmitReadiness::authorize()";
        EXPECT_EQ(count_substring(f.code, "friend class SubmitReadyToken"), 0u) << f.rel;
        EXPECT_EQ(count_substring(f.code, "SubmitReadyToken{"), 0u) << f.rel << " brace-constructs a SubmitReadyToken";
    }
}

// --- the user-data drain is wired to the barrier where there is one ---------------------------------------------------------------------------

TEST(SubmitReadinessGuard, EveryProductionUserDataDrainIsPinnedWithItsBarrierArgument) {
    std::vector<std::string> problems;
    for (const SourceFile& f : production_sources()) {
        const std::vector<std::vector<std::string>> calls = call_args(f.code, "drain_user_data_events");
        if (calls.empty()) continue;

        const DrainPin* pin = nullptr;
        for (const DrainPin& p : kUserDataDrains) {
            if (f.rel == p.file) pin = &p;
        }
        if (pin == nullptr) {
            problems.push_back(f.rel + ": " + std::to_string(calls.size()) +
                               " call(s) to drain_user_data_events() that are not pinned in kUserDataDrains -- a call on the WebSocket API "
                               "stream must pass the ReconnectBarrier (7 arguments) or be pinned, with the reason it needs none");
            continue;
        }
        if (calls.size() != pin->calls) {
            problems.push_back(f.rel + ": expected " + std::to_string(pin->calls) + " call(s), found " + std::to_string(calls.size()));
        }
        for (const std::vector<std::string>& args : calls) {
            if (args.size() != pin->args) {
                problems.push_back(f.rel + ": a call has " + std::to_string(args.size()) + " arguments, expected " + std::to_string(pin->args) +
                                   " (" + pin->why + ")");
            } else if (pin->args == kBarrierArgCount && args.back() != pin->barrier_arg) {
                problems.push_back(f.rel + ": the seventh argument is `" + args.back() + "`, expected `" + pin->barrier_arg +
                                   "` -- anything else (an explicit nullptr above all) is a drain with no barrier in disguise");
            }
        }
    }
    for (const DrainPin& p : kUserDataDrains) {
        const SourceFile* f = find_source(p.file);
        if (f == nullptr || call_args(f->code, "drain_user_data_events").empty()) {
            problems.push_back(std::string(p.file) + ": pinned in kUserDataDrains but no call found");
        }
    }
    for (const std::string& s : problems) ADD_FAILURE() << s;
    EXPECT_TRUE(problems.empty());
}

TEST(SubmitReadinessGuard, TheOrchestratorOrdersGate2bAfterTheKillSwitchAndBeforeTheEvidence) {
    // Behaviour pins the same order (KillSwitchCheckedBeforeReadiness / ReadinessCheckedBeforeEvidence in test_live_submit_orchestrator.cpp);
    // this is the cheap textual twin: the three gate checks appear in orchestrate_submit() in this order, once each.
    const SourceFile* f = find_source("include/hengyuan/live_submit_orchestrator.hpp");
    ASSERT_NE(f, nullptr);
    const std::size_t kill = f->code.find("OrchestratorGate::KillSwitchNotNormal;");
    const std::size_t ready = f->code.find("OrchestratorGate::NotReady;");
    const std::size_t evidence = f->code.find("OrchestratorGate::DryRunEvidenceIncomplete;");
    ASSERT_NE(kill, std::string::npos);
    ASSERT_NE(ready, std::string::npos);
    ASSERT_NE(evidence, std::string::npos);
    EXPECT_LT(kill, ready);
    EXPECT_LT(ready, evidence);
    EXPECT_EQ(count_substring(f->code, "OrchestratorGate::NotReady;"), 1u) << "one place assigns the gate";
}
