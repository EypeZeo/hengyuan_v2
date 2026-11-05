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
//          exist -- may be used only by the listed drills and harnesses, each of which says so in its banner; a new use is a review item.
//          (The preflight harness left the list in M5b-1: it builds no context by name any more, it takes the pipeline's make_context(), whose
//          readiness is the real one.)
//        * SubmitReadyToken has exactly one mint site (inside SubmitReadiness::authorize) and exactly one friend;
//        * every production call of drain_user_data_events() is pinned with its argument count, and for a call that passes a barrier, the
//          exact text of that argument: a call that leaves it out still compiles and quietly merges events of a generation the sweep
//          already settled, and never learns that a serverShutdown ended its generation. The memory-only calls are on the retired listenKey
//          stream, which stamps no generation and has no serverShutdown; M5b-2 removes them together with that stream.
//        * (D5 slice R4) the same pin carries the late-fill alarm: the orchestrator's call passes ctx.late_fill as its eighth argument, the
//          listenKey calls pass none (their harnesses open a scratch store that no operator has resolved anything in), the alarm's hook in the
//          drain sits ahead of the barrier's stale-generation filter, and only the private-stream pipeline builds a LateFillAlarm or assigns
//          .late_fill -- any other construction is a review item.
//        * (M5b-1) which production files so much as name a context's `confirmation` or `submit_port` is a short reviewed list, and the
//          preflight harness -- whose whole point is that it can never submit -- names neither: its confirmation is reset, and its port a mock,
//          inside fill_preflight_order() (preflight_probe.hpp, tested in test_preflight_probe.cpp); the only submit port it ever hands to that
//          function is the mock, and the real port it assembles for its diagnostic goes nowhere else.
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

// Every production place that builds an OrchestratorContext, and what it gives it as a readiness. Two have no private stream (a drill rig and a
// mock-submit dry run) and say so in their banner or comment; the third is the private-stream pipeline (M5a-2b), whose readiness is the one it owns,
// built over the real supervisor, barrier, recovery and account path. The preflight harness (M5b-1) is not here on purpose: it builds no context by name,
// it takes the pipeline's make_context() (`auto`) and fills the order into it.
const ContextPin kContexts[] = {
    {"include/hengyuan/verified_dry_run_evidence.hpp", 1, kSyntheticRhs,
     "DrillRig: the verified dry-run drills run with no private stream; the readiness is the synthetic one (real barrier + startup recovery)"},
    {"src/live_submit_evidence_harness.cpp", 1, kSyntheticRhs,
     "the evidence harness is a mock-submit dry run; its banner says the readiness is synthetic"},
    {"include/hengyuan/private_stream_pipeline.hpp", 1, "&hot_.readiness",
     "the pipeline's wiring context: the one readiness the pipeline owns (its barrier is fed by the real supervisor, its recovery by the live parts); make_context() "
     "returns a copy of it with `auto`, so the type is named once"},
};

// The only files allowed to use SyntheticReadiness (by name or by including its header).
const char* const kSyntheticUsers[] = {
    "include/hengyuan/verified_dry_run_evidence.hpp",
    "src/live_submit_evidence_harness.cpp",
};

struct DrainPin {
    const char* file;
    std::size_t calls;
    std::size_t args;
    const char* barrier_arg;    // the exact text of the seventh argument; "" for a call that passes none
    const char* late_fill_arg;  // the exact text of the eighth argument (D5 slice R4); "" for a call that passes none
    const char* durable_arg;    // the exact text of the ninth argument (private-stream migration M4); "" for a call that passes none
    const char* why;
};

constexpr std::size_t kDurableArgCount = 9;
constexpr std::size_t kLateFillArgCount = 8;
constexpr std::size_t kBarrierArgCount = 7;
constexpr std::size_t kNoBarrierArgCount = 6;

// Every production call of drain_user_data_events(), and what kind it is.
const DrainPin kUserDataDrains[] = {
    {"include/hengyuan/live_submit_orchestrator.hpp", 1, kDurableArgCount, "ctx.readiness != nullptr ? &ctx.readiness->barrier() : nullptr",
     "ctx.late_fill", "ctx.user_data_durable",
     "orchestrate_submit(): the barrier of the readiness Gate 2b consults (no readiness: no barrier, and Gate 2b refuses anyway), the late-fill "
     "alarm of the context (unset: the drain ignores a report about a written-off order, as before R4), and the durable publisher of the context "
     "(unset: the memory-first drain, acceptable only where no durable sink exists; a process that has one wires it -- M4)"},
    {"src/live_submit_real_credentials_demo.cpp", 3, kNoBarrierArgCount, "", "", "",
     "NO BARRIER, NO ALARM, MEMORY-FIRST: the listenKey stream stamps no generation and has no serverShutdown; the SubmitPort is assembled but "
     "never invoked; there is no durable store at all"},
    {"src/live_submit_reconcile_harness_demo.cpp", 2, kNoBarrierArgCount, "", "", "",
     "NO BARRIER, NO ALARM, MEMORY-FIRST: the listenKey stream stamps no generation and has no serverShutdown; nothing is ever submitted; there is "
     "no durable store at all"},
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
    EXPECT_EQ(first_call("drain_user_data_events(a, b, c, d, e, f, g, ctx.late_fill);").size(), 8u);
    EXPECT_EQ(first_call("drain_user_data_events(a, b, c, d, e, f, g, ctx.late_fill);").back(), "ctx.late_fill");
    EXPECT_EQ(first_call("drain_user_data_events(a, b, c, d, e, f, g, nullptr);").back(), "nullptr")
        << "an explicit nullptr is eight arguments, and visibly not an alarm";
    EXPECT_EQ(first_call("drain_user_data_events(a, b, c, d, e, f, g, h, ctx.user_data_durable);").size(), 9u);
    EXPECT_EQ(first_call("drain_user_data_events(a, b, c, d, e, f, g, h, ctx.user_data_durable);").back(), "ctx.user_data_durable");
    EXPECT_EQ(first_call("drain_user_data_events(a, b, c, d, e, f, g, h, nullptr);").back(), "nullptr")
        << "an explicit nullptr is nine arguments, and visibly not a publisher";
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
                               "stream must pass the ReconnectBarrier, the late-fill alarm and the durable publisher (9 arguments) or be "
                               "pinned, with the reason it needs none of them");
            continue;
        }
        if (calls.size() != pin->calls) {
            problems.push_back(f.rel + ": expected " + std::to_string(pin->calls) + " call(s), found " + std::to_string(calls.size()));
        }
        for (const std::vector<std::string>& args : calls) {
            if (args.size() != pin->args) {
                problems.push_back(f.rel + ": a call has " + std::to_string(args.size()) + " arguments, expected " + std::to_string(pin->args) +
                                   " (" + pin->why + ")");
                continue;
            }
            if (pin->args >= kBarrierArgCount && args[kBarrierArgCount - 1] != pin->barrier_arg) {
                problems.push_back(f.rel + ": the seventh argument is `" + args[kBarrierArgCount - 1] + "`, expected `" + pin->barrier_arg +
                                   "` -- anything else (an explicit nullptr above all) is a drain with no barrier in disguise");
            }
            if (pin->args >= kLateFillArgCount && args[kLateFillArgCount - 1] != pin->late_fill_arg) {
                problems.push_back(f.rel + ": the eighth argument is `" + args[kLateFillArgCount - 1] + "`, expected `" + pin->late_fill_arg +
                                   "` -- anything else (an explicit nullptr above all) is a drain with no late-fill alarm in disguise");
            }
            if (pin->args >= kDurableArgCount && args[kDurableArgCount - 1] != pin->durable_arg) {
                problems.push_back(f.rel + ": the ninth argument is `" + args[kDurableArgCount - 1] + "`, expected `" + pin->durable_arg +
                                   "` -- anything else (an explicit nullptr above all) is a memory-first drain in disguise: a report would reach "
                                   "PositionTruth before its frame is durable");
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

// --- D5 slice R4: the late-fill alarm ------------------------------------------------------------------------------------------------------
//
// Behaviour is test_late_fill_alarm.cpp (the table, the trip, the audit, the drain, the recovery round trip) and test_live_submit_orchestrator.cpp
// (the very call that drains the report is refused). What behaviour cannot see is the order of two statements and the absence of a user.

TEST(SubmitReadinessGuard, TheLateFillAlarmSeesAnEventBeforeTheBarrierCanDropIt) {
    // The barrier drops an event of an older generation because "the sweep already reflects whatever it said". For an order nothing sweeps -- a
    // written-off one -- that reason is false, so the alarm's hook must come first; behaviour pins it too (AnOlderGenerationsReport...), this is the twin.
    const SourceFile* f = find_source("include/hengyuan/binance_user_data_event.hpp");
    ASSERT_NE(f, nullptr);
    const std::size_t alarm = f->code.find("late_fill->on_execution_report(");
    const std::size_t stale_filter = f->code.find("barrier->admit_event(");
    ASSERT_NE(alarm, std::string::npos);
    ASSERT_NE(stale_filter, std::string::npos);
    EXPECT_LT(alarm, stale_filter);
    EXPECT_EQ(count_substring(f->code, "late_fill->on_execution_report("), 1u) << "one hook";
    EXPECT_EQ(count_substring(f->code, "barrier->admit_event("), 1u);
    // ... and it asks the registry itself: a watched id that is in flight is a tracked order, not a written-off one.
    const std::size_t asks_registry = f->code.find("!in_flight.is_in_flight(ev.coid.view())", f->code.find("late_fill->watches("));
    ASSERT_NE(asks_registry, std::string::npos);
    EXPECT_LT(asks_registry, alarm);
}

TEST(SubmitReadinessGuard, OnlyThePipelineBuildsALateFillAlarmAndWiresItIntoAContext) {
    // The alarm is real and tested; the private-stream pipeline (M5a-2b) is the first production code that builds one, and it builds exactly one, over the list the
    // recovered store holds and the ONE KillSwitch the pipeline's contexts carry. A second construction, or a context that gets an alarm from anywhere else,
    // is a review item and not a side effect. The two other OrchestratorContexts of kContexts stay unwired for the same reason as their drains: scratch stores.
    constexpr const char* kPipeline = "include/hengyuan/private_stream_pipeline.hpp";
    std::vector<std::string> problems;
    for (const SourceFile& f : production_sources()) {
        if (f.rel == "include/hengyuan/late_fill_alarm.hpp") continue;  // the definition
        const std::size_t built = constructions_of(f.code, "LateFillAlarm");
        const std::vector<std::string> wired = member_assignments(f.code, "late_fill");
        if (f.rel == kPipeline) {
            if (built != 1) problems.push_back(f.rel + ": " + std::to_string(built) + " LateFillAlarm construction(s), expected the one member");
            if (wired != std::vector<std::string>{"&hot_.late_fill"}) problems.push_back(f.rel + ": .late_fill must be assigned `&hot_.late_fill` exactly once");
            continue;
        }
        if (built != 0) {
            problems.push_back(f.rel + ": builds " + std::to_string(built) +
                               " LateFillAlarm(s); pin it (with the store whose list feeds it and the KillSwitch it trips) or remove it");
        }
        if (!wired.empty()) {
            problems.push_back(f.rel + ": assigns .late_fill " + std::to_string(wired.size()) +
                               " time(s); pin the context (and say which alarm it gets) or remove it");
        }
    }
    // The pipeline's alarm: the recovered store's own list and total, and the process's one breaker -- the same object the context hands to Gate 2.
    const SourceFile* pipeline = find_source(kPipeline);
    if (pipeline == nullptr) {
        problems.push_back(std::string(kPipeline) + ": not found");
    } else {
        std::string squeezed;
        for (const char c : pipeline->code) {
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t') squeezed += c;
        }
        const auto has = [&](std::string_view needle) { return squeezed.find(needle) != std::string::npos; };
        if (!has("late_fill(parts.sink.recovered_operator_resolved(),parts.sink.recovered_operator_resolved_total(),parts.kill_switch)")) {
            problems.push_back("the pipeline's LateFillAlarm must be built from the sink's recovered_operator_resolved() list and total and parts.kill_switch");
        }
        if (!has("context_.kill_switch=&parts_.kill_switch;")) problems.push_back("the pipeline's contexts must carry the same KillSwitch the alarm trips");
    }
    for (const std::string& s : problems) ADD_FAILURE() << s;
    EXPECT_TRUE(problems.empty());

    // The negative controls of this check, on text with known answers.
    const auto stripped = [](const char* t) { return code_only(t); };
    EXPECT_EQ(constructions_of(stripped("LateFillAlarm alarm(list, list.size(), ks);"), "LateFillAlarm"), 1u);
    EXPECT_EQ(constructions_of(stripped("auto a = std::make_unique<LateFillAlarm>(l, n, ks);"), "LateFillAlarm"), 1u);
    EXPECT_EQ(constructions_of(stripped("LateFillAlarm* late_fill{nullptr};"), "LateFillAlarm"), 0u) << "a pointer declarator";
    EXPECT_EQ(constructions_of(stripped("void f(LateFillAlarm& a);"), "LateFillAlarm"), 0u) << "a reference";
    EXPECT_EQ(member_assignments(stripped("ctx.late_fill = &alarm;"), "late_fill"), (std::vector<std::string>{"&alarm"}));
    EXPECT_TRUE(member_assignments(stripped("late_fill->on_execution_report(a, b, c, d);"), "late_fill").empty()) << "a call through it";
    EXPECT_TRUE(member_assignments(stripped("if (ctx.late_fill == nullptr) {}"), "late_fill").empty()) << "a comparison";
}

// --- M5b-1: the confirmation and the submit port of a context -----------------------------------------------------------------------------------
//
// An order gets past the OperatorNotConfirmed gate only with a bound confirmation, and then goes to the context's submit port. Which production files so much as
// name `ctx.confirmation` / `ctx.submit_port` is therefore a short list somebody reviewed: the orchestrator that reads them, the preflight harness's one fill
// function (which resets the first to "not confirmed" and installs a mock for the second -- test_preflight_probe.cpp), and the two mock-submit dry-run rigs that
// bind a confirmation to a scratch port on purpose. The preflight harness itself names neither: that its context can never submit is the fill function's property,
// pinned in its own tests, and the harness's only way to hand it a port is the mock (pinned below).

namespace {

struct OrderGatePin {
    const char* file;
    std::size_t confirmation;  // occurrences of `ctx.confirmation` in the code of the file
    std::size_t submit_port;   // occurrences of `ctx.submit_port`
    const char* why;
};

const OrderGatePin kOrderGates[] = {
    {"include/hengyuan/live_submit_orchestrator.hpp", 2, 3, "orchestrate_submit(): reads the confirmation (Gate 11) and the port (the rules-version fence, Gate 12, the call)"},
    {"include/hengyuan/preflight_probe.hpp", 1, 1,
     "fill_preflight_order(): resets the confirmation to the default (not confirmed) and installs the mock port it is handed"},
    {"include/hengyuan/verified_dry_run_evidence.hpp", 8, 1,
     "DrillRig: the dry-run drills bind a confirmation to the drill's own scratch port, on a scratch store"},
    {"src/live_submit_evidence_harness.cpp", 8, 1, "the evidence harness: a mock-submit dry run that binds a confirmation to its mock port on purpose"},
};

}  // namespace

TEST(SubmitReadinessGuard, OnlyTheListedFilesNameAContextsConfirmationOrSubmitPort) {
    std::vector<std::string> problems;
    for (const SourceFile& f : production_sources()) {
        const std::size_t confirmations = count_substring(f.code, "ctx.confirmation");
        const std::size_t ports = count_substring(f.code, "ctx.submit_port");
        if (confirmations == 0 && ports == 0) continue;
        const OrderGatePin* pin = nullptr;
        for (const OrderGatePin& p : kOrderGates) {
            if (f.rel == p.file) pin = &p;
        }
        if (pin == nullptr) {
            problems.push_back(f.rel + ": names ctx.confirmation " + std::to_string(confirmations) + "x and ctx.submit_port " + std::to_string(ports) +
                               "x but is not pinned in kOrderGates -- a place that binds a confirmation or installs a submit port is a review item, "
                               "and the preflight harness must never be one");
            continue;
        }
        if (confirmations != pin->confirmation || ports != pin->submit_port) {
            problems.push_back(f.rel + ": ctx.confirmation " + std::to_string(confirmations) + "x / ctx.submit_port " + std::to_string(ports) +
                               "x, expected " + std::to_string(pin->confirmation) + "x / " + std::to_string(pin->submit_port) + "x (" + pin->why + ")");
        }
    }
    for (const OrderGatePin& p : kOrderGates) {
        if (find_source(p.file) == nullptr) problems.push_back(std::string(p.file) + ": pinned in kOrderGates but not found");
    }
    for (const std::string& s : problems) ADD_FAILURE() << s;
    EXPECT_TRUE(problems.empty());
}

TEST(SubmitReadinessGuard, ThePreflightHarnessCanOnlyHandItsOrderFillTheMockPort) {
    const SourceFile* f = find_source("src/live_submit_preflight_harness.cpp");
    ASSERT_NE(f, nullptr);
    using Rhs = std::vector<std::string>;
    EXPECT_EQ(member_assignments(f->code, "submit_port"), (Rhs{"{mock_submit_must_never_be_called, nullptr, mock_current_rules_version}"}))
        << "the only submit port the harness assigns anywhere is the mock (to the environment of the fill, never to a context)";
    EXPECT_EQ(count_substring(f->code, "fill_preflight_order("), 1u) << "the one place the harness puts an order into a context";
    EXPECT_EQ(count_substring(f->code, "orchestrate_submit("), 1u) << "...and the one place it asks the chain";
    EXPECT_EQ(count_substring(f->code, "real_submit_port_unused"), 3u)
        << "the real port is assembled for a diagnostic: its declaration and the two reads that print it, nothing else";
    EXPECT_EQ(count_substring(f->code, "confirmed = true"), 0u);
    EXPECT_EQ(count_substring(f->code, "confirmed=true"), 0u);
    EXPECT_EQ(count_substring(f->code, "OrderConfirmation"), 0u) << "the harness builds no confirmation at all";

    // ...and it moves the private stream only through the pipeline's two ticks: no hand-made drain, poll or supervisor poll of its own (a second poller over a
    // second tracker, or a memory-first drain, is the very drift the pipeline exists to prevent).
    for (const char* forbidden : {"poll_once(", "drain_hot_inputs(", "drain_reconcile_events(", "drain_user_data_events(", "supervisor().poll(", "ws_supervisor"}) {
        EXPECT_EQ(count_substring(f->code, forbidden), 0u) << "the preflight harness must not call " << forbidden << " itself";
    }
    EXPECT_EQ(count_substring(f->code, "reconcile_tick("), 1u) << "the one loop calls the reconcile half once per cycle";
    EXPECT_EQ(count_substring(f->code, "hot_tick("), 2u) << "...the hot half once per cycle, and once more after the sessions are gone";

    // The negative controls of this check, on text with known answers.
    const auto stripped = [](const char* t) { return code_only(t); };
    EXPECT_EQ(member_assignments(stripped("env.submit_port = {mock, nullptr, version};"), "submit_port"), (Rhs{"{mock, nullptr, version}"}));
    EXPECT_EQ(member_assignments(stripped("ctx.submit_port = real_port;"), "submit_port"), (Rhs{"real_port"})) << "a real port assigned anywhere is visible";
    EXPECT_EQ(count_substring(stripped("// ctx.confirmation.confirmed = true;\n"), "ctx.confirmation"), 0u) << "prose";
}
