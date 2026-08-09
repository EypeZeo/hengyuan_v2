// Pure in-memory tests for compaction_breadcrumb_io.hpp's platform-
// independent surface: is_traversal_safe_name() and ValidatedArtifactName.
// No file I/O in this file -- the POSIX openat/linkat/renameat2 wrappers
// (the other half of compaction_breadcrumb_io.hpp) and the Windows NT
// native equivalents (windows_native_io.hpp) are exercised for real by
// test_compaction_lease.cpp, which needs an actual directory to test
// against.
//
// Round D's own production code never calls is_traversal_safe_name() with
// anything except a fixed literal (see compaction_breadcrumb_io.hpp's
// header comment) -- these tests exist so the validator is proven correct
// now, for Round E/F's parameterized for_x1()/for_xgc() factories, not
// because Round D has a live attack surface here.
#include <gtest/gtest.h>
#include <hengyuan/compaction_breadcrumb_io.hpp>

#include <string>

using hy::compaction_detail::is_traversal_safe_name;
using hy::compaction_detail::kMaxArtifactNameLen;
using hy::compaction_detail::ValidatedArtifactName;

TEST(IsTraversalSafeName, RejectsEmpty) { EXPECT_FALSE(is_traversal_safe_name("")); }
TEST(IsTraversalSafeName, RejectsDot) { EXPECT_FALSE(is_traversal_safe_name(".")); }
TEST(IsTraversalSafeName, RejectsDotDot) { EXPECT_FALSE(is_traversal_safe_name("..")); }
TEST(IsTraversalSafeName, RejectsDotDotSlashX) { EXPECT_FALSE(is_traversal_safe_name("../x")); }
TEST(IsTraversalSafeName, RejectsEmbeddedSlash) { EXPECT_FALSE(is_traversal_safe_name("a/b")); }
TEST(IsTraversalSafeName, RejectsEmbeddedBackslash) { EXPECT_FALSE(is_traversal_safe_name("a\\b")); }
TEST(IsTraversalSafeName, RejectsAbsolutePathUnix) { EXPECT_FALSE(is_traversal_safe_name("/etc/passwd")); }
TEST(IsTraversalSafeName, RejectsAbsolutePathWindowsDriveStyleSlash) {
    // "C:\x" itself contains a backslash, already rejected by that rule --
    // this case specifically checks a drive-relative form with a forward
    // slash separator, which is also a real Windows path form.
    EXPECT_FALSE(is_traversal_safe_name("C:/x"));
}
TEST(IsTraversalSafeName, RejectsEmbeddedNul) {
    std::string s = "a";
    s.push_back('\0');
    s.push_back('b');
    EXPECT_FALSE(is_traversal_safe_name(s));
}
TEST(IsTraversalSafeName, RejectsOverlong) {
    const std::string too_long(kMaxArtifactNameLen + 1, 'a');
    EXPECT_FALSE(is_traversal_safe_name(too_long));
    const std::string exactly_max(kMaxArtifactNameLen, 'a');
    EXPECT_TRUE(is_traversal_safe_name(exactly_max));
}
TEST(IsTraversalSafeName, RejectsNonAscii) {
    // A UTF-8-encoded non-ASCII character (e.g. 'é' = 0xC3 0xA9) -- both
    // bytes have the high bit set, either one alone is enough to reject.
    const std::string s = "a\xC3\xA9z";
    EXPECT_FALSE(is_traversal_safe_name(s));
}
TEST(IsTraversalSafeName, RejectsLeadingDash) {
    EXPECT_FALSE(is_traversal_safe_name("-rf"));
}
TEST(IsTraversalSafeName, AcceptsLegalName) {
    EXPECT_TRUE(is_traversal_safe_name("compaction-candidate-intent"));
    EXPECT_TRUE(is_traversal_safe_name("compaction-intent-x-0123456789abcdef-0000000000000001.x1"));
    EXPECT_TRUE(is_traversal_safe_name("a"));
}

TEST(ValidatedArtifactName, ForCompactionCandidateIntentSatisfiesItsOwnValidator) {
    const auto name = ValidatedArtifactName::for_compaction_candidate_intent();
    EXPECT_TRUE(is_traversal_safe_name(name.relative_name()));
    EXPECT_EQ(name.relative_name(), "compaction-candidate-intent");
}
