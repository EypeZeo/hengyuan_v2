// SPDX-License-Identifier: proprietary
// test_never_sent_guard.cpp -- Q23 slices S2/S3 guard (docs/NOTSENT_CLOSURE_DESIGN.md 4.6 and 8).
//
// S2/S3 teach the recovery reader to accept ONE new kind of frame, OrderNeverSent (event 23, resulting state NeverSent = 12),
// which the writer of slice S4 (DurableAuditSink::append_never_sent()) writes for its callers, S5 (the final gate's refusal)
// and S6 (a refusal before the Prepared frame). What keeps that acceptance safe NOW is the absence of anything that could
// produce such a frame, or act on one: behaviour cannot see an absence, so this test reads the sources, exactly as
// test_operator_resolution_guard.cpp does for the operator's frame:
//
//   1. AuditEventType::OrderNeverSent is NAMED only where it is defined, where the codec decides which version may carry it,
//      where the closure rule (never_sent_rules.hpp) reads it, and where the recovery scan recognises the frame. Each pin
//      carries the exact number of mentions. A further file naming it is a producer (or a new reader) somebody has to look at.
//   2. OrderState::NeverSent likewise: the state machine, the codec's version bound, the rule and the reader. In particular
//      nothing live -- the orchestrator, the tracker, the publishers, the reconcile path -- moves an order into it.
//   3. The closure rule, check_closure(), is applied by the recovery scan and by the one writer, append_never_sent() (Q23 S4),
//      and nothing else: what the writer lets through, the reader accepts. The callers of the writer join in S5 and S6.
//   4. is_resolved() is called by the recovery scan and nothing else (the slot release and the retention filters join in the
//      writer slices). is_exchange_final() keeps meaning what it says: its callers are not pinned here (they predate this slice),
//      but test_order_lifecycle.cpp pins that NeverSent is not exchange-final.
//   5. The closure counter, recovered_never_sent(), is read by nothing in production.
//   6. (Q23 S4) The writer, DurableAuditSink::append_never_sent(), has NO caller in production, and the switch that lets a sink
//      write the frame at all, NeverSentWrites, is named by the sink header alone -- so no process can turn it on. The callers
//      (S5's final gate, S6's refusals before the Prepared frame) add themselves here, with the reason, in the PR that wires them.
//
// The scan strips comments and the contents of string/char literals first, so prose that mentions a name does not count and a
// string that contains one cannot hide a real use. (test_helpers/cxx_source_view.hpp is the one shared scanner.)

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>
#include "test_helpers/cxx_source_view.hpp"

#ifndef HY_NATIVE_SOURCE_DIR
#error "HY_NATIVE_SOURCE_DIR must name the native/ directory (see native/CMakeLists.txt)"
#endif

namespace fs = std::filesystem;

namespace {

using hy::test_helpers::code_only;

bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// How many times `name` appears as a whole identifier (not as part of a longer one) in `code`.
std::size_t count_identifier(const std::string& code, std::string_view name) {
    std::size_t n = 0;
    for (std::size_t pos = code.find(name); pos != std::string::npos; pos = code.find(name, pos + name.size())) {
        if (pos > 0 && is_ident_char(code[pos - 1])) continue;
        const std::size_t end = pos + name.size();
        if (end < code.size() && is_ident_char(code[end])) continue;
        ++n;
    }
    return n;
}

// The text between the braces of the body of the function `name` (its first occurrence followed by a parameter list and a
// brace), braces matched. Empty if there is none.
std::string function_body(const std::string& code, std::string_view name) {
    for (std::size_t pos = code.find(name); pos != std::string::npos; pos = code.find(name, pos + name.size())) {
        if (pos > 0 && is_ident_char(code[pos - 1])) continue;
        std::size_t i = pos + name.size();
        while (i < code.size() && std::isspace(static_cast<unsigned char>(code[i])) != 0) ++i;
        if (i >= code.size() || code[i] != '(') continue;
        int paren = 0;
        for (; i < code.size(); ++i) {  // to the end of the parameter list
            if (code[i] == '(') ++paren;
            if (code[i] == ')' && --paren == 0) break;
        }
        while (i < code.size() && code[i] != '{' && code[i] != ';') ++i;  // qualifiers, then the body or the end of a declaration
        if (i >= code.size() || code[i] != '{') continue;
        const std::size_t begin = i + 1;
        int depth = 1;
        for (++i; i < code.size(); ++i) {
            if (code[i] == '{') ++depth;
            if (code[i] == '}' && --depth == 0) return code.substr(begin, i - begin);
        }
    }
    return {};
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

constexpr const char* kTrail = "include/hengyuan/audit_trail.hpp";
constexpr const char* kLifecycle = "include/hengyuan/order_lifecycle.hpp";
constexpr const char* kCodec = "include/hengyuan/durable_frame_codec.hpp";
constexpr const char* kRules = "include/hengyuan/never_sent_rules.hpp";
constexpr const char* kSink = "include/hengyuan/durable_audit_sink.hpp";

const SourceFile* find_source(std::string_view rel) {
    for (const SourceFile& f : production_sources()) {
        if (f.rel == rel) return &f;
    }
    return nullptr;
}

struct Pin {
    const char* file;
    std::size_t mentions;
    const char* why;
};

// Fails for every file that names `name` and is not in `pins`, for every pin whose count differs, and for a pinned file that no
// longer names it at all.
void expect_named_only_in(std::string_view name, const std::vector<Pin>& pins) {
    std::vector<std::string> problems;
    for (const SourceFile& f : production_sources()) {
        const std::size_t n = count_identifier(f.code, name);
        if (n == 0) continue;
        const Pin* pin = nullptr;
        for (const Pin& p : pins) {
            if (f.rel == p.file) pin = &p;
        }
        if (pin == nullptr) {
            problems.push_back(f.rel + " names " + std::string(name) + " " + std::to_string(n) +
                               " time(s) and is not pinned: a new user of it is a new producer or reader, and somebody has "
                               "to look at it before it is added here with the reason");
        } else if (pin->mentions != n) {
            problems.push_back(f.rel + " names " + std::string(name) + " " + std::to_string(n) + " time(s), pinned " +
                               std::to_string(pin->mentions) + " (" + pin->why + ")");
        }
    }
    for (const Pin& p : pins) {
        bool found = false;
        for (const SourceFile& f : production_sources()) {
            if (f.rel == p.file && count_identifier(f.code, name) != 0) found = true;
        }
        if (!found) problems.push_back(std::string(p.file) + ": pinned for " + std::string(name) + " but no longer names it");
    }
    for (const std::string& s : problems) ADD_FAILURE() << s;
    EXPECT_TRUE(problems.empty());
}

}  // namespace

TEST(NeverSentGuard, TheScanSeesRealSources) {
    // A guard that scans an empty tree passes for the wrong reason.
    std::size_t files = 0;
    for (const SourceFile& f : production_sources()) {
        (void)f;
        ++files;
    }
    EXPECT_GT(files, 50u);
    for (const char* rel : {kTrail, kLifecycle, kCodec, kRules, kSink}) {
        EXPECT_NE(find_source(rel), nullptr) << rel;
    }
}

TEST(NeverSentGuard, TheScannerHelpersHaveKnownAnswers) {
    // The negative controls of the guard itself: the helpers the pins use, on text with known answers.
    EXPECT_EQ(count_identifier(code_only("a(X); b(X);"), "X"), 2u);
    EXPECT_EQ(count_identifier(code_only("a(MyX); b(XY); c(X_1);"), "X"), 0u) << "part of a longer identifier";
    EXPECT_EQ(count_identifier(code_only("// X\n/* X */ const char* s = \"X\"; char c = 'X';"), "X"), 0u)
        << "prose and literals do not count";
    EXPECT_EQ(count_identifier(code_only("E::X == v; E::X"), "X"), 2u) << "a qualified name is the identifier";
    EXPECT_EQ(count_identifier(code_only("OrderNeverSent NeverSent never_sent"), "NeverSent"), 1u)
        << "the event and the state are different identifiers";
    EXPECT_EQ(function_body(code_only("int f(int a) { if (a) { return 1; } return 2; } int g() { return 3; }"), "f"),
              " if (a) { return 1; } return 2; ");
    EXPECT_TRUE(function_body(code_only("int f(int a);"), "f").empty()) << "a declaration has no body";
}

TEST(NeverSentGuard, TheNeverSentEventIsNamedOnlyByItsDefinitionTheCodecTheRuleAndTheReader) {
    expect_named_only_in("OrderNeverSent",
                         {{kTrail, 2, "the enumerator and its name in audit_event_name()"},
                          {kCodec, 2, "is_legal_audit_event_type_for_version(): the v6 bound and the assertion that the two "
                                      "v6-only events are consecutive"},
                          {kRules, 2, "pairing_holds() and check_closing_frame(): the rule reads the event, it never builds one"},
                          {kSink, 1, "the recovery scan recognising the frame -- it compares, it constructs nothing"}});
}

TEST(NeverSentGuard, TheNeverSentStateIsNamedOnlyByTheStateMachineTheCodecTheRuleAndTheReader) {
    expect_named_only_in("NeverSent",
                         {{kLifecycle, 6, "the enumerator, order_state_name(), is_terminal(), is_resolved() and the two edges "
                                          "of validate_transition()"},
                          {kCodec, 1, "is_legal_order_state_for_version(): the v6 bound"},
                          {kRules, 3, "check_order_closable() asks the state machine about the edge; pairing_holds() and "
                                      "check_closing_frame() read the frame's state"},
                          {kSink, 1, "the recovery scan, after the rule accepted the frame"}});
}

TEST(NeverSentGuard, NothingLiveNamesTheEventOrTheState) {
    // The point of the two pins above, said once by name: the live process is the one that would CALL the writer (S5, S6),
    // and none of it may mention the event or the state yet.
    for (const char* rel :
         {"include/hengyuan/live_submit_orchestrator.hpp", "include/hengyuan/order_tracker.hpp",
          "include/hengyuan/order_durable_path.hpp", "include/hengyuan/durable_order_audit_port.hpp",
          "include/hengyuan/binance_user_data_event.hpp", "include/hengyuan/late_fill_alarm.hpp",
          "include/hengyuan/startup_recovery.hpp", "include/hengyuan/submit_readiness.hpp",
          "include/hengyuan/operator_resolution_rules.hpp", "include/hengyuan/operator_resolution_flow.hpp"}) {
        const SourceFile* f = find_source(rel);
        ASSERT_NE(f, nullptr) << rel << " moved or was renamed: update this list";
        EXPECT_EQ(count_identifier(f->code, "OrderNeverSent"), 0u) << rel;
        EXPECT_EQ(count_identifier(f->code, "NeverSent"), 0u) << rel;
    }
    for (const SourceFile& f : production_sources()) {
        if (f.rel.rfind("src/", 0) == 0) {
            EXPECT_EQ(count_identifier(f.code, "OrderNeverSent"), 0u) << f.rel;
            EXPECT_EQ(count_identifier(f.code, "NeverSent"), 0u) << f.rel;
        }
    }
}

TEST(NeverSentGuard, TheClosureRuleIsAppliedByTheRecoveryScanAndTheNeverSentWriterAlone) {
    // The writer applies the very same function the reader does, before it writes: a frame the writer accepts is a frame the
    // next start accepts. Nothing else may decide whether a closure is legitimate.
    expect_named_only_in("check_closure", {{kRules, 1, "the definition"},
                                           {kSink, 2, "the recovery scan, and append_never_sent() before it writes"}});
    const SourceFile* sink = find_source(kSink);
    ASSERT_NE(sink, nullptr);
    EXPECT_EQ(count_identifier(function_body(sink->code, "run_recovery_scan_impl"), "check_closure"), 1u)
        << "one call is inside the recovery scan";
    EXPECT_EQ(count_identifier(function_body(sink->code, "append_never_sent"), "check_closure"), 1u)
        << "the other is inside the writer, before it appends";
    EXPECT_EQ(count_identifier(function_body(sink->code, "append_durable"), "check_closure"), 0u)
        << "append_durable() judges no frame's legality (it never has)";
}

TEST(NeverSentGuard, TheClosureWriterHasNoCallerAndTheSwitchIsNamedByTheSinkAlone) {
    // Q23 S4: the writer exists and nothing in the tree calls it or turns it on. Every caller is a producer of the frame, so the
    // callers (S5, S6) are added here one by one, each with the reason, by the PR that wires them.
    expect_named_only_in("append_never_sent", {{kSink, 1, "the definition"}});
    // The switch is a constructor argument: the only way to pass Enabled is to name the enum, so a process that turns the writer
    // on shows up here. In the sink: the enum, the constructor parameter and its default, the accessor, the member and its
    // initialiser, and the one check inside append_never_sent().
    expect_named_only_in("NeverSentWrites", {{kSink, 7, "the enum, the constructor, the accessor, the member, the check"}});
    // The default really is Disabled, and the only check of it refuses everything but Enabled.
    const SourceFile* sink = find_source(kSink);
    ASSERT_NE(sink, nullptr);
    EXPECT_NE(sink->code.find("NeverSentWrites never_sent_writes = NeverSentWrites::Disabled"), std::string::npos)
        << "the constructor's default must stay Disabled";
    EXPECT_NE(function_body(sink->code, "append_never_sent").find("never_sent_writes_ != NeverSentWrites::Enabled"),
              std::string::npos)
        << "the writer's first check refuses everything but Enabled";
}

TEST(NeverSentGuard, TheResolvedPredicateIsAppliedByTheRecoveryScanAlone) {
    expect_named_only_in("is_resolved", {{kLifecycle, 1, "the definition"}, {kSink, 1, "the recovery scan's skip of a closed order"}});
    const SourceFile* sink = find_source(kSink);
    ASSERT_NE(sink, nullptr);
    EXPECT_EQ(count_identifier(function_body(sink->code, "run_recovery_scan_impl"), "is_resolved"), 1u);
}

TEST(NeverSentGuard, NothingInProductionReadsTheClosureCounter) {
    expect_named_only_in("recovered_never_sent", {{kSink, 1, "the accessor"}});
}
