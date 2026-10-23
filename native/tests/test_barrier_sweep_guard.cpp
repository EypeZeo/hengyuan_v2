// SPDX-License-Identifier: proprietary
// test_barrier_sweep_guard.cpp -- private-stream migration M3b guard (design 5.3; docs/SPEC_INVARIANTS.md; formal/reconnect_sweep.tla).
//
// The barrier sweep is correct because of a few orderings and a few single points. The behaviour is pinned by test_barrier_sweep.cpp,
// test_order_tracker_sweep.cpp, test_sweep_orchestrator.cpp and test_sweep_concurrency.cpp; the TLA+ model proves the orderings are what the two claims
// rest on and that each, turned off, breaks one. This test pins what behaviour cannot see, in the SOURCE:
//
//   1. poll_once() TAKES the sweep request before it DRAINS the ingress ring. Swap the two and every behavioural test still passes (single-threaded, both
//      orders see the same rings); the model's reconnect_sweep_drain_first_bug.cfg fails, and a real race loses an order that preceded the request.
//   2. A SweepDone marker is made in exactly one place (poll_once()), delivered in exactly one place (drain_reconcile_events()), and only the pump turns
//      one into a barrier report: on_sweep_report() has two production callers -- the pump and the synthetic readiness' scripted exchange.
//   3. A sweep request is made and pushed in exactly one place (the pump), and taken from the ring in exactly one (SweepJob).
//   4. Every production drain_reconcile_events() call is pinned with its argument count, and the one that serves a barrier (the orchestrator) with the
//      exact text of its sink argument: a call that leaves the sink out still compiles and quietly drops every marker, so the barrier never completes.
//   5. No production code runs poll_once() with a sweep job yet (M5 wires the live process and pins its own call).
//
// The scan strips comments and the contents of string/char literals first, so prose that mentions a call does not count and a string that contains one
// cannot hide a real use. (The helpers are copies of the ones in test_submit_readiness_guard.cpp; the guards share no header on purpose, so neither can
// break the other.)

#include <gtest/gtest.h>

#include <cctype>
#include <algorithm>
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

// Does a function body follow the parameter list that closes at `close`? A definition has its body right after the list, possibly behind const /
// noexcept / override / final / volatile; a call is followed by `;`, `)`, `,`, `.` or an operator, never by a brace.
bool followed_by_body(const std::string& code, std::size_t close) {
    std::size_t i = close + 1;
    for (bool advanced = true; advanced;) {
        advanced = false;
        while (i < code.size() && std::isspace(static_cast<unsigned char>(code[i])) != 0) ++i;
        for (const std::string_view qualifier : {"const", "noexcept", "override", "final", "volatile"}) {
            if (code.compare(i, qualifier.size(), qualifier) == 0 &&
                (i + qualifier.size() >= code.size() || !is_ident_char(code[i + qualifier.size()]))) {
                i += qualifier.size();
                advanced = true;
                break;
            }
        }
    }
    return i < code.size() && code[i] == '{';
}

// The arguments (as squeezed text) of every CALL of `name` in `code`. A definition or declaration is not a call: the name directly preceded by a type
// name, as in `inline void name(`, or followed by a function body (a return type that ends in a template bracket, `std::optional<T> name(...) {`, has
// no word in front of the name, so the body is what gives it away). A call is preceded by a statement boundary, `->`, `.`, `::`, an operator, or one
// of the words `return`, `else`, `do`.
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
        std::size_t close = std::string::npos;
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
                    close = i;
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
        if (close != std::string::npos && followed_by_body(code, close)) continue;  // a definition with a body
        calls.push_back(std::move(args));
    }
    return calls;
}

// The right-hand side (squeezed) of every ASSIGNMENT to a member named `member` through `.` or `->`: `x.member = rhs;`. A comparison (`==`, `!=`), a
// call (`x.member->f()`), a declaration (`T* member{nullptr};`) and a longer identifier are not assignments.
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

// The body (from its opening brace to the matching closing brace, braces included) of the function DEFINED as `... name(...) ... { ... }` in `code`,
// or "" if there is none. The first match of `name(` preceded by a type word is the definition.
std::string function_body(const std::string& code, std::string_view name) {
    for (std::size_t pos = code.find(name); pos != std::string::npos; pos = code.find(name, pos + name.size())) {
        if (pos > 0 && is_ident_char(code[pos - 1])) continue;
        std::size_t open = pos + name.size();
        while (open < code.size() && std::isspace(static_cast<unsigned char>(code[open])) != 0) ++open;
        if (open >= code.size() || code[open] != '(') continue;
        const std::string_view word = previous_word(code, pos);
        if (word.empty() || word == "return" || word == "else" || word == "do") continue;  // a call, not a definition
        int depth = 0;
        std::size_t i = open;
        for (; i < code.size(); ++i) {
            if (code[i] == '(') ++depth;
            if (code[i] == ')' && --depth == 0) break;
        }
        const std::size_t brace = code.find('{', i);
        if (brace == std::string::npos) return {};
        int braces = 0;
        for (std::size_t j = brace; j < code.size(); ++j) {
            if (code[j] == '{') ++braces;
            if (code[j] == '}' && --braces == 0) return code.substr(brace, j - brace + 1);
        }
        return {};
    }
    return {};
}

struct SourceFile {
    std::string rel;   // relative to native/, forward slashes
    std::string code;  // code_only() of the file
};

// Everything the production binaries are built from: include/ and src/, SORTED by path. Directory iteration order is the filesystem's (alphabetical on
// NTFS, arbitrary on ext4 -- the first GCC run of this guard failed on exactly that), and several checks below compare a list of files with an expected one.
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
        std::sort(v.begin(), v.end(), [](const SourceFile& a, const SourceFile& b) { return a.rel < b.rel; });
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

// How many times `needle` occurs in the code of each production file, as {file, count} for every file with at least one.
std::vector<std::pair<std::string, std::size_t>> occurrences(std::string_view needle) {
    std::vector<std::pair<std::string, std::size_t>> out;
    for (const SourceFile& f : production_sources()) {
        const std::size_t n = count_substring(f.code, needle);
        if (n > 0) out.emplace_back(f.rel, n);
    }
    return out;
}

using Occ = std::vector<std::pair<std::string, std::size_t>>;

std::string show(const Occ& o) {
    std::string s;
    for (const auto& [file, n] : o) s += file + " x" + std::to_string(n) + "; ";
    return s.empty() ? "<nowhere>" : s;
}

}  // namespace

// --- the guard's own negative controls ----------------------------------------------------------------------------------------------------

TEST(BarrierSweepGuard, TheScanSeesRealSources) {
    EXPECT_GT(production_sources().size(), 50u);
    EXPECT_NE(find_source("include/hengyuan/order_tracker.hpp"), nullptr);
    EXPECT_NE(find_source("include/hengyuan/sweep_pump.hpp"), nullptr);
    EXPECT_NE(find_source("include/hengyuan/barrier_sweep.hpp"), nullptr);
    const auto& files = production_sources();
    EXPECT_TRUE(std::is_sorted(files.begin(), files.end(), [](const SourceFile& a, const SourceFile& b) { return a.rel < b.rel; }))
        << "the checks compare lists of files: they must not depend on the order the filesystem happens to list a directory in";
}

TEST(BarrierSweepGuard, TheExtractorsKnowWhatTheyAreLookingFor) {
    const auto stripped = [](const char* t) { return code_only(t); };

    // function_body: the definition's braces, nested braces and all; a call is not a definition; prose does not count.
    EXPECT_EQ(function_body(stripped("inline void f(int a, int (*g)(int)) noexcept { if (a) { g(1); } }"), "f"),
              "{ if (a) { g(1); } }");
    EXPECT_EQ(function_body(stripped("void h() { f(1); }"), "f"), "") << "a call is not a definition";
    EXPECT_EQ(function_body(stripped("// void f() { x }\nvoid g() {}"), "f"), "") << "prose";
    EXPECT_EQ(function_body(stripped("void f();"), "f"), "") << "a declaration has no body";

    // The order check itself, on text with a known answer.
    const std::string good = function_body(stripped("void p() { take_requests(1); while (in.try_pop(x)) {} }"), "p");
    const std::string bad = function_body(stripped("void p() { while (in.try_pop(x)) {} take_requests(1); }"), "p");
    EXPECT_LT(good.find("take_requests("), good.find("try_pop("));
    EXPECT_GT(bad.find("take_requests("), bad.find("try_pop(")) << "the swapped order is seen as swapped";

    using Args = std::vector<std::string>;
    const auto first_call = [](const char* text, std::string_view name) {
        const auto calls = call_args(code_only(text), name);
        return calls.empty() ? Args{"<no call found>"} : calls[0];
    };
    EXPECT_EQ(first_call("void f() { drain_reconcile_events(a, &b, c, d, &e, &f, &g, h ? &s : nullptr); }", "drain_reconcile_events").size(), 8u);
    EXPECT_EQ(first_call("drain_reconcile_events(a, b, c, d);", "drain_reconcile_events").size(), 4u);
    EXPECT_TRUE(call_args(code_only("inline void drain_reconcile_events(A& a, B* b) noexcept {}"), "drain_reconcile_events").empty()) << "a definition";
    EXPECT_TRUE(call_args(code_only("[[nodiscard]] constexpr std::optional<std::uint64_t> take_sweep_request(std::int64_t now_ms) noexcept { return {}; }"),
                          "take_sweep_request")
                    .empty())
        << "a definition whose return type ends in a template bracket";
    EXPECT_TRUE(call_args(code_only("T f(int a) const noexcept override { return a; }"), "f").empty()) << "qualifiers between the list and the body";
    EXPECT_EQ(call_args(code_only("if (const auto e = b.take_sweep_request(now)) { use(e); }"), "take_sweep_request").size(), 1u) << "a call in a condition";
    EXPECT_EQ(call_args(code_only("while (more(x)) { more(y); }"), "more").size(), 2u) << "calls that are followed by `)` or `;`";

    using Rhs = std::vector<std::string>;
    EXPECT_EQ(member_assignments(stripped("marker.kind = ReconcileEventKind::SweepDone;"), "kind"), (Rhs{"ReconcileEventKind::SweepDone"}));
    EXPECT_TRUE(member_assignments(stripped("if (ev.kind == ReconcileEventKind::SweepDone) {}"), "kind").empty()) << "a comparison";
    EXPECT_TRUE(member_assignments(stripped("// m.kind = ReconcileEventKind::SweepDone;"), "kind").empty()) << "prose";
    EXPECT_EQ(count_substring("SweepRequest{1} SweepRequest request{}; SweepRequest{2}", "SweepRequest{"), 2u);
}

// --- 1. the order inside poll_once() --------------------------------------------------------------------------------------------------------

TEST(BarrierSweepGuard, PollOnceTakesTheSweepRequestBeforeItDrainsTheIngressRing) {
    const SourceFile* f = find_source("include/hengyuan/order_tracker.hpp");
    ASSERT_NE(f, nullptr);
    const std::string body = function_body(f->code, "poll_once");
    ASSERT_FALSE(body.empty()) << "poll_once() was not found in order_tracker.hpp";
    const std::size_t take = body.find("take_requests(");
    const std::size_t drain = body.find("inbound.try_pop(");
    ASSERT_NE(take, std::string::npos) << "poll_once() no longer takes sweep requests";
    ASSERT_NE(drain, std::string::npos) << "poll_once() no longer drains the ingress ring";
    EXPECT_LT(take, drain) << "poll_once() must take the sweep request BEFORE it drains the ingress ring: the orders the hot thread sent before it posted "
                              "the request are in the ring by then; the other way round, one of them can miss the sweep (formal/reconnect_sweep_drain_first_bug.cfg)";
    EXPECT_EQ(count_substring(body, "take_requests("), 1u) << "exactly one intake per call";
}

// --- 2./3. who makes, pushes, takes, delivers and applies --------------------------------------------------------------------------------------

TEST(BarrierSweepGuard, AMarkerIsMadeInOnePlaceAndAReportIsAppliedByTwoReviewedOnes) {
    // SweepDone markers are made by assigning .kind = ReconcileEventKind::SweepDone: poll_once() does it, nothing else.
    std::vector<std::string> made;
    for (const SourceFile& f : production_sources()) {
        for (const std::string& rhs : member_assignments(f.code, "kind")) {
            if (rhs == "ReconcileEventKind::SweepDone") made.push_back(f.rel);
        }
    }
    EXPECT_EQ(made, (std::vector<std::string>{"include/hengyuan/order_tracker.hpp"})) << "a marker may be made only by poll_once()";
    const SourceFile* tracker = find_source("include/hengyuan/order_tracker.hpp");
    ASSERT_NE(tracker, nullptr);
    EXPECT_NE(function_body(tracker->code, "poll_once").find("ReconcileEventKind::SweepDone"), std::string::npos);

    // The barrier's sweep protocol has two production drivers: the pump, and the synthetic readiness that scripts the whole exchange.
    for (const char* call : {"on_sweep_report", "take_sweep_request"}) {
        std::vector<std::string> callers;
        for (const SourceFile& f : production_sources()) {
            for (std::size_t n = 0; n < call_args(f.code, call).size(); ++n) callers.push_back(f.rel);
        }
        EXPECT_EQ(callers, (std::vector<std::string>{"include/hengyuan/sweep_pump.hpp", "include/hengyuan/synthetic_readiness.hpp"})) << call;
    }
}

TEST(BarrierSweepGuard, ARequestIsMadeAndPushedByThePumpAndTakenBySweepJobOnly) {
    EXPECT_EQ(occurrences("SweepRequest{"), (Occ{{"include/hengyuan/sweep_pump.hpp", 1}})) << show(occurrences("SweepRequest{"));
    // the ring's two ends
    std::vector<std::string> takers;
    std::vector<std::string> pushers;
    for (const SourceFile& f : production_sources()) {
        for (const auto& args : call_args(f.code, "take_requests")) {
            (void)args;
            takers.push_back(f.rel);
        }
        for (const auto& args : call_args(f.code, "try_push")) {
            if (!args.empty() && args[0].rfind("SweepRequest{", 0) == 0) pushers.push_back(f.rel);
        }
    }
    EXPECT_EQ(pushers, (std::vector<std::string>{"include/hengyuan/sweep_pump.hpp"}));
    EXPECT_EQ(takers, (std::vector<std::string>{"include/hengyuan/order_tracker.hpp"})) << "only poll_once() takes requests from the ring";
    // ...and the ring is only popped inside SweepJob
    std::vector<std::string> poppers;
    for (const SourceFile& f : production_sources()) {
        if (f.code.find("requests_.try_pop(") != std::string::npos) poppers.push_back(f.rel);
    }
    EXPECT_EQ(poppers, (std::vector<std::string>{"include/hengyuan/barrier_sweep.hpp"}));
}

TEST(BarrierSweepGuard, ASinkIsBuiltByThePumpAndHandedOverByTheOrchestratorOnly) {
    EXPECT_EQ(occurrences("SweepSink{"),
              (Occ{{"include/hengyuan/live_submit_orchestrator.hpp", 1}, {"include/hengyuan/sweep_pump.hpp", 1}}))
        << show(occurrences("SweepSink{"));
    // the pump's own service() has one production caller
    std::vector<std::string> servicers;
    for (const SourceFile& f : production_sources()) {
        for (const auto& args : call_args(f.code, "service")) {
            if (args.empty()) servicers.push_back(f.rel);
        }
    }
    EXPECT_EQ(servicers, (std::vector<std::string>{"include/hengyuan/live_submit_orchestrator.hpp"}));
}

// --- 4. the drain calls ------------------------------------------------------------------------------------------------------------------------

namespace {

struct DrainPin {
    const char* file;
    std::size_t calls;
    std::size_t args;
    const char* sink_arg;  // the exact text of the eighth argument; "" for a call that passes none
    const char* why;
};

constexpr std::size_t kSinkArgCount = 8;

// Every production call of drain_reconcile_events(), and what kind it is. A call with no sink is a harness with no private stream and no barrier:
// it never receives a marker because it never asks for a sweep.
const DrainPin kReconcileDrains[] = {
    {"include/hengyuan/live_submit_orchestrator.hpp", 1, kSinkArgCount, "sweep_sink.is_valid() ? &sweep_sink : nullptr",
     "orchestrate_submit(): the pump's sink when ctx.sweep is wired, none when it is not"},
};

}  // namespace

TEST(BarrierSweepGuard, EveryProductionReconcileDrainIsPinnedWithItsSink) {
    std::vector<std::string> problems;
    for (const SourceFile& f : production_sources()) {
        const auto calls = call_args(f.code, "drain_reconcile_events");
        if (calls.empty()) continue;
        const DrainPin* pin = nullptr;
        for (const DrainPin& p : kReconcileDrains) {
            if (f.rel == p.file) pin = &p;
        }
        if (pin == nullptr) {
            // Not a barrier user: it must not pass a sink at all (a stray eighth argument would be an unreviewed consumer of markers).
            for (const auto& args : calls) {
                if (args.size() >= kSinkArgCount) {
                    problems.push_back(f.rel + ": drain_reconcile_events() with " + std::to_string(args.size()) +
                                       " arguments passes a sink and is not pinned in kReconcileDrains");
                }
            }
            continue;
        }
        if (calls.size() != pin->calls) {
            problems.push_back(f.rel + ": expected " + std::to_string(pin->calls) + " call(s) of drain_reconcile_events(), found " + std::to_string(calls.size()));
        }
        for (const auto& args : calls) {
            if (args.size() != pin->args) {
                problems.push_back(f.rel + ": drain_reconcile_events() with " + std::to_string(args.size()) + " arguments, expected " + std::to_string(pin->args) +
                                   " (" + pin->why + ")");
            } else if (args.back() != pin->sink_arg) {
                problems.push_back(f.rel + ": the sink argument is `" + args.back() + "`, expected `" + pin->sink_arg +
                                   "` -- a call that leaves the sink out quietly drops every marker and the barrier never completes");
            }
        }
    }
    for (const DrainPin& p : kReconcileDrains) {
        const SourceFile* f = find_source(p.file);
        if (f == nullptr || call_args(f->code, "drain_reconcile_events").empty()) {
            problems.push_back(std::string(p.file) + ": pinned in kReconcileDrains but makes no drain_reconcile_events() call");
        }
    }
    for (const std::string& s : problems) ADD_FAILURE() << s;
    EXPECT_TRUE(problems.empty());
}

// --- 5. nobody runs a sweep job in production yet ----------------------------------------------------------------------------------------------

TEST(BarrierSweepGuard, NoProductionPollOnceCallPassesASweepJobYet) {
    // poll_once(tracker, inbound, outbound, port, policy, now_ms[, wall_now_ms[, sweep]]): six, seven or eight arguments. The live process that runs
    // a SweepJob is M5's; its call gets pinned there. Until then an eighth argument in production is a wiring nobody reviewed.
    std::vector<std::string> problems;
    std::size_t calls_seen = 0;
    for (const SourceFile& f : production_sources()) {
        for (const auto& args : call_args(f.code, "poll_once")) {
            ++calls_seen;
            if (args.size() >= 8) {
                problems.push_back(f.rel + ": poll_once() with " + std::to_string(args.size()) + " arguments passes a sweep job; M5 wires the live one and pins it");
            }
        }
    }
    EXPECT_GE(calls_seen, 5u) << "the scan must see the real poll_once() callers (the harnesses and the dry-run rig)";
    for (const std::string& s : problems) ADD_FAILURE() << s;
    EXPECT_TRUE(problems.empty());
}
