// SPDX-License-Identifier: proprietary
// test_order_durable_path_guard.cpp -- private-stream migration M4 guard (docs/PRIVATE_STREAM_MIGRATION_DESIGN.md 6.3 and risk R8).
//
// The durable log has to replay, and the writers of an order's frames plan the next frame from a SHADOW of what the log holds (order_durable_path.hpp).
// A shadow that runs AHEAD of the log (moved before the ACK) or BEHIND it (a writer that forgot to move it) makes the next planned frame illegal, and one
// illegal frame makes the whole log Corrupt at the next start. Behaviour is test_order_durable_path.cpp (the planner against the real recovery scan, both
// writers over a real log), test_user_data_durable.cpp (the user-data publisher), test_order_tracker.cpp's DrainDurableTest (the reconcile publisher) and
// test_live_submit_orchestrator.cpp's LiveSubmitShadowTest. What behaviour cannot see is a NEW writer, or an old one that stops moving the shadow, so this
// test pins, over include/ and src/:
//   * who may append a frame to the durable log through the order-audit port (a table of the files, with their counts and why);
//   * every call that moves a shadow (durable_acked, seed_shadow, track) -- who, how many, with how many arguments;
//   * that nothing but OrderFillContext assigns a shadow;
//   * the TEXT ORDER inside each writer: append, then the shadow, then (for a report) the fold -- an ACK must precede every memory effect;
//   * that a production OrchestratorContext which wires the user-data ring also wires the durable publisher (vacuous today: nothing in the tree builds one
//     with a ring; M5 does -- and has to update this pin and wire the publisher to pass it);
//   * that the planner header stays pure.
// The scan strips comments and the contents of string/char literals first, so prose that mentions a call does not count (the helpers are copies of the
// ones in test_reconcile_durable_guard.cpp; guards share no header on purpose, so neither can break the other).

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef HY_NATIVE_SOURCE_DIR
#error "HY_NATIVE_SOURCE_DIR must name the native/ directory (see native/CMakeLists.txt)"
#endif

namespace fs = std::filesystem;

namespace {

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
                    ++i;
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

// The arguments of every CALL of `name` in `code` (a definition -- the name directly after a type name -- is not a call).
std::vector<std::vector<std::string>> call_args(const std::string& code, std::string_view name) {
    std::vector<std::vector<std::string>> calls;
    for (std::size_t pos = code.find(name); pos != std::string::npos; pos = code.find(name, pos + name.size())) {
        if (pos > 0 && is_ident_char(code[pos - 1])) continue;
        std::size_t open = pos + name.size();
        while (open < code.size() && std::isspace(static_cast<unsigned char>(code[open])) != 0) ++open;
        if (open >= code.size() || code[open] != '(') continue;

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

// Assignments to a member called `member` (`x.member = ...`, `x->member = ...`; not `==`, not a longer name).
std::size_t member_assignments(const std::string& code, std::string_view member) {
    std::size_t n = 0;
    for (std::size_t pos = code.find(member); pos != std::string::npos; pos = code.find(member, pos + member.size())) {
        if (pos == 0 || (code[pos - 1] != '.' && code[pos - 1] != '>')) continue;
        if (pos >= 2 && code[pos - 1] == '>' && code[pos - 2] != '-') continue;
        std::size_t after = pos + member.size();
        if (after < code.size() && is_ident_char(code[after])) continue;
        while (after < code.size() && std::isspace(static_cast<unsigned char>(code[after])) != 0) ++after;
        if (after < code.size() && code[after] == '=' && (after + 1 >= code.size() || code[after + 1] != '=')) ++n;
    }
    return n;
}

struct SourceFile {
    std::string rel;   // relative to native/, forward slashes
    std::string code;  // code_only() of the file
};

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

// The position of `needle` in `code` at or after `from`, or npos.
std::size_t find_after(const std::string& code, std::string_view needle, std::size_t from) { return code.find(needle, from); }

// Counts per production file of the calls of `name`, with the files that have none left out.
std::map<std::string, std::size_t> calls_by_file(std::string_view name) {
    std::map<std::string, std::size_t> m;
    for (const SourceFile& f : production_sources()) {
        const std::size_t n = call_args(f.code, name).size();
        if (n != 0) m[f.rel] = n;
    }
    return m;
}

std::string describe(const std::map<std::string, std::size_t>& m) {
    std::string s;
    for (const auto& [file, n] : m) s += "\n    " + file + ": " + std::to_string(n);
    return s.empty() ? " (none)" : s;
}

}  // namespace

TEST(OrderDurablePathGuard, TheScanSeesRealSources) {
    std::size_t files = 0;
    for (const SourceFile& f : production_sources()) {
        (void)f;
        ++files;
    }
    EXPECT_GT(files, 50u);
    EXPECT_NE(find_source("include/hengyuan/order_durable_path.hpp"), nullptr);
    EXPECT_NE(find_source("include/hengyuan/order_fill_context.hpp"), nullptr);
}

TEST(OrderDurablePathGuard, TheScansOwnHelpersBehave) {
    // negative controls of the guard itself
    EXPECT_EQ(call_args(code_only("void f() { x.durable_acked(a, b, c); }"), "durable_acked").size(), 1u);
    EXPECT_EQ(call_args(code_only("void durable_acked(A a, B b) {}"), "durable_acked").size(), 0u) << "a definition";
    EXPECT_EQ(call_args(code_only("// x.durable_acked(a, b, c);\n"), "durable_acked").size(), 0u) << "prose";
    EXPECT_EQ(call_args(code_only("auto s = \"durable_acked(a)\";"), "durable_acked").size(), 0u) << "a string";
    EXPECT_EQ(call_args(code_only("f(x.track(a, b, g(c, d), e));"), "track").at(0).size(), 4u) << "a nested call is one argument";
    EXPECT_EQ(member_assignments(code_only("s.ctx.shadow = D{}; t->shadow = u; v.shadow == w; x.shadow_of = 1;"), "shadow"), 2u);
    EXPECT_EQ(member_assignments(code_only("// y.shadow = 1;\n"), "shadow"), 0u) << "prose";
}

// --- who appends a frame of an order's life -----------------------------------------------------------------------------------------------------

TEST(OrderDurablePathGuard, EveryProductionAppendOfAnOrderFrameIsOneOfTheKnownWriters) {
    // The calls of the durable port's append_durable(). The orchestrator's two are the sink adapter of the port and the ONE function every frame of an
    // order's life goes through (append_order_frame); the other two are the publishers' own append(). A fifth writer is a review item: it must plan
    // against the shadow and move it after the ACK, and then be added here.
    const std::map<std::string, std::size_t> expected = {
        {"include/hengyuan/binance_user_data_event.hpp", 1},     // UserDataDurablePublisher::append
        {"include/hengyuan/live_submit_orchestrator.hpp", 2},    // durable_audit_sink_append_adapter() and detail::append_order_frame()
        {"include/hengyuan/order_tracker.hpp", 1},               // ReconcileDurablePublisher::append
    };
    const std::map<std::string, std::size_t> actual = calls_by_file("append_durable");
    EXPECT_EQ(actual, expected) << "expected:" << describe(expected) << "\n  actual:" << describe(actual);
}

TEST(OrderDurablePathGuard, TheOrchestratorAppendsEveryFrameOfAnOrdersLifeThroughOneFunction) {
    const SourceFile* f = find_source("include/hengyuan/live_submit_orchestrator.hpp");
    ASSERT_NE(f, nullptr);
    // the seven frames: Intent (Gate 10b), Prepared (12d), and the five outcome branches (Accepted, Rejected, Timeout, NetworkError, StaleRulesVersion)
    EXPECT_EQ(call_args(f->code, "append_order_frame").size(), 7u);
    // ... each as `if (!detail::append_order_frame(ctx, ar))`: the result is the ACK, nothing else is read
    std::size_t acked_checks = 0;
    for (std::size_t pos = f->code.find("append_order_frame(ctx, ar)"); pos != std::string::npos; pos = f->code.find("append_order_frame(ctx, ar)", pos + 1)) {
        ++acked_checks;
    }
    EXPECT_EQ(acked_checks, 7u);
    // inside it: the append, THEN the shadow, and the shadow only if the append was acknowledged
    const std::size_t def = f->code.find("inline bool append_order_frame(");
    ASSERT_NE(def, std::string::npos);
    const std::size_t append = find_after(f->code, "ctx.durable_audit.append_durable(ar, ctx.now_ms).acked()", def);
    const std::size_t acked = find_after(f->code, "ctx.fill_context->durable_acked(", def);
    ASSERT_NE(append, std::string::npos);
    ASSERT_NE(acked, std::string::npos);
    EXPECT_LT(append, acked) << "the shadow moves only after the ACK";
    const std::size_t refuse = find_after(f->code, "[[unlikely]] return false;", append);
    ASSERT_NE(refuse, std::string::npos);
    EXPECT_LT(refuse, acked) << "a failed append returns before the shadow is touched";
    EXPECT_EQ(call_args(f->code, "durable_acked").size(), 1u);
}

TEST(OrderDurablePathGuard, EveryShadowMoveIsOneOfTheKnownCalls) {
    const std::map<std::string, std::size_t> acked = {
        {"include/hengyuan/binance_user_data_event.hpp", 2},   // the Ambiguous frame, then the observation's own
        {"include/hengyuan/live_submit_orchestrator.hpp", 1},  // detail::append_order_frame
        {"include/hengyuan/order_tracker.hpp", 2},             // the Ambiguous re-record, then the resolution
    };
    const std::map<std::string, std::size_t> actual_acked = calls_by_file("durable_acked");
    EXPECT_EQ(actual_acked, acked) << "expected:" << describe(acked) << "\n  actual:" << describe(actual_acked);

    const std::map<std::string, std::size_t> seeded = {
        {"include/hengyuan/startup_recovery.hpp", 1},  // apply_recovery(): shadow_of_recovered(checkpoint)
    };
    const std::map<std::string, std::size_t> actual_seeded = calls_by_file("seed_shadow");
    EXPECT_EQ(actual_seeded, seeded) << "expected:" << describe(seeded) << "\n  actual:" << describe(actual_seeded);
}

TEST(OrderDurablePathGuard, EveryTrackOfAnOrderCarriesItsIntendedQuantity) {
    // Seven arguments: the seventh is the quantity the order was sent with, the bound a planned fill is held against. Left out, it defaults to 0 =
    // unknown, and every fill of the order is refused.
    for (const SourceFile& f : production_sources()) {
        for (const std::vector<std::string>& args : call_args(f.code, "track")) {
            if (f.rel == "include/hengyuan/order_fill_context.hpp") continue;  // not a call of ours
            if (args.empty() || (args[0].find("coid") == std::string::npos)) continue;  // another class's track(): not the fill table's
            EXPECT_EQ(args.size(), 7u) << f.rel << ": track() of the fill context without the intended quantity";
        }
    }
    const SourceFile* orchestrator = find_source("include/hengyuan/live_submit_orchestrator.hpp");
    const SourceFile* recovery = find_source("include/hengyuan/startup_recovery.hpp");
    ASSERT_NE(orchestrator, nullptr);
    ASSERT_NE(recovery, nullptr);
    ASSERT_EQ(call_args(orchestrator->code, "track").size(), 1u);
    EXPECT_EQ(call_args(orchestrator->code, "track")[0].back(), "ctx.qty_ticks");
    ASSERT_EQ(call_args(recovery->code, "track").size(), 1u);
    EXPECT_EQ(call_args(recovery->code, "track")[0].back(), "cp.intended_qty_ticks");
}

TEST(OrderDurablePathGuard, NothingButTheFillContextAssignsAShadow) {
    for (const SourceFile& f : production_sources()) {
        const std::size_t n = member_assignments(f.code, "shadow");
        if (f.rel == "include/hengyuan/order_fill_context.hpp") {
            EXPECT_EQ(n, 3u) << "track(), seed_shadow() and durable_acked(): the three ways a shadow changes";
        } else {
            EXPECT_EQ(n, 0u) << f.rel << " assigns a shadow: it must go through durable_acked() or seed_shadow()";
        }
    }
}

// --- the order inside each writer ----------------------------------------------------------------------------------------------------------------

TEST(OrderDurablePathGuard, TheUserDataPublisherAcksBeforeTheShadowAndTheShadowBeforeTheFold) {
    const SourceFile* f = find_source("include/hengyuan/binance_user_data_event.hpp");
    ASSERT_NE(f, nullptr);
    const std::size_t start = f->code.find("bool publish_one(");
    ASSERT_NE(start, std::string::npos);
    const std::size_t plan = find_after(f->code, "plan_observation(e->shadow", start);
    const std::size_t prefix = find_after(f->code, "append(detail::make_user_data_ambiguous_record(", start);
    const std::size_t prefix_shadow = find_after(f->code, "durable_acked(coid, OrderState::Ambiguous", start);
    const std::size_t frame = find_after(f->code, "append(frame, now_ms)", start);
    const std::size_t frame_shadow = find_after(f->code, "durable_acked(coid, h.ev.order_status", start);
    const std::size_t fold = find_after(f->code, "consume_delta(coid, h.cumulative_filled_qty_ticks)", start);
    const std::size_t apply = find_after(f->code, "apply_fill(", start);
    const std::size_t ring = find_after(f->code, "env.audit->append(detail::make_user_data_frame(", start);
    for (const std::size_t p : {plan, prefix, prefix_shadow, frame, frame_shadow, fold, apply, ring}) ASSERT_NE(p, std::string::npos);
    EXPECT_LT(plan, prefix) << "the plan comes first";
    EXPECT_LT(prefix, prefix_shadow) << "the shadow follows the ACK of the Ambiguous frame";
    EXPECT_LT(prefix_shadow, frame);
    EXPECT_LT(frame, frame_shadow) << "the shadow follows the ACK of the observation's frame";
    EXPECT_LT(frame_shadow, fold) << "PositionTruth moves only after the frame is durable";
    EXPECT_LT(fold, apply);
    EXPECT_LT(apply, ring) << "and the audit ring learns of it last";
    // the memory-first body of the drain is untouched: no durable_acked() and no publisher inside the legacy loop's own fold
    EXPECT_EQ(call_args(f->code, "durable_acked").size(), 2u);
}

TEST(OrderDurablePathGuard, TheReconcilePublisherAcksBeforeTheShadowInItsPlannedPath) {
    const SourceFile* f = find_source("include/hengyuan/order_tracker.hpp");
    ASSERT_NE(f, nullptr);
    const std::size_t start = f->code.find("bool publish_planned(");
    ASSERT_NE(start, std::string::npos);
    const std::size_t plan = find_after(f->code, "plan_resolution(e->shadow", start);
    const std::size_t remap = find_after(f->code, "append(remap, now_ms)", start);
    const std::size_t remap_shadow = find_after(f->code, "durable_acked(coid, OrderState::Ambiguous", start);
    const std::size_t record = find_after(f->code, "append(record, now_ms)", start);
    const std::size_t record_shadow = find_after(f->code, "durable_acked(coid, pending_.resulting_state", start);
    for (const std::size_t p : {plan, remap, remap_shadow, record, record_shadow}) ASSERT_NE(p, std::string::npos);
    EXPECT_LT(plan, remap);
    EXPECT_LT(remap, remap_shadow);
    EXPECT_LT(remap_shadow, record);
    EXPECT_LT(record, record_shadow);
    // the drain hands the shadow source to the publisher: without it the publisher falls back to the prior_state rule, which is wrong once a second writer
    // exists
    const std::size_t drain = f->code.find("inline void drain_reconcile_events(");
    ASSERT_NE(drain, std::string::npos);
    EXPECT_NE(find_after(f->code, "durable->next_acked(events, now_ms, ev, ar, fill_context)", drain), std::string::npos);
}

// --- wiring -----------------------------------------------------------------------------------------------------------------------------------------

TEST(OrderDurablePathGuard, AProductionContextThatWiresTheUserDataRingWiresTheDurablePublisherToo) {
    // Vacuous today: no file under include/ or src/ builds an OrchestratorContext with a user-data ring (the harnesses call the drain themselves, pinned in
    // test_submit_readiness_guard.cpp). M5 wires one: it must set user_data_durable beside user_data_events -- or a report would reach PositionTruth before
    // its frame is durable.
    for (const SourceFile& f : production_sources()) {
        if (f.rel == "include/hengyuan/live_submit_orchestrator.hpp") continue;  // the field's own declaration and the drain call
        const std::size_t ring = member_assignments(f.code, "user_data_events");
        const std::size_t publisher = member_assignments(f.code, "user_data_durable");
        EXPECT_GE(publisher, ring) << f.rel << ": wires user_data_events without user_data_durable";
    }
}

TEST(OrderDurablePathGuard, ThePlannerHeaderIsPure) {
    const SourceFile* f = find_source("include/hengyuan/order_durable_path.hpp");
    ASSERT_NE(f, nullptr);
    std::vector<std::string> includes;
    for (std::size_t pos = f->code.find("#include"); pos != std::string::npos; pos = f->code.find("#include", pos + 1)) {
        const std::size_t end = f->code.find('\n', pos);
        includes.push_back(squeeze(f->code.substr(pos, end - pos)));
    }
    const std::vector<std::string> allowed = {"#include <hengyuan/order_lifecycle.hpp>", "#include <cstdint>"};
    EXPECT_EQ(includes, allowed) << "pure functions over values: no I/O, no allocation, nothing but the state machine";
    EXPECT_EQ(f->code.find("new "), std::string::npos);
    EXPECT_EQ(f->code.find("std::string"), std::string::npos);
    EXPECT_EQ(f->code.find("std::vector"), std::string::npos);
}
