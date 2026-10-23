// SPDX-License-Identifier: proprietary
// test_reconcile_durable_guard.cpp -- SAFE-03 slice 1 guard (audit P1-002).
//
// "A reconcile resolution reaches memory only after its durable ACK" is carried by two things:
//   1. behaviour: test_order_tracker.cpp's DrainDurableTest (the ordering, observed from inside the sink's
//      append call), test_startup_recovery.cpp's DurableReconcileLogTest (what the drain writes must replay on a
//      real log) and test_live_submit_orchestrator.cpp / test_reconcile_concurrency.cpp (the production entry
//      point and two real threads);
//   2. this test, for the one thing behaviour cannot see: drain_reconcile_events() takes its
//      ReconcileDurablePublisher as a trailing DEFAULT argument, so a call site that simply leaves it out still
//      compiles and quietly publishes to memory with no durable record. Every call under include/ and src/ is
//      pinned here with its argument count (7 = durable, 6 = memory-only, 8 = durable plus the M3b sweep sink,
//      whose own text test_barrier_sweep_guard.cpp pins) AND, for a durable call, the exact
//      text of its seventh argument -- an explicit `nullptr` there still makes seven arguments and would be a
//      memory-only call in disguise (the mutation check found exactly that hole in a count-only version of
//      this guard). A new call site, a pinned one that loses or changes the argument, or a pinned one that gains
//      or loses a call fails the test until a reviewer has looked at it and updated the table -- and the table
//      says why each memory-only call is allowed to be one.
//
// It also pins that the publisher's next_acked() -- the one function that hands a resolution out only after an
// ACK -- has no caller but the drain itself, so nothing can pop a resolution around it.
//
// The scan strips comments and the contents of string/char literals first, so prose that mentions a call does
// not count and a string that contains one cannot hide a real use. (The stripping helper is a copy of the one
// in test_rest_freeze_guard.cpp; the two guards share no header on purpose, so neither can break the other.)

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

// The arguments (as squeezed text) of every CALL of `name` in `code` (comments and literals already
// stripped). A definition or declaration -- the name directly preceded by a type name, as in
// `inline void name(` or `bool name(` -- is not a call; a call is preceded by a statement boundary, `->`, `.`,
// `::`, an operator, or one of the words `return`, `else`, `do`.
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

// A drain call is durable-wired iff it names the publisher: drain_reconcile_events(in_flight, audit, events,
// now_ms, position_truth, fill_context, durable) -- the seventh argument.
constexpr std::size_t kDurableArgCount = 7;
constexpr std::size_t kMemoryOnlyArgCount = 6;
// Private-stream migration M3b: an eighth argument is the sweep sink (the hot thread's receiver of the SweepDone marker). It follows the durable
// one, which stays the seventh -- test_barrier_sweep_guard.cpp pins the sink's text.
constexpr std::size_t kDurableWithSweepSinkArgCount = 8;

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

struct Pin {
    const char* file;
    std::size_t calls;
    std::size_t args;
    const char* durable_arg;  // the exact text of the seventh argument; "" for a memory-only call
    const char* why;
};

// Every production call of drain_reconcile_events(), and what kind it is.
const Pin kDrainCalls[] = {
    {"include/hengyuan/live_submit_orchestrator.hpp", 1, kDurableWithSweepSinkArgCount, "ctx.reconcile_durable",
     "orchestrate_submit(): drains only when ctx.reconcile_durable is wired (no publisher = no drain)"},
    {"include/hengyuan/verified_dry_run_evidence.hpp", 1, kDurableArgCount, "&rig.reconcile_durable",
     "the ambiguity drill reconciles on the drill rig's real scratch log"},
    {"src/live_submit_preflight_harness.cpp", 2, kDurableArgCount, "&reconcile_durable",
     "the loop and the shutdown drain, over the harness's real (temp) DurableAuditSink"},
    {"src/live_submit_reconcile_harness_demo.cpp", 2, kMemoryOnlyArgCount, "",
     "MEMORY-ONLY: no DurableAuditSink and nothing is ever submitted, so the ring is always empty"},
    {"src/live_submit_real_credentials_demo.cpp", 2, kMemoryOnlyArgCount, "",
     "MEMORY-ONLY: no DurableAuditSink and the SubmitPort is assembled but never invoked, so the ring is always empty"},
};

}  // namespace

TEST(ReconcileDurableGuard, TheScanSeesRealSources) {
    // A guard that scans an empty tree passes for the wrong reason.
    std::size_t files = 0;
    bool saw_tracker = false;
    for (const SourceFile& f : production_sources()) {
        ++files;
        if (f.rel == "include/hengyuan/order_tracker.hpp") saw_tracker = true;
    }
    EXPECT_GT(files, 50u);
    EXPECT_TRUE(saw_tracker);
}

TEST(ReconcileDurableGuard, TheArgumentExtractorSplitsCallsAndSkipsDefinitionsAndProse) {
    // The negative controls of the guard itself: the same extractor the pins use, on text with known answers.
    const std::string name = "drain_reconcile_events";
    using Args = std::vector<std::string>;
    const auto calls_in = [&](const char* text) { return call_args(code_only(text), name); };
    const auto first_call = [&](const char* text) {
        const auto calls = calls_in(text);
        return calls.empty() ? Args{"<no call found>"} : calls[0];
    };

    EXPECT_EQ(first_call("void f() { drain_reconcile_events(a, &b, c, d, e, f, &g); }"),
              (Args{"a", "&b", "c", "d", "e", "f", "&g"}));
    EXPECT_EQ(first_call("void f() { drain_reconcile_events(a, &b, c, d, e, f); }").size(), 6u);
    EXPECT_EQ(first_call("x = drain_reconcile_events(a, g(b, c), {1, 2}, d);"), (Args{"a", "g(b, c)", "{1, 2}", "d"}))
        << "commas inside nested calls and braces are not top-level arguments";
    EXPECT_EQ(first_call("drain_reconcile_events (\n  a,\n   b  +  c\n);"), (Args{"a", "b + c"}))
        << "whitespace and line breaks do not matter";
    EXPECT_EQ(first_call("drain_reconcile_events(a, b, c, d, e, f, nullptr);").back(), "nullptr")
        << "an explicit nullptr is seven arguments, and visibly not the publisher";
    EXPECT_TRUE(first_call("drain_reconcile_events();").empty());
    EXPECT_TRUE(calls_in("inline void drain_reconcile_events(A& a, B* b) noexcept {}").empty())
        << "a definition is not a call";
    EXPECT_TRUE(calls_in("// drain_reconcile_events(a, b)\n"
                         "const char* s = \"drain_reconcile_events(a, b)\";\n")
                    .empty())
        << "prose and string literals are not calls";
    EXPECT_TRUE(calls_in("my_drain_reconcile_events(a, b);").empty()) << "a longer identifier is not the function";
    EXPECT_EQ(calls_in("if (x) y(); else drain_reconcile_events(a, b);").size(), 1u)
        << "a call right after `else` is still a call";
    EXPECT_EQ(calls_in("return drain_reconcile_events(a, b);").size(), 1u);
}

TEST(ReconcileDurableGuard, EveryProductionDrainCallIsPinnedWithItsDurableArgument) {
    std::vector<std::string> problems;
    for (const SourceFile& f : production_sources()) {
        const std::vector<std::vector<std::string>> counts = call_args(f.code, "drain_reconcile_events");
        if (counts.empty()) continue;

        const Pin* pin = nullptr;
        for (const Pin& p : kDrainCalls) {
            if (f.rel == p.file) pin = &p;
        }
        if (pin == nullptr) {
            problems.push_back(f.rel + ": " + std::to_string(counts.size()) +
                               " call(s) to drain_reconcile_events() that are not pinned in kDrainCalls -- a new call "
                               "site must pass the ReconcileDurablePublisher (7 arguments) or be pinned, with the "
                               "reason it may publish to memory without a durable record");
            continue;
        }
        if (counts.size() != pin->calls) {
            problems.push_back(f.rel + ": expected " + std::to_string(pin->calls) + " call(s), found " +
                               std::to_string(counts.size()));
        }
        for (const std::vector<std::string>& args : counts) {
            if (args.size() != pin->args) {
                problems.push_back(f.rel + ": a call has " + std::to_string(args.size()) + " arguments, expected " +
                                   std::to_string(pin->args) + " (" + pin->why + ")");
            } else if (pin->args >= kDurableArgCount && args[kDurableArgCount - 1] != pin->durable_arg) {
                problems.push_back(f.rel + ": the seventh argument is `" + args[kDurableArgCount - 1] + "`, expected `" +
                                   pin->durable_arg + "` -- anything else (an explicit nullptr above all) is a "
                                   "memory-only call with a durable-looking argument list");
            }
        }
    }
    // ...and every pin must still be there (a deleted call site is a change somebody should look at too).
    for (const Pin& p : kDrainCalls) {
        bool found = false;
        for (const SourceFile& f : production_sources()) {
            if (f.rel == p.file && !call_args(f.code, "drain_reconcile_events").empty()) found = true;
        }
        if (!found) problems.push_back(std::string(p.file) + ": pinned in kDrainCalls but no call found");
    }
    for (const std::string& s : problems) ADD_FAILURE() << s;
    EXPECT_TRUE(problems.empty());
}

TEST(ReconcileDurableGuard, NextAckedHasNoCallerButTheDrainItself) {
    // The only way to take a resolution off the ring on the durable path is ReconcileDurablePublisher::
    // next_acked(), which returns it only after the ACK. A second caller is a second way to pop around that.
    std::size_t in_tracker = 0;
    std::vector<std::string> elsewhere;
    for (const SourceFile& f : production_sources()) {
        const std::size_t n = call_args(f.code, "next_acked").size();
        if (f.rel == "include/hengyuan/order_tracker.hpp") {
            in_tracker = n;
        } else if (n != 0) {
            elsewhere.push_back(f.rel);
        }
    }
    for (const std::string& f : elsewhere) ADD_FAILURE() << f << " calls next_acked()";
    EXPECT_EQ(in_tracker, 1u) << "exactly one call, inside drain_reconcile_events()";
}
