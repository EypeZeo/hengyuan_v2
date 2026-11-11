// SPDX-License-Identifier: proprietary
// test_key_copy_wipe_guard.cpp -- audit F-B-002 / L-60 (Owner decision 2026-11-10): a derived HMAC key that was copied onto the stack must not outlive
// the scope that copied it.
//
// Two halves.
//   * Behaviour: ScopedSecureWipe (secure_wipe.hpp) wipes exactly the buffer it was given, when the scope ends, on EVERY way out of it -- the early
//     `return` of a failure branch as much as the normal end. That is the thing the hand-written secure_wipe() calls it replaced could not promise: each
//     had to be repeated before every return.
//   * Source: every local `std::array<std::byte, kKeyBlockSize> NAME{};` in the two durable sinks and in the audit export is followed, as the very next
//     statement, by `ScopedSecureWipe wipe_NAME(NAME);`. A key copy added later without one is a test failure, not a leftover in a dead stack frame.
//     The scanner is shown to see a bad declaration (it is run on small sources that are wrong in each way it knows), and each file must hold at least
//     as many declarations as it did when this was written, so that a scanner that matched nothing cannot pass.
// What this does NOT prove: that no other copy of the key exists (a register, a span's referent inside a callee -- HmacSha256 wipes its own, see
// sha256.hpp SEC-WIPE-018), and nothing about key_ring.hpp itself, whose own copies are wiped by hand and have their own tests.

#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <hengyuan/secure_wipe.hpp>

#include "test_helpers/cxx_source_view.hpp"

#ifndef HY_NATIVE_SOURCE_DIR
#error "HY_NATIVE_SOURCE_DIR must name the native/ directory (see native/CMakeLists.txt)"
#endif

namespace fs = std::filesystem;

namespace {

using hy::ScopedSecureWipe;
using hy::secure_wipe;

constexpr std::size_t kBlock = 64;
using Block = std::array<std::byte, kBlock>;

Block filled_with(std::byte value) {
    Block b{};
    b.fill(value);
    return b;
}

bool all_equal(const Block& b, std::byte value) {
    for (const std::byte x : b) {
        if (x != value) return false;
    }
    return true;
}

// ---------------------------------------------------------------- behaviour ------------------------------------------------------------------

TEST(ScopedSecureWipe, TheBufferIsLeftAloneWhileTheScopeLivesAndWipedWhenItEnds) {
    Block key = filled_with(std::byte{0xAB});
    {
        ScopedSecureWipe wipe_key(key);
        EXPECT_TRUE(all_equal(key, std::byte{0xAB})) << "constructing the guard must not touch the key";
    }
    EXPECT_TRUE(all_equal(key, std::byte{0})) << "the whole block is zero once the scope has ended";
}

TEST(ScopedSecureWipe, EveryEarlyReturnOfAFailureBranchWipesToo) {
    Block key{};
    // The shape of the sinks' functions: copy the key, then leave through whichever branch fails first.
    const auto function_with_many_returns = [&key](int leave_at) {
        key.fill(std::byte{0xCD});
        ScopedSecureWipe wipe_key(key);
        if (leave_at == 1) return 1;
        key[0] = std::byte{0xEE};
        if (leave_at == 2) return 2;
        if (leave_at == 3) {
            return 3;
        }
        return 4;
    };
    for (const int leave_at : {1, 2, 3, 4}) {
        EXPECT_EQ(function_with_many_returns(leave_at), leave_at);
        EXPECT_TRUE(all_equal(key, std::byte{0})) << "left through return #" << leave_at << " with key bytes still in the buffer";
    }
}

TEST(ScopedSecureWipe, WipesTheBufferItWasGivenAndNothingNextToIt) {
    // Two blocks side by side in one object: wrapping the first must leave the second exactly as it was (a wipe one byte too long would not).
    struct Pair {
        Block first;
        Block second;
    };
    Pair p{filled_with(std::byte{0x11}), filled_with(std::byte{0x22})};
    {
        ScopedSecureWipe wipe_first(p.first);
    }
    EXPECT_TRUE(all_equal(p.first, std::byte{0}));
    EXPECT_TRUE(all_equal(p.second, std::byte{0x22})) << "the neighbouring block was touched";
}

TEST(ScopedSecureWipe, TwoGuardsInOneScopeWipeTheirOwnBlocks) {
    Block a = filled_with(std::byte{0x01});
    Block b = filled_with(std::byte{0x02});
    {
        ScopedSecureWipe wipe_a(a);
        ScopedSecureWipe wipe_b(b);
    }
    EXPECT_TRUE(all_equal(a, std::byte{0}));
    EXPECT_TRUE(all_equal(b, std::byte{0}));
}

TEST(SecureWipe, WipesExactlyTheRequestedLength) {
    Block b = filled_with(std::byte{0x7F});
    secure_wipe(b.data(), 10);
    for (std::size_t i = 0; i < kBlock; ++i) {
        EXPECT_EQ(b[i], i < 10 ? std::byte{0} : std::byte{0x7F}) << "byte " << i;
    }
    secure_wipe(nullptr, 5);   // no-ops, not crashes
    secure_wipe(b.data(), 0);
    EXPECT_EQ(b[10], std::byte{0x7F});
}

// ---------------------------------------------------------------- source ---------------------------------------------------------------------

bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

void skip_ws(const std::string& s, std::size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
}

// Consume `token` (after optional whitespace) at `i`; false (and `i` unchanged) if it is not there.
bool eat(const std::string& s, std::size_t& i, std::string_view token) {
    std::size_t j = i;
    skip_ws(s, j);
    if (s.compare(j, token.size(), token) != 0) return false;
    i = j + token.size();
    return true;
}

bool eat_ident(const std::string& s, std::size_t& i, std::string& out) {
    std::size_t j = i;
    skip_ws(s, j);
    const std::size_t start = j;
    while (j < s.size() && is_ident_char(s[j])) ++j;
    if (j == start) return false;
    out = s.substr(start, j - start);
    i = j;
    return true;
}

std::size_t line_of(const std::string& s, std::size_t pos) {
    std::size_t line = 1;
    for (std::size_t i = 0; i < pos && i < s.size(); ++i) {
        if (s[i] == '\n') ++line;
    }
    return line;
}

struct KeyCopyScan {
    std::size_t declarations = 0;  // local key blocks seen
    std::vector<std::string> unguarded;  // "name (line N): what is wrong"
};

// Every occurrence of the key-block type that declares a variable: a local `NAME{};`. A reference or pointer (a parameter), a member (name ends in `_`)
// and anything that is not followed by an identifier are not declarations of a stack copy. A declaration in any other shape than `NAME{};` is reported
// rather than guessed at, because the guard could not tell what it holds.
KeyCopyScan scan_key_copies(const std::string& code) {
    KeyCopyScan out;
    constexpr std::string_view kType = "std::array<std::byte";
    for (std::size_t pos = code.find(kType); pos != std::string::npos; pos = code.find(kType, pos + kType.size())) {
        std::size_t i = pos + kType.size();
        if (!eat(code, i, ",") || !eat(code, i, "kKeyBlockSize") || !eat(code, i, ">")) continue;  // some other array
        std::size_t after_type = i;
        skip_ws(code, after_type);
        if (after_type < code.size() && (code[after_type] == '&' || code[after_type] == '*')) continue;  // a parameter
        std::string name;
        if (!eat_ident(code, i, name)) continue;  // a return type or a template argument
        if (!name.empty() && name.back() == '_') continue;  // a member
        ++out.declarations;
        const std::string where = name + " (line " + std::to_string(line_of(code, pos)) + ")";

        std::size_t j = i;
        if (!eat(code, j, "{") || !eat(code, j, "}") || !eat(code, j, ";")) {
            out.unguarded.push_back(where + ": declared in a shape other than `NAME{};` -- the guard cannot see what it holds");
            continue;
        }
        std::string guard_name;
        std::string buffer_arg;
        if (!eat(code, j, "ScopedSecureWipe") || !eat_ident(code, j, guard_name) || !eat(code, j, "(") || !eat_ident(code, j, buffer_arg) ||
            !eat(code, j, ")") || !eat(code, j, ";")) {
            out.unguarded.push_back(where + ": the next statement is not a ScopedSecureWipe over it");
            continue;
        }
        if (buffer_arg != name) out.unguarded.push_back(where + ": the ScopedSecureWipe next to it wipes `" + buffer_arg + "`, not this block");
    }
    return out;
}

std::string read_code(const char* rel) {
    const fs::path p = fs::path(HY_NATIVE_SOURCE_DIR) / rel;
    std::ifstream in(p, std::ios::binary);
    EXPECT_TRUE(in.is_open()) << "cannot open " << p.string();
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return hy::test_helpers::code_only(text);
}

TEST(KeyCopyWipeGuard, TheScannerSeesEveryWayADeclarationCanBeWrong) {
    // Good: guarded, with comments between the pieces and a reference parameter and a member that are not stack copies.
    const std::string good = R"(
        void f(const std::array<std::byte, kKeyBlockSize>& param) {
            std::array<std::byte, kKeyBlockSize> key_block{};
            ScopedSecureWipe wipe_key_block(key_block);
            use(key_block, param);
        }
        std::array<std::byte, kKeyBlockSize> member_{};
    )";
    const KeyCopyScan g = scan_key_copies(hy::test_helpers::code_only(good));
    EXPECT_EQ(g.declarations, 1u);
    EXPECT_TRUE(g.unguarded.empty());

    const std::pair<const char*, const char*> bad[] = {
        {"no guard at all", "void f() { std::array<std::byte, kKeyBlockSize> k{}; use(k); }"},
        {"guard is not the next statement", "void f() { std::array<std::byte, kKeyBlockSize> k{}; use(k); ScopedSecureWipe w(k); }"},
        {"guard wipes another block",
         "void f() { std::array<std::byte, kKeyBlockSize> k{}; std::array<std::byte, kKeyBlockSize> other{}; ScopedSecureWipe w(other); ScopedSecureWipe v(k); }"},
        {"declared with an assignment", "void f() { std::array<std::byte, kKeyBlockSize> k = make(); ScopedSecureWipe w(k); }"},
        {"a hand-written wipe is not enough", "void f() { std::array<std::byte, kKeyBlockSize> k{}; secure_wipe(k.data(), k.size()); }"},
    };
    for (const auto& [what, source] : bad) {
        const KeyCopyScan s = scan_key_copies(hy::test_helpers::code_only(source));
        EXPECT_GE(s.declarations, 1u) << what;
        EXPECT_FALSE(s.unguarded.empty()) << "the scanner let this through: " << what;
    }
}

TEST(KeyCopyWipeGuard, TheDurableAuditSinkWipesEveryKeyBlockItCopies) {
    const KeyCopyScan scan = scan_key_copies(read_code("include/hengyuan/durable_audit_sink.hpp"));
    EXPECT_GE(scan.declarations, 9u) << "fewer key blocks than when this guard was written: the scanner has lost sight of some";
    for (const std::string& problem : scan.unguarded) ADD_FAILURE() << "durable_audit_sink.hpp: " << problem;
}

TEST(KeyCopyWipeGuard, TheControlPlaneSinkWipesEveryKeyBlockItCopies) {
    const KeyCopyScan scan = scan_key_copies(read_code("include/hengyuan/control_plane_log_sink.hpp"));
    EXPECT_GE(scan.declarations, 6u) << "fewer key blocks than when this guard was written: the scanner has lost sight of some";
    for (const std::string& problem : scan.unguarded) ADD_FAILURE() << "control_plane_log_sink.hpp: " << problem;
}

TEST(KeyCopyWipeGuard, TheAuditExportWipesEveryKeyBlockItCopies) {
    const KeyCopyScan scan = scan_key_copies(read_code("include/hengyuan/durable_audit_export.hpp"));
    EXPECT_GE(scan.declarations, 4u) << "fewer key blocks than when this guard was written: the scanner has lost sight of some";
    for (const std::string& problem : scan.unguarded) ADD_FAILURE() << "durable_audit_export.hpp: " << problem;
}

}  // namespace
